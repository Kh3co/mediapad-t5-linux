// SPDX-License-Identifier: GPL-2.0-only
/* AGS2-L03 RAM-only, bounded DMSS diagnostic. Addresses from Huawei Hi6250 GPL. */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/io.h>
#include <linux/mutex.h>
#include <linux/slab.h>
struct ddr_test {void __iomem *base;struct mutex lock;unsigned int scans,changed;};
static ssize_t dmss_scan_store(struct device *dev,struct device_attribute *a,const char *buf,size_t n)
{
 struct ddr_test *t=dev_get_drvdata(dev);unsigned int s,r,num;u32 lock;
 if(!sysfs_streq(buf,"1"))return -EINVAL;
 mutex_lock(&t->lock);
 dev_info(dev,"DMSS_READ_BEGIN physical=fffc0000 no_writes=1\n");
 dev_info(dev,"DMSS_RTL version=%08x info0=%08x info1=%08x\n",readl(t->base+0x6000),readl(t->base+0x6004),readl(t->base+0x6008));
 lock=readl(t->base+0x6020);dev_info(dev,"DMSS_LOCK value=%08x\n",lock);
 /* Vendor fastboot validates security-interface numbers0..6. */
 for(s=0;s<7;s++){
  u32 info=readl(t->base+s*0x800+0xc);num=info&31;
  dev_info(dev,"DMSS_ASI asi=%u rtl3=%08x region_count=%u\n",s,info,num);
  if(num>16){dev_warn(dev,"DMSS region count exceeds diagnostic bound; skipped\n");continue;}
  for(r=0;r<num;r++){
   void __iomem *p=t->base+s*0x800+0x500+r*16;
   u32 map=readl(p),attr=readl(p+4),wr=readl(p+8),rd=readl(p+12);u32 code=(map>>24)&63;
   u64 start=(u64)(map&0xffffff)<<16,end=code<40?start+(1ULL<<(code+1)):0;
   dev_info(dev,"DMSS_REGION asi=%u region=%u map=%08x attr=%08x wr=%08x rd=%08x start=%llx end=%llx\n",s,r,map,attr,wr,rd,start,end);
  }
 }
 t->scans++;dev_info(dev,"DMSS_READ_DONE scans=%u\n",t->scans);mutex_unlock(&t->lock);return n;
}
static DEVICE_ATTR_WO(dmss_scan);
static ssize_t dmss_unlock_store(struct device *dev,struct device_attribute *a,const char *buf,size_t n)
{
 struct ddr_test *t=dev_get_drvdata(dev);unsigned int s,r,num,found=0;u32 lock;
 if(!sysfs_streq(buf,"1"))return -EINVAL;
 mutex_lock(&t->lock);
 if(!t->scans){mutex_unlock(&t->lock);return -EPERM;}
 lock=readl(t->base+0x6020);
 if(lock&1){dev_info(dev,"DMSS_UNLOCK_REFUSED configuration locked=%08x; no writes performed\n",lock);mutex_unlock(&t->lock);return -EACCES;}
 for(s=0;s<7;s++){
  num=readl(t->base+s*0x800+0xc)&31;if(num>16)continue;
  for(r=0;r<num;r++){
   void __iomem *p=t->base+s*0x800+0x500+r*16;u32 map=readl(p),attr=readl(p+4),wr,rd;u32 code=(map>>24)&63;u64 start,end;
   if(!(map&BIT(31))||code>=40)continue;
   start=(u64)(map&0xffffff)<<16;end=start+(1ULL<<(code+1));
   /* Only enabled, fully contained modem regions, simple allow-list MID mode.
    * Never widen/disable a region, touch global locks or secure firmware ranges. */
   if(start<0x38000000ULL||end>0x3ec00000ULL||end<=start)continue;
   if(!(attr&BIT(8))||(attr&BIT(9))||(attr>>16))continue;
   wr=readl(p+8);rd=readl(p+12);found++;
   /*0xc is the exact AP read-MID mask used by this device's fastboot. */
   writel(wr|0xc,p+8);writel(rd|0xc,p+12);
   if(readl(p+8)!=(wr|0xc)||readl(p+12)!=(rd|0xc)){
    writel(wr,p+8);writel(rd,p+12);dev_info(dev,"DMSS_WRITE_REJECTED asi=%u region=%u restoration attempted\n",s,r);
   }else{t->changed++;dev_info(dev,"DMSS_MODEM_PERMISSION_UPDATED asi=%u region=%u oldwr=%08x oldrd=%08x newwr=%08x newrd=%08x\n",s,r,wr,rd,readl(p+8),readl(p+12));}
  }
 }
 dev_info(dev,"DMSS_UNLOCK_DONE matching_regions=%u changed=%u\n",found,t->changed);mutex_unlock(&t->lock);return n;
}
static DEVICE_ATTR_WO(dmss_unlock);
static struct attribute *attrs[]={&dev_attr_dmss_scan.attr,&dev_attr_dmss_unlock.attr,NULL};
static const struct attribute_group group={.attrs=attrs};
static int ddr_probe(struct platform_device *pdev)
{
 struct ddr_test *t=devm_kzalloc(&pdev->dev,sizeof(*t),GFP_KERNEL);if(!t)return -ENOMEM;
 mutex_init(&t->lock);t->base=devm_ioremap(&pdev->dev,0xfffc0000,0x7000);if(!t->base)return -ENOMEM;
 platform_set_drvdata(pdev,t);return devm_device_add_group(&pdev->dev,&group);
}
static const struct of_device_id matches[]={{.compatible="huawei,ags2l03-ddr-test"},{}};
static struct platform_driver t5_ddr_driver={.probe=ddr_probe,.driver={.name="t5-ddr-test",.of_match_table=matches}};
module_platform_driver(t5_ddr_driver);
MODULE_LICENSE("GPL");
