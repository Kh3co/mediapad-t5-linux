/* AGS2-L03 pre-kernel selector. No storage, heap, IRQ or GPU access. */
#include "types.h"
#include "fonts.h"
#include "menu-art.h"
#define FB 0x5d000000UL
#define GPIO 0xfff0e000UL
#define PMIC 0xfff34000UL
#define SYSCTRL 0xfff0a000UL
#define PAPER 0xeeeee7U
#define INK 0x151717U
#define FG 0xeef3faU
static inline u32 rd(u64 p) { return *(volatile u32 *)p; }
static inline void wr(u64 p,u32 v) { *(volatile u32 *)p=v; __asm__ volatile("dsb sy" ::: "memory"); }
static u64 ticks(void) { u64 x; __asm__ volatile("mrs %0,cntpct_el0":"=r"(x)); return x; }
static u64 frequency(void) { u64 x; __asm__ volatile("mrs %0,cntfrq_el0":"=r"(x)); return x; }
static void delay(u32 ms) { u64 end=ticks()+frequency()*ms/1000; while(ticks()<end) __asm__ volatile("yield"); }
static void pixel(u32 x,u32 y,u32 rgb) {
 /* Clockwise rotation, identical to Linux fbcon=rotate:1. */
 *(volatile u32 *)(FB+4UL*(x*1200UL+1199-y))=0xff000000U|rgb;
}
static void rect(int x,int y,int w,int h,u32 rgb) {
 for(int xx=x;xx<x+w;xx++) for(int yy=y;yy<y+h;yy++) pixel(xx,yy,rgb);
}
static u32 blend(u32 fg,u32 bg,u32 a) {
 u32 r=(((fg>>16)&255)*a+((bg>>16)&255)*(255-a))/255;
 u32 g=(((fg>>8)&255)*a+((bg>>8)&255)*(255-a))/255;
 u32 b=((fg&255)*a+(bg&255)*(255-a))/255;
 return (r<<16)|(g<<8)|b;
}
static void text_at(const char *s,int x,int y,const struct glyph *g,const u8 *data,int h,u32 fg,u32 bg) {
 for(;*s;s++) {
  const struct glyph *q=&g[(u8)*s-32];
  for(u32 xx=0;xx<q->width;xx++) for(int yy=0;yy<h;yy++) {
   u32 a=data[q->offset+yy*q->width+xx];
   if(a) pixel(x+xx,y+yy,blend(fg,bg,a));
  }
  x+=q->width;
 }
}
/* RLE runs never cross a column. The first pixel is the bottom one,
 * matching the native portrait framebuffer's contiguous addresses. */
static void draw_art(u32 offset, u32 x, u32 y, u32 width, u32 height) {
 const u8 *run=menu_art+offset;
 for(u32 column=0;column<width;column++) {
  volatile u32 *out=(volatile u32 *)(FB+4UL*((x+column)*1200UL+1200-y-height));
  u32 remaining=height;
  while(remaining) {
   u32 length=1U+*run++;
   u32 color=menu_palette[*run++];
   if(length>remaining) return;
   remaining-=length;
   for(u32 i=0;i<length;i++) *out++=color;
  }
 }
}

static const u32 rows[4][2]={
 {ART_ROW_0_0,ART_ROW_0_1}, {ART_ROW_1_0,ART_ROW_1_1},
 {ART_ROW_2_0,ART_ROW_2_1}, {ART_ROW_3_0,ART_ROW_3_1},
};
static const u32 details[4]={ART_DETAIL_0,ART_DETAIL_1,ART_DETAIL_2,ART_DETAIL_3};

static void select_item(int previous, int selected) {
 draw_art(rows[previous][0],73,382+122*previous,1051,105);
 draw_art(rows[selected][1],73,382+122*selected,1051,105);
 draw_art(details[selected],1204,815,645,201);
 __asm__ volatile("dsb sy" ::: "memory");
}

static void status(int what) {
 const char *message;
 switch(what) {
 case 3: message="Rebooting..."; break;
 case 4: message="Powering off..."; break;
 case 5: message="Power off did not complete. Try disconnecting USB."; break;
 case 6: message="Reboot: preparing reset"; break;
 case 7: message="Reboot: reset requested"; break;
 case 8: message="Reboot: reset unavailable"; break;
 case 10: message="Unpacking failed. Select another option."; break;
 default: message=""; break;
 }
 rect(73,906,1050,40,PAPER);
 text_at(message,73,906,font22_glyphs,font22_data,36,INK,PAPER);
 __asm__ volatile("dsb sy" ::: "memory");
}
/* The 32 kHz SCTRL slice also supplies Linux's HiFi audio timestamps. */
#define TRACE_SLICE (SYSCTRL+0x534)
#define TRACE_HZ 32768UL
static u32 trace_start;
static u64 last_update;
static unsigned trace_row;
static unsigned boot_failed;

static void trace_text(const char *line, int y) {
 rect(36,y,1200,40,0);
 text_at(line,36,y,font22_glyphs,font22_data,36,FG,0);
 __asm__ volatile("dsb sy" ::: "memory");
}

