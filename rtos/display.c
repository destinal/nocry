// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * nocry-rtos: display bring-up for the Nebula Pad (480x272 RGB565 panel on the X2000's
 * display controller, PWM backlight).
 *
 * Creality's Linux loads its display drivers with lcd_is_inited=1 and expects all of this to
 * have been done before it starts.
 *
 * Register names and the simple-read ("SRDMA") descriptor come from Ingenic's GPL
 * libbare-cpu (drivers-x2000/x2000_hal_lcd.c, x2000_hal_dpu.c, soc-x2000/include/dpu.h).
 * The values are the ones read from a running stock system (firmware 1.1.0.30).
 */
#include "nocry_rtos.h"

/* clocks (CPM) */
#define CPM		0xb0000000u
#define CPM_CLKGR0	(CPM + 0x20)
#define CPM_CLKGR1	(CPM + 0x28)
#define CPM_LPCDR	(CPM + 0x64)	/* pixel clock: source, divider */
#define CPM_PWMCDR	(CPM + 0x6c)
#define CDR_CE		(1u << 29)
#define CDR_BUSY	(1u << 28)

/* GPIO ports, 0x100 apart; set/clear registers */
#define GPIO(port)	(0xb0010000u + (port) * 0x100)
#define PB		1
#define PC		2
#define GP_INTC		0x18
#define GP_MSKS		0x24
#define GP_MSKC		0x28
#define GP_PAT1S	0x34
#define GP_PAT1C	0x38
#define GP_PAT0S	0x44
#define GP_PAT0C	0x48

/* display controller (DPU) */
#define DPU		0xb3050000u
#define DC_FRM_CFG_ADDR		(DPU + 0x0000)
#define DC_FRM_CFG_CTRL		(DPU + 0x0004)
#define DC_CLR_ST		(DPU + 0x2008)
#define DC_INT_EN		(DPU + 0x200c)
#define DC_INT_FLAG		(DPU + 0x2010)
#define DC_COM_CONFIG		(DPU + 0x2014)
#define DISP_COM		(DPU + 0x8000)
#define TFT_HSYNC		(DPU + 0x9000)
#define TFT_VSYNC		(DPU + 0x9004)
#define TFT_HDE			(DPU + 0x9008)
#define TFT_VDE			(DPU + 0x900c)
#define TFT_CFG			(DPU + 0x9010)
#define INT_FRAME_DONE		(1u << 17)

/* PWM */
#define PWM		0xb34c0000u
#define PWM_CCFG0	(PWM + 0x00)
#define PWM_LVL		(PWM + 0x30)
#define PWM_DCR0	(PWM + 0x110)

/* board: where the screen connector's reset line and (K1 family) the panel's serial link are */
#ifdef NOCRY_BOARD_K1
#define RESET_PIN	29		/* PB29 */
#define LINK_CS		(1u << 2)	/* PA2 */
#define LINK_SCL	(1u << 27)	/* PC27 */
#define LINK_SDA	(1u << 28)	/* PC28 */
#define LINK_SDO	(1u << 16)	/* PB16 */
#else
#define RESET_PIN	16		/* PB16 */
#endif
#define PA		0

const struct panel *nocry_panel;

/*
 * Descriptors and frame buffer at the physical addresses the stock image uses (its heap,
 * right after its own data).
 */
#define FRAME0_PHYS	0x06070480u
#define FRAME1_PHYS	0x06070900u
#define LAYER_PHYS(n)	(0x06070960u + (n) * 0x60)
#define FB_PHYS		0x06070b40u
#define KSEG1(p)	((p) | 0xa0000000u)

struct frame_desc {		/* struct FrameDesc */
	u32 next;
	u32 size;		/* height << 16 | width */
	u32 ctrl;
	u32 wback_addr;
	u32 wback_stride;
	u32 layer[4];
	u32 lay_cfg_en;
	u32 irq_ctrl;
};

struct layer_desc {		/* struct LayerDesc */
	u32 size;
	u32 cfg;
	u32 buf;
	u32 scale;
	u32 rotation;
	u32 scratch;
	u32 pos;
	u32 resize_x;
	u32 resize_y;
	u32 stride;
	u32 uv_buf;
	u32 uv_stride;
};

