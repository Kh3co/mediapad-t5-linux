// SPDX-License-Identifier: GPL-2.0-only
/* AGS2-L03 RAM-only diagnostic. No firmware/storage writes. */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/mailbox_client.h>
#include <linux/io.h>
#include <linux/mutex.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/bitmap.h>
#include <asm/cacheflush.h>
#include <asm/barrier.h>
int hi3660_t5_modem_sync(struct mbox_chan *chan, u32 *tx, u32 *rx);
void hi3660_t5_modem_release(struct mbox_chan *chan);
struct modem_test {struct mbox_client cl;struct mbox_chan *ch;struct mutex lock;void __iomem *crg;int attempted,ret,page_ret;u32 ack[8],before,after;bool accepted;};
static ssize_t modem_state_show(struct device *dev,struct device_attribute *a,char *buf)
{
 struct modem_test *t=dev_get_drvdata(dev);
 return sysfs_emit(buf,"attempted=%d\ntransport_ret=%d\nreply0=0x%08x\nreply1=0x%08x\nlpm3_off_ack=%u\ncrg_perstat5_before=0x%08x\ncrg_perstat5_after=0x%08x\npage_test_ret=%d\n",t->attempted,t->ret,t->ack[0],t->ack[1],t->accepted,t->before,t->after,t->page_ret);
}
static DEVICE_ATTR_RO(modem_state);
static ssize_t modem_off_store(struct device *dev,struct device_attribute *a,const char *buf,size_t n)
{
 struct modem_test *t=dev_get_drvdata(dev);u32 tx[8]={0x00090300,0};
 if(!sysfs_streq(buf,"1"))return -EINVAL;
 mutex_lock(&t->lock);
 if(t->attempted){mutex_unlock(&t->lock);return -EALREADY;}
 t->attempted=1;t->before=readl(t->crg+0x5c);
 /* Exact downstream MODEM_POWER_OFF(0), MDRV_RESET_CB_BEFORE(0), MBX16. */
 t->ret=hi3660_t5_modem_sync(t->ch,tx,t->ack);
 t->after=readl(t->crg+0x5c);
 t->accepted=!t->ret && t->ack[1]==0;
 dev_info(dev,"modem-off transport=%d ack=%08x/%08x accepted=%u CRG=%08x->%08x\n",t->ret,t->ack[0],t->ack[1],t->accepted,t->before,t->after);
 mutex_unlock(&t->lock);return n;
}
static DEVICE_ATTR_WO(modem_off);
static ssize_t modem_page_test_store(struct device *dev,struct device_attribute *a,const char *buf,size_t n)
{
 struct modem_test *t=dev_get_drvdata(dev);volatile u64 *v;u64 *saved;unsigned int i,p;int ret=0;
 if(!sysfs_streq(buf,"1"))return -EINVAL;
 mutex_lock(&t->lock);
 if(!t->accepted){mutex_unlock(&t->lock);return -EPERM;}
 saved=kmalloc(PAGE_SIZE,GFP_KERNEL);if(!saved){mutex_unlock(&t->lock);return -ENOMEM;}
 v=memremap(0x38800000,PAGE_SIZE,MEMREMAP_WB);if(!v){kfree(saved);mutex_unlock(&t->lock);return -ENOMEM;}
 dcache_clean_inval_poc((unsigned long)v,(unsigned long)v+PAGE_SIZE);dsb(sy);
 for(i=0;i<PAGE_SIZE/8;i++)saved[i]=v[i];
 for(p=0;p<16&&!ret;p++){
  for(i=0;i<PAGE_SIZE/8;i++)v[i]=((u64)i*0x9e3779b97f4a7c15ULL)^((u64)(p+1)*0xd1b54a32d192ed03ULL);
  dcache_clean_inval_poc((unsigned long)v,(unsigned long)v+PAGE_SIZE);dsb(sy);msleep(20);
  for(i=0;i<PAGE_SIZE/8;i++)if(v[i]!=(((u64)i*0x9e3779b97f4a7c15ULL)^((u64)(p+1)*0xd1b54a32d192ed03ULL))){ret=-EIO;break;}
 }
 for(i=0;i<PAGE_SIZE/8;i++)v[i]=saved[i];
 dcache_clean_inval_poc((unsigned long)v,(unsigned long)v+PAGE_SIZE);dsb(sy);
 memunmap((void *)v);kfree(saved);t->page_ret=ret;
 dev_info(dev,"modem reserved-page0x38800000 cache-flushed comparison ret=%d, original page restoration attempted\n",ret);
 mutex_unlock(&t->lock);return n;
}
static DEVICE_ATTR_WO(modem_page_test);

