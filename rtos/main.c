// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * nocry-rtos, stage 1: the smallest image the board's SPL accepts in place of Creality's.
 *
 * It does the hand-over the SPL waits for and nothing else (no picture yet):
 *   - report the panel id (0, as the retail Nebula Pad image does),
 *   - change the first word of the image so the SPL knows we are done,
 *   - put this core to sleep; the SPL then resets it and goes on to start Linux.
 * See docs/rtos-partition.md for how this was worked out.
 */
typedef unsigned int u32;

#define REG(a)		(*(volatile u32 *)(a))
#define CCU		0xb2200000u	/* core control unit, through kseg1 */
#define CCU_PIMR	(CCU + 0x120)
#define CCU_MIMR	(CCU + 0x160)
#define CCU_OIMR	(CCU + 0x1a0)

/* in .data, not .bss: the SPL writes it before we run */
u32 nocry_spl_word = 0xffffffff;

extern u32 _start[];
extern char __heap_end[];

/* A log in RAM for post-mortems: `fastboot fetch ram` after BOOT+RESET, look for "NCRTLOG1". */
#define LOG_ADDR	0xa6e00000u	/* kseg1: reaches RAM without a cache flush */
static char *logp;

static void log_str(const char *s)
{
	while (*s)
		*logp++ = *s++;
	*logp = 0;
}

static void log_hex(const char *name, u32 v)
{
	static const char d[] = "0123456789abcdef";
	int i;

	log_str(name);
	for (i = 28; i >= 0; i -= 4)
		*logp++ = d[(v >> i) & 15];
	*logp++ = '\n';
	*logp = 0;
}

#define mfc0(reg, sel) ({ u32 v_; __asm__ volatile("mfc0 %0, $" #reg ", " #sel : "=r"(v_)); v_; })
#define mtc0(reg, sel, v) __asm__ volatile("mtc0 %0, $" #reg ", " #sel "\n ehb" : : "r"((u32)(v)))

static void delay(u32 n)
{
	while (n--)
		__asm__ volatile("nop");
}

/* write back and invalidate the whole data cache of this core */
static void flush_dcache(void)
{
	u32 c1 = mfc0(16, 1);
	u32 sets = 64u << ((c1 >> 13) & 7), dl = (c1 >> 10) & 7, ways = 1 + ((c1 >> 7) & 7);
	u32 line = dl ? 2u << dl : 0, a;

	if (!line)
		return;
	for (a = 0x80000000u; a < 0x80000000u + sets * ways * line; a += line)
		__asm__ volatile("cache 0x01, 0(%0)" : : "r"(a));
	__asm__ volatile("sync");
}

void nocry_rtos_main(void *spl_args)
{
	u32 cpu = mfc0(15, 1) & 0x3ff;	/* EBase.CPUNum: 1 */
	u32 bit = 1u << (cpu & 31);

	logp = (char *)LOG_ADDR;
	log_str("NCRTLOG1\nnocry-rtos stage 1\n");
	log_hex("cpu ", cpu);
	log_hex("status ", mfc0(12, 0));
	log_hex("config ", mfc0(16, 0));
	log_hex("spl args ", (u32)spl_args);
	log_hex("sn_mac size from spl ", nocry_spl_word);

	/* as the stock image: this core's interrupt lines on, kseg0 cached */
	REG(CCU_PIMR) |= bit;
	REG(CCU_MIMR) |= bit;
	REG(CCU_OIMR) |= bit;
	mtc0(16, 0, (mfc0(16, 0) & ~7u) | 3);

	/* (the picture will be drawn here) */

	nocry_spl_word = 0;		/* panel id for the kernel command line: lcm_id=0 */
	_start[0] = 1;			/* "done": the SPL watches this word */
	log_str("handing back to the SPL\n");

	/* power this core down the way the stock image does; the SPL waits for it to sleep */
	REG(CCU_PIMR) &= ~bit;
	REG(CCU_MIMR) &= ~bit;
	REG(CCU_OIMR) &= ~bit;
	__asm__ volatile("sync");
	flush_dcache();
	delay(100000);
	__asm__ volatile("sync\n wait");
	log_str("woke up after wait: unexpected\n");
	for (;;)
		__asm__ volatile("wait");
}
