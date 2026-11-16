// SPDX-License-Identifier: GPL-2.0-only
/* Hi6250 AGS2-L03 DRM scanout. Inherit the established DSI/panel timing,
 * own D0/MCTL0 shadow flips and real display IRQs. No CPU pixel copies. */
#include <linux/aperture.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <drm/clients/drm_client_setup.h>
#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_atomic_state_helper.h>
#include <drm/drm_connector.h>
#include <drm/drm_drv.h>
#include <drm/drm_fb_dma_helper.h>
#include <drm/drm_fbdev_dma.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_managed.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_simple_kms_helper.h>
#include <drm/drm_vblank.h>

#define PDP_ST 0x224
#define PDP_MASK 0x228
#define ITF_ST 0x180
#define ITF_MASK 0x184
#define D0 0x50000
#define D0_ADDR (D0+0x60)
#define D0_STRIDE (D0+0x64)
#define MCTL_MUTEX 0x10804
#define RCH6_FLUSH 0x10118
#define FW_FB 0x5d000000U
#define T5_SCANOUT_BYTES (4800U * 1920U)

struct t5_dss {
 struct drm_device drm;
 struct drm_simple_display_pipe pipe;
 struct drm_connector connector;
 void __iomem *base;
 spinlock_t reg_lock;
 int irq;
 u32 saved_pdp, saved_itf, saved_ctrl, saved_chctl;
 u32 last_addr, last_stride;
 bool active, vblank_enabled, pending, submitted, event_ref;
 u32 next_addr,next_pitch,next_format;
 struct drm_pending_vblank_event *event;
 atomic64_t vsync, underflow, flips, presented, missed_windows;
};
static struct t5_dss *to_t5(struct drm_device *drm)
{return container_of(drm,struct t5_dss,drm);}
static const struct drm_display_mode t5_mode={
 .clock=160000,
 .hdisplay=1200,.hsync_start=1240,.hsync_end=1260,.htotal=1300,
 .vdisplay=1920,.vsync_start=2070,.vsync_end=2083,.vtotal=2115,
 .type=DRM_MODE_TYPE_DRIVER|DRM_MODE_TYPE_PREFERRED,
};
static void t5_scanout_locked(struct t5_dss *d,u32 addr,u32 pitch,u32 format)
{
 u32 ctrl=(d->saved_ctrl & ~(0x1fU<<3)) |
          ((format==DRM_FORMAT_XRGB8888 ? 6U : 5U)<<3);
 /* MCTL holds shadow writes until the selected channel's frame boundary.
  * Match the original driver's mutex / channel-flush sequence. */
 writel(1,d->base+MCTL_MUTEX);
 writel(ctrl,d->base+D0+0x1c);
 writel(addr,d->base+D0_ADDR);
 writel(pitch/16,d->base+D0_STRIDE);
 writel(d->saved_chctl,d->base+D0+0xd4);
 writel(1,d->base+RCH6_FLUSH);
 wmb();
 writel(0,d->base+MCTL_MUTEX);
 readl(d->base+MCTL_MUTEX);
 d->last_addr=addr;d->last_stride=pitch;
 atomic64_inc(&d->flips);
}
static void t5_scanout(struct t5_dss *d,u32 addr,u32 pitch,u32 format)
{
 unsigned long flags;
 spin_lock_irqsave(&d->reg_lock,flags);
 t5_scanout_locked(d,addr,pitch,format);
 spin_unlock_irqrestore(&d->reg_lock,flags);
}
static void t5_finish_event(struct t5_dss *d,struct drm_pending_vblank_event *e,bool ref)
{
 unsigned long flags;
 if(!e)return;
 spin_lock_irqsave(&d->drm.event_lock,flags);
 drm_crtc_send_vblank_event(&d->pipe.crtc,e);
 spin_unlock_irqrestore(&d->drm.event_lock,flags);
 if(ref)drm_crtc_vblank_put(&d->pipe.crtc);
}
static void t5_set_fb(struct drm_simple_display_pipe *pipe)
{
 struct drm_plane_state *ps=pipe->plane.state;
 dma_addr_t addr;
 if(!ps || !ps->fb)return;
 addr=drm_fb_dma_get_gem_addr(ps->fb,ps,0);
 t5_scanout(to_t5(pipe->crtc.dev),(u32)addr,ps->fb->pitches[0],ps->fb->format->format);
}
static irqreturn_t t5_irq(int irq,void *arg)
{
 struct t5_dss *d=arg;
 u32 top=readl(d->base+PDP_ST),st;
 struct drm_pending_vblank_event *e=NULL;
 bool ref=false;
 unsigned long flags;
 if(!(top&BIT(16)))return IRQ_NONE;
 st=readl(d->base+ITF_ST);
 writel(st,d->base+ITF_ST);writel(BIT(16),d->base+PDP_ST);
 if(st&BIT(4)) {
  atomic64_inc(&d->vsync);
  if(READ_ONCE(d->vblank_enabled))drm_crtc_handle_vblank(&d->pipe.crtc);
 }
 /* Queue DMA shadow changes only in the real vertical blank window.
  * Complete flips at the following active-start, after the new address
  * has latched. This keeps the old GEM buffer alive until scanout moves. */
 spin_lock_irqsave(&d->reg_lock,flags);
 if((st&BIT(7)) && d->submitted) {
  e=d->event;ref=d->event_ref;d->event=NULL;d->event_ref=false;
  d->submitted=false;atomic64_inc(&d->presented);
 }
 if(st&BIT(8)) {
  if((st&BIT(7)) && d->pending)atomic64_inc(&d->missed_windows);
  else if(d->pending) {
   t5_scanout_locked(d,d->next_addr,d->next_pitch,d->next_format);
   d->pending=false;d->submitted=true;
  }
 }
 spin_unlock_irqrestore(&d->reg_lock,flags);
 t5_finish_event(d,e,ref);
 if(st&BIT(2))atomic64_inc(&d->underflow);
 return IRQ_HANDLED;
}
static int t5_enable_vblank(struct drm_simple_display_pipe *pipe)
{
 struct t5_dss *d=to_t5(pipe->crtc.dev);
 WRITE_ONCE(d->vblank_enabled,true);
 return 0;
}
static void t5_disable_vblank(struct drm_simple_display_pipe *pipe)
{
 struct t5_dss *d=to_t5(pipe->crtc.dev);
 WRITE_ONCE(d->vblank_enabled,false);
}
static void t5_enable(struct drm_simple_display_pipe *pipe,
                     struct drm_crtc_state *cs,struct drm_plane_state *ps)
{
 struct t5_dss *d=to_t5(pipe->crtc.dev);
 d->active=true;
 drm_crtc_vblank_on(&pipe->crtc);
 t5_set_fb(pipe);
}
static void t5_disable(struct drm_simple_display_pipe *pipe)
{
 struct t5_dss *d=to_t5(pipe->crtc.dev);
 struct drm_pending_vblank_event *e;
 bool ref;
 unsigned long flags;
 /* Switch to the cleared, permanently reserved black frame before GEM buffers can
  * be released. The DSI timing engine keeps running throughout. */
 spin_lock_irqsave(&d->reg_lock,flags);
 e=d->event;ref=d->event_ref;d->event=NULL;d->event_ref=false;
 d->pending=false;d->submitted=false;d->active=false;
 spin_unlock_irqrestore(&d->reg_lock,flags);
 t5_scanout(d,FW_FB,4800,DRM_FORMAT_XRGB8888);
 usleep_range(40000,45000);
 t5_finish_event(d,e,ref);
 drm_crtc_vblank_off(&pipe->crtc);
}
static enum drm_mode_status t5_mode_valid(struct drm_simple_display_pipe *p,
                                        const struct drm_display_mode *m)
{
 return drm_mode_match(m,&t5_mode,DRM_MODE_MATCH_TIMINGS|DRM_MODE_MATCH_CLOCK) ? MODE_OK : MODE_BAD;
}
static int t5_check(struct drm_simple_display_pipe *pipe,
                   struct drm_plane_state *ps,struct drm_crtc_state *cs)
{
 struct drm_gem_dma_object *obj;
 dma_addr_t addr;
 u64 end;
 if(!ps->fb)return 0;
 if(ps->fb->pitches[0]!=4800 || ps->fb->modifier!=DRM_FORMAT_MOD_LINEAR ||
    ps->src_x || ps->src_y || ps->src_w!=(1200U<<16) || ps->src_h!=(1920U<<16) ||
    ps->crtc_x || ps->crtc_y || ps->crtc_w!=1200 || ps->crtc_h!=1920 ||
    ps->rotation!=DRM_MODE_ROTATE_0)return -EINVAL;
 obj=drm_fb_dma_get_gem_obj(ps->fb,0);if(!obj)return -EINVAL;
 addr=drm_fb_dma_get_gem_addr(ps->fb,ps,0);
 end=(u64)ps->fb->offsets[0]+4800ULL*1920;
 if(!addr || (addr&15) || (u64)addr+4800ULL*1920>0x100000000ULL ||
    end>obj->base.size)return -EINVAL;
 return 0;
}
static void t5_update(struct drm_simple_display_pipe *pipe,
                     struct drm_plane_state *old)
{
 struct drm_crtc *crtc=&pipe->crtc;
 struct t5_dss *d=to_t5(crtc->dev);
 struct drm_plane_state *ps=pipe->plane.state;
 struct drm_pending_vblank_event *event=crtc->state->event;
 bool ref=false;
 unsigned long flags;
 crtc->state->event=NULL;
 if(!d->active || !ps || !ps->fb) {
  t5_set_fb(pipe);t5_finish_event(d,event,false);return;
 }
 if(event)ref=drm_crtc_vblank_get(crtc)==0;
 spin_lock_irqsave(&d->reg_lock,flags);
 d->next_addr=(u32)drm_fb_dma_get_gem_addr(ps->fb,ps,0);
 d->next_pitch=ps->fb->pitches[0];d->next_format=ps->fb->format->format;
 d->event=event;d->event_ref=ref;d->pending=true;
 spin_unlock_irqrestore(&d->reg_lock,flags);
}
static int t5_prepare_fb(struct drm_simple_display_pipe *pipe,
                         struct drm_plane_state *state)
{return drm_gem_plane_helper_prepare_fb(&pipe->plane,state);}
static const struct drm_simple_display_pipe_funcs t5_pipe_funcs={
 .mode_valid=t5_mode_valid,.check=t5_check,.enable=t5_enable,.disable=t5_disable,
 .update=t5_update,.prepare_fb=t5_prepare_fb,
 .enable_vblank=t5_enable_vblank,.disable_vblank=t5_disable_vblank,
};
static int t5_get_modes(struct drm_connector *connector)
{
 struct drm_display_mode *m=drm_mode_duplicate(connector->dev,&t5_mode);
 if(!m)return -ENOMEM;
 drm_mode_set_name(m);drm_mode_probed_add(connector,m);
 connector->display_info.width_mm=135;connector->display_info.height_mm=216;
 return 1;
}
static enum drm_connector_status t5_detect(struct drm_connector *c,bool force)
{return connector_status_connected;}
static const struct drm_connector_helper_funcs t5_connector_helpers={.get_modes=t5_get_modes};
static const struct drm_connector_funcs t5_connector_funcs={
 .reset=drm_atomic_helper_connector_reset,.detect=t5_detect,
 .fill_modes=drm_helper_probe_single_connector_modes,.destroy=drm_connector_cleanup,
 .atomic_duplicate_state=drm_atomic_helper_connector_duplicate_state,
 .atomic_destroy_state=drm_atomic_helper_connector_destroy_state,
};
static const struct drm_mode_config_funcs t5_mode_config_funcs={
 .fb_create=drm_gem_fb_create,.atomic_check=drm_atomic_helper_check,
 .atomic_commit=drm_atomic_helper_commit,
};
static ssize_t scanout_status_show(struct device *dev,struct device_attribute *a,char *buf)
{
 struct t5_dss *d=dev_get_drvdata(dev);
 return sysfs_emit(buf,"native_dss=1\nactive=%u\nvsync=%lld\nflips=%lld\npresented=%lld\nmissed_windows=%lld\nunderflow=%lld\nlast_dma=%08x\nlast_stride=%u\nprogrammed_dma=%08x\ncpu_pixel_copies=0\nldi=%08x\n",d->active,atomic64_read(&d->vsync),atomic64_read(&d->flips),atomic64_read(&d->presented),atomic64_read(&d->missed_windows),atomic64_read(&d->underflow),d->last_addr,d->last_stride,readl(d->base+D0_ADDR),readl(d->base+0x7d024));
}
static DEVICE_ATTR_RO(scanout_status);
static const struct attribute_group t5_group={.attrs=(struct attribute *[]){&dev_attr_scanout_status.attr,NULL}};
DEFINE_DRM_GEM_DMA_FOPS(t5_fops);
static const struct drm_driver t5_drm_driver={
 .driver_features=DRIVER_GEM|DRIVER_MODESET|DRIVER_ATOMIC,
 .fops=&t5_fops,DRM_GEM_DMA_DRIVER_OPS,DRM_FBDEV_DMA_DRIVER_OPS,
 .name="kirin",.desc="AGS2 Hi6250 native DSS",.major=1,.minor=0,
};
/* The display continues fetching during DPMS and suspend. Reuse precisely
 * the visible bytes of the permanently reserved boot buffer for a black
 * frame after the console driver has given up ownership. The adjacent tail
 * reclaimed by the page allocator must never be remapped or written here.
 */
