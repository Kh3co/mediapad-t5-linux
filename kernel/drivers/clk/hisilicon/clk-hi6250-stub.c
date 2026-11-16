#include <linux/of_platform.h>
// SPDX-License-Identifier: GPL-2.0-or-later
/* Hi6250 firmware clocks; derived from the Tildeguy Hi6250 clock port
 * and HiSilicon/Linaro hi3660 stub driver. Assisted-by: LLM.
 * Frequencies, commands and SCBAKDATA4 layout checked against this T5's DT
 * and Huawei's GPL clk-hi3xxx implementation. LPM3 owns voltage sequencing.
 */
#include <linux/clk-provider.h>
#include <linux/workqueue.h>
#include <linux/cpu.h>
#include <linux/ktime.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/math64.h>
#include <linux/hi6250-t5.h>
#include <linux/mailbox_client.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <dt-bindings/clock/hi6250-clock.h>

struct hi6250_fw;
struct hi6250_clock {
	struct clk_hw hw;
	struct hi6250_fw *fw;
	const unsigned long *rates;
	unsigned int count, shift, command;
};
struct hi6250_fw {
 bool cpu1_daily;
 struct delayed_work cpu1_guard;bool cpu1_oc_active;u32 cpu1_errors,cpu1_restores,cpu1_vol_idx,cpu1_request_mhz;unsigned long cpu1_deadline;
 bool cpu0_daily;struct delayed_work cpu0_guard;bool cpu0_oc_active;u32 cpu0_errors,cpu0_restores,cpu0_vol_idx,cpu0_request_mhz;unsigned long cpu0_deadline;
 void __iomem *gpu_pmctrl;
 bool gpu_oc_active;
 u64 gpu_oc_up,gpu_oc_down,gpu_oc_errors;
 int gpu_oc_last_error;
	struct mbox_client client;
	struct mbox_chan *channel;
	struct regmap *sysctrl;
	struct mutex lock;
	struct hi6250_clock clocks[HI6250_CLK_STUB_NUM];
	u32 message[8];
};

static const unsigned long little_rates[] = {480000000, 807000000, 1306000000, 1709000000, 1900000000};
static const unsigned long big_rates[] = {1402000000, 1805000000, 2016000000, 2112000000, 2362000000, 2570000000};
static const unsigned long ddr_rates[] = {120000000, 240000000, 360000000, 533000000, 800000000, 933000000};
static const unsigned long gpu_firmware_rates[] = {120000000, 240000000, 360000000, 480000000, 680000000, 800000000, 900000000};
static const unsigned long gpu_rates[] = {120000000, 240000000, 360000000, 480000000, 680000000, 800000000, 900000000, 1146000000};

/* Real GPU clock decoding: LPM3 index6 describes the stock900MHz rail
 * profile, while the dedicated APLL2 can run the validated1146MHz clock. */
static unsigned long t5_gpu_real_rate(struct hi6250_fw *fw,u32 index)
{
 void __iomem *p=fw->gpu_pmctrl;
 u32 sel,div,a,b,ref,post1,post2;
 u64 feedback;
 if(index>=ARRAY_SIZE(gpu_firmware_rates))return 0;
 if(index!=6)return gpu_firmware_rates[index];
 sel=readl(p+0x220);div=readl(p+0x4ac);
 div=(div>>((sel&BIT(4))?8:0))&0xff;
 if(div!=0xa0)return gpu_firmware_rates[index];
 a=readl(p+0x4a4);b=readl(p+0x4a8);
 ref=(a>>2)&63;post1=(a>>20)&7;post2=(a>>23)&7;
 if(!(a&1)||!ref||!post1||!post2)return gpu_firmware_rates[index];
 feedback=((u64)((a>>8)&0xfff)<<24)+((b&BIT(24))?0:(b&0xffffff));
 return div64_u64(feedback*19200000ULL,(u64)ref*post1*post2*0x1000000);
}

/* Same cfg_vld sequence and small fractional steps verified in v037.
 * Only feedback fields are written: voltage/mux/ref/postdiv stay untouched. */
static int t5_gpu_ramp(struct hi6250_fw *fw,u32 target)
{
 void __iomem *p=fw->gpu_pmctrl;u32 a,b,next,feedback;int n;
 if(target<0x5dc00000 || target>0x77600000)return -EINVAL;
 for(n=0;n<128;n++) {
  a=readl(p+0x4a4);b=readl(p+0x4a8);
  if((a&~(0xfff00|BIT(26)))!=0x00a00005 || (b&BIT(24)))return -EIO;
  feedback=((a>>8)&0xfff)*0x1000000+(b&0xffffff);
  if(feedback==target)return 0;
  if(feedback<0x5dc00000 || feedback>0x77600000)return -EIO;
  next=feedback<target?feedback+min(target-feedback,0x400000U):feedback-min(feedback-target,0x400000U);
  writel(b&~BIT(25),p+0x24);udelay(1);
  writel((a&~(0xfff00|BIT(26)))|((next>>24)<<8),p+0x20);
  writel((b&0xff000000&~BIT(25))|(next&0xffffff),p+0x24);
  writel((b&0xff000000)|BIT(25)|(next&0xffffff),p+0x24);
  udelay(5);
  a=readl(p+0x4a4);b=readl(p+0x4a8);
  if((((a>>8)&0xfff)*0x1000000+(b&0xffffff))!=next)return -EIO;
 }
 return -ETIMEDOUT;
}