/* port function / direction changes go through the shadow port PZ and are applied at once */
#define PZ		7
#define GP_PZGID2LD	0xf0

static void gpio_apply(int port, u32 pins, int int_, int msk, int pat1, int pat0)
{
	u32 z = GPIO(PZ);

	REG(z + (int_ ? 0x14 : GP_INTC)) = pins;
	REG(z + (msk ? GP_MSKS : GP_MSKC)) = pins;
	REG(z + (pat1 ? GP_PAT1S : GP_PAT1C)) = pins;
	REG(z + (pat0 ? GP_PAT0S : GP_PAT0C)) = pins;
	REG(z + GP_PZGID2LD) = port;
}

static void gpio_func(int port, u32 pins, int func)
{
	gpio_apply(port, pins, 0, 0, func & 2, func & 1);
}

static void gpio_out(int port, u32 pins, int level)
{
	gpio_apply(port, pins, 0, 1, 0, level);
}

static void set_divider(u32 reg, u32 value)
{
	int n = 100000;

	REG(reg) = value | CDR_CE;
	while ((REG(reg) & CDR_BUSY) && --n)
		;
	REG(reg) = value;
	if (!n)
		log_hex("divider busy ", reg);
}

__attribute__((unused)) static void gpio_in(int port, u32 pins)
{
	gpio_apply(port, pins, 0, 1, 1, 0);
}

__attribute__((unused)) static int gpio_get(int port, u32 pin)
{
	return (REG(GPIO(port)) & pin) != 0;
}

/* ---- panels -------------------------------------------------------------------------------- */

/* set-up script words (struct panel.script) */
#define C(x)	(x)			/* serial command byte */
#define D(x)	(0x100 | (x))		/* serial data byte */
#define MS(n)	(0x8000 | (n))		/* wait n milliseconds */
#define PWR(v)	(0x4000 | (v))		/* panel power pin PC21 (0 = on) */
#define RST(v)	(0x4002 | (v))		/* panel reset pin */
#define END	0xffff

/* the Nebula Pad's panel: no serial link, power and reset only */
static const unsigned short script_nebula[] = {
	PWR(1), RST(0), PWR(0), MS(30), RST(1), MS(5), RST(0), MS(5), RST(1), MS(25), END
};

__attribute__((unused)) static const struct panel panel_nebula = {
	"general_480x272", NEBULA_LCM_ID, 480, 272, 0x4400008b,	/* MPLL / 140, about 10.7 MHz, inverted */
	{ 0x0014021c, 0x0014014c, 0x00280208, 0x00280138, 0x00000102 }, script_nebula, 0, 0
};

#if defined(NOCRY_BOARD_K1) && !defined(NOCRY_PANEL_NEBULA)
/*
 * K1 family: four 480x800 panels behind a bit-banged 3-wire serial link (9-bit words: a
 * command/data bit, then 8 bits, most significant first). Routines and detection follow the
 * stock K1 image (docs/rtos-partition.md); the scripts in panels_k1.h are taken from its traces.
 */
static void link_idle(void)
{
	gpio_out(PA, LINK_CS, 1);
	gpio_out(PC, LINK_SCL, 0);
	gpio_out(PC, LINK_SDA, 0);
}

static void link_write(u32 word)
{
	int i;

	gpio_out(PA, LINK_CS, 0);
	for (i = 8; i >= 0; i--) {
		gpio_out(PC, LINK_SDA, (word >> i) & 1);
		udelay(2);
		gpio_out(PC, LINK_SCL, 1);
		udelay(2);
		gpio_out(PC, LINK_SCL, 0);
	}
	gpio_out(PA, LINK_CS, 1);
}

