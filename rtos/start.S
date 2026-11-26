/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * nocry-rtos: entry and image header.
 *
 * The header is `struct rtos_header` of Ingenic's U-Boot SPL (common/spl/spl_rtos.h): the SPL
 * reads it from the first sector of the rtos partition, loads the image to img_start and
 * enters it at img_start on CPU core 1 with a0 = its argument block.
 */
	.set	mips32r2
	.set	noreorder
	.set	noat
	.section .start_section, "ax", @progbits
	.globl	_start
	.ent	_start
_start:
	b	1f			/* code[0], code[1] */
	 nop
	.word	0x534f5452		/* tag "RTOS" */
	.word	1			/* version; bit 0: the SPL waits for us, then resets this core */
	.word	_start			/* img_start */
	.word	__image_end		/* img_end */
	.word	__heap_start
	.word	__heap_end		/* the SPL puts the sn_mac partition right below this */
	.word	nocry_spl_word		/* SPL writes the sn_mac size here; we answer with the panel id */
1:
	la	$k0, __bss_start
	la	$k1, __bss_end
2:	beq	$k0, $k1, 3f
	 nop
	sw	$zero, 0($k0)
	b	2b
	 addiu	$k0, 4
3:
	la	$sp, __stack_end
	la	$t9, nocry_rtos_main
	jalr	$t9
	 nop
4:	b	4b
	 nop
	.end	_start