static int t5_gpu_restore(struct hi6250_fw *fw)
{
 u32 idx;int ret;
 if(!fw->gpu_oc_active)return 0;
 ret=regmap_read(fw->sysctrl,0x41c,&idx);if(ret)return ret;
 /* A firmware power cycle may already have restored or disabled APLL2. */
 if(t5_gpu_real_rate(fw,(idx>>12)&15)>900000000) {
  ret=t5_gpu_ramp(fw,0x5dc00000);if(ret)return ret;
  fw->gpu_oc_down++;
 }
 fw->gpu_oc_active=false;
 return 0;
}

static ssize_t gpu_oc_state_show(struct device *dev,struct device_attribute *a,char *buf)
{
 struct hi6250_fw *fw=dev_get_drvdata(dev);u32 idx;int ret;
 mutex_lock(&fw->lock);
 ret=regmap_read(fw->sysctrl,0x41c,&idx);
 if(!ret)ret=sysfs_emit(buf,"firmware_index=%u\nactual_register_hz=%lu\noc_active=%u\nup_transitions=%llu\ndown_transitions=%llu\nerrors=%llu\nlast_error=%d\n",
 (idx>>12)&15,t5_gpu_real_rate(fw,(idx>>12)&15),fw->gpu_oc_active,
 (unsigned long long)fw->gpu_oc_up,(unsigned long long)fw->gpu_oc_down,
 (unsigned long long)fw->gpu_oc_errors,fw->gpu_oc_last_error);
 mutex_unlock(&fw->lock);return ret;
}
static DEVICE_ATTR_RO(gpu_oc_state);


/* Bounded PMCTRL CPU snapshot. No PLL/mux/voltage writes. */
static ssize_t cpu_clock_registers_show(struct device *dev,struct device_attribute *a,char *buf)
{
 struct hi6250_fw *fw=dev_get_drvdata(dev);void __iomem *p=fw->gpu_pmctrl;
 static const u16 offsets[]={0x10,0x14,0x18,0x1c,0x494,0x498,0x49c,0x4a0,
  0x98,0x9c,0xa0,0xa4,0xa8,0xac,0xb0,0xb4,0xb8,0xbc,0xc0,0xe8,0xf0,0x154,
  0x158,0x15c,0x160,0x164,0x168,0x16c,0x170,0x174,0x178,0x17c,0x180,0x1a8,0x1b0,0x214};
 unsigned int i,n=0;u32 v;
 mutex_lock(&fw->lock);
 if(!regmap_read(fw->sysctrl,0x41c,&v))n+=sysfs_emit_at(buf,n,"SCBAKDATA4=%08x\n",v);
 for(i=0;i<ARRAY_SIZE(offsets);i++)n+=sysfs_emit_at(buf,n,"PMCTRL_%03x=%08x\n",offsets[i],readl(p+offsets[i]));
 mutex_unlock(&fw->lock);return n;
}
static DEVICE_ATTR_RO(cpu_clock_registers);

/* RAM-only CPU0-cluster experiment, exact v062 live maximum baseline.
 * PLL0 19.2MHz*89 -> *99, no changes to mux, ref/post dividers or voltage. */