/* ST7701: read n bytes of a register; the panel answers on its own data-out line */
static void st7701_read(u32 reg, u8 *buf, int n)
{
	int i, k;

	gpio_out(PA, LINK_CS, 1);
	gpio_out(PC, LINK_SCL, 0);
	gpio_out(PC, LINK_SDA, 0);
	gpio_out(PA, LINK_CS, 0);
	gpio_in(PB, LINK_SDO);
	for (i = 8; i >= 0; i--) {
		gpio_out(PC, LINK_SDA, (reg >> i) & 1);
		udelay(2);
		gpio_out(PC, LINK_SCL, 1);
		udelay(2);
		gpio_out(PC, LINK_SCL, 0);
	}
	gpio_in(PC, LINK_SDA);
	if (n > 1) {			/* one extra clock before a multi-byte answer */
		udelay(2);
		gpio_out(PC, LINK_SCL, 1);
		udelay(2);
		gpio_out(PC, LINK_SCL, 0);
	}
	for (k = 0; k < n; k++) {
		u32 v = 0;

		for (i = 0; i < 8; i++) {
			udelay(2);
			v = v << 1 | gpio_get(PB, LINK_SDO);
			gpio_out(PC, LINK_SCL, 1);
			udelay(2);
			gpio_out(PC, LINK_SCL, 0);
		}
		buf[k] = v;
	}
	gpio_out(PB, LINK_SDO, 0);
	gpio_out(PC, LINK_SDA, 0);
	gpio_out(PC, LINK_SCL, 1);
	udelay(2);
	gpio_out(PC, LINK_SCL, 0);
	gpio_out(PA, LINK_CS, 1);
}

/* GC9503: its own framing for commands, and answers on the shared data line */
static void gc9503_cmd(u32 cmd)
{
	int i;

	gpio_out(PC, LINK_SCL, 0);
	gpio_out(PC, LINK_SDA, 0);
	udelay(2);
	gpio_out(PA, LINK_CS, 1);
	udelay(2);
	gpio_out(PA, LINK_CS, 0);
	udelay(2);
	gpio_out(PC, LINK_SCL, 0);
	udelay(2);
	gpio_out(PC, LINK_SDA, 0);	/* command */
	udelay(2);
	gpio_out(PC, LINK_SCL, 1);
	udelay(2);
	for (i = 7; i >= 0; i--) {
		gpio_out(PC, LINK_SCL, 0);
		udelay(2);
		gpio_out(PC, LINK_SDA, (cmd >> i) & 1);
		udelay(2);
		gpio_out(PC, LINK_SCL, 1);
		udelay(2);
	}
	gpio_out(PC, LINK_SCL, 0);
	udelay(2);
}

static void gc9503_data(u32 v)
{
	int i;

	gpio_out(PC, LINK_SCL, 0);
	gpio_out(PC, LINK_SDA, 0);
	udelay(2);
	gpio_out(PC, LINK_SCL, 0);
	udelay(2);
	gpio_out(PC, LINK_SDA, 1);	/* data */
	udelay(2);
	gpio_out(PC, LINK_SCL, 1);
	udelay(2);
	for (i = 7; i >= 0; i--) {
		gpio_out(PC, LINK_SCL, 0);
		udelay(2);
		gpio_out(PC, LINK_SDA, (v >> i) & 1);
		udelay(2);
		gpio_out(PC, LINK_SCL, 1);
	}
	gpio_out(PC, LINK_SCL, 0);
	udelay(2);
}

static void gc9503_write(u32 word)
{
	if (word & 0x100)
		gc9503_data(word & 0xff);
	else
		gc9503_cmd(word);
}

static u32 gc9503_read(void)
{
	u32 v = 0;
	int i;

	gpio_out(PC, LINK_SCL, 0);
	gpio_in(PC, LINK_SDA);
	udelay(2);
	for (i = 0; i < 8; i++) {
		udelay(2);
		gpio_out(PC, LINK_SCL, 0);
		udelay(2);
		gpio_out(PC, LINK_SCL, 1);
		udelay(2);
		v = v << 1 | gpio_get(PC, LINK_SDA);
	}
	gpio_out(PC, LINK_SCL, 0);
	gpio_out(PC, LINK_SDA, 0);
	udelay(2);
	return v;
}

static void panel_power_cycle(int ms_on, int ms_reset, int ms_after)
{
	gpio_out(PC, 1u << 21, 1);
	gpio_out(PB, 1u << RESET_PIN, 0);
	gpio_out(PC, 1u << 21, 0);
	mdelay(ms_on);
	gpio_out(PB, 1u << RESET_PIN, 1);
	mdelay(ms_reset);
	gpio_out(PB, 1u << RESET_PIN, 0);
	mdelay(ms_reset);
	gpio_out(PB, 1u << RESET_PIN, 1);
	mdelay(ms_reset);
	mdelay(ms_after);
}

