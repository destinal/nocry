/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NOCRY_RTOS_H
#define NOCRY_RTOS_H

typedef unsigned int u32;
typedef unsigned char u8;

#define REG(a)	(*(volatile u32 *)(a))

void log_str(const char *s);
void log_hex(const char *name, u32 v);
void mdelay(u32 ms);

/* display.c: returns the 480x272 frame buffer (0xffRRGGBB per pixel) */
u32 *nocry_display_init(void);
void nocry_display_start(void);

#endif