static u64 t5_cpu0_pll_hz(struct hi6250_fw *fw)
{
 void __iomem *p=fw->gpu_pmctrl;u32 a=readl(p+0x494),b=readl(p+0x498);
 u32 ref=(a>>2)&63,post1=(a>>20)&7,post2=(a>>23)&7;u64 feedback;
 if(readl(p+0x9c)!=2 || ((readl(p+0xa4)>>8)&3)!=0 || !(a&1) || !ref || !post1 || !post2)return 0;
 feedback=((u64)((a>>8)&4095)<<24)+((b&BIT(24))?0:(b&0xffffff));
 return div64_u64(feedback*19200000ULL,(u64)ref*post1*post2*0x1000000);
}
static int t5_cpu0_ramp(struct hi6250_fw *fw,u32 target)
{
 void __iomem *p=fw->gpu_pmctrl;u32 a,b,feedback,next;int i;
 if(target!=0x59000000U && (target<0x5dc00000U || target>0x682aaaabU))return -EINVAL;
 for(i=0;i<64;i++) {
  a=readl(p+0x494);b=readl(p+0x498);
  if(readl(p+0x9c)!=2 || readl(p+0xa4)!=0x10 || readl(p+0xa8)!=0x9595 ||
     (a&~0xfff00U)!=0x00900005 || (b&0xff000000)!=0x06000000 ||
     readl(p+0xf0)!=0 || (target!=0x59000000U && ((readl(p+0x154)>>12)&255)!=fw->cpu0_vol_idx))return -EIO;
  feedback=(((a>>8)&4095)<<24)+(b&0xffffff);
  if(feedback<0x59000000U || feedback>0x682aaaabU)return -EIO;
  if(feedback==target)return 0;
  next=feedback<target?feedback+min(target-feedback,0x400000U):feedback-min(feedback-target,0x400000U);
  writel(b&~BIT(25),p+0x14);udelay(1);
  writel((a&~0xfff00U)|((next>>24)<<8),p+0x10);
  writel((b&0xff000000&~BIT(25))|(next&0xffffff),p+0x14);
  writel((b&0xff000000)|BIT(25)|(next&0xffffff),p+0x14);udelay(10);
  a=readl(p+0x494);b=readl(p+0x498);
  if((((a>>8)&4095)*0x1000000+(b&0xffffff))!=next)return -EIO;
 }
 return -ETIMEDOUT;
}
static int t5_cpu0_restore(struct hi6250_fw *fw)
{
 int ret;u64 hz;if(!fw->cpu0_oc_active)return 0;
 hz=t5_cpu0_pll_hz(fw);
 /* Firmware may have replaced the OC with a lower native PLL. Never ramp it back up. */
 if(hz && hz<=1708800000ULL){fw->cpu0_daily=false;fw->cpu0_oc_active=false;fw->cpu0_restores++;return 0;}
 ret=t5_cpu0_ramp(fw,0x59000000U);
 if(!ret){fw->cpu0_daily=false;fw->cpu0_oc_active=false;fw->cpu0_restores++;}
 return ret;
}
static void t5_cpu0_guard(struct work_struct *work)
{
 struct hi6250_fw *fw=container_of(to_delayed_work(work),struct hi6250_fw,cpu0_guard);
 int temp=0,tret=hi6250_t5_read_max_temp(&temp),ret;u32 expected,live;
 bool terr=tret!=0,over=temp>=90000,volt,hot,expired;
 mutex_lock(&fw->lock);
 /* Compare the same DVFS generation: sampling before lock can mistake
  * the previous idle rail for a new OC rail during a legitimate upshift. */
 expected=fw->cpu0_vol_idx;live=(readl(fw->gpu_pmctrl+0x154)>>12)&255;
 volt=live!=expected;hot=terr||over||volt;
 expired=time_after_eq(jiffies,fw->cpu0_deadline);
 if(fw->cpu0_oc_active && (hot || (!fw->cpu0_daily && expired))) {
  void __iomem *p=fw->gpu_pmctrl;u32 idx=0,v0=readl(p+0x154),v1=readl(p+0x214);
  u32 a0=readl(p+0x494),b0=readl(p+0x498),a1=readl(p+0x49c),b1=readl(p+0x4a0),ag=readl(p+0x4a4),bg=readl(p+0x4a8);
  u32 av0=readl(p+0xf0),av1=readl(p+0x1b0),s0=readl(p+0x9c),m0=readl(p+0xa4),d0=readl(p+0xa8),s1=readl(p+0x15c),m1=readl(p+0x164),d1=readl(p+0x168);
  int ir=regmap_read(fw->sysctrl,0x41c,&idx);
  ret=t5_cpu0_restore(fw);if(ret)fw->cpu0_errors++;
  dev_info(fw->client.dev,"T5_OC_REASON cpu=0 temp_ret=%d temp=%d over90=%u volt_changed=%u expected_idx=%u live_before_lock=%u expired=%u restore=%d BEFORE v0=%08x v1=%08x pll0=%08x/%08x pll1=%08x/%08x pllg=%08x/%08x scbak=%08x idx_ret=%d\n",tret,temp,over,volt,expected,live,expired,ret,v0,v1,a0,b0,a1,b1,ag,bg,idx,ir);
  dev_info(fw->client.dev,"T5_OC_BEFORE_MODE cpu=0 avs0=%08x avs1=%08x mux0=%08x/%08x/%08x mux1=%08x/%08x/%08x\n",av0,av1,s0,m0,d0,s1,m1,d1);
 }
 if(fw->cpu0_oc_active)queue_delayed_work_on(4,system_dfl_wq,&fw->cpu0_guard,msecs_to_jiffies(100));
 mutex_unlock(&fw->lock);
}
static ssize_t cpu0_oc_show(struct device *dev,struct device_attribute *a,char *buf)
{
 struct hi6250_fw *fw=dev_get_drvdata(dev);ssize_t n;
 mutex_lock(&fw->lock);
 n=sysfs_emit(buf,"active=%u\nrequested_mhz=%u\nactual_pll_hz=%llu\nvoltage_index=%u\nlive_voltage_index=%u\nerrors=%u\nrestores=%u\nlease_remaining_ms=%u\n",fw->cpu0_oc_active,fw->cpu0_request_mhz,t5_cpu0_pll_hz(fw),fw->cpu0_vol_idx,(readl(fw->gpu_pmctrl+0x154)>>12)&255,fw->cpu0_errors,fw->cpu0_restores,time_before(jiffies,fw->cpu0_deadline)?jiffies_to_msecs(fw->cpu0_deadline-jiffies):0);
 mutex_unlock(&fw->lock);return n;
}
static ssize_t cpu0_oc_store(struct device *dev,struct device_attribute *a,const char *buf,size_t n)
{
 struct hi6250_fw *fw=dev_get_drvdata(dev);u32 req,idx,target;int ret=0,temp;
 if(kstrtou32(buf,0,&req)||(req!=0 && req!=1 && (req<1800 || req>1950 || req%10)))return -EINVAL;
 mutex_lock(&fw->lock);
 if(req==0){ret=t5_cpu0_restore(fw);goto out;}
 if(req==1){if(!fw->cpu0_oc_active){ret=-EINVAL;goto out;}goto lease;}
 if(fw->cpu0_oc_active){ret=-EBUSY;goto out;}
 ret=hi6250_t5_read_max_temp(&temp);if(ret || temp>=85000){ret=-EAGAIN;goto out;}
 ret=regmap_read(fw->sysctrl,0x41c,&idx);if(ret)goto out;
 if((idx&15)!=3 || readl(fw->gpu_pmctrl+0x494)!=0x00905905 || readl(fw->gpu_pmctrl+0x498)!=0x06000000 || t5_cpu0_pll_hz(fw)!=1708800000ULL){ret=-EOPNOTSUPP;goto out;}
 fw->cpu0_vol_idx=(readl(fw->gpu_pmctrl+0x154)>>12)&255;
 if(!fw->cpu0_vol_idx || fw->cpu0_vol_idx==255){ret=-EOPNOTSUPP;goto out;}
 fw->cpu0_oc_active=true;
 target=(u32)div64_u64((u64)req*5*0x1000000ULL+48,96);
 fw->cpu0_request_mhz=req;ret=t5_cpu0_ramp(fw,target);
 if(ret){fw->cpu0_errors++;t5_cpu0_restore(fw);goto out;}
 dev_info(dev,"RAM CPU0-cluster APLL0 request%uMHz actual%lluHz; voltage index unchanged=%u\n",req,t5_cpu0_pll_hz(fw),fw->cpu0_vol_idx);
lease:
 fw->cpu0_deadline=jiffies+msecs_to_jiffies(20000);
 mod_delayed_work_on(4,system_dfl_wq,&fw->cpu0_guard,msecs_to_jiffies(100));
out:
 mutex_unlock(&fw->lock);return ret?ret:n;
}
static DEVICE_ATTR_RW(cpu0_oc);
static u64 t5_cpu1_pll_hz(struct hi6250_fw *fw)
{
 void __iomem *p=fw->gpu_pmctrl;u32 a=readl(p+0x49c),b=readl(p+0x4a0);
 u32 ref=(a>>2)&63,post1=(a>>20)&7,post2=(a>>23)&7;u64 feedback;
 if(readl(p+0x15c)!=2 || ((readl(p+0x164)>>8)&3)!=1 || !(a&1) || !ref || !post1 || !post2)return 0;
 feedback=((u64)((a>>8)&4095)<<24)+((b&BIT(24))?0:(b&0xffffff));
 return div64_u64(feedback*19200000ULL,(u64)ref*post1*post2*0x1000000);
}
static int t5_cpu1_ramp(struct hi6250_fw *fw,u32 target)
{
 void __iomem *p=fw->gpu_pmctrl;u32 a,b,feedback,next;int i;
 if(target!=0x7b000000U && (target<0x7d000000U || target>0x9c400000U))return -EINVAL;
 for(i=0;i<192;i++) {
  a=readl(p+0x49c);b=readl(p+0x4a0);
  if(readl(p+0x15c)!=2 || readl(p+0x164)!=0x121 || readl(p+0x168)!=0x9595 ||
     (a&~0xfff00U)!=0x00900005 || (b&0xff000000)!=0x06000000 ||
     readl(p+0x1b0)!=0 || (target!=0x7b000000U && ((readl(p+0x214)>>12)&255)!=fw->cpu1_vol_idx))return -EIO;
  feedback=(((a>>8)&4095)<<24)+(b&0xffffff);
  if(feedback<0x7b000000U || feedback>0x9c400000U)return -EIO;
  if(feedback==target)return 0;
  next=feedback<target?feedback+min(target-feedback,0x400000U):feedback-min(feedback-target,0x400000U);
  writel(b&~BIT(25),p+0x1c);udelay(1);
  writel((a&~0xfff00U)|((next>>24)<<8),p+0x18);
  writel((b&0xff000000&~BIT(25))|(next&0xffffff),p+0x1c);
  writel((b&0xff000000)|BIT(25)|(next&0xffffff),p+0x1c);udelay(10);
  a=readl(p+0x49c);b=readl(p+0x4a0);
  if((((a>>8)&4095)*0x1000000+(b&0xffffff))!=next)return -EIO;
 }
 return -ETIMEDOUT;
}
static int t5_cpu1_restore(struct hi6250_fw *fw)
{
 int ret;u64 hz;if(!fw->cpu1_oc_active)return 0;
 hz=t5_cpu1_pll_hz(fw);
 /* Firmware may have replaced the OC with a lower native PLL. Never ramp it back up. */
 if(hz && hz<=2361600000ULL){fw->cpu1_daily=false;fw->cpu1_oc_active=false;fw->cpu1_restores++;return 0;}
 ret=t5_cpu1_ramp(fw,0x7b000000U);
 if(!ret){fw->cpu1_daily=false;fw->cpu1_oc_active=false;fw->cpu1_restores++;}
 return ret;
}
static void t5_cpu1_guard(struct work_struct *work)
{
 struct hi6250_fw *fw=container_of(to_delayed_work(work),struct hi6250_fw,cpu1_guard);
 int temp=0,tret=hi6250_t5_read_max_temp(&temp),ret;u32 expected,live;
 bool terr=tret!=0,over=temp>=90000,volt,hot,expired;
 mutex_lock(&fw->lock);
 /* Compare the same DVFS generation: sampling before lock can mistake
  * the previous idle rail for a new OC rail during a legitimate upshift. */
 expected=fw->cpu1_vol_idx;live=(readl(fw->gpu_pmctrl+0x214)>>12)&255;
 volt=live!=expected;hot=terr||over||volt;
 expired=time_after_eq(jiffies,fw->cpu1_deadline);
 if(fw->cpu1_oc_active && (hot || (!fw->cpu1_daily && expired))) {
  void __iomem *p=fw->gpu_pmctrl;u32 idx=0,v0=readl(p+0x154),v1=readl(p+0x214);
  u32 a0=readl(p+0x494),b0=readl(p+0x498),a1=readl(p+0x49c),b1=readl(p+0x4a0),ag=readl(p+0x4a4),bg=readl(p+0x4a8);
  u32 av0=readl(p+0xf0),av1=readl(p+0x1b0),s0=readl(p+0x9c),m0=readl(p+0xa4),d0=readl(p+0xa8),s1=readl(p+0x15c),m1=readl(p+0x164),d1=readl(p+0x168);
  int ir=regmap_read(fw->sysctrl,0x41c,&idx);
  ret=t5_cpu1_restore(fw);if(ret)fw->cpu1_errors++;
  dev_info(fw->client.dev,"T5_OC_REASON cpu=1 temp_ret=%d temp=%d over90=%u volt_changed=%u expected_idx=%u live_before_lock=%u expired=%u restore=%d BEFORE v0=%08x v1=%08x pll0=%08x/%08x pll1=%08x/%08x pllg=%08x/%08x scbak=%08x idx_ret=%d\n",tret,temp,over,volt,expected,live,expired,ret,v0,v1,a0,b0,a1,b1,ag,bg,idx,ir);
  dev_info(fw->client.dev,"T5_OC_BEFORE_MODE cpu=1 avs0=%08x avs1=%08x mux0=%08x/%08x/%08x mux1=%08x/%08x/%08x\n",av0,av1,s0,m0,d0,s1,m1,d1);
 }
 if(fw->cpu1_oc_active)queue_delayed_work_on(0,system_dfl_wq,&fw->cpu1_guard,msecs_to_jiffies(100));
 mutex_unlock(&fw->lock);
}
static ssize_t cpu1_oc_show(struct device *dev,struct device_attribute *a,char *buf)
{
 struct hi6250_fw *fw=dev_get_drvdata(dev);ssize_t n;
 mutex_lock(&fw->lock);
 n=sysfs_emit(buf,"active=%u\nrequested_mhz=%u\nactual_pll_hz=%llu\nvoltage_index=%u\nlive_voltage_index=%u\nerrors=%u\nrestores=%u\nlease_remaining_ms=%u\n",fw->cpu1_oc_active,fw->cpu1_request_mhz,t5_cpu1_pll_hz(fw),fw->cpu1_vol_idx,(readl(fw->gpu_pmctrl+0x214)>>12)&255,fw->cpu1_errors,fw->cpu1_restores,time_before(jiffies,fw->cpu1_deadline)?jiffies_to_msecs(fw->cpu1_deadline-jiffies):0);
 mutex_unlock(&fw->lock);return n;
}
static ssize_t cpu1_oc_store(struct device *dev,struct device_attribute *a,const char *buf,size_t n)
{
 struct hi6250_fw *fw=dev_get_drvdata(dev);u32 req,idx,target;int ret=0,temp;
 if(kstrtou32(buf,0,&req)||(req!=0 && req!=1 && (req<2400 || req>3000 || req%10)))return -EINVAL;
 mutex_lock(&fw->lock);
 if(req==0){ret=t5_cpu1_restore(fw);goto out;}
 if(req==1){if(!fw->cpu1_oc_active){ret=-EINVAL;goto out;}goto lease;}
 if(fw->cpu1_oc_active){ret=-EBUSY;goto out;}
 ret=hi6250_t5_read_max_temp(&temp);if(ret || temp>=85000){ret=-EAGAIN;goto out;}
 ret=regmap_read(fw->sysctrl,0x41c,&idx);if(ret)goto out;
 if(((idx>>4)&15)!=4 || readl(fw->gpu_pmctrl+0x49c)!=0x00907b05 || readl(fw->gpu_pmctrl+0x4a0)!=0x06000000 || t5_cpu1_pll_hz(fw)!=2361600000ULL){ret=-EOPNOTSUPP;goto out;}
 fw->cpu1_vol_idx=(readl(fw->gpu_pmctrl+0x214)>>12)&255;
 if(!fw->cpu1_vol_idx || fw->cpu1_vol_idx==255){ret=-EOPNOTSUPP;goto out;}
 fw->cpu1_oc_active=true;
 target=(u32)div64_u64((u64)req*5*0x1000000ULL+48,96);
 fw->cpu1_request_mhz=req;ret=t5_cpu1_ramp(fw,target);
 if(ret){fw->cpu1_errors++;t5_cpu1_restore(fw);goto out;}
 dev_info(dev,"RAM CPU1-cluster APLL1 request%uMHz actual%lluHz; voltage index unchanged=%u\n",req,t5_cpu1_pll_hz(fw),fw->cpu1_vol_idx);
lease:
 fw->cpu1_deadline=jiffies+msecs_to_jiffies(20000);
 mod_delayed_work_on(0,system_dfl_wq,&fw->cpu1_guard,msecs_to_jiffies(100));
out:
 mutex_unlock(&fw->lock);return ret?ret:n;
}
static DEVICE_ATTR_RW(cpu1_oc);
struct t5_cycle_measure {u64 hz,cycles,ns;int error;};
static long t5_cpu0_measure(void *data)
{
 struct t5_cycle_measure *m=data;u64 ctrl,enable,filter,counter,c0,c1,t0,t1;
 preempt_disable();
 asm volatile("mrs %0, pmcr_el0":"=r"(ctrl));
 asm volatile("mrs %0, pmcntenset_el0":"=r"(enable));
 if(enable&BIT(31)){m->error=-EBUSY;preempt_enable();return -EBUSY;}
 asm volatile("mrs %0, pmccfiltr_el0":"=r"(filter));
 asm volatile("mrs %0, pmccntr_el0":"=r"(counter));
 asm volatile("msr pmccfiltr_el0, %0"::"r"(0ULL));
 asm volatile("msr pmcr_el0, %0"::"r"((ctrl|1ULL)&~8ULL));
 asm volatile("msr pmcntenset_el0, %0\n isb"::"r"(1ULL<<31):"memory");
 t0=ktime_get_raw_ns();asm volatile("mrs %0, pmccntr_el0":"=r"(c0));
 do{cpu_relax();t1=ktime_get_raw_ns();}while(t1-t0<20000000ULL);
 asm volatile("mrs %0, pmccntr_el0":"=r"(c1));
 asm volatile("msr pmcntenclr_el0, %0\n isb"::"r"(1ULL<<31):"memory");
 asm volatile("msr pmccntr_el0, %0"::"r"(counter));
 asm volatile("msr pmccfiltr_el0, %0"::"r"(filter));
 asm volatile("msr pmcr_el0, %0\n isb"::"r"(ctrl):"memory");
 preempt_enable();m->ns=t1-t0;m->cycles=(u32)(c1-c0);m->hz=div64_u64(m->cycles*1000000000ULL,m->ns);return 0;
}
static ssize_t cpu0_cycle_measure_show(struct device *dev,struct device_attribute *a,char *buf)
{
 struct t5_cycle_measure m={0};long ret;
 ret=work_on_cpu(0,t5_cpu0_measure,&m);
 return sysfs_emit(buf,"measurement_ret=%ld\ncycle_hz=%llu\ncycles=%llu\ninterval_ns=%llu\n",ret,m.hz,m.cycles,m.ns);
}
static DEVICE_ATTR_RO(cpu0_cycle_measure);