static ssize_t modem_range_test_store(struct device *dev,struct device_attribute *a,const char *buf,size_t n)
{
 struct modem_test *t=dev_get_drvdata(dev);
 const size_t size=113UL*1024*1024, pages=size/PAGE_SIZE;
 volatile u64 *v;u64 *saved;unsigned long *bad;size_t i,p,round;unsigned long total=0;
 if(!sysfs_streq(buf,"1"))return -EINVAL;
 mutex_lock(&t->lock);
 if(!t->accepted){mutex_unlock(&t->lock);return -EPERM;}
 saved=vmalloc(size);bad=bitmap_zalloc(pages,GFP_KERNEL);
 if(!saved||!bad){vfree(saved);bitmap_free(bad);mutex_unlock(&t->lock);return -ENOMEM;}
 v=memremap(0x38000000,size,MEMREMAP_WB);
 if(!v){vfree(saved);bitmap_free(bad);mutex_unlock(&t->lock);return -ENOMEM;}
 dcache_clean_inval_poc((unsigned long)v,(unsigned long)v+size);dsb(sy);
 for(i=0;i<size/8;i++)saved[i]=v[i];
 dev_info(dev,"MODEM_RANGE_TEST_START base=38000000 bytes=%zu saved_in_RAM=1\n",size);
 for(round=0;round<2;round++){
  for(i=0;i<size/8;i++)v[i]=((u64)i*0x9e3779b97f4a7c15ULL)^((u64)(round+1)*0xd1b54a32d192ed03ULL);
  dcache_clean_inval_poc((unsigned long)v,(unsigned long)v+size);dsb(sy);msleep(500);
  for(p=0;p<pages;p++){
   for(i=p*(PAGE_SIZE/8);i<(p+1)*(PAGE_SIZE/8);i++){
    if(v[i]!=(((u64)i*0x9e3779b97f4a7c15ULL)^((u64)(round+1)*0xd1b54a32d192ed03ULL))){set_bit(p,bad);break;}
   }
  }
 }
 for(i=0;i<size/8;i++)v[i]=saved[i];
 dcache_clean_inval_poc((unsigned long)v,(unsigned long)v+size);dsb(sy);
 for(p=0;p<pages;){bool fail=test_bit(p,bad);size_t first=p;while(p<pages&&test_bit(p,bad)==fail)p++;
  dev_info(dev,"MODEM_RANGE_RESULT start=%08lx end=%08lx pages=%zu mismatch=%u\n",0x38000000UL+first*PAGE_SIZE,0x38000000UL+p*PAGE_SIZE,p-first,fail);
 }
 total=bitmap_weight(bad,pages);t->page_ret=total?-EIO:0;
 dev_info(dev,"MODEM_RANGE_TEST_COMPLETE total_pages=%zu bad_pages=%lu original_restore_attempted=1\n",pages,total);
 memunmap((void *)v);vfree(saved);bitmap_free(bad);mutex_unlock(&t->lock);return n;
}
static DEVICE_ATTR_WO(modem_range_test);

static struct attribute *attrs[]={&dev_attr_modem_state.attr,&dev_attr_modem_off.attr,&dev_attr_modem_page_test.attr,&dev_attr_modem_range_test.attr,NULL};
static const struct attribute_group group={.attrs=attrs};
static void release_channel(void *p){hi3660_t5_modem_release(p);mbox_free_channel(p);}
static int modem_probe(struct platform_device *pdev)
{
 struct modem_test *t;int ret;
 t=devm_kzalloc(&pdev->dev,sizeof(*t),GFP_KERNEL);if(!t)return -ENOMEM;
 mutex_init(&t->lock);t->page_ret=-EAGAIN;t->ret=-EAGAIN;t->cl.dev=&pdev->dev;t->cl.knows_txdone=true;
 t->ch=mbox_request_channel(&t->cl,0);if(IS_ERR(t->ch))return dev_err_probe(&pdev->dev,PTR_ERR(t->ch),"MBX16 unavailable\n");
 ret=devm_add_action_or_reset(&pdev->dev,release_channel,t->ch);if(ret)return ret;
 t->crg=devm_ioremap(&pdev->dev,0xfff35000,0x1000);if(!t->crg)return -ENOMEM;
 platform_set_drvdata(pdev,t);return devm_device_add_group(&pdev->dev,&group);
}
static const struct of_device_id matches[]={{.compatible="huawei,ags2l03-modem-test"},{}};
static struct platform_driver t5_modem_driver={.probe=modem_probe,.driver={.name="t5-modem-test",.of_match_table=matches}};
module_platform_driver(t5_modem_driver);
MODULE_LICENSE("GPL");
