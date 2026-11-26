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

#define XRES		480
#define YRES		272

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

/*
 * Everything below follows the order of the stock image's hardware accesses (recorded by
 * running that image in an emulator, see docs/rtos-partition.md).
 */
u32 *nocry_display_init(void)
{
	struct frame_desc *f;
	struct layer_desc *l;
	int i;

	/* "main init": clocks for PWM and the timers the stock image uses, backlight off */
	log_str("> gates\n");
	REG(CPM_CLKGR1) &= ~((1u << 17) | (1u << 26));
	REG(CPM_CLKGR0) &= ~((1u << 20) | (1u << 18));
	log_str("> pwm divider\n");
	set_divider(CPM_PWMCDR, 0x40000004);
	REG(CPM_CLKGR1) &= ~(1u << 5);
	log_str("> pins\n");
	gpio_func(PB, 0x0f00ffff, 1);		/* PB0-15 data, PB24-27 clock, enable, syncs */
	gpio_out(PC, 1u << 22, 0);		/* backlight supply off */
	log_str("> pwm regs\n");
	REG(PWM_CCFG0) = 0;
	REG(PWM_LVL) = 1;
	REG(PWM_DCR0) = 0;

	/* "enable fb": controller set-up, pixel clock, panel power-on */
	log_str("> lcd gate\n");
	REG(CPM_CLKGR0) &= ~(1u << 23);
	/*
	 * The pixel clock has to run before the display registers are written: a write to them
	 * without it never completes (second hardware test, 2026-11-26). The stock image sets
	 * the rate here and only the "inverted" bit after the TFT registers.
	 */
	log_str("> pixel clock\n");
	set_divider(CPM_LPCDR, 0x4400008b);	/* MPLL / 140, about 10.7 MHz, clock inverted */
	log_str("> dpu regs\n");
	REG(DC_INT_EN) = 0x00028142;
	REG(DC_CLR_ST) = 0;
	REG(DC_COM_CONFIG) = 0x3c;
	REG(DISP_COM) = 0x00260011;		/* TFT, RGB565 out with dithering */
	REG(TFT_HSYNC) = 0x0014021c;
	REG(TFT_VSYNC) = 0x0014014c;
	REG(TFT_HDE) = 0x00280208;
	REG(TFT_VDE) = 0x00280138;
	REG(TFT_CFG) = 0x00000102;

	log_str("> panel power\n");
	gpio_out(PC, 1u << 21, 1);		/* panel power off (active low) */
	gpio_out(PB, 1u << 16, 0);		/* reset asserted */
	gpio_out(PC, 1u << 21, 0);
	mdelay(30);
	gpio_out(PB, 1u << 16, 1);
	mdelay(5);
	gpio_out(PB, 1u << 16, 0);
	mdelay(5);
	gpio_out(PB, 1u << 16, 1);
	mdelay(25);
	log_str("> com config\n");
	REG(DC_COM_CONFIG) |= 2;

	/* one visible layer, 480x272 xRGB; two frame descriptors as the stock image leaves them */
	log_str("> descriptors\n");
	for (i = 0; i < 4; i++) {
		static const u32 cfg[4] = { 0x001500ff, 0x00000641, 0x000005e1, 0x00000581 };
		int k;

		l = (struct layer_desc *)KSEG1(LAYER_PHYS(i));
		for (k = 0; k < 12; k++)
			((u32 *)l)[k] = 0;
		l->cfg = cfg[i];
	}
	l = (struct layer_desc *)KSEG1(LAYER_PHYS(0));
	l->size = YRES << 16 | XRES;
	l->buf = FB_PHYS;
	l->resize_x = 0x200;
	l->resize_y = 0x200;
	l->stride = XRES;
	for (i = 0; i < 2; i++) {
		int k;

		f = (struct frame_desc *)KSEG1(i ? FRAME1_PHYS : FRAME0_PHYS);
		f->next = FRAME1_PHYS;
		f->size = YRES << 16 | XRES;
		f->ctrl = 4;
		f->wback_addr = 0;
		f->wback_stride = XRES;
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

	log_str("> frame start\n");
	REG(DC_FRM_CFG_ADDR) = FRAME0_PHYS;
	REG(DC_FRM_CFG_CTRL) = 1;
	/* the stock image waits up to 300 ms for the "frame done" interrupt */
	for (n = 0; n < 300 && !(REG(DC_INT_FLAG) & INT_FRAME_DONE); n++)
		mdelay(1);
	log_hex("frame done after ms ", n);
	log_hex("int flags ", REG(DC_INT_FLAG));
	REG(DC_CLR_ST) = INT_FRAME_DONE;

	/* backlight: PC0 driven high (full brightness), then its supply PC22 on */
	gpio_out(PC, 1u << 0, 1);
	gpio_out(PC, 1u << 22, 1);
}