static ssize_t cpu1_cycle_measure_show(struct device *dev,struct device_attribute *a,char *buf)
{
 struct t5_cycle_measure m={0};long ret;
 ret=work_on_cpu(4,t5_cpu0_measure,&m);
 return sysfs_emit(buf,"measurement_ret=%ld\ncycle_hz=%llu\ncycles=%llu\ninterval_ns=%llu\n",ret,m.hz,m.cycles,m.ns);
}
static DEVICE_ATTR_RO(cpu1_cycle_measure);
static ssize_t cpu1_test_frequencies_show(struct device *dev,struct device_attribute *a,char *buf)
{unsigned int i,n=0;for(i=2400;i<=3000;i+=10)n+=sysfs_emit_at(buf,n,"%u%s",i,i==3000?"\n":" ");return n;}
static DEVICE_ATTR_RO(cpu1_test_frequencies);
static ssize_t cpu0_test_frequencies_show(struct device *dev,struct device_attribute *a,char *buf)
{unsigned int i,n=0;for(i=1800;i<=1950;i+=10)n+=sysfs_emit_at(buf,n,"%u%s",i,i==1950?"\n":" ");return n;}
static DEVICE_ATTR_RO(cpu0_test_frequencies);
static const struct attribute_group t5_gpu_daily_group={.attrs=(struct attribute *[]){
 &dev_attr_gpu_oc_state.attr,&dev_attr_cpu_clock_registers.attr,&dev_attr_cpu0_oc.attr,&dev_attr_cpu0_cycle_measure.attr,&dev_attr_cpu1_oc.attr,&dev_attr_cpu1_cycle_measure.attr,&dev_attr_cpu1_test_frequencies.attr,&dev_attr_cpu0_test_frequencies.attr,NULL}};

