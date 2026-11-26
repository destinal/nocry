// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Register capture, started by the board's loader in place of the Linux kernel (dump_main is
 * the entry point and runs on the loader's stack): copies the
 * display-related hardware registers into RAM (physical 0x06e80000, "NCRDUMP1", count, then
 * address/value pairs) and stops. Read it back after BOOT+RESET with `fastboot fetch ram`.
 * Used to compare what the stock rtos image and nocry-rtos leave behind for Linux.
 */
typedef unsigned int u32;

#define OUT	((volatile u32 *)0xa6e80000u)

static const u32 ranges[][2] = {
	/* known to be readable (read under Linux before) */
	{ 0x10000000, 0x10000100 },	/* CPM */
	{ 0x10010000, 0x10010500 },	/* GPIO A-E */
	{ 0x134c0000, 0x134c0120 },	/* PWM */
	{ 0x134c0300, 0x134c0304 },
	{ 0x13050000, 0x13050008 },	/* DPU */
	{ 0x13051000, 0x13051008 },
	{ 0x13052000, 0x13052038 },
	{ 0x13052100, 0x13052118 },
	{ 0x13052200, 0x13052220 },
	{ 0x13053000, 0x13053220 },
	{ 0x13058000, 0x13058004 },
	{ 0x13059000, 0x13059018 },
	/* not read before: last, so a fault here loses nothing above */
	{ 0x10002000, 0x10002120 },	/* TCU */
	{ 0x12200000, 0x12200200 },	/* CCU */
	{ 0x12300000, 0x12300200 },	/* interrupt controller */
	{ 0x12100000, 0x12100120 },	/* OST */
	{ 0x12000000, 0x12000040 },
};

void dump_main(void)
{
	volatile u32 *o = OUT;
	u32 n = 0, i, a;

	o[0] = 0x4452434e;	/* "NCRD" */
	o[1] = 0x31504d55;	/* "UMP1" */
	o[2] = 0;
	for (i = 0; i < sizeof(ranges) / sizeof(ranges[0]); i++)
		for (a = ranges[i][0]; a < ranges[i][1]; a += 4) {
			o[3 + 2 * n] = a;
			o[4 + 2 * n] = *(volatile u32 *)(a | 0xa0000000u);
			o[2] = ++n;
		}
	o[3 + 2 * n] = 0xffffffff;	/* complete */

	/*
	 * Then watch the display controller's "display end" status (DC_ST bit 17) for a while:
	 * 64 samples of DC_ST: 32 untouched, then 32 with the bit cleared after each sample.
	 * Stored after the table as "NCRS", then the samples.
	 */
	{
		volatile u32 *st = (volatile u32 *)0xb3052004u, *clr = (volatile u32 *)0xb3052008u;
		volatile u32 *q = o + 4 + 2 * n, d;

		q[0] = 0x5352434e;
		for (i = 0; i < 64; i++) {
			q[1 + i] = *st;
			if (i >= 32)		/* first half: only look */
				*clr = 0x20000;
			for (d = 0; d < 200000; d++)
				;
		}
		q[65] = 0xffffffff;
	}
	for (;;)
		__asm__ volatile("wait");
}
