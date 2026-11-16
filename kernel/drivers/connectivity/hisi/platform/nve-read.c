#include <linux/firmware.h>
#include <linux/mtd/hisi_nve_interface.h>
/* Ramdisk validation reads the two board-specific records copied from backup. */
int hisi_nve_direct_access(struct hisi_nve_info_user *req)
{
 const struct firmware *fw;
 const char *path;
 unsigned int size;
 int ret;
 if (!req || req->nv_operation != NV_READ) return -EACCES;
 switch (req->nv_number) {
 case 193: path="t5/nv193.bin";size=20;break;
 case 340: path="t5/nv340.bin";size=104;break;
 default:return -ENOENT;
 }
 if (req->valid_size > size) return -EINVAL;
 ret=request_firmware_direct(&fw,path,NULL);
 if(ret) return ret;
 if(fw->size!=size) ret=-EINVAL;
 else memcpy(req->nv_data,fw->data,req->valid_size);
 release_firmware(fw);
 return ret;
}
