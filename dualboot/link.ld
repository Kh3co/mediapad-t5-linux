ENTRY(_start)
SECTIONS {
 . = 0;
 .text : { *(.text.boot) *(.text .text.*) }
 .rodata : { *(.rodata .rodata.*) }
 .data : { *(.data.boot) *(.data .data.*) }
 .bss : { *(.bss .bss.*) *(COMMON) }
 . = ALIGN(16); . += 0x10000; stack_top = .;
 ASSERT(. < 0x200000, "Menu exceeds loader allocation")
 . = 0x200000;
 .payload : { *(.payload) }
 image_end = .;
 ASSERT(image_end + 0x4000000 < 0x7b00000, "Relocated loader reaches firmware reserved RAM")
 /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