static int detect_gc9503(void)
{
	u32 a, b;

	panel_power_cycle(20, 150, 10);
	gc9503_cmd(0xb2);
	gc9503_data(0x00);
	gc9503_cmd(0xdb);
	a = gc9503_read();
	gc9503_cmd(0xdc);
	b = gc9503_read();
	log_hex("gc9503 id ", b << 8 | a);
	return a == 0x95 && b == 0x04;
}

static int detect_tjc(void)
{
	u8 id[5] = { 0 }, v[3] = { 0 };
	u32 w;

	panel_power_cycle(20, 5, 10);
	st7701_read(0xa1, id, 5);
	w = id[1] << 8 | id[0];
	log_hex("st7701 a1 ", w);
	if (w != 0x177 && w != 0x288 && w != 0x399)
		return 0;
	st7701_read(0x04, v, 3);
	log_hex("st7701 04 ", v[0] << 16 | v[1] << 8 | v[2]);
	return v[0] == 0x54 && v[1] == 0x01 && v[2] == 0x01;
}

static int detect_st7701_04(u32 b0, u32 b1, u32 b2)
{
	u8 id[5], v[3] = { 0 };

	panel_power_cycle(20, 5, 10);
	st7701_read(0xa1, id, 5);
	st7701_read(0x04, v, 3);
	log_hex("st7701 04 ", v[0] << 16 | v[1] << 8 | v[2]);
	return v[0] == b0 && v[1] == b1 && v[2] == b2;
}

static int detect_pj(void)
{
	return detect_st7701_04(0x43, 0x02, 0x04);
}

static int detect_dc(void)
{
	return detect_st7701_04(0xdc, 0x43, 0x48);
}

#include "panels_k1.h"

#define K1_DEFAULT_PANEL	1	/* st7701_tjc: what the stock image uses when none answers */

static const struct panel *choose_panel(void)
{
	u32 i;

	link_idle();
	for (i = 0; i < sizeof(panels_k1) / sizeof(panels_k1[0]); i++)
		if (panels_k1[i].detect()) {
			log_str("found lcd ");
			log_str(panels_k1[i].name);
			log_str("\n");
			return &panels_k1[i];
		}
	log_str("default lcd ");
	log_str(panels_k1[K1_DEFAULT_PANEL].name);
	log_str("\n");
	return &panels_k1[K1_DEFAULT_PANEL];
}
#else
static const struct panel *choose_panel(void)
{
	return &panel_nebula;
}

#endif

static void run_script(const struct panel *pn)
{
	const unsigned short *p;

	for (p = pn->script; *p != END; p++) {
		u32 w = *p;

		if (w & 0x8000)
			mdelay(w & 0x7fff);
		else if ((w & 0xfffe) == 0x4000)
			gpio_out(PC, 1u << 21, w & 1);
		else if ((w & 0xfffe) == 0x4002)
			gpio_out(PB, 1u << RESET_PIN, w & 1);
		else if (pn->write)
			pn->write(w);
	}
}

/*
 * Everything below follows the order of the stock image's hardware accesses (recorded by
 * running that image in an emulator, see docs/rtos-partition.md).
 */