static unsigned long hi6250_read_rate(struct clk_hw *hw, unsigned long parent)
{
	struct hi6250_clock *c = container_of(hw, struct hi6250_clock, hw);
	u32 value, index;
	if (regmap_read(c->fw->sysctrl, 0x41c, &value))
		return 0;
	index = (value >> c->shift) & 0xf;
	
	if(c==&c->fw->clocks[HI6250_CLK_STUB_GPU])return t5_gpu_real_rate(c->fw,index);
 if(c==&c->fw->clocks[HI6250_CLK_STUB_CLUSTER0] && index==3 && t5_cpu0_pll_hz(c->fw)==1899999999ULL)return 1900000000;
 if(c==&c->fw->clocks[HI6250_CLK_STUB_CLUSTER1] && index==4 && t5_cpu1_pll_hz(c->fw)==2570000000ULL)return 2570000000;
	return index < c->count ? c->rates[index] : 0;
}

/* Board audio validation uses the stock interactive clock commands.
 * Serialize channel 13 with CPU/GPU firmware frequency requests. */
int hi6250_t5_asp_vote(bool on);
int hi6250_t5_asp_vote(bool on)
{
	struct device_node *node;
	struct platform_device *pdev;
	struct hi6250_fw *fw;
	int ret;

	node=of_find_compatible_node(NULL,NULL,"hisilicon,hi6250-stub-clk");
	if (!node)return -ENODEV;
	pdev=of_find_device_by_node(node);
	of_node_put(node);
	if (!pdev)return -ENODEV;
	fw=platform_get_drvdata(pdev);
	if (!fw) { put_device(&pdev->dev); return -ENODEV; }
	mutex_lock(&fw->lock);
	memset(fw->message,0,sizeof(fw->message));
	fw->message[0]=on ? 0x000d0002 : 0x000d0102;
	fw->message[1]=0x0b;
	ret=mbox_send_message(fw->channel,fw->message);
	if (ret>=0) {
		usleep_range(8000,12000);
		mbox_client_txdone(fw->channel,0);
		ret=0;
	}
	mutex_unlock(&fw->lock);
	put_device(&pdev->dev);
	return ret;
}
static int hi6250_send_rate_locked(struct hi6250_clock *c,unsigned long rate)
{
 struct hi6250_fw *fw=c->fw;unsigned int i;int ret;
 memset(fw->message,0,sizeof(fw->message));
 fw->message[0]=c->command;fw->message[1]=rate/1000000;
 ret=mbox_send_message(fw->channel,fw->message);
 if(ret>=0) {
  for(i=0;i<100;i++) {
   if(hi6250_read_rate(&c->hw,0)==rate)break;
   usleep_range(500,1000);
  }
  ret=i==100?-ETIMEDOUT:0;
  mbox_client_txdone(fw->channel,ret);
 }
 return ret;
}