u64 boot_trace_elapsed(void) {
 return (u64)(u32)(rd(TRACE_SLICE)-trace_start)*1000/TRACE_HZ;
}

void boot_trace_tick(void) {
 u64 now=boot_trace_elapsed();
 if(last_update!=~0UL && now-last_update<100) return;
 last_update=now;
 char line[]="Elapsed 0000.000 s";
 u64 seconds=now/1000, fraction=now%1000;
 for(int i=11;i>=8;i--) {line[i]='0'+seconds%10; seconds/=10;}
 for(int i=15;i>=13;i--) {line[i]='0'+fraction%10; fraction/=10;}
 trace_text(line,24);
}

void boot_trace_line(const char *line) {
 boot_trace_tick();
 trace_text(line,104+48*trace_row++);
}

void boot_trace_failure(void) {
 boot_failed=1;
}

void boot_trace_begin(unsigned android) {
 trace_start=rd(TRACE_SLICE);
 trace_row=0;
 last_update=~0UL;
 rect(0,0,1920,1200,0);
 boot_trace_line(android ? "Android selected" : "Linux selected");
}

static void reboot(void) {
 /* Extracted from this device's 05-fastboot.img: reboot op a9f8c,
  * helper a9f18. SCPEREN1 bit31 bypasses the DDR self-refresh wait. */
 status(6);delay(180);
 wr(PMIC+0x62c,(rd(PMIC+0x62c)&~255U)|0x10);
 u32 old=rd(SYSCTRL+0x188);
 wr(SYSCTRL+0x170,0x80000000U);
 (void)rd(SYSCTRL+0x188);
 status(7);delay(180);
 for(int i=0;i<100;i++) { wr(SYSCTRL+4,0xdeadbeefU);delay(10); }
 if(!(old&0x80000000U)) wr(SYSCTRL+0x174,0x80000000U);
 status(8);
}
u64 menu_main(void) {
 /* Disable the already-armed SP805 watchdog, matching v108 primary_entry. */
 wr(0xe8a06c00UL,0x1acce551);wr(0xe8a06008UL,0);wr(0xe8a06c00UL,1);
 /* Preserve all non-key GPIO configuration and original interrupt mask. */
 u32 dir=rd(GPIO+0x400),mux1=rd(0xfff11058UL),mux2=rd(0xfff1105cUL);
 u32 pad1=rd(0xfff11864UL),pad2=rd(0xfff11868UL),mask=rd(PMIC+0x4c0);
 wr(0xfff11058UL,mux1&~7U);wr(0xfff1105cUL,mux2&~7U);
 wr(0xfff11864UL,(pad1&~3U)|1);wr(0xfff11868UL,(pad2&~3U)|1);
 wr(GPIO+0x400,dir&~6U);
 wr(PMIC+0x6c0,0xc0);wr(PMIC+0x4c0,mask&~0xc0U);
 draw_art(ART_BASE,0,0,1920,1200);
 __asm__ volatile("dsb sy" ::: "memory");
 int selected=1,power_down=0,stable=0,last=-1,volume_down=0;
 u64 presses=0;
 if(boot_failed) status(10);
 boot_failed=0;
 /* Ignore boot-time held keys until both volume switches are released. */
 int armed=0;
 while(1) {
  delay(10);
  int sample=(~rd(GPIO+24))&6;
  if(sample==last) { if(stable<3) stable++; } else {last=sample;stable=0;}
  if(stable>=2) {
   if(!armed && sample==0) armed=1;
   if(armed && (sample==2 || sample==4)) {
    if(volume_down==0) volume_down=sample;
    else if(volume_down!=sample) volume_down=-1;
   }
   if(armed && sample==6) volume_down=-1;
   if(armed && sample==0 && volume_down!=0) {
    if(volume_down==2 || volume_down==4) {
     int previous=selected;
     selected=(selected+(volume_down==2?3:1))%4;
     select_item(previous,selected);status(0);presses++;
    }
    volume_down=0;
   }
  }
  u32 key=rd(PMIC+0x6c0)&0xc0;
  if(key) wr(PMIC+0x6c0,key);
  if(key&0x80) power_down=1;
  if((key&0x40) && power_down) {
   power_down=0;
   if(selected<=1) {
    boot_trace_begin(selected==0);
    wr(PMIC+0x4c0,mask);wr(GPIO+0x400,dir);
    wr(0xfff11058UL,mux1);wr(0xfff1105cUL,mux2);
    wr(0xfff11864UL,pad1);wr(0xfff11868UL,pad2);
    return (presses<<8)|(u64)selected;
   }
   status(selected+1);delay(120);
   if(selected==2) reboot();
   if(selected==3) {
    /* Stock /powerhold: GPIO25.4 active-high, drop it to power down. */
    wr(PMIC+0x62c,rd(PMIC+0x62c)&~255U);
    wr(GPIO+0x400,rd(GPIO+0x400)|16);wr(GPIO+64,0);
    delay(1500);status(5);
   }
  }
 }
}
