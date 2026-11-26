/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Same memory use as the stock image: loaded at 96 MiB, heap ends at 0x86fa1200. */
OUTPUT_ARCH(mips)
ENTRY(_start)
SECTIONS
{
	. = 0x86000000;
	.text : { *(.start_section) *(.text*) }
	.rodata : { . = ALIGN(4); *(.rodata*) }
	.data : { . = ALIGN(4); *(.data*) *(.sdata*) }
	. = ALIGN(16);
	/* the build appends the picture store here and sets img_end in the header behind it */
	__code_end = .;
	__image_end = .;
	/* working memory at fixed addresses, clear of the pictures and below the display
	   descriptors and frame buffer (0x86070480 and up, see display.c) */
	. = 0x86060000;
	.bss (NOLOAD) : { __bss_start = .; *(.sbss*) *(.bss*) *(COMMON) . = ALIGN(4); __bss_end = .; }
	__stack_end = 0x86070000;
	__heap_start = 0x86070400;
	__heap_end = 0x86fa1200;
	/DISCARD/ : { *(.MIPS.abiflags) *(.reginfo) *(.comment) *(.note*) *(.eh_frame*) *(.pdr) *(.gnu.attributes) *(.mdebug*) }
}
