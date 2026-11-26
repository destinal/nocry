/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NOCRY_RTOS_H
#define NOCRY_RTOS_H

typedef unsigned int u32;
typedef unsigned char u8;

#define REG(a)	(*(volatile u32 *)(a))

void log_str(const char *s);
void log_hex(const char *name, u32 v);
void mdelay(u32 ms);

void udelay(u32 us);

struct panel {
	const char *name;
	u32 lcm_id;		/* reported to the loader, ends up as lcm_id= on the kernel command line */
	u32 xres, yres;
	u32 lpcdr;		/* pixel clock source and divider */
	u32 tft[5];		/* TFT_HSYNC, TFT_VSYNC, TFT_HDE, TFT_VDE, TFT_CFG */
	const unsigned short *script;	/* power-on sequence, see display.c */
	int (*detect)(void);
	void (*write)(u32 word);	/* sends one serial word of the script; 0 if the panel has no link */
};

/* the panel in use, valid after nocry_display_init() */
extern const struct panel *nocry_panel;

/*
 * What the Nebula panel driver reports: 0 on the Nebula Pad. On a K1-family board with a Nebula
 * screen attached (experiment) 3, as that board's stock image does when no K1 panel answers.
 */
#ifdef NOCRY_BOARD_K1
#define NEBULA_LCM_ID	3
#else
#define NEBULA_LCM_ID	0
#endif

/* display.c: returns the frame buffer (xres * yres, 0xffRRGGBB per pixel) */
u32 *nocry_display_init(void);
void nocry_display_start(void);
void nocry_display_quiesce(void);

/* pictures.c: draws the picture for the board's model; 0 if the image carries none */
int nocry_show_picture(u32 *fb, u32 xres, u32 yres, const char *snmac, u32 snmac_len);

#endif
