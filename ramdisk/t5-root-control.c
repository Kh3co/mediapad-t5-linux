/* SPDX-License-Identifier: GPL-2.0-only */
/* Static AArch64, confined content identity and root-window activation. */
#include "t5-emmc-window.h"
typedef unsigned long U;typedef unsigned int u32;typedef unsigned char u8;
static long sc(long n,long a,long b,long c,long d,long e,long f){
 register long x0 __asm__("x0")=a,x1 __asm__("x1")=b,x2 __asm__("x2")=c,x3 __asm__("x3")=d,x4 __asm__("x4")=e,x5 __asm__("x5")=f,x8 __asm__("x8")=n;
 __asm__ volatile("svc #0":"+r"(x0):"r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5),"r"(x8):"memory","cc");return x0;
}
static void str(const char*s){U n=0;while(s[n])n++;sc(64,1,(long)s,n,0,0,0);}
static int eq(const char*a,const char*b){while(*a&&*a==*b){a++;b++;}return *a==*b;}
static void num(U n){char b[32];int i=31;b[i]=0;do{b[--i]='0'+n%10;n/=10;}while(n);str(b+i);}
static void closefd(int fd){sc(57,fd,0,0,0,0,0);}
static int contains(const char*s,const char*t){for(;*s;s++){const char*a=s,*b=t;while(*b&&*a==*b){a++;b++;}if(!*b)return 1;}return 0;}
static long op(const char*p,int flags){return sc(56,-100,(long)p,flags,0,0,0);}
static long confined(int root,const char*p,int flags){U how[3]={(U)flags|0x80000U,0,0x12};return sc(437,root,(long)p,(long)how,sizeof(how),0,0);}
static int regular(int fd,int exec,U *stat){
 if(sc(80,fd,(long)stat,0,0,0,0))return 0;
 u32 mode=*(u32*)((u8*)stat+16);
 return (mode&0170000)==0100000 && (!exec || (mode&0111));
}
static int text(int root,const char*path,char*b,U cap){
 int fd=confined(root,path,0);if(fd<0)return 0;
 U st[16];int ok=regular(fd,0,st)&&st[6]<cap;
 long n=ok?sc(63,fd,(long)b,cap-1,0,0,0):-1;closefd(fd);
 if(n<0||(U)n!=st[6])return 0;b[n]=0;return 1;
}
static int identity(const char*dir){
 char b[2048];int root=op(dir,0x84000);if(root<0)return 1;
 if(!text(root,"etc/t5-rootfs-id",b,sizeof(b))||!eq(b,"T5_DEBIAN_EMMC_ROOT_V1\n"))goto fail;
 if(!text(root,"etc/t5-release.json",b,sizeof(b))||!eq(b,"{\"schema\":1,\"platform\":\"huawei-agassi2\",\"architecture\":\"arm64\",\"root\":\"debian\"}\n"))goto fail;
 if(!text(root,"usr/lib/os-release",b,sizeof(b))||!contains(b,"\nID=debian\n")||!contains(b,"\nVERSION_ID=\"13\"\n"))goto fail;
 if(!text(root,"etc/debian_version",b,sizeof(b))||b[0]!='1'||b[1]!='3'||(b[2]!='.'&&b[2]!='\n'))goto fail;
 int sd=confined(root,"usr/sbin",0x4000);if(sd<0)goto fail;
 long n=sc(78,sd,(long)"init",(long)b,sizeof(b)-1,0,0);closefd(sd);
 if(n<0)goto fail;b[n]=0;
 if(!eq(b,"../lib/systemd/systemd")&&!eq(b,"/lib/systemd/systemd")&&!eq(b,"/usr/lib/systemd/systemd"))goto fail;
 int init=confined(root,"sbin/init",0),systemd=confined(root,"usr/lib/systemd/systemd",0);
 if(init<0||systemd<0){if(init>=0)closefd(init);if(systemd>=0)closefd(systemd);goto fail;}
 U a[16],c[16];int ok=regular(init,1,a)&&regular(systemd,1,c)&&a[0]==c[0]&&a[1]==c[1];
 u8 elf[64];ok=ok&&sc(63,init,(long)elf,sizeof(elf),0,0,0)==64;
 closefd(init);closefd(systemd);
 if(!ok||elf[0]!=0x7f||elf[1]!='E'||elf[2]!='L'||elf[3]!='F'||elf[4]!=2||elf[5]!=1||elf[18]!=183||elf[19]!=0)goto fail;
 closefd(root);str("T5_ROOT_IDENTITY_PASS confined-markers Debian13 ARM64 systemd; UUID/labels unused\n");return 0;
fail:closefd(root);return 1;
}
long run(U *sp){char**av=(char**)(sp+1);if(*sp!=3)return 3;
 if(eq(av[1],"identity"))return identity(av[2]);
 int fd=op(av[2],0);if(fd<0)return 4;
 if(eq(av[1],"arm")){
  long r=sc(29,fd,T5_EMMC_ARM_ROOT,0,0,0,0);
  if(r){str("ROOT_ARM_DENIED errno=");num(-r);str("\n");closefd(fd);return 5;}
 }else if(!eq(av[1],"query")){closefd(fd);return 3;}
 struct t5_root_window_info w={0};long r=sc(29,fd,T5_EMMC_QUERY_ROOT,(long)&w,0,0,0);closefd(fd);if(r)return 6;
 str("ROOT_WINDOW armed=");num(w.armed);str(" part=");num(w.partno);str(" first=");num(w.first);str(" blocks=");num(w.blocks);str("\n");return 0;
}
__asm__(".global _start\n_start:\nmov x0, sp\nbl run\nmov x8, #93\nsvc #0\n");
