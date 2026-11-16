/* Freestanding AArch64 read-only O_DIRECT benchmark; no block write path. */
typedef unsigned long U;
struct ts {long s,n;};
static unsigned char buffer[1048576] __attribute__((aligned(4096)));
static long sc(long n,long a,long b,long c,long d){
 register long x0 __asm__("x0")=a,x1 __asm__("x1")=b,x2 __asm__("x2")=c,x3 __asm__("x3")=d,x8 __asm__("x8")=n;
 __asm__ volatile("svc #0":"+r"(x0):"r"(x1),"r"(x2),"r"(x3),"r"(x8):"memory","cc");return x0;
}
static void str(const char*s){U n=0;while(s[n])n++;sc(64,1,(long)s,n,0);}
static void num(U n){char b[32];int i=31;b[i]=0;do{b[--i]='0'+n%10;n/=10;}while(n);str(b+i);}
static U dec(const char*s){U n=0;while(*s){if(*s<'0'||*s>'9')return ~0UL;n=n*10+*s++-'0';if(n>32768)return ~0UL;}return n;}
static int eq(const char*a,const char*b){while(*a&&*a==*b){a++;b++;}return *a==*b;}
static U ns(int clock){struct ts t;if(sc(113,clock,(long)&t,0,0))return 0;return (U)t.s*1000000000UL+t.n;}
long run(U*sp){
 char**av=(char**)(sp+1);if(*sp!=4)return 2;
 if(!eq(av[1],"/dev/mmcblk0")&&!eq(av[1],"/dev/block/mmcblk0"))return 3;
 U offset=dec(av[2]),length=dec(av[3]);if(offset<512||offset>14000||!length||length>1024)return 4;
 long fd=sc(56,-100,(long)av[1],0x90000,0); /* AArch64 O_RDONLY|O_DIRECT|O_CLOEXEC */
 if(fd<0){str("OPEN_ERROR=");num(-fd);str("\n");return 5;}
 if(sc(62,fd,offset*1048576UL,0,0)<0)return 6;
 U t=ns(1),c=ns(2),bytes=0;
 for(U i=0;i<length;i++){
  long r=sc(63,fd,(long)buffer,sizeof(buffer),0);
  if(r!=sizeof(buffer)){str("READ_ERROR=");num(r<0?-r:r);str("\n");return 7;}
  bytes+=r;
 }
 c=ns(2)-c;t=ns(1)-t;sc(57,fd,0,0,0);
 if(!t)return 8;
 str("T5_DIRECT_READ bytes=");num(bytes);str(" wall_ns=");num(t);str(" process_cpu_ns=");num(c);
 str(" KiB_per_sec=");num((bytes/1024)*1000000000UL/t);str("\n");return 0;
}
__asm__(".global _start\n_start:\n mov x0, sp\n bl run\n mov x8, #93\n svc #0\n");