static int hi6250_set_rate(struct clk_hw *hw,unsigned long rate,unsigned long parent)
{
 struct hi6250_clock *c=container_of(hw,struct hi6250_clock,hw);
 struct hi6250_fw *fw=c->fw;
 bool gpu=c==&fw->clocks[HI6250_CLK_STUB_GPU];
 unsigned int i;u32 idx;int ret,temp;
 for(i=0;i<c->count && c->rates[i]!=rate;i++);
 
 if(i==c->count)return -EINVAL;
 mutex_lock(&fw->lock);

 if(!gpu){
  bool little=c==&fw->clocks[HI6250_CLK_STUB_CLUSTER0];
  bool big=c==&fw->clocks[HI6250_CLK_STUB_CLUSTER1];
  bool boost=(little && rate==1900000000)||(big && rate==2570000000);
  if(little){ret=t5_cpu0_restore(fw);if(ret)goto out;}
  if(big){ret=t5_cpu1_restore(fw);if(ret)goto out;}
  if(!boost){ret=hi6250_send_rate_locked(c,rate);goto out;}
  /* LPM3 establishes the stock maximum rail first. PLL feedback only;
   * no manual voltage, mux or divider changes. */
  ret=hi6250_t5_read_max_temp(&temp);if(ret || temp>=85000){ret=-EAGAIN;goto out;}
  ret=hi6250_send_rate_locked(c,little?1709000000:2362000000);if(ret)goto out;
  if(little){
   fw->cpu0_vol_idx=(readl(fw->gpu_pmctrl+0x154)>>12)&255;
   if(!fw->cpu0_vol_idx || fw->cpu0_vol_idx==255){ret=-EIO;goto out;}
   fw->cpu0_oc_active=true;fw->cpu0_daily=true;fw->cpu0_request_mhz=1900;
   ret=t5_cpu0_ramp(fw,(u32)div64_u64(1900ULL*5*0x1000000+48,96));
   if(ret){fw->cpu0_errors++;t5_cpu0_restore(fw);goto out;}
   mod_delayed_work_on(4,system_dfl_wq,&fw->cpu0_guard,msecs_to_jiffies(100));
  }else{
   fw->cpu1_vol_idx=(readl(fw->gpu_pmctrl+0x214)>>12)&255;
   if(!fw->cpu1_vol_idx || fw->cpu1_vol_idx==255){ret=-EIO;goto out;}
   fw->cpu1_oc_active=true;fw->cpu1_daily=true;fw->cpu1_request_mhz=2570;
   ret=t5_cpu1_ramp(fw,(u32)div64_u64(2570ULL*5*0x1000000+48,96));
   if(ret){fw->cpu1_errors++;t5_cpu1_restore(fw);goto out;}
   mod_delayed_work_on(0,system_dfl_wq,&fw->cpu1_guard,msecs_to_jiffies(100));
  }
  goto out;
 }
 if(rate==1146000000) {
  /* The20ms binary governor and native thermal cooling own90/85C
   * hysteresis. Independently refuse a hot or failed-sensor upshift. */
  ret=hi6250_t5_read_max_temp(&temp);
  if(ret || temp>=90000){ret=-EAGAIN;goto out;}
  if(hi6250_read_rate(hw,0)==rate){ret=0;goto out;}
 }
 ret=t5_gpu_restore(fw);if(ret)goto gpu_error;
 if(rate!=1146000000){ret=hi6250_send_rate_locked(c,rate);goto gpu_result;}
 /* Firmware must first establish its original900MHz power/voltage
  * profile. Never send1146MHz to the clamping firmware MHz interface. */
 ret=hi6250_send_rate_locked(c,900000000);if(ret)goto gpu_error;
 ret=regmap_read(fw->sysctrl,0x41c,&idx);if(ret)goto gpu_error;
 if(((idx>>12)&15)!=6 || t5_gpu_real_rate(fw,6)!=900000000 ||
    readl(fw->gpu_pmctrl+0x4a4)!=0x00a05d05 ||
    readl(fw->gpu_pmctrl+0x4a8)!=0x06c00000){ret=-EOPNOTSUPP;goto gpu_error;}
 fw->gpu_oc_active=true;
 ret=t5_gpu_ramp(fw,0x77600000);
 if(!ret && t5_gpu_real_rate(fw,6)!=1146000000)ret=-EIO;
 if(ret)goto gpu_error;
 fw->gpu_oc_up++;
 fw->gpu_oc_last_error=0;
 goto out;
gpu_error:
 fw->gpu_oc_errors++;fw->gpu_oc_last_error=ret;
 dev_err_ratelimited(fw->client.dev,"GPU1146MHz transition failed: %d; restoring stock clock\n",ret);
 /* Do not lower the rail until the PLL is confirmed back at stock. */
 if(!t5_gpu_restore(fw))hi6250_send_rate_locked(c,120000000);
 goto out;
gpu_result:
 if(ret){fw->gpu_oc_errors++;fw->gpu_oc_last_error=ret;}
out:
 mutex_unlock(&fw->lock);return ret;
}

