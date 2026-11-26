/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Reading the FPU and MSA control registers, with the instruction words of the stock image
 * (the assembler in use need not know MSA).
 */
	.set	noreorder
	.text
	.globl	nocry_read_fcsr
	.ent	nocry_read_fcsr
nocry_read_fcsr:
	.word	0x4442f800		/* cfc1 $v0, $31 */
	jr	$ra
	 nop
	.end	nocry_read_fcsr

	.globl	nocry_read_msacsr
	.ent	nocry_read_msacsr
nocry_read_msacsr:
	.word	0x787e0899		/* cfcmsa $v0, $1 */
	jr	$ra
	 nop
	.end	nocry_read_msacsr
