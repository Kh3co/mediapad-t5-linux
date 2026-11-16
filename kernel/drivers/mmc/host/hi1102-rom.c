// SPDX-License-Identifier: GPL-2.0-only
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/mmc/sdio_func.h>
#include <linux/module.h>
#include <linux/slab.h>
#define CHUNK 4096
static int exchange(struct sdio_func *f, u8 *buf, size_t len, const char *expect)
{
 int err;
 u8 status;
 unsigned int count;
 unsigned long deadline = jiffies + 2 * HZ;
 size_t padded = len < 512 ? ALIGN(len,4) : ALIGN(len,512);
 memset(buf+len,0,padded-len);
 err=sdio_writesb(f,0,buf,padded);
 if (err || !expect) return err;
 do {
  status=sdio_readb(f,8,&err);
  if(err) return err;
  if(status & 7) break;
  usleep_range(500,1000);
 } while(time_before(jiffies,deadline));
 if (!(status & 7)) return -ETIMEDOUT;
 sdio_writeb(f,status,8,&err);
 if(err) return err;
 count=sdio_readl(f,12,&err);
 if(err) return err;
 if(!count || count>CHUNK) return -EPROTO;
 err=sdio_readsb(f,buf,0,count);
 if(err) return err;
 if(count<strlen(expect) || memcmp(buf,expect,strlen(expect))) {
  dev_err(&f->dev,"Unexpected ROM reply %.*s (wanted %s)\n",(int)min(count,32U),buf,expect);
  return -EPROTO;
 }
 return 0;
}
static int hi1102_probe(struct sdio_func *f,const struct sdio_device_id *id)
{
 const struct firmware *fw;
 size_t off=0,n,len;
 u8 *buf;
 int ret=request_firmware(&fw,"t5/cr4_asic.bin",&f->dev);
 if(ret) return ret;
 if(fw->size!=391664){ret=-EINVAL;goto done;}
 buf=kmalloc(CHUNK,GFP_KERNEL);
 if(!buf){ret=-ENOMEM;goto done;}
 sdio_claim_host(f);
 ret=sdio_enable_func(f);
 if(ret) goto unlock;
 ret=sdio_set_block_size(f,512);
 if(ret) goto disable;
 memcpy(buf,"VERSION ",8);
 ret=exchange(f,buf,8,"Hi1102V100");
 if(ret) goto disable;
 dev_info(&f->dev,"Hi1102V100 ROM handshake confirmed\n");
 for(off=0;off<fw->size;off+=n){
  n=min_t(size_t,CHUNK,fw->size-off);
  len=scnprintf(buf,CHUNK,"FILES 1 0x%zx ",(size_t)0x70000+off);
  ret=exchange(f,buf,len,"READY");
  if(ret)goto disable;
  usleep_range(5000,6000);
  memcpy(buf,fw->data+off,n);
  ret=exchange(f,buf,n,"FILES OK");
  if(ret)goto disable;
 }
 memcpy(buf,"QUIT ",5);
 ret=exchange(f,buf,5,NULL);
 if(!ret)dev_info(&f->dev,"Firmware acknowledged %zu bytes, QUIT sent\n",off);
disable:
 if(ret){dev_err(&f->dev,"Bootstrap stopped at offset %zu: %d\n",off,ret);sdio_disable_func(f);}
unlock:
 sdio_release_host(f);kfree(buf);
done:
 release_firmware(fw);return ret;
}
static void hi1102_remove(struct sdio_func *f)
{
 sdio_claim_host(f);sdio_disable_func(f);sdio_release_host(f);
}
static const struct sdio_device_id ids[]={ {SDIO_DEVICE(0x0296,0x5347)},{} };
static struct sdio_driver hi1102={.name="hi1102-rom",.id_table=ids,.probe=hi1102_probe,.remove=hi1102_remove};
module_sdio_driver(hi1102);
MODULE_LICENSE("GPL");
