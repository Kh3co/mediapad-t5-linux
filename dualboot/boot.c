#include "types.h"
#include "payload-layout.h"
#include "tinf/tinf.h"
extern const u8 linux_gz_start[],linux_gz_end[],android_gz_start[],android_gz_end[];
extern u64 boot_record[16];
extern u64 boot_trace_elapsed(void);
extern void boot_trace_tick(void);
extern void boot_trace_line(const char *line);
extern void boot_trace_failure(void);
static u64 inflate_ms;

void t5_boot_trace_checksum(void) {
 inflate_ms=boot_trace_elapsed();
 boot_trace_line("Checking kernel CRC32");
}
static void flush(u64 start,u64 end) {
 u64 ctr;__asm__ volatile("mrs %0,ctr_el0":"=r"(ctr));
 u64 line=4UL<<((ctr>>16)&15);
 for(u64 p=start&~(line-1);p<end;p+=line) __asm__ volatile("dc cvac,%0"::"r"(p):"memory");
 __asm__ volatile("dsb sy; ic iallu; dsb sy; isb":::"memory");
}
u64 boot_payload(u64 choice) {
 const u8 *source;u32 compressed,expected;
 u32 android=(choice&255)==0;
 if(android) {source=android_gz_start;compressed=android_gz_end-android_gz_start;expected=ANDROID_RAW_SIZE;}
 else {source=linux_gz_start;compressed=linux_gz_end-linux_gz_start;expected=LINUX_RAW_SIZE;}
 u32 length=expected;
 boot_trace_line("Decompressing kernel");
 int result=tinf_gzip_uncompress((void*)0x480000UL,&length,source,compressed);
 if(result || length!=expected || *(volatile u32*)0x480038UL!=0x644d5241) {
  boot_trace_failure();
  return 0;
 }
 boot_record[1]=2;boot_record[9]=0x480000;boot_record[10]=choice>>8;
 boot_record[11]=android;boot_record[12]=length;boot_record[13]=compressed;
 u64 verified_ms=boot_trace_elapsed();
 boot_trace_line("Kernel verified. Preparing handoff");
 flush(0x480000,0x480000UL+length);
 boot_trace_line("Entering kernel");
 if(!android) {
  boot_record[14]=(verified_ms << 32) | (u32)inflate_ms;
  boot_record[15]=boot_trace_elapsed();
  u64 *target=(u64*)(0x480000UL+LINUX_HANDOFF_OFFSET);
  for(int i=0;i<16;i++) target[i]=boot_record[i];
  flush((u64)target,(u64)(target+16));
 }
 return 0x480000;
}
