/*
 * nocry-banner: writes a few lines of text on the board's screen (nocry-rescue, until the
 * touch interface exists). SPDX-License-Identifier: GPL-2.0-or-later
 *
 * usage: nocry-banner [--fb DEV] [--raw FILE WIDTH HEIGHT] LINE...
 *   A line starting with "#" is drawn double size; "" leaves a gap.
 *   --raw writes a 32-bit picture to a file instead (for testing on a PC).
 *   The 480x272 panel of the Nebula Pad family is written upright (272 wide), turned the same
 *   way as the boot logo and the touch interface.
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>
#include "font.h"

static uint8_t *px; static int W, H, BPP, STRIDE, TURN;	/* W, H: as the text sees the screen */

static void put(int x, int y, uint32_t rgb)
{
	uint8_t *p;
	if (x < 0 || y < 0 || x >= W || y >= H) return;
	if (TURN) { int t = x; x = y; y = W - 1 - t; }
	p = px + y * STRIDE + x * BPP;
	if (BPP == 4) { p[0] = rgb; p[1] = rgb >> 8; p[2] = rgb >> 16; p[3] = 0xff; }
	else { uint16_t v = ((rgb >> 19) & 31) << 11 | ((rgb >> 10) & 63) << 5 | ((rgb >> 3) & 31); p[0] = v; p[1] = v >> 8; }
}

static void text(int x, int y, const char *s, int scale, uint32_t rgb)
{
	for (; *s; s++, x += FONT_W * scale) {
		int c = (unsigned char)*s, r, k, i, j;
		if (c < 32 || c > 126) c = '?';
		for (r = 0; r < FONT_H; r++)
			for (k = 0; k < FONT_W; k++)
				if (font[c - 32][r] >> (FONT_W - 1 - k) & 1)
					for (i = 0; i < scale; i++) for (j = 0; j < scale; j++)
						put(x + k * scale + i, y + r * scale + j, rgb);
	}
}

int main(int argc, char **argv)
{
	const char *fb = "/dev/fb0", *raw = NULL; int i = 1, n, y, total = 0, fd = -1; size_t size;

	if (i + 1 < argc && !strcmp(argv[i], "--fb")) { fb = argv[i + 1]; i += 2; }
	if (i + 3 < argc && !strcmp(argv[i], "--raw")) { raw = argv[i + 1]; W = atoi(argv[i + 2]); H = atoi(argv[i + 3]); i += 4; }
	if (raw) {
		BPP = 4; STRIDE = W * 4; size = (size_t)STRIDE * H;
		px = calloc(1, size);
	} else {
		struct fb_var_screeninfo v; struct fb_fix_screeninfo f;
		fd = open(fb, O_RDWR);
		if (fd < 0 || ioctl(fd, FBIOGET_VSCREENINFO, &v) || ioctl(fd, FBIOGET_FSCREENINFO, &f)) { perror(fb); return 1; }
		W = v.xres; H = v.yres; BPP = v.bits_per_pixel / 8;
		if (W == 480 && H == 272) { TURN = 1; W = 272; H = 480; } STRIDE = f.line_length; size = (size_t)STRIDE * v.yres_virtual;
		if (BPP != 4 && BPP != 2) { fprintf(stderr, "nocry-banner: %d bits per pixel not supported\n", v.bits_per_pixel); return 1; }
		px = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		if (px == MAP_FAILED) { perror("mmap"); return 1; }
		memset(px, 0, size);		/* also what the driver left there */
		v.yoffset = 0; ioctl(fd, FBIOPAN_DISPLAY, &v);
	}
	for (n = i; n < argc; n++) total += argv[n][0] == '#' ? FONT_H * 2 + 6 : argv[n][0] ? FONT_H + 2 : FONT_H / 2;
	y = (H - total) / 2; if (y < 2) y = 2;
	for (n = i; n < argc; n++) {
		const char *s = argv[n]; int scale = 1; uint32_t col = 0xe8e8e8;
		if (!*s) { y += FONT_H / 2; continue; }
		if (*s == '#') { s++; scale = 2; col = 0xffffff; }
		text((W - (int)strlen(s) * FONT_W * scale) / 2, y, s, scale, col);
		y += scale == 2 ? FONT_H * 2 + 6 : FONT_H + 2;
	}
	if (raw) { FILE *o = fopen(raw, "wb"); if (!o) { perror(raw); return 1; } fwrite(px, 1, size, o); fclose(o); }
	return 0;
}
