.section .text.boot
.global _start
_start:
 b relocate
 nop
 .quad 0x80000
 .quad 0x2189000
 .quad 10
 .quad 0,0,0
 .ascii "ARM\x64"
 .word 0
relocate:
 /* Keep x0-x3 intact. Firmware enters at physical 0x480000. */
 movz x8,#0xe8a0,lsl #16
 movk x8,#0x6000
 movz w9,#0xe551
 movk w9,#0x1acc,lsl #16
 str w9,[x8,#0xc00]
 str wzr,[x8,#8]
 mov w9,#1
 str w9,[x8,#0xc00]
 adr x8,_start
 adrp x9,image_end
 add x9,x9,:lo12:image_end
 mov x10,#0x4000000
 mov x11,x10
1:
 ldr x12,[x8],#8
 str x12,[x11],#8
 cmp x8,x9
 b.lo 1b
 dsb sy
 /* The copied instructions must be visible even if firmware left I-cache on. */
 mrs x13,ctr_el0
 ubfx x13,x13,#16,#4
 mov x14,#4
 lsl x14,x14,x13
 mov x8,x10
2:
 dc cvac,x8
 add x8,x8,x14
 cmp x8,x11
 b.lo 2b
 dsb sy
 ic iallu
 dsb sy
 isb
 adr x8,_start
 adr x9,loader_entry
 sub x9,x9,x8
 add x9,x9,x10
 br x9
loader_entry:
 mov x24,x0
 mov x25,x1
 mov x26,x2
 mov x27,x3
 adrp x9,boot_record
 add x9,x9,:lo12:boot_record
 adr x10,_start
 stp x10,x24,[x9,#16]
 stp x25,x26,[x9,#32]
 str x27,[x9,#48]
 mrs x10,CurrentEL
 str x10,[x9,#56]
 mrs x10,sctlr_el1
 str x10,[x9,#64]
 ldr x10,magic
 str x10,[x9]
 mov x10,#1
 str x10,[x9,#8]
 adrp x10,stack_top
 add x10,x10,:lo12:stack_top
 mov sp,x10
3:
 bl menu_main
 bl boot_payload
 cbz x0,3b
 mov x19,x0
 msr daifset,#0xf
 mov x0,x24
 mov x1,x25
 mov x2,x26
 mov x3,x27
 dsb sy
 isb
 br x19
.balign 8
magic: .ascii "T5D115OK"
.section .data.boot,"aw"
.balign 8
.global boot_record
boot_record: .space 128
.section .payload,"a"
.balign 8
.global linux_gz_start,linux_gz_end,android_gz_start,android_gz_end
linux_gz_start: .incbin "linux.gz"
linux_gz_end:
.balign 8
android_gz_start: .incbin "android.gz"
android_gz_end:
.balign 8

.section .rodata.menu,"a"
.balign 8
.global menu_art
menu_art: .include "menu-art.inc"
