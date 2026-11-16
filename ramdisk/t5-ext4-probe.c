// SPDX-License-Identifier: GPL-2.0-only
/* Static read-only ext4 geometry probe and non-writing guard verification. */
typedef unsigned long U;typedef unsigned int u32;typedef unsigned char u8;
static long sc(long n,long a,long b,long c,long d,long e,long f){
 register long x0 __asm__("x0")=a,x1 __asm__("x1")=b,x2 __asm__("x2")=c,x3 __asm__("x3")=d,x4 __asm__("x4")=e,x5 __asm__("x5")=f,x8 __asm__("x8")=n;
 __asm__ volatile("svc #0":"+r"(x0):"r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5),"r"(x8):"memory","cc");return x0;
}
static void str(const char*s){U n=0;while(s[n])n++;sc(64,1,(long)s,n,0,0,0);}
static void num(U n){char b[32];int i=31;b[i]=0;do{b[--i]='0'+n%10;n/=10;}while(n);str(b+i);}
static u32 le32(u8*b){return (u32)b[0]|(u32)b[1]<<8|(u32)b[2]<<16|(u32)b[3]<<24;}
static long op(const char*p,int mode){return sc(56,-100,(long)p,mode,0,0,0);}
static long io(int fd,U req,void*p){return sc(29,fd,req,(long)p,0,0,0);}
static int eq(const char*a,const char*b){while(*a&&*a==*b){a++;b++;}return *a==*b;}
long run(U *sp){char**av=(char**)(sp+1);int guard=(*sp==3&&eq(av[1],"--guard"));if(!guard&&*sp!=2)return 3;
 const char*path=av[guard?2:1];int fd=op(path,0);if(fd<0)return 3;
 U cap=0;int ro=0;if(io(fd,0x80081272UL,&cap)<0||!cap||io(fd,0x125e,&ro)<0||ro!=1){str("GUARD_NOT_READONLY\n");return 4;}
 if(guard){int zero=0,one=1;
  long r=io(fd,0x125d,&zero);
  if(r!=-30){io(fd,0x125d,&one);str("BLKROSET_UNLOCK_NOT_REJECTED\n");return 5;}
  if(io(fd,0x125e,&ro)<0||ro!=1)return 6;
  int w=op(path,1);if(w>=0){sc(57,w,0,0,0,0,0);str("WRITE_OPEN_NOT_REJECTED\n");return 7;}if(w!=-30)return 8;
  /* NULL user buffers ensure no raw card command can execute even if this
   * check were accidentally run against an ordinary unprotected MMC disk. */
  if(io(fd,0xc048b300UL,(void*)0)!=-30||io(fd,0xc008b301UL,(void*)0)!=-30){str("RAW_IOCTL_NOT_REJECTED\n");return 9;}
  str("GUARD_PASS EROFS: BLKROSET-clear WRITE-open RAW-single RAW-multi; no data write attempted\n");return 0;
 }
 u8 b[4096];if(sc(67,fd,(long)b,sizeof(b),0,0,0)!=sizeof(b))return 3;
 if(b[1080]!=0x53||b[1081]!=0xef)return 1;
 u32 shift=le32(b+1048);if(shift>6)return 2;
 U blocks=le32(b+1028);if(le32(b+1120)&0x80)blocks|=(U)le32(b+1360)<<32;
 U bytes=0;int fit=blocks&&blocks<=(cap>>(10+shift));if(fit)bytes=blocks<<(10+shift);
 str(fit?"EXT4\t":"INVALID_EXT4\t");num(cap);str("\t");if(fit)num(bytes);else str("OVERSIZE_OR_INVALID");str("\t");
 const char*h="0123456789abcdef";char uuid[37];int k=0;for(int i=0;i<16;i++){if(i==4||i==6||i==8||i==10)uuid[k++]='-';u8 v=b[1128+i];uuid[k++]=h[v>>4];uuid[k++]=h[v&15];}uuid[k]=0;str(uuid);str("\n");return fit?0:2;
}
__asm__(".global _start\n_start:\nmov x0, sp\nbl run\nmov x8, #93\nsvc #0\n");
