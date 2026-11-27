// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * nocry-rtos: the boot logo.
 *
 * Pictures are not compiled in. tools/rtos-build/mkpictures.py packs them into a store that
 * the build appends to the image; each has a model name, and the one whose name equals the
 * model field of the board's sn_mac record ("serial;mac;model;...") is shown, like the stock
 * image chooses its logo. Pictures with an empty name are fallbacks; the one matching the
 * panel's size is preferred.
 *
 * Store: "NCPX", count, then per picture { char model[32]; u32 width, height, offset, length }
 * with offset from the start of the store. Pixel data: 16-bit pairs (run length, RGB565),
 * rows top to bottom.
 */
#include "nocry_rtos.h"

struct pic_entry {
	char model[32];
	u32 width, height, offset, length;
};

struct pic_store {
	u32 magic;		/* "NCPX" */
	u32 count;
	struct pic_entry e[];
};

extern const char __code_end[];

static int same(const char *a, const char *b, u32 blen)
{
	u32 i;

	for (i = 0; i < blen; i++)
		if (a[i] != b[i] || !a[i])
			return 0;
	return a[blen] == 0;
}

/* the model field: third of the ';'-separated fields */
static const char *model_field(const char *rec, u32 len, u32 *mlen)
{
	u32 i, field = 0, start = 0;

	if (len > 256)
		len = 256;
	for (i = 0; i < len && rec[i]; i++) {
		if (rec[i] != ';')
			continue;
		if (field == 2) {
			*mlen = i - start;
			return rec + start;
		}
		field++;
		start = i + 1;
	}
	return 0;
}

static u32 rgb(u32 p)
{
	u32 r = (p >> 11) & 31, g = (p >> 5) & 63, b = p & 31;

	return 0xff000000 | (r << 3 | r >> 2) << 16 | (g << 2 | g >> 4) << 8 | (b << 3 | b >> 2);
}

/* Returns 1 if a picture was drawn. `snmac` may be 0. */
int nocry_show_picture(u32 *fb, u32 XRES, u32 YRES, const char *snmac, u32 snmac_len)
{
	const struct pic_store *s = (const struct pic_store *)__code_end;
	const struct pic_entry *pick = 0;
	const char *model = 0;
	const unsigned short *d;
	u32 mlen = 0, i, n, x, y, run, color = 0, bg;
	int ox, oy;

	if (s->magic != 0x5850434e || !s->count || s->count > 64)
		return 0;
	if (snmac && snmac_len)
		model = model_field(snmac, snmac_len, &mlen);
	for (i = 0; i < s->count; i++) {
		if (model && mlen && mlen < 32 && same(s->e[i].model, model, mlen)) {
			pick = &s->e[i];
			break;
		}
		/* fallback: a picture without a name, preferably one made for this panel's size */
		if (!s->e[i].model[0] && (!pick || (s->e[i].width == XRES && s->e[i].height == YRES)))
			pick = &s->e[i];
	}
	if (!pick || !pick->width || !pick->height || pick->width > 2048 || pick->height > 2048)
		return 0;
	log_str("picture for model \"");
	log_str(pick->model);
	log_str("\"\n");

	d = (const unsigned short *)((const char *)s + pick->offset);
	n = pick->length / 4;
	bg = n ? rgb(d[1]) : 0xff000000;
	for (i = 0; i < XRES * YRES; i++)
		fb[i] = bg;
	/* centred; a larger picture is cropped around its centre, like the stock image does */
	ox = ((int)XRES - (int)pick->width) / 2;
	oy = ((int)YRES - (int)pick->height) / 2;
	run = 0;
	for (y = 0; y < pick->height; y++)
		for (x = 0; x < pick->width; x++) {
			int px = (int)x + ox, py = (int)y + oy;

			if (!run) {
				if (!n)
					return 1;
				run = d[0];
				color = rgb(d[1]);
				d += 2;
				n--;
				if (!run)
					return 1;
			}
			run--;
			if (px >= 0 && px < (int)XRES && py >= 0 && py < (int)YRES)
				fb[py * XRES + px] = color;
		}
	return 1;
}
