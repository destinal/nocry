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
	. = ALIGN(512);
	__image_end = .;
	.bss (NOLOAD) : { __bss_start = .; *(.sbss*) *(.bss*) *(COMMON) . = ALIGN(4); __bss_end = .; }
	. = ALIGN(16);
	. += 0x4000;
	__stack_end = .;
	. = ALIGN(1024);
	__heap_start = .;
	__heap_end = 0x86fa1200;
	/DISCARD/ : { *(.MIPS.abiflags) *(.reginfo) *(.comment) *(.note*) *(.eh_frame*) *(.pdr) *(.gnu.attributes) *(.mdebug*) }
}