static int t5_takeover_framebuffer(struct device *dev)
{
 struct device_node *region;
 struct resource res;
 void __iomem *fb;
 int ret;

 region=of_find_node_by_path("/reserved-memory/framebuffer@5d000000");
 if (!region)
  return -ENODEV;
 ret=of_address_to_resource(region,0,&res);
 if (!ret && (!of_property_present(region,"no-map") ||
              res.start!=FW_FB || resource_size(&res)<T5_SCANOUT_BYTES))
  ret=-EINVAL;
 of_node_put(region);
 if (ret)
  return dev_err_probe(dev,ret,"invalid permanent framebuffer reservation\n");
 fb=ioremap_wc(res.start,T5_SCANOUT_BYTES);
 if (!fb)
  return -ENOMEM;
 ret=aperture_remove_conflicting_devices(res.start,resource_size(&res),
                                         t5_drm_driver.name);
 if (!ret) {
  memset_io(fb,0,T5_SCANOUT_BYTES);
  wmb();
  if (readl(fb) || readl(fb+T5_SCANOUT_BYTES/2) ||
      readl(fb+T5_SCANOUT_BYTES-sizeof(u32)))
   ret=-EIO;
  else
   dev_info(dev,"black fallback scanout ready, %u bytes\n",T5_SCANOUT_BYTES);
 }
 iounmap(fb);
 return ret;
}
static int t5_probe(struct platform_device *pdev)
{
 static const u32 formats[]={DRM_FORMAT_XRGB8888,DRM_FORMAT_ARGB8888};
 static const u64 modifiers[]={DRM_FORMAT_MOD_LINEAR,DRM_FORMAT_MOD_INVALID};
 struct device *dev=&pdev->dev;
 struct t5_dss *d;
 struct drm_device *drm;
 int ret;
 d=devm_drm_dev_alloc(dev,&t5_drm_driver,struct t5_dss,drm);
 if(IS_ERR(d))return PTR_ERR(d);
 drm=&d->drm;platform_set_drvdata(pdev,d);spin_lock_init(&d->reg_lock);
 d->base=devm_platform_ioremap_resource(pdev,0);if(IS_ERR(d->base))return PTR_ERR(d->base);
 if(readl(d->base+D0_ADDR)!=FW_FB || readl(d->base+D0_STRIDE)!=300 ||
    readl(d->base+D0+8)!=299 || readl(d->base+D0+12)!=1919 ||
    (readl(d->base+D0+0x1c)&BIT(8)) || !(readl(d->base+0x7d024)&1))
  return dev_err_probe(dev,-ENODEV,"DSS inherited D0 configuration does not match validated board\n");
 d->saved_ctrl=readl(d->base+D0+0x1c);d->saved_chctl=readl(d->base+D0+0xd4);
 d->saved_pdp=readl(d->base+PDP_MASK);d->saved_itf=readl(d->base+ITF_MASK);
 ret=dma_set_mask_and_coherent(dev,DMA_BIT_MASK(32));if(ret)return ret;
 ret=drmm_mode_config_init(drm);if(ret)return ret;
 drm->mode_config.funcs=&t5_mode_config_funcs;
 drm->mode_config.min_width=1200;drm->mode_config.max_width=1200;
 drm->mode_config.min_height=1920;drm->mode_config.max_height=1920;
 drm->mode_config.preferred_depth=32;
 ret=drm_connector_init(drm,&d->connector,&t5_connector_funcs,DRM_MODE_CONNECTOR_DSI);if(ret)return ret;
 drm_connector_helper_add(&d->connector,&t5_connector_helpers);
 ret=drm_connector_set_panel_orientation(&d->connector,DRM_MODE_PANEL_ORIENTATION_RIGHT_UP);if(ret)return ret;
 ret=drm_simple_display_pipe_init(drm,&d->pipe,&t5_pipe_funcs,formats,ARRAY_SIZE(formats),modifiers,&d->connector);if(ret)return ret;
 ret=drm_vblank_init(drm,1);if(ret)return ret;
 drm_mode_config_reset(drm);
 d->irq=platform_get_irq(pdev,0);if(d->irq<0)return d->irq;
 ret=devm_request_irq(dev,d->irq,t5_irq,IRQF_NO_AUTOEN,dev_name(dev),d);if(ret)return ret;
 ret=devm_device_add_group(dev,&t5_group);if(ret)return ret;
 ret=t5_takeover_framebuffer(dev);if(ret)return ret;
 writel(~0U,d->base+PDP_MASK);writel(~0U,d->base+ITF_MASK);
 writel(~0U,d->base+ITF_ST);writel(BIT(16),d->base+PDP_ST);
 writel(~(u32)(BIT(2)|BIT(4)|BIT(7)|BIT(8)),d->base+ITF_MASK);writel(~(u32)BIT(16),d->base+PDP_MASK);
 enable_irq(d->irq);
 ret=drm_dev_register(drm,0);
 if(ret) {disable_irq(d->irq);writel(d->saved_itf,d->base+ITF_MASK);writel(d->saved_pdp,d->base+PDP_MASK);return ret;}
 dev_info(dev,"DSS native DMA scanout 1200x1920 ~58.2Hz; GPU landscape composition; real vblank; no pixel copies\n");
 drm_client_setup(drm,NULL);
 return 0;
}
static void t5_remove(struct platform_device *pdev)
{
 struct t5_dss *d=platform_get_drvdata(pdev);
 drm_dev_unplug(&d->drm);drm_atomic_helper_shutdown(&d->drm);
 disable_irq(d->irq);writel(d->saved_itf,d->base+ITF_MASK);writel(d->saved_pdp,d->base+PDP_MASK);
}
static void t5_shutdown(struct platform_device *pdev)
{drm_atomic_helper_shutdown(&((struct t5_dss *)platform_get_drvdata(pdev))->drm);}
static const struct of_device_id t5_match[]={{.compatible="huawei,ags2l03-dss"},{}};
MODULE_DEVICE_TABLE(of,t5_match);
static struct platform_driver t5_driver={.probe=t5_probe,.remove=t5_remove,.shutdown=t5_shutdown,.driver={.name="t5-dss",.of_match_table=t5_match}};
module_platform_driver(t5_driver);
MODULE_LICENSE("GPL");