u32 *nocry_display_init(void)
{
	const struct panel *pn;
	struct frame_desc *f;
	struct layer_desc *l;
	int i;

	/* "main init": PWM clock, panel choice, pins, backlight off */
	set_divider(CPM_PWMCDR, 0x40000004);
	REG(CPM_CLKGR1) &= ~(1u << 5);
	nocry_panel = pn = choose_panel();
	gpio_func(PB, 0x0f00ffff, 1);		/* PB0-15 data, PB24-27 clock, enable, syncs */
	gpio_out(PC, 1u << 22, 0);		/* backlight supply off */
	REG(PWM_CCFG0) = 0;
	REG(PWM_LVL) = 1;
	REG(PWM_DCR0) = 0;

	/* "enable fb": controller set-up, pixel clock, panel power-on */
	REG(CPM_CLKGR0) &= ~(1u << 23);
	/*
	 * The pixel clock has to run before the display registers are written: a write to them
	 * without it never completes (second hardware test, 2026-11-26). The stock image sets
	 * the rate here and only the "inverted" bit after the TFT registers.
	 */
	set_divider(CPM_LPCDR, pn->lpcdr);
	REG(DC_INT_EN) = 0x00028142;
	REG(DC_CLR_ST) = 0;
	/*
	 * The stock image sets fields in these two registers and keeps the rest of their reset
	 * values (clock-gate enables: 0x00cf0000 and 0xe0). A capture on the board showed them;
	 * the emulator trace, where registers start at 0, did not.
	 */
	REG(DC_COM_CONFIG) = 0x00cf003c;
	REG(DISP_COM) = 0x002600f1;		/* TFT, RGB565 out with dithering */
	REG(TFT_HSYNC) = pn->tft[0];
	REG(TFT_VSYNC) = pn->tft[1];
	REG(TFT_HDE) = pn->tft[2];
	REG(TFT_VDE) = pn->tft[3];
	REG(TFT_CFG) = pn->tft[4];

	run_script(pn);		/* panel power, reset, and its commands if it takes any */
	REG(DC_COM_CONFIG) |= 2;

	/* one visible layer, 32-bit xRGB; two frame descriptors as the stock image leaves them */
	for (i = 0; i < 4; i++) {
		static const u32 cfg[4] = { 0x001500ff, 0x00000641, 0x000005e1, 0x00000581 };
		int k;

		l = (struct layer_desc *)KSEG1(LAYER_PHYS(i));
		for (k = 0; k < 12; k++)
			((u32 *)l)[k] = 0;
		l->cfg = cfg[i];
	}
	l = (struct layer_desc *)KSEG1(LAYER_PHYS(0));
	l->size = pn->yres << 16 | pn->xres;
	l->buf = FB_PHYS;
	l->resize_x = 0x200;
	l->resize_y = 0x200;
	l->stride = pn->xres;
	for (i = 0; i < 2; i++) {
		int k;

		f = (struct frame_desc *)KSEG1(i ? FRAME1_PHYS : FRAME0_PHYS);
		f->next = FRAME1_PHYS;
		f->size = pn->yres << 16 | pn->xres;
		f->ctrl = 4;
		f->wback_addr = 0;
		f->wback_stride = pn->xres;
		for (k = 0; k < 4; k++)
			f->layer[k] = LAYER_PHYS(k);
		f->lay_cfg_en = 0x0000e410;
		f->irq_ctrl = 0x00024200;
	}
	return (u32 *)KSEG1(FB_PHYS);
}

void nocry_display_start(void)
{
	int n;

	REG(DC_FRM_CFG_ADDR) = FRAME0_PHYS;
	REG(DC_FRM_CFG_CTRL) = 1;
	/* the stock image waits up to 300 ms for the "frame done" interrupt */
	for (n = 0; n < 300 && !(REG(DC_INT_FLAG) & INT_FRAME_DONE); n++)
		mdelay(1);
	log_hex("frame done after ms ", n);
	REG(DC_CLR_ST) = INT_FRAME_DONE;

	/* backlight: PC0 driven high (full brightness), then its supply PC22 on */
	gpio_out(PC, 1u << 0, 1);
	gpio_out(PC, 1u << 22, 1);
}

/*
 * The controller raises "frame done" again at the end of every frame. The stock image's
 * interrupt handler clears it each time, so when that image hands over, the flag was last
 * cleared exactly at a frame end. Do the same: wait for the next frame end, clear, return.
 * (Captures on the board: after the stock image the flag stays clear for several
 * milliseconds; cleared at an arbitrary moment it was already set again.)
 */
void nocry_display_quiesce(void)
{
	volatile u32 n;

	REG(DC_CLR_ST) = INT_FRAME_DONE;
	for (n = 0; n < 4000000 && !(REG(DC_INT_FLAG) & INT_FRAME_DONE); n++)
		;
	REG(DC_CLR_ST) = INT_FRAME_DONE;
}