static const struct clk_ops hi6250_fw_ops = {
	.recalc_rate = hi6250_read_rate,
	.determine_rate = clk_determine_rate_noop,
	.set_rate = hi6250_set_rate,
};

static struct clk_hw *hi6250_fw_get(struct of_phandle_args *spec, void *data)
{
	struct hi6250_fw *fw = data;
	if (spec->args_count != 1 || spec->args[0] >= HI6250_CLK_STUB_NUM)
		return ERR_PTR(-EINVAL);
	return &fw->clocks[spec->args[0]].hw;
}

static void hi6250_free_channel(void *data)
{
	mbox_free_channel(data);
}

static int hi6250_fw_probe(struct platform_device *pdev)
{
	static const struct {
		const char *name;
		const unsigned long *rates;
		unsigned int count, shift, command;
	} desc[] = {
		[HI6250_CLK_STUB_CLUSTER0] = {"cpu-cluster.0", little_rates, ARRAY_SIZE(little_rates), 0, 0x0001030a},
		[HI6250_CLK_STUB_CLUSTER1] = {"cpu-cluster.1", big_rates, ARRAY_SIZE(big_rates), 4, 0x0002030a},
		[HI6250_CLK_STUB_DDR] = {"clk_ddrc", ddr_rates, ARRAY_SIZE(ddr_rates), 8, 0x00040309},
		[HI6250_CLK_STUB_GPU] = {"clk_g3d", gpu_rates, ARRAY_SIZE(gpu_rates), 12, 0x0003030a},
	};
	struct device *dev = &pdev->dev;
	struct hi6250_fw *fw;
	unsigned int i;
	int ret;
	fw = devm_kzalloc(dev, sizeof(*fw), GFP_KERNEL);
	if (!fw)
		return -ENOMEM;
	mutex_init(&fw->lock);
 fw->gpu_pmctrl=devm_ioremap(dev,0xfff31000,0x1000);
 if(!fw->gpu_pmctrl)return -ENOMEM;
 INIT_DELAYED_WORK(&fw->cpu0_guard,t5_cpu0_guard);
 INIT_DELAYED_WORK(&fw->cpu1_guard,t5_cpu1_guard);
 platform_set_drvdata(pdev,fw);
	fw->sysctrl = syscon_regmap_lookup_by_phandle(dev->of_node, "hisilicon,hi6250-sys-ctrl");
	if (IS_ERR(fw->sysctrl))
		return dev_err_probe(dev, PTR_ERR(fw->sysctrl), "sysctrl unavailable\n");
	fw->client.dev = dev;
	fw->client.knows_txdone = true;
	fw->channel = mbox_request_channel(&fw->client, 0);
	if (IS_ERR(fw->channel))
		return dev_err_probe(dev, PTR_ERR(fw->channel), "LPM3 mailbox unavailable\n");
	ret = devm_add_action_or_reset(dev, hi6250_free_channel, fw->channel);
	if (ret)
		return ret;
	for (i = 0; i < ARRAY_SIZE(desc); i++) {
		struct hi6250_clock *c = &fw->clocks[i];
		struct clk_init_data init = {
			.name = desc[i].name, .ops = &hi6250_fw_ops,
			.flags = CLK_GET_RATE_NOCACHE,
		};
		c->fw = fw;
		c->rates = desc[i].rates;
		c->count = desc[i].count;
		c->shift = desc[i].shift;
		c->command = desc[i].command;
		c->hw.init = &init;
		ret = devm_clk_hw_register(dev, &c->hw);
		if (ret)
			return ret;
	}
	ret=devm_device_add_group(dev,&t5_gpu_daily_group);
 if(ret)return ret;
	dev_info(dev, "LPM3 firmware clocks ready: CPU %lu/%lu Hz\n",
		hi6250_read_rate(&fw->clocks[HI6250_CLK_STUB_CLUSTER0].hw, 0),
		hi6250_read_rate(&fw->clocks[HI6250_CLK_STUB_CLUSTER1].hw, 0));
	return devm_of_clk_add_hw_provider(dev, hi6250_fw_get, fw);
}

static const struct of_device_id hi6250_fw_match[] = {
	{ .compatible = "hisilicon,hi6250-stub-clk" }, {}
};
static struct platform_driver hi6250_fw_driver = {
	.probe = hi6250_fw_probe,
	.driver = { .name = "hi6250-stub-clk", .of_match_table = hi6250_fw_match },
};
builtin_platform_driver(hi6250_fw_driver);
