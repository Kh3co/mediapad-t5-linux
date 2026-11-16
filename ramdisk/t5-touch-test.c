/* SPDX-License-Identifier: GPL-2.0-only
 * AGS2 touch validation, static AArch64 Linux userspace. No storage writes.
 */
typedef unsigned long U; typedef unsigned int u32;
static long sc(long n,long a,long b,long c,long d,long e,long f){
 register long x0 __asm__("x0")=a,x1 __asm__("x1")=b,x2 __asm__("x2")=c,x3 __asm__("x3")=d,x4 __asm__("x4")=e,x5 __asm__("x5")=f,x8 __asm__("x8")=n;
 __asm__ volatile("svc #0":"+r"(x0):"r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5),"r"(x8):"memory","cc");return x0;
}
static void str(const char*s){U n=0;while(s[n])n++;sc(64,1,(long)s,n,0,0,0);}
static void num(long n){char b[32];int i=31;b[i]=0;U v=n<0?-n:n;do{b[--i]='0'+v%10;v/=10;}while(v);if(n<0)b[--i]='-';str(b+i);}
static int eq(const char*a,const char*b){while(*a&&*a==*b){a++;b++;}return *a==*b;}
static long op(const char*p,int flags){return sc(56,-100,(long)p,flags,0,0,0);}
static long io(int fd,U req,void*p){return sc(29,fd,req,(long)p,0,0,0);}
struct ev {long sec,usec;unsigned short type,code;int value;};
struct pollfd{int fd;short events,revents;};
struct timespec{long s,ns;};
struct slot{int id,x,y;};
static struct slot slots[10];static u32 *fb;static u32 saved[1200*1920];
static const u32 colors[10]={0xff0000,0x00ff00,0x00aaff,0xffff00,0xff00ff,0x00ffff,0xff8800,0x99ff44,0xaa88ff,0xffffff};
static void pixel(int x,int y,u32 c){if(x>=0&&x<1920&&y>=0&&y<1200)fb[(1919-x)*1200+y]=0xff000000|c;}
static void box(int x,int y,int w,int h,u32 c){for(int j=0;j<h;j++)for(int i=0;i<w;i++)pixel(x+i,y+j,c);}
static void ring(int x,int y,int r,u32 c){for(int j=-r;j<=r;j++)for(int i=-r;i<=r;i++){int d=i*i+j*j;if(d<=r*r&&d>=(r-4)*(r-4))pixel(x+i,y+j,c);}}
static void digit(int n,int x,int y,u32 c){static const int bits[]={63,6,91,79,102,109,125,7,127,111};int b=bits[n%10];if(b&1)box(x+4,y,20,4,c);if(b&2)box(x+24,y+4,4,20,c);if(b&4)box(x+24,y+28,4,20,c);if(b&8)box(x+4,y+48,20,4,c);if(b&16)box(x,y+28,4,20,c);if(b&32)box(x,y+4,4,20,c);if(b&64)box(x+4,y+24,20,4,c);}
static void draw(int count){
 if(!fb)return;
 for(int i=0;i<1200*1920;i++)fb[i]=0xff101820;
 for(int x=0;x<1920;x+=120)box(x,0,1,1200,0x24303c);
 for(int y=0;y<1200;y+=120)box(0,y,1920,1,0x24303c);
 ring(40,40,24,0xffffff);ring(1879,40,24,0xffffff);ring(40,1159,24,0xffffff);ring(1879,1159,24,0xffffff);ring(960,600,30,0xffffff);
 digit(count/10,900,20,0xffffff);digit(count%10,940,20,0xffffff);
 for(int i=0;i<10;i++){ring(700+i*55,1150,20,slots[i].id>=0?colors[i]:0x333333);if(slots[i].id>=0){ring(slots[i].x,slots[i].y,40,colors[i]);digit(i,slots[i].x-14,slots[i].y-26,colors[i]);}}
}
long run(U *sp){
 char**av=(char**)(sp+1);int duration=600,gui=1;
 if(*sp>1){duration=0;for(char*p=av[1];*p;p++){if(*p<'0'||*p>'9')return 2;duration=duration*10+*p-'0';}if(duration<1||duration>3600)return 2;}
 if(*sp>2&&eq(av[2],"nogui"))gui=0;
 int fd=-1;char path[]="/dev/input/event00";char name[256];int abs[6];
 for(int i=0;i<32;i++){path[16]='0'+i/10;path[17]='0'+i%10;path[18]=0;if(i<10){path[16]='0'+i;path[17]=0;}
  int candidate=op(path,0x800);if(candidate<0)continue;
  if(io(candidate,0x8018456fUL,abs)>=0&&abs[2]>=9){fd=candidate;break;}sc(57,candidate,0,0,0,0,0);
 }
 if(fd<0){str("TOUCH_NO_10_SLOT_EVDEV\n");return 3;}
 io(fd,0x81004506UL,name);name[255]=0;str("TOUCH_DEVICE ");str(name);str("\n");
 io(fd,0x80184575UL,abs);int xmax=abs[2];io(fd,0x80184576UL,abs);int ymax=abs[2];
 str("TOUCH_RANGE ");num(xmax);str(" ");num(ymax);str("\n");if(xmax!=1919||ymax!=1199)return 4;
 if(gui){int f=op("/dev/fb0",2);if(f<0)return 5;u32 v[64];if(io(f,0x4600,v)<0||v[0]!=1200||v[1]!=1920||v[6]!=32)return 6;
  long m=sc(222,0,sizeof(saved),3,1,f,0);if(m<0&&m>-4096)return 7;fb=(u32*)m;for(int i=0;i<1200*1920;i++)saved[i]=fb[i];}
 for(int i=0;i<10;i++)slots[i].id=-1;
 int current=0,max=0,frames=0,downs=0,ups=0,moves=0,empty=0,bad=0;long start;struct timespec t;sc(113,1,(long)&t,0,0,0,0);start=t.s;draw(0);str("TOUCH_TEST_READY\n");
 while(1){sc(113,1,(long)&t,0,0,0,0);if(t.s-start>=duration)break;
  struct pollfd p={fd,1,0};struct timespec timeout={1,0};long r=sc(73,(long)&p,1,(long)&timeout,0,0,0);if(r<0)break;if(!r)continue;
  struct ev events[64];long bytes=sc(63,fd,(long)events,sizeof(events),0,0,0);if(bytes<0)continue;
  for(unsigned j=0;j<(unsigned)bytes/sizeof(struct ev);j++){struct ev*e=&events[j];
   if(e->type==3){if(e->code==0x2f){current=e->value;if(current<0||current>=10){bad++;current=0;}}else if(e->code==0x39){if(e->value<0&&slots[current].id>=0)ups++;if(e->value>=0&&slots[current].id<0)downs++;slots[current].id=e->value;}else if(e->code==0x35){slots[current].x=e->value;moves++;if(e->value<0||e->value>1919)bad++;}else if(e->code==0x36){slots[current].y=e->value;moves++;if(e->value<0||e->value>1199)bad++;}}
   if(e->type==0&&e->code==3){str("TOUCH_SYN_DROPPED\n");bad++;}
   if(e->type==0&&e->code==0){int active=0;for(int i=0;i<10;i++)if(slots[i].id>=0)active++;if(active>max)max=active;if(!active)empty++;frames++;
    str("FRAME ");num(e->sec);str(".");num(e->usec);str(" n=");num(active);
    for(int i=0;i<10;i++)if(slots[i].id>=0){str(" [");num(i);str(":");num(slots[i].id);str(" ");num(slots[i].x);str(",");num(slots[i].y);str("]");}str("\n");draw(active);
   }
  }
 }
 int active=0;for(int i=0;i<10;i++)if(slots[i].id>=0)active++;
 str("TOUCH_SUMMARY max=");num(max);str(" frames=");num(frames);str(" downs=");num(downs);str(" ups=");num(ups);str(" moves=");num(moves);str(" empty=");num(empty);str(" bad=");num(bad);str(" remaining=");num(active);str("\n");
 if(fb)for(int i=0;i<1200*1920;i++)fb[i]=saved[i];sc(57,fd,0,0,0,0,0);return bad?8:0;
}
__asm__(".global _start\n_start:\nmov x0, sp\nbl run\nmov x8, #93\nsvc #0\n");
