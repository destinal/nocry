/*
 * nocry-ui: the rescue system's touch interface (docs/nocry-rescue.md).
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Draws with LVGL (MIT) straight into the frame buffer and reads the Nebula Pad's resistive
 * touch chip (NS2009, I2C) itself.  It does no work on the board's storage: every action is
 * one of the commands the web pages use too (nocry-rescue-do, nocry-wifi), started as a
 * separate program with its arguments as they are, never through a shell.  So the checks
 * exist once.
 *
 *   nocry-ui [--fb DEV] [--i2c DEV] [--turn 0|90|180|270]
 *   nocry-ui --sim SCRIPT        no screen and no touch chip: follow a script of
 *                                "tap X Y", "hold X Y MS", "wait MS", "shot FILE.ppm" lines
 *                                (testing on a PC; commands come from $PATH; the panel is
 *                                taken to be 480x272; --turn 90 for the upright layout)
 *
 * Which way up: the panel is used as it is (the Nebula Pad's is wide, 480x272) unless the
 * owner turned the interface with the button on the home screen: a quarter turn clockwise each time,
 * so upright (272 wide, 480 high) and both ways round.  The button sits in the same corner
 * of the glass wide and upright (top right when wide, bottom right when upright).  --turn
 * overrides.  $NOCRY_UI_LOG=1 writes every touch to standard error (where it landed, how hard).
 *
 * The touch position needs calibrating once (three crosses).  It is worked out for the panel,
 * not for one way up, so turning the interface needs no new calibration.  It is kept on the
 * board beside the rescue image, with the turn and the other settings (/run/nocry-touch.cal
 * while running).  Holding a finger anywhere for ten seconds starts the calibration again, so
 * a wrong one cannot lock anybody out.
 */
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <linux/fb.h>
#include <linux/i2c-dev.h>
#include "lvgl.h"

#define BAR 44			/* the title bar */
#define MAXPIX (800 * 480)

static int W = 480, H = 272;	/* as the interface sees the screen */
static int PW = 480, PH = 272;	/* the panel itself */
static int turn;		/* 0, 90, 180 or 270: how far the interface is turned on the panel */
static int eject_ask = 1;	/* ask about ejecting the flash drive after using it */
static int remote_lan, remote_adb, remote_ssh; static char ssh_pass[70];	/* remote access the owner turned on (nocry-remote reads these) */
static char ap_ssid[40], ap_pass[70]; static int ap_own;	/* the owner's name and password for the board's own network, if set */

/* a point of the interface on the panel, and back */
static void to_panel(int x, int y, int *fx, int *fy)
{
	if (turn == 90) { *fx = y; *fy = W - 1 - x; }
	else if (turn == 180) { *fx = W - 1 - x; *fy = H - 1 - y; }
	else if (turn == 270) { *fx = H - 1 - y; *fy = x; }
	else { *fx = x; *fy = y; }
}
static void from_panel(int fx, int fy, int *x, int *y)
{
	if (turn == 90) { *x = PH - 1 - fy; *y = fx; }
	else if (turn == 180) { *x = PW - 1 - fx; *y = PH - 1 - fy; }
	else if (turn == 270) { *x = fy; *y = PW - 1 - fx; }
	else { *x = fx; *y = fy; }
}
static int touch_log;

static const char *RUN = "/run";
static int sim;
static uint32_t sim_ms;

uint32_t nocry_ui_ms(void)
{
	struct timespec t;
	if (sim) return sim_ms;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint32_t)(t.tv_sec * 1000 + t.tv_nsec / 1000000);
}

/* ---- the screen -------------------------------------------------------------------- */
static uint8_t *fb; static int fb_bpp, fb_stride;
static lv_color_t frame[MAXPIX];

static void flush(lv_disp_drv_t *d, const lv_area_t *a, lv_color_t *c)
{
	int x, y;
	(void)c; (void)a;
	/* "frame" always holds the whole picture and LVGL calls this once per changed area with
	   that same buffer: copy it out once, after the last one, turned if the panel is */
	if (lv_disp_flush_is_last(d))
	for (y = 0; fb && y < H; y++)
		for (x = 0; x < W; x++) {
			lv_color_t px = frame[y * W + x];
			int fx, fy;
			to_panel(x, y, &fx, &fy);
			if (fb_bpp == 4) memcpy(fb + fy * fb_stride + fx * 4, &px, 4);
			else { uint16_t v = (px.ch.red >> 3) << 11 | (px.ch.green >> 2) << 5 | px.ch.blue >> 3; memcpy(fb + fy * fb_stride + fx * 2, &v, 2); }
		}
	lv_disp_flush_ready(d);
}

static int screen_open(const char *dev)
{
	struct fb_var_screeninfo v; struct fb_fix_screeninfo f; int fd = open(dev, O_RDWR | O_CLOEXEC); size_t size;
	if (fd < 0 || ioctl(fd, FBIOGET_VSCREENINFO, &v) || ioctl(fd, FBIOGET_FSCREENINFO, &f)) { perror(dev); return -1; }
	fb_bpp = v.bits_per_pixel / 8; fb_stride = f.line_length; size = (size_t)fb_stride * v.yres_virtual;
	if ((fb_bpp != 4 && fb_bpp != 2) || v.xres * v.yres > MAXPIX || v.xres < 272 || v.yres < 272) { fprintf(stderr, "nocry-ui: this screen is not supported\n"); return -1; }
	PW = v.xres; PH = v.yres;
	fb = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (fb == MAP_FAILED) { perror("mmap"); fb = NULL; return -1; }
	memset(fb, 0, size);
	v.yoffset = 0; ioctl(fd, FBIOPAN_DISPLAY, &v);
	return 0;
}

/* ---- the touch chip ---------------------------------------------------------------- */
static int i2c = -1;
static double cal[6]; static int cal_ok;	/* panel x = c0*rx + c1*ry + c2, y = c3*rx + c4*ry + c5 */
static int raw_x, raw_y, raw_down;		/* the last reading, for the calibration screen */
static int sim_x, sim_y, sim_down;
static uint32_t held_since; static int want_calibration;

static int ns_read(int cmd)
{
	unsigned char c = cmd, b[2];
	if (write(i2c, &c, 1) != 1 || read(i2c, b, 2) != 2) return -1;
	return b[0] << 4 | b[1] >> 4;
}

static void touch_read(lv_indev_drv_t *d, lv_indev_data_t *data)
{
	static int px, py, have, down, last_x, last_y, errs;
	int z, x, y, x2, y2;
	(void)d;
	if (sim) {
		raw_down = sim_down; raw_x = sim_x; raw_y = sim_y;
		data->point.x = sim_x; data->point.y = sim_y; data->state = sim_down ? LV_INDEV_STATE_PR : LV_INDEV_STATE_REL;
		return;
	}
	z = i2c < 0 ? -1 : ns_read(0xe0);
	/* The pressure reading is 0 with nothing on the screen, but how high it gets under a finger
	   depends on where the finger is (it is a voltage divider across the panel): towards one
	   edge it stays low.  So the bar is low: pressed from 150 up, let go below 80. */
	if (z >= (down ? 80 : 150)) {
		x = ns_read(0xc0); y = ns_read(0xd0); x2 = ns_read(0xc0); y2 = ns_read(0xd0);
		/* two readings that agree, or the reading is taken while the finger settles */
		if (x >= 0 && y >= 0 && abs(x - x2) < 40 && abs(y - y2) < 40) {
			x = (x + x2) / 2; y = (y + y2) / 2;
			if (!down) { down = 1; have = 0; held_since = nocry_ui_ms(); }
			/* report one reading late: the last one before the finger lifts is the worst */
			if (have) { raw_x = px; raw_y = py; raw_down = 1; }
			px = x; py = y; have = 1;
		}
		errs = 0;
	} else if (z >= 0 || i2c < 0 || ++errs >= 3) {
		down = 0; have = 0; raw_down = 0; if (z >= 0) errs = 0;
	}
	if (raw_down && cal_ok) {
		from_panel((int)(cal[0] * raw_x + cal[1] * raw_y + cal[2]), (int)(cal[3] * raw_x + cal[4] * raw_y + cal[5]), &last_x, &last_y);
		if (last_x < 0) last_x = 0;
		if (last_x >= W) last_x = W - 1;
		if (last_y < 0) last_y = 0;
		if (last_y >= H) last_y = H - 1;
	}
	if (touch_log) {
		static int was;
		if (raw_down && !was) fprintf(stderr, "touch down: raw %d,%d pressure %d -> screen %d,%d\n", raw_x, raw_y, z, last_x, last_y);
		if (!raw_down && was) fprintf(stderr, "touch up:   last screen %d,%d\n", last_x, last_y);
		was = raw_down;
	}
	if (down && nocry_ui_ms() - held_since > 10000) { want_calibration = 1; held_since = nocry_ui_ms(); }
	data->point.x = last_x; data->point.y = last_y;
	data->state = raw_down && cal_ok ? LV_INDEV_STATE_PR : LV_INDEV_STATE_REL;
}

/* ---- small helpers ----------------------------------------------------------------- */
static char *slurp(const char *path)
{
	FILE *f = fopen(path, "r"); char *b; size_t n;
	if (!f) return strdup("");
	b = malloc(16385); n = fread(b, 1, 16384, f); b[n] = 0; fclose(f);
	return b;
}
static char *run_file(const char *name) { char p[128]; snprintf(p, sizeof p, "%s/%s", RUN, name); return slurp(p); }

/* the value of "key=" in key=value lines; "" if there is none */
static const char *kv(const char *text, const char *key)
{
	static char out[16][400]; static int k; char *o = out[k = (k + 1) & 15]; size_t n = strlen(key); const char *p = text;
	o[0] = 0;
	while (p && *p) {
		if (!strncmp(p, key, n) && p[n] == '=') {
			size_t l = strcspn(p + n + 1, "\r\n"); if (l > 399) l = 399;
			memcpy(o, p + n + 1, l); o[l] = 0; return o;
		}
		p = strchr(p, '\n'); if (p) p++;
	}
	return o;
}

/* "7.3 GiB", "12 MiB", "1.1 MiB", "640 KiB" */
static const char *amount(long long bytes)
{
	static char out[4][24]; static int k; char *o = out[k = (k + 1) & 3];
	if (bytes >= 1LL << 30) snprintf(o, 24, "%.1f GiB", bytes / 1073741824.0);
	else if (bytes >= 10 << 20) snprintf(o, 24, "%lld MiB", bytes >> 20);
	else if (bytes >= 1 << 20) snprintf(o, 24, "%.1f MiB", bytes / 1048576.0);
	else snprintf(o, 24, "%lld KiB", bytes >> 10);
	return o;
}

/* ---- running the board's commands without stopping the screen --------------------- */
typedef void (*done_fn)(int rc, char *out);
static struct proc { pid_t pid; done_fn fn; int gen; char out[96]; } procs[6];
static int gen;		/* changes with every screen: an answer for a screen that is gone is dropped */
static int serial;

static void start(done_fn fn, const char *input, ...)
{
	char *argv[10]; int n = 0, i; va_list ap; struct proc *p = NULL;
	va_start(ap, input);
	while (n < 9 && (argv[n] = va_arg(ap, char *))) n++;
	argv[n] = NULL; va_end(ap);
	for (i = 0; i < 6; i++) if (!procs[i].pid) { p = &procs[i]; break; }
	if (!p) { if (fn) { char e[] = "too much at once"; fn(127, e); } return; }
	snprintf(p->out, sizeof p->out, "%s/nocry-ui.%d.%d", RUN, (int)getpid(), serial++);
	p->fn = fn; p->gen = gen;
	p->pid = fork();
	if (p->pid == 0) {
		int o = open(p->out, O_WRONLY | O_CREAT | O_TRUNC, 0600), in = open(input ? input : "/dev/null", O_RDONLY);
		dup2(in, 0); dup2(o, 1); dup2(o, 2);
		execvp(argv[0], argv);
		_exit(127);
	}
	if (p->pid < 0) { p->pid = 0; if (fn) { char e[] = "could not start it"; fn(127, e); } }
}

static void reap(lv_timer_t *t)
{
	int i, st;
	(void)t;
	for (i = 0; i < 6; i++) {
		struct proc *p = &procs[i];
		if (!p->pid || waitpid(p->pid, &st, WNOHANG) != p->pid) continue;
		p->pid = 0;
		if (p->fn && p->gen == gen) {
			char *out = slurp(p->out); size_t l = strlen(out);
			while (l && (out[l - 1] == '\n' || out[l - 1] == ' ')) out[--l] = 0;
			unlink(p->out);
			p->fn(WIFEXITED(st) ? WEXITSTATUS(st) : 126, out);
			free(out);
		} else unlink(p->out);
	}
}

/* The settings another program may have changed since they were read (the web pages change
   remote access, the SSH password and the access point's name): read again before this
   program shows or changes any setting, so that writing the file does not undo theirs. */
static void settings_refresh(void)
{
	char *s = run_file("nocry-touch.cal"), *p;
	if (!*s) { free(s); return; }
	remote_lan = strstr(s, "\nremote_lan on") != NULL; remote_adb = strstr(s, "\nremote_adb on") != NULL; remote_ssh = strstr(s, "\nremote_ssh on") != NULL;
	if ((p = strstr(s, "\nssh_pass "))) snprintf(ssh_pass, sizeof ssh_pass, "%.*s", (int)strcspn(p + 10, "\n"), p + 10);
	if ((p = strstr(s, "\nap_ssid ")) && p[9] != '\n' && p[9]) {
		snprintf(ap_ssid, sizeof ap_ssid, "%.*s", (int)strcspn(p + 9, "\n"), p + 9); ap_own = 1;
		ap_pass[0] = 0;
		if ((p = strstr(s, "\nap_pass "))) snprintf(ap_pass, sizeof ap_pass, "%.*s", (int)strcspn(p + 9, "\n"), p + 9);
	}
	free(s);
}
/* the settings, into the file the start-up script read them from and onto the board */
static void settings_store(done_fn fn)
{
	char p[128]; FILE *f;
	snprintf(p, sizeof p, "%s/nocry-touch.cal", RUN);
	f = fopen(p, "w"); if (!f) { if (fn) { char e[] = ""; fn(1, e); } return; }
	if (cal_ok) fprintf(f, "fb %dx%d %.9g %.9g %.9g %.9g %.9g %.9g\n", PW, PH, cal[0], cal[1], cal[2], cal[3], cal[4], cal[5]);
	else fprintf(f, "no calibration\n");
	fprintf(f, "turn %d\neject %s\n", turn, eject_ask ? "ask" : "never");
	if (ap_own) fprintf(f, "ap_ssid %s\nap_pass %s\n", ap_ssid, ap_pass);	/* nocry-wifi reads these two */
	fprintf(f, "remote_lan %s\nremote_adb %s\nremote_ssh %s\n", remote_lan ? "on" : "off", remote_adb ? "on" : "off", remote_ssh ? "on" : "off");
	if (ssh_pass[0]) fprintf(f, "ssh_pass %s\n", ssh_pass);
	fclose(f);
	start(fn, p, "nocry-rescue-do", "cal-put", NULL);	/* kept beside the copies of the rescue image */
}

/* ---- the frame every screen shares ------------------------------------------------- */
typedef void (*screen_fn)(void);
static screen_fn stack[10]; static int depth;
static lv_obj_t *title, *back_btn, *code_lbl, *content, *modal, *rot_btn;
static lv_timer_t *screen_timer;

static lv_timer_t *busy_timer;
static void modal_close(void)
{
	if (busy_timer) { lv_timer_del(busy_timer); busy_timer = NULL; }
	if (modal) { lv_obj_del(modal); modal = NULL; }
}

static void show(void)
{
	gen++;
	if (screen_timer) { lv_timer_del(screen_timer); screen_timer = NULL; }
	modal_close();
	lv_obj_clean(content);
	lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
	lv_obj_set_style_pad_all(content, 8, 0);
	lv_obj_scroll_to_y(content, 0, LV_ANIM_OFF);
	if (depth > 1) lv_obj_clear_flag(back_btn, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(back_btn, LV_OBJ_FLAG_HIDDEN);
	if (rot_btn) { if (depth > 1) lv_obj_add_flag(rot_btn, LV_OBJ_FLAG_HIDDEN); else lv_obj_clear_flag(rot_btn, LV_OBJ_FLAG_HIDDEN); }
	stack[depth - 1]();
}
static void go(screen_fn f) { if (depth < 10) stack[depth++] = f; show(); }
static void back(void) { if (depth > 1) depth--; show(); }
static void home(void) { depth = 1; show(); }
static void back_cb(lv_event_t *e) { (void)e; back(); }
static void set_title(const char *t) { lv_label_set_text(title, t); }

static lv_obj_t *text(lv_obj_t *parent, const char *s)
{
	lv_obj_t *l = lv_label_create(parent);
	lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(l, lv_pct(100));
	lv_label_set_text(l, s);
	return l;
}
static lv_obj_t *dim(lv_obj_t *l) { lv_obj_set_style_text_color(l, lv_color_hex(0xa0a8b0), 0); return l; }

static lv_obj_t *button(lv_obj_t *parent, const char *label, lv_event_cb_t cb, void *arg)
{
	lv_obj_t *b = lv_btn_create(parent), *l = lv_label_create(b);
	lv_label_set_text(l, label); lv_obj_center(l);
	lv_obj_set_height(b, 44);		/* a finger on a resistive screen needs room */
	lv_obj_set_style_pad_hor(b, 14, 0);
	if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, arg);
	return b;
}
static lv_obj_t *wide(lv_obj_t *b)
{
	lv_obj_t *l = lv_obj_get_child(b, 0);
	lv_obj_set_width(b, lv_pct(100));
	/* a long label takes a second line instead of being cut off */
	lv_obj_set_height(b, LV_SIZE_CONTENT); lv_obj_set_style_min_height(b, 44, 0);
	lv_obj_set_width(l, lv_pct(100)); lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
	lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
	return b;
}
static lv_obj_t *grey(lv_obj_t *b) { lv_obj_set_style_bg_color(b, lv_color_hex(0x48505a), 0); return b; }
static lv_obj_t *red(lv_obj_t *b) { lv_obj_set_style_bg_color(b, lv_color_hex(0xb03a3a), 0); return b; }

static lv_obj_t *row(lv_obj_t *parent)
{
	lv_obj_t *r = lv_obj_create(parent);
	lv_obj_remove_style_all(r);
	lv_obj_set_size(r, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW_WRAP);
	lv_obj_set_style_pad_column(r, 8, 0); lv_obj_set_style_pad_row(r, 8, 0);
	return r;
}

/* A list that fills the screen, with a strip beside it for moving it: page up, page down, and
   in between a place to drag that cannot choose an entry by accident (on this kind of touch
   screen a drag across the entries easily ends as a tap). */
static void strip_drag(lv_event_t *e)
{
	lv_point_t v; lv_indev_t *in = lv_indev_get_act();
	if (!in) return;
	lv_indev_get_vect(in, &v);
	lv_obj_scroll_by_bounded((lv_obj_t *)lv_event_get_user_data(e), 0, v.y, LV_ANIM_OFF);
}
static void strip_page(lv_event_t *e)
{
	lv_obj_t *list = lv_event_get_user_data(e); int up = lv_obj_get_index(lv_event_get_target(e)) == 0;
	lv_obj_scroll_by_bounded(list, 0, (up ? 1 : -1) * (lv_obj_get_height(list) - 50), LV_ANIM_ON);
}
static lv_obj_t *scroll_list(void)
{
	lv_obj_t *list, *strip, *b, *l; int i;
	lv_obj_set_style_pad_all(content, 0, 0);
	lv_obj_set_flex_flow(content, LV_FLEX_FLOW_ROW);
	list = lv_list_create(content); lv_obj_set_height(list, lv_pct(100)); lv_obj_set_flex_grow(list, 1);
	lv_obj_set_style_radius(list, 0, 0);
	strip = lv_obj_create(content); lv_obj_remove_style_all(strip);
	lv_obj_set_size(strip, 26, lv_pct(100));
	lv_obj_set_style_bg_color(strip, lv_color_hex(0x1c232c), 0); lv_obj_set_style_bg_opa(strip, LV_OPA_COVER, 0);
	lv_obj_set_flex_flow(strip, LV_FLEX_FLOW_COLUMN);
	for (i = 0; i < 3; i++) {
		if (i == 1) {		/* the place to drag */
			b = lv_obj_create(strip); lv_obj_remove_style_all(b);
			lv_obj_set_width(b, lv_pct(100)); lv_obj_set_flex_grow(b, 1);
			lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE); lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
			lv_obj_add_event_cb(b, strip_drag, LV_EVENT_PRESSING, list);
			l = dim(lv_label_create(b)); lv_label_set_text(l, LV_SYMBOL_UP "\n\n" LV_SYMBOL_DOWN); lv_obj_center(l);
			lv_obj_set_style_text_opa(l, LV_OPA_40, 0);
			continue;
		}
		b = lv_btn_create(strip); lv_obj_set_size(b, lv_pct(100), 64);
		lv_obj_set_style_radius(b, 0, 0); lv_obj_set_style_bg_color(b, lv_color_hex(0x38424e), 0); lv_obj_set_style_shadow_width(b, 0, 0);
		l = lv_label_create(b); lv_label_set_text(l, i ? LV_SYMBOL_DOWN : LV_SYMBOL_UP); lv_obj_center(l);
		lv_obj_add_event_cb(b, strip_page, LV_EVENT_CLICKED, list);
	}
	return list;
}

/* a list entry with a second, dimmer line; nothing in it scrolls sideways */
static lv_obj_t *entry(lv_obj_t *list, const char *line1, const char *line2, lv_event_cb_t cb, void *arg)
{
	lv_obj_t *b = lv_list_add_btn(list, NULL, NULL), *l;
	lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_ver(b, 8, 0); lv_obj_set_style_pad_row(b, 2, 0);
	l = lv_label_create(b); lv_obj_set_width(l, lv_pct(100)); lv_label_set_long_mode(l, LV_LABEL_LONG_DOT); lv_label_set_text(l, line1);
	if (line2 && *line2) { l = dim(lv_label_create(b)); lv_obj_set_width(l, lv_pct(100)); lv_label_set_long_mode(l, LV_LABEL_LONG_DOT); lv_label_set_text(l, line2); }
	/* a short tap only: a finger that rests or drags does not choose */
	if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_SHORT_CLICKED, arg);
	return b;
}

/* a box over everything: a question, a message, or "one moment" */
static lv_obj_t *modal_box(void)
{
	lv_obj_t *box;
	modal_close();
	modal = lv_obj_create(lv_layer_top());
	lv_obj_remove_style_all(modal);
	lv_obj_set_size(modal, W, H);
	lv_obj_set_style_bg_color(modal, lv_color_hex(0x000000), 0); lv_obj_set_style_bg_opa(modal, LV_OPA_70, 0);
	lv_obj_add_flag(modal, LV_OBJ_FLAG_CLICKABLE);		/* nothing underneath can be touched */
	box = lv_obj_create(modal);
	lv_obj_set_size(box, W - (W > 400 ? 40 : 16), LV_SIZE_CONTENT);
	lv_obj_set_style_max_height(box, H - 16, 0);
	lv_obj_center(box);
	lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(box, 10, 0);
	return box;
}
static void show_cb(lv_event_t *e);
static lv_obj_t *wide(lv_obj_t *b);
/* a command that never answers must not hold the screen for good */
static void busy_late(lv_timer_t *t)
{
	lv_obj_t *box; int i;
	lv_timer_del(t); busy_timer = NULL;
	for (i = 0; i < 6; i++) if (procs[i].pid && procs[i].gen == gen) { kill(procs[i].pid, SIGTERM); procs[i].fn = NULL; }
	box = modal_box();
	text(box, "No response from the board's command.");
	wide(button(box, "OK", show_cb, NULL));
}
static void busy(const char *what)
{
	lv_obj_t *box = modal_box(), *s;
	busy_timer = lv_timer_create(busy_late, 180000, NULL);
	lv_obj_set_flex_flow(box, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	s = lv_spinner_create(box, 1000, 60); lv_obj_set_size(s, 36, 36);
	lv_obj_set_style_arc_width(s, 5, 0); lv_obj_set_style_arc_width(s, 5, LV_PART_INDICATOR);
	lv_obj_set_flex_grow(text(box, what), 1);
}
static void modal_close_cb(lv_event_t *e) { (void)e; modal_close(); }
static void show_cb(lv_event_t *e) { (void)e; show(); }
/* a message; OK draws the screen under it again, so it shows what is true now */
static void message(const char *head, const char *body)
{
	lv_obj_t *box = modal_box(), *h = text(box, head);
	lv_obj_set_style_text_font(h, &lv_font_montserrat_18, 0);
	if (body && *body) text(box, body);
	wide(button(box, "OK", show_cb, NULL));
}
/* a remark that leaves the screen under it as it is */
static void remark(const char *head, const char *body)
{
	lv_obj_t *box = modal_box(), *h = text(box, head);
	lv_obj_set_style_text_font(h, &lv_font_montserrat_18, 0);
	if (body && *body) text(box, body);
	wide(button(box, "OK", modal_close_cb, NULL));
}
static void (*yes_fn)(void);
static void yes_cb(lv_event_t *e) { void (*f)(void) = yes_fn; (void)e; modal_close(); if (f) f(); }
static void ask2(const char *head, const char *body, const char *no, const char *yes, int danger, void (*fn)(void))
{
	lv_obj_t *box = modal_box(), *h = text(box, head), *r, *b;
	lv_obj_set_style_text_font(h, &lv_font_montserrat_18, 0);
	if (body && *body) text(box, body);
	yes_fn = fn;
	r = row(box);
	b = grey(button(r, no, modal_close_cb, NULL)); lv_obj_set_flex_grow(b, 1);
	b = button(r, yes, yes_cb, NULL); lv_obj_set_flex_grow(b, 1);
	if (danger) red(b);
}
static void ask(const char *head, const char *body, const char *yes, int danger, void (*fn)(void)) { ask2(head, body, "Cancel", yes, danger, fn); }

/* the pairing code, top right on every screen */
static void code_tick(lv_timer_t *t)
{
	char *c = run_file("nocry-pair-code"), s[32];
	(void)t;
	c[strcspn(c, "\r\n")] = 0;
	if (strlen(c) == 6) snprintf(s, sizeof s, "code  %.3s %s", c, c + 3); else s[0] = 0;
	if (W < 400) s[0] = 0;		/* no room in the bar: the home screen shows it */
	if (strcmp(lv_label_get_text(code_lbl), s)) lv_label_set_text(code_lbl, s);
	free(c);
	if (want_calibration) {
		extern void calibrate_screen(void);
		want_calibration = 0;
		if (stack[depth - 1] != calibrate_screen) go(calibrate_screen);
	}
}

/* ---- a job: something long that runs on the board, with progress and a way to stop -- */
static lv_obj_t *job_what, *job_bar, *job_msg, *job_btns;
static char job_risk[400]; static int job_over, job_home, job_poll, job_stopping; static screen_fn job_next;
static const char *job_undo;	/* a nocry-rescue-do verb to run if the job fails or is cancelled */

static void job_cancel_yes(void) { job_stopping = 1; start(NULL, NULL, "nocry-rescue-do", "cancel", NULL); lv_obj_clean(job_btns); }
static void job_cancel_cb(lv_event_t *e)
{
	(void)e;
	ask2("Cancel this job?", job_risk[0] ? job_risk : "It stops at the next safe point.", "Keep running", "Cancel job", 1, job_cancel_yes);
}
static int usb_used;		/* the flash drive was used by what is going on: ask about ejecting it at the end */
static int job_pop;		/* screens under the job screen that a finished job leaves as well */
static void eject_prompt(void);
static void job_done_cb(lv_event_t *e)
{
	screen_fn n = job_next; int h = job_home, ok = lv_event_get_user_data(e) != NULL, u = usb_used, pop = job_pop;
	job_next = NULL; job_home = 0; job_undo = NULL; job_pop = 0;
	if (!(ok && n)) usb_used = 0;	/* the last step, or it ended here */
	depth--;			/* the job screen itself */
	if (ok && depth > pop) depth -= pop;
	if (ok && n) { depth = 1; go(n); }
	else if (ok && h) home();	/* the screen that started it has nothing left to show */
	else show();			/* failed or cancelled: back where it was started, to try again */
	if (ok && !n && u) eject_prompt();
}
/* How fast the step that is running goes, and how long it still takes at that speed: from the
   progress of the last twenty seconds, so it follows a drive that slows down.  A new step
   (another total, or the count starting over) starts the measuring over. */
static struct { uint32_t t; long long done; } eta_s[20]; static int eta_n; static long long eta_total;
static const char *eta(long long done, long long total)
{
	static char out[80]; uint32_t now = nocry_ui_ms(); double rate, left;
	out[0] = 0;
	if (total != eta_total || (eta_n && done < eta_s[eta_n - 1].done)) { eta_n = 0; eta_total = total; }
	if (eta_n == 20) { memmove(eta_s, eta_s + 1, sizeof eta_s[0] * 19); eta_n--; }
	eta_s[eta_n].t = now; eta_s[eta_n].done = done; eta_n++;
	if (eta_n < 4 || now - eta_s[0].t < 3000 || done <= eta_s[0].done || done >= total) return out;
	rate = (double)(done - eta_s[0].done) * 1000 / (now - eta_s[0].t);	/* bytes a second */
	left = (total - done) / rate;
	if (left < 50) snprintf(out, sizeof out, "%.1f MiB/s, under a minute left", rate / 1048576);
	else if (left < 5400) snprintf(out, sizeof out, "%.1f MiB/s, about %d min left", rate / 1048576, (int)((left + 30) / 60));
	else snprintf(out, sizeof out, "%.1f MiB/s, about %d h %d min left", rate / 1048576, (int)(left / 3600), (int)(left / 60) % 60);
	return out;
}
static void job_answer(int rc, char *out)
{
	const char *st = kv(out, "state"); long long done = atoll(kv(out, "done")), total = atoll(kv(out, "total")); char line[900];
	job_poll = 0;
	if (job_over) return;
	/* only a clear answer ends it: a poll that failed or says nothing is asked again */
	if (rc || (strcmp(st, "running") && strcmp(st, "done") && strcmp(st, "failed") && strcmp(st, "cancelled"))) return;
	lv_label_set_text(job_what, kv(out, "what"));
	snprintf(job_risk, sizeof job_risk, "%s", kv(out, "risk"));
	if (total > 1) {
		lv_obj_clear_flag(job_bar, LV_OBJ_FLAG_HIDDEN);
		lv_bar_set_value(job_bar, (int)(done * 1000 / total), LV_ANIM_OFF);
		const char *e = strcmp(st, "running") ? "" : eta(done, total);
		snprintf(line, sizeof line, "%s of %s\n%s%s%s", amount(done), amount(total), kv(out, "message"), *e ? "\n" : "", e);
	} else {	/* a phase without a measure: no bar that looks stuck */
		lv_obj_add_flag(job_bar, LV_OBJ_FLAG_HIDDEN);
		snprintf(line, sizeof line, "%s", kv(out, "message"));
	}
	if (!strcmp(st, "running")) {
		if (job_stopping) { size_t l = strlen(line); snprintf(line + l, sizeof line - l, "\ncancelling at the next safe point..."); }
		else if (!strcmp(kv(out, "stoppable"), "no") && lv_obj_get_child_cnt(job_btns)) lv_obj_clean(job_btns);
		lv_label_set_text(job_msg, line);
		return;
	}
	job_over = 1;
	modal_close();			/* a question that is still open has nothing left to ask about */
	lv_obj_clean(job_btns);
	if (!strcmp(st, "done")) {
		lv_obj_clear_flag(job_bar, LV_OBJ_FLAG_HIDDEN);
		lv_bar_set_value(job_bar, 1000, LV_ANIM_OFF);
		snprintf(line, sizeof line, "Completed.  %s", kv(out, "message"));
		wide(button(job_btns, job_next ? "Continue" : "OK", job_done_cb, (void *)1));
	} else {
		snprintf(line, sizeof line, "%s  %s", !strcmp(st, "cancelled") ? "Cancelled." : "Failed.", kv(out, "message"));
		if (job_risk[0]) { size_t l = strlen(line); snprintf(line + l, sizeof line - l, "\n%s", job_risk); }
		if (job_undo) start(NULL, NULL, "nocry-rescue-do", job_undo, NULL);
		wide(button(job_btns, "OK", job_done_cb, NULL));
	}
	lv_label_set_text(job_msg, line);
}
static void job_tick(lv_timer_t *t)
{
	(void)t;
	if (job_over || job_poll) return;	/* one question at a time */
	job_poll = 1;
	start(job_answer, NULL, "nocry-rescue-do", "job", NULL);
}
static void job_screen(void)
{
	set_title("Working");
	lv_obj_add_flag(back_btn, LV_OBJ_FLAG_HIDDEN);		/* it runs whether anyone watches or not */
	job_over = 0; job_poll = 0; job_stopping = 0; job_risk[0] = 0; eta_n = 0;
	job_what = text(content, "Starting...");
	lv_obj_set_style_text_font(job_what, &lv_font_montserrat_18, 0);
	job_bar = lv_bar_create(content); lv_obj_set_size(job_bar, lv_pct(100), 18); lv_bar_set_range(job_bar, 0, 1000);
	job_msg = text(content, "");
	job_btns = row(content);
	wide(grey(button(job_btns, "Cancel job", job_cancel_cb, NULL)));
	screen_timer = lv_timer_create(job_tick, 1000, NULL);
	job_tick(NULL);
}
/* the answer of a command that starts a job: watch it, or say why it did not start */
static void job_started(int rc, char *out)
{
	modal_close();
	if (rc) { job_next = NULL; job_home = 0; job_undo = NULL; job_pop = 0; usb_used = 0; message("Could not start", out); return; }
	go(job_screen);
}

/* ---- Wi-Fi --------------------------------------------------------------------------- */
static char nets[24][72]; static int net_lock[24], net_count; static char chosen[72];
static lv_obj_t *pw_area;

static void wifi_done(int rc, char *out)
{
	char *st = run_file("nocry-wifi.state"), body[400]; const char *why = strrchr(out, '\n');
	modal_close();
	why = why ? why + 1 : out;		/* the command's own last line says why */
	if (!strncmp(why, "nocry-wifi: ", 12)) why += 12;
	if (!rc && !strcmp(kv(st, "mode"), "client")) {
		snprintf(body, sizeof body, "Connected to \"%s\".\nWeb interface: http://%s", kv(st, "ssid"), kv(st, "address"));
		depth = 1; show(); message("Connected", body);
	} else if (rc == 2) remark("Not accepted", why);		/* refused before anything was tried: the typed text stays */
	else if (rc == 3) show();					/* the owner cancelled it: nothing to explain */
	else if (rc) {
		snprintf(body, sizeof body, "%s%s%s", why, *why ? "\n" : "", !strcmp(kv(st, "mode"), "ap") ? "The board is in access point mode." : "");
		remark("Connection failed", body);
	} else if (!strcmp(kv(st, "mode"), "off")) { while (depth > 2) depth--; show(); }	/* the Wi-Fi screen says it is off */
	else { depth = 1; show(); }
	free(st);
}
/* a fixed address for the network being joined (no DHCP server there); ip_on says it is wanted */
static char ip_addr[20], ip_mask[20] = "255.255.255.0", ip_gw[20], ip_dns[20] = "8.8.8.8", join_pw[80]; static int ip_on;
static void wifi_join(const char *pw)
{
	char what[160];
	snprintf(what, sizeof what, "Connecting to \"%s\"...\nThis can take up to a minute.", chosen);
	busy(what);
	if (ip_on) start(wifi_done, NULL, "nocry-wifi", "join", chosen, pw ? pw : "", "static", ip_addr, ip_mask, ip_gw, ip_dns, NULL);
	else if (pw && *pw) start(wifi_done, NULL, "nocry-wifi", "join", chosen, pw, NULL);
	else start(wifi_done, NULL, "nocry-wifi", "join", chosen, NULL);
}

/* A screen of a few labelled one-line fields over a keyboard: the field touched last is the
   one the keyboard types into. */
static lv_obj_t *form_kb, *form_f[4], *form_cells; static int form_n;
static void form_pick(lv_obj_t *ta)
{
	int i;
	for (i = 0; i < form_n; i++) if (form_f[i] == ta) lv_obj_add_state(form_f[i], LV_STATE_FOCUSED); else lv_obj_clear_state(form_f[i], LV_STATE_FOCUSED);
	lv_keyboard_set_textarea(form_kb, ta);
}
static void form_touch(lv_event_t *e) { form_pick(lv_event_get_target(e)); }
static void form_begin(void)
{
	form_n = 0;
	lv_obj_set_style_pad_all(content, 4, 0); lv_obj_set_style_pad_row(content, 4, 0);
	form_cells = row(content);
	lv_obj_set_style_pad_column(form_cells, 6, 0); lv_obj_set_style_pad_row(form_cells, 4, 0);
}
static lv_obj_t *form_field(const char *label, const char *value, const char *hint, int max, int per_row)
{
	lv_obj_t *cell = lv_obj_create(form_cells), *l, *ta;
	lv_obj_remove_style_all(cell);
	lv_obj_set_size(cell, W < 400 ? W - 8 : (W - 8 - 6 * (per_row - 1)) / per_row, 38);
	lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(cell, 4, 0);
	l = dim(lv_label_create(cell)); lv_label_set_text(l, label); lv_obj_set_width(l, 76);
	ta = lv_textarea_create(cell);
	lv_textarea_set_one_line(ta, true); lv_textarea_set_max_length(ta, max);
	lv_textarea_set_text(ta, value); if (hint) lv_textarea_set_placeholder_text(ta, hint);
	lv_obj_set_flex_grow(ta, 1); lv_obj_set_height(ta, 36); lv_obj_set_style_pad_ver(ta, 6, 0);
	lv_obj_add_event_cb(ta, form_touch, LV_EVENT_PRESSED, NULL);
	if (form_n < 4) form_f[form_n++] = ta;
	return ta;
}
static void keyboard_upright(lv_obj_t *kb);
/* digits and the dot only, for addresses */
static void keyboard_digits(lv_obj_t *kb)
{
	static const char *map[] = { "1","2","3","4","5","6","\n", "7","8","9","0",".",LV_SYMBOL_BACKSPACE,"\n", LV_SYMBOL_OK,"" };
	static const lv_btnmatrix_ctrl_t ctrl[] = { 1,1,1,1,1,1, 1,1,1,1,1, LV_KEYBOARD_CTRL_BTN_FLAGS | 1, LV_KEYBOARD_CTRL_BTN_FLAGS | 1 };
	lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_NUMBER, map, ctrl);
	lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_NUMBER);
}
static void form_end(int digits, int rows, lv_event_cb_t key)
{
	int used = 8 + rows * 42, h = H - BAR - used;
	form_kb = lv_keyboard_create(content);
	lv_obj_set_size(form_kb, lv_pct(100), h > 300 ? 300 : h);
	if (digits) keyboard_digits(form_kb); else if (W < 400) keyboard_upright(form_kb);
	lv_obj_add_event_cb(form_kb, key, LV_EVENT_READY, NULL);
	lv_obj_add_event_cb(form_kb, key, LV_EVENT_CANCEL, NULL);
	form_pick(form_f[0]);
}

/* the fixed address: asked for from the password screen, or for an open network before joining */
static void ip_key(lv_event_t *e)
{
	if (lv_event_get_code(e) == LV_EVENT_CANCEL) { back(); return; }
	snprintf(ip_addr, sizeof ip_addr, "%s", lv_textarea_get_text(form_f[0])); snprintf(ip_mask, sizeof ip_mask, "%s", lv_textarea_get_text(form_f[1]));
	snprintf(ip_gw, sizeof ip_gw, "%s", lv_textarea_get_text(form_f[2])); snprintf(ip_dns, sizeof ip_dns, "%s", lv_textarea_get_text(form_f[3]));
	if (!ip_addr[0] || !ip_mask[0]) { remark("Address missing", "Enter the IP address and the subnet mask. Gateway and DNS may stay empty."); return; }
	ip_on = 1;
	wifi_join(join_pw);
}
static void ip_screen(void)
{
	set_title(W < 400 ? "Static IP" : "Static IP settings");
	form_begin();
	form_field("IP address", ip_addr, "192.168.1.50", 15, 2);
	form_field("Subnet", ip_mask, NULL, 15, 2);
	form_field("Gateway", ip_gw, "optional", 15, 2);
	form_field("DNS", ip_dns, "optional", 15, 2);
	form_end(1, W < 400 ? 4 : 2, ip_key);
}
static int manual;		/* the SSID was typed, not picked from the scan */
/* is what is typed a password that can be used?  (an SSID typed by hand may be an open network: none then) */
static int pw_ok(void)
{
	size_t n = strlen(lv_textarea_get_text(pw_area));
	if ((manual && !n) || n >= 8) return 1;
	remark("Password too short", manual ? "WPA passwords are 8 to 63 characters. Leave it empty for an open network." : "WPA passwords are 8 to 63 characters.");
	return 0;
}
static void pw_key(lv_event_t *e)
{
	if (lv_event_get_code(e) == LV_EVENT_CANCEL) { back(); return; }
	if (!pw_ok()) return;
	snprintf(join_pw, sizeof join_pw, "%s", lv_textarea_get_text(pw_area));
	ip_on = 0;
	wifi_join(join_pw);
}
static void pw_ip_cb(lv_event_t *e)
{
	(void)e;
	if (!pw_ok()) return;
	snprintf(join_pw, sizeof join_pw, "%s", lv_textarea_get_text(pw_area));
	go(ip_screen);
}
/* An upright screen is 272 wide: ten keys in a row is the most a finger can hit, and they are
   made tall.  Everything that can be in a password is on the letters and the two "1#" pages. */
static void keyboard_upright(lv_obj_t *kb)
{
	static const char *lower[] = { "q","w","e","r","t","y","u","i","o","p","\n", "a","s","d","f","g","h","j","k","l","\n",
		"ABC","z","x","c","v","b","n","m",LV_SYMBOL_BACKSPACE,"\n", "1#"," ",LV_SYMBOL_OK,"" };
	static const char *upper[] = { "Q","W","E","R","T","Y","U","I","O","P","\n", "A","S","D","F","G","H","J","K","L","\n",
		"abc","Z","X","C","V","B","N","M",LV_SYMBOL_BACKSPACE,"\n", "1#"," ",LV_SYMBOL_OK,"" };
	static const char *special[] = { "1","2","3","4","5","6","7","8","9","0","\n", "!","@","#","$","%","^","&","*","(",")","\n",
		"-","_","=","+","[","]","{","}","\\","|","\n", ";",":","'","\"",",",".","<",">","/","?","\n",
		"abc","`","~"," ",LV_SYMBOL_BACKSPACE,LV_SYMBOL_OK,"" };
	#define K LV_BTNMATRIX_CTRL_POPOVER | 2
	#define F LV_KEYBOARD_CTRL_BTN_FLAGS | 3
	static const lv_btnmatrix_ctrl_t letters[] = { K,K,K,K,K,K,K,K,K,K, K,K,K,K,K,K,K,K,K, F,K,K,K,K,K,K,K,F, F | 1, 12, F | 1 };
	static const lv_btnmatrix_ctrl_t signs[] = { K,K,K,K,K,K,K,K,K,K, K,K,K,K,K,K,K,K,K,K, K,K,K,K,K,K,K,K,K,K, K,K,K,K,K,K,K,K,K,K, F,K,K,6,F,F };
	#undef K
	#undef F
	lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_TEXT_LOWER, lower, letters);
	lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_TEXT_UPPER, upper, letters);
	lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_SPECIAL, special, signs);
	lv_obj_set_style_pad_all(kb, 2, 0); lv_obj_set_style_pad_gap(kb, 3, 0);
}
static void password_screen(void)
{
	lv_obj_t *kb; char t[100];
	set_title(W < 400 ? "Password" : (snprintf(t, sizeof t, "Password for %s", chosen), t));
	lv_obj_set_style_pad_all(content, 4, 0);
	{	/* the password, and beside it the way to a fixed address (a network without DHCP) */
		lv_obj_t *r = row(content), *b;
		lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW); lv_obj_set_style_pad_column(r, 4, 0);
		pw_area = lv_textarea_create(r);
		lv_obj_set_flex_grow(pw_area, 1);
		b = grey(button(r, "Static IP", pw_ip_cb, NULL)); lv_obj_set_height(b, 40); lv_obj_set_style_pad_hor(b, 8, 0);
	}
	lv_textarea_set_one_line(pw_area, true); lv_textarea_set_max_length(pw_area, 63);
	if (manual) snprintf(t, sizeof t, W < 400 ? "password (empty: open)" : "password (empty: open network)");
	else snprintf(t, sizeof t, "password for %s", chosen);
	lv_textarea_set_placeholder_text(pw_area, t);
	kb = lv_keyboard_create(content);
	lv_obj_set_size(kb, lv_pct(100), H - BAR - 52 > 300 ? 300 : H - BAR - 52);
	if (W < 400) keyboard_upright(kb);
	lv_keyboard_set_textarea(kb, pw_area);
	lv_obj_add_event_cb(kb, pw_key, LV_EVENT_READY, NULL);
	lv_obj_add_event_cb(kb, pw_key, LV_EVENT_CANCEL, NULL);
}
/* a network that is not in the list (hidden, or not seen by this scan): type its SSID */
static void ssid_key(lv_event_t *e)
{
	const char *t = lv_textarea_get_text(pw_area);
	if (lv_event_get_code(e) == LV_EVENT_CANCEL) { back(); return; }
	if (!*t) { remark("SSID missing", "Enter the network's SSID."); return; }
	snprintf(chosen, sizeof chosen, "%s", t);
	manual = 1;
	go(password_screen);
}
static void ssid_screen(void)
{
	lv_obj_t *kb;
	set_title("Enter SSID");
	lv_obj_set_style_pad_all(content, 4, 0);
	pw_area = lv_textarea_create(content);
	lv_textarea_set_one_line(pw_area, true); lv_textarea_set_max_length(pw_area, 32);
	lv_textarea_set_placeholder_text(pw_area, "SSID (case-sensitive)");
	lv_obj_set_width(pw_area, lv_pct(100));
	kb = lv_keyboard_create(content);
	lv_obj_set_size(kb, lv_pct(100), H - BAR - 52 > 300 ? 300 : H - BAR - 52);
	if (W < 400) keyboard_upright(kb);
	lv_keyboard_set_textarea(kb, pw_area);
	lv_obj_add_event_cb(kb, ssid_key, LV_EVENT_READY, NULL);
	lv_obj_add_event_cb(kb, ssid_key, LV_EVENT_CANCEL, NULL);
}
static void ssid_cb(lv_event_t *e) { (void)e; go(ssid_screen); }
static void open_join_cb(lv_event_t *e) { (void)e; modal_close(); ip_on = 0; join_pw[0] = 0; wifi_join(NULL); }
static void open_ip_cb(lv_event_t *e) { (void)e; modal_close(); join_pw[0] = 0; go(ip_screen); }
static void net_cb(lv_event_t *e)
{
	int i = (int)(intptr_t)lv_event_get_user_data(e);
	manual = 0;
	snprintf(chosen, sizeof chosen, "%s", nets[i]);
	if (net_lock[i]) { go(password_screen); return; }
	{	/* an open network: join it, or set a fixed address first */
		lv_obj_t *box = modal_box(), *h; char t[120];
		snprintf(t, sizeof t, "Connect to \"%s\"?", chosen);
		h = text(box, t); lv_obj_set_style_text_font(h, &lv_font_montserrat_18, 0);
		text(box, "Open network: no password.");
		wide(button(box, "Connect", open_join_cb, NULL));
		wide(grey(button(box, "Static IP...", open_ip_cb, NULL)));
		wide(grey(button(box, "Cancel", modal_close_cb, NULL)));
	}
}
/* push-button WPS: the router gives the board its name and password after its button was pressed */
static lv_obj_t *wps_text, *wps_stop;
static void wps_cancel_cb(lv_event_t *e)
{
	(void)e;
	start(NULL, NULL, "nocry-wifi", "wps-cancel", NULL);	/* the waiting command ends within a second and answers */
	lv_label_set_text(wps_text, "Cancelling...\nReturning to access point mode.");
	lv_obj_add_flag(wps_stop, LV_OBJ_FLAG_HIDDEN);
}
static void wps_yes(void)
{
	lv_obj_t *box, *r, *s;
	snprintf(chosen, sizeof chosen, "WPS");
	box = modal_box();		/* like busy(), with a way out; the command has its own two-minute limit */
	r = row(box); lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(r, 12, 0);
	s = lv_spinner_create(r, 1000, 60); lv_obj_set_size(s, 36, 36);
	lv_obj_set_style_arc_width(s, 5, 0); lv_obj_set_style_arc_width(s, 5, LV_PART_INDICATOR);
	wps_text = text(r, "Waiting for the router's WPS button...\nUp to two minutes.");
	lv_obj_set_width(wps_text, LV_SIZE_CONTENT); lv_obj_set_flex_grow(wps_text, 1);
	wps_stop = wide(grey(button(box, "Cancel", wps_cancel_cb, NULL)));
	start(wifi_done, NULL, "nocry-wifi", "wps", NULL);
}
static void wps_cb(lv_event_t *e)
{
	(void)e;
	ask("Connect with WPS?", "Press the WPS button on the router first, then tap Start. The board joins the router that offers WPS within two minutes (2.4 GHz, WPA2). If none does, it returns to access point mode.", "Start", 0, wps_yes);
}
static void scan_done(int rc, char *out)
{
	char *line, *save = NULL; lv_obj_t *list;
	modal_close(); net_count = 0;
	lv_obj_clean(content);
	if (rc || !*out) {
		text(content, "No networks found. Go back and scan again, or enter the SSID.");
		wide(button(content, "Other network...", ssid_cb, NULL));
		wide(grey(button(content, "WPS (router button)", wps_cb, NULL)));
		return;
	}
	list = scroll_list();
	entry(list, "WPS (router button)", "no password to type: press WPS on the router", wps_cb, NULL);
	for (line = strtok_r(out, "\n", &save); line && net_count < 24; line = strtok_r(NULL, "\n", &save)) {
		char *lock = strchr(line, '\t'), *name = lock ? strchr(lock + 1, '\t') : NULL, label[60];
		if (!name || !name[1]) continue;
		*lock = 0; *name++ = 0;
		snprintf(nets[net_count], sizeof nets[0], "%s", name);
		net_lock[net_count] = lock[1] && lock[1] != '-' && lock[1] != '0' && strcmp(lock + 1, "open");
		snprintf(label, sizeof label, "%s dBm   %s", line, net_lock[net_count] ? "secured" : "open");
		entry(list, name, label, net_cb, (void *)(intptr_t)net_count);
		net_count++;
	}
	entry(list, "Other network...", "enter the SSID manually", ssid_cb, NULL);
}
static void scan_screen(void)
{
	set_title("Select network");
	busy("Scanning...");
	start(scan_done, NULL, "nocry-wifi", "scan", NULL);
}
static void scan_cb(lv_event_t *e) { (void)e; go(scan_screen); }
/* the board's own network: its name and password can be changed; an empty password makes it open */
static void ap_start(int rc, char *out) { (void)rc; (void)out; start(wifi_done, NULL, "nocry-wifi", "ap", NULL); }
static void ap_key(lv_event_t *e)
{
	const char *n = lv_textarea_get_text(form_f[0]), *p = lv_textarea_get_text(form_f[1]); size_t l = strlen(p);
	if (lv_event_get_code(e) == LV_EVENT_CANCEL) { back(); return; }
	if (!*n) { remark("SSID missing", "Enter a name for the access point."); return; }
	if (l && (l < 8 || strchr(p, '"'))) { remark("Password not usable", "WPA2 passwords are 8 to 63 characters, without a double quote. Leave it empty for an open access point."); return; }
	snprintf(ap_ssid, sizeof ap_ssid, "%s", n); snprintf(ap_pass, sizeof ap_pass, "%s", p); ap_own = 1;
	busy("Starting the access point...");
	settings_store(ap_start);		/* kept, then started with them */
}
static void ap_screen(void)
{
	settings_refresh();
	set_title(W < 400 ? "Access point" : "Access point settings");
	form_begin();
	form_field("SSID", ap_own ? ap_ssid : "nocry", NULL, 32, 2);
	form_field("Password", ap_own ? ap_pass : "nocry111", "empty: open", 63, 2);
	form_end(0, W < 400 ? 2 : 1, ap_key);
}
static void ap_cb(lv_event_t *e) { (void)e; go(ap_screen); }
static void off_yes(void) { busy("Turning Wi-Fi off..."); start(wifi_done, NULL, "nocry-wifi", "off", NULL); }
static void disconnect_cb(lv_event_t *e)
{
	char *st = run_file("nocry-wifi.state"), h[120];
	(void)e;
	snprintf(h, sizeof h, "Disconnect from \"%s\"?", kv(st, "ssid"));
	ask(h, "Wi-Fi stays off until a network is chosen, the access point is started, or the board restarts. The web interface and nocry over the network stop responding.", "Disconnect", 0, off_yes);
	free(st);
}
static void ap_off_cb(lv_event_t *e)
{
	(void)e;
	ask("Turn the access point off?", "Wi-Fi stays off until a network is chosen, the access point is started, or the board restarts. The web interface and nocry over the network stop responding.", "Turn off", 0, off_yes);
}

/* The board's own network as a QR code a phone's camera joins it from. */
static void qr_text(char *out, size_t n, const char *ssid, const char *pass)
{
	const char *src[2] = { ssid, pass }; size_t o; int k;
	o = snprintf(out, n, "WIFI:T:%s;S:", *pass ? "WPA" : "nopass");
	for (k = 0; k < 2; k++) {
		const char *c;
		if (k) { if (!*pass) break; o += snprintf(out + o, n - o, ";P:"); }
		for (c = src[k]; *c && o + 4 < n; c++) { if (strchr("\\;,:\"", *c)) out[o++] = '\\'; out[o++] = *c; }
		out[o] = 0;
	}
	snprintf(out + o, n - o, ";;");
}
static lv_obj_t *qr_make(lv_obj_t *parent, int size)
{
	char *st = run_file("nocry-wifi.state"), data[260]; lv_obj_t *q;
	qr_text(data, sizeof data, kv(st, "ssid"), kv(st, "password"));
	q = lv_qrcode_create(parent, size, lv_color_hex(0x000000), lv_color_hex(0xffffff));
	lv_qrcode_update(q, data, strlen(data));
	lv_obj_set_style_border_color(q, lv_color_hex(0xffffff), 0); lv_obj_set_style_border_width(q, size > 100 ? 8 : 2, 0);
	free(st);
	return q;
}
static void qr_screen(void)
{
	char *st = run_file("nocry-wifi.state"), s[300]; int size = W < 400 ? W - 56 : H - BAR - 32; lv_obj_t *t;
	set_title(W < 400 ? "Wi-Fi QR code" : "Join with a phone");
	lv_obj_set_flex_flow(content, W < 400 ? LV_FLEX_FLOW_COLUMN : LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(content, 14, 0);
	qr_make(content, size);
	snprintf(s, sizeof s, "Scan with the phone's camera to join.\n\nSSID: %s\n%s%s\n\nThen open\nhttp://%s", kv(st, "ssid"), *kv(st, "password") ? "Password: " : "Open network", kv(st, "password"), kv(st, "address"));
	t = text(content, s);
	if (W >= 400) { lv_obj_set_width(t, LV_SIZE_CONTENT); lv_obj_set_flex_grow(t, 1); }
	free(st);
}
static void qr_cb(lv_event_t *e) { (void)e; go(qr_screen); }
static void save_done(int rc, char *out) { modal_close(); message(rc ? "Not saved" : "Saved", rc ? out : "Written to the firmware's wpa_supplicant.conf on userdata. Networks saved there before are replaced."); }
static void save_yes(void) { busy("Saving..."); start(save_done, NULL, "nocry-rescue-do", "wifi-save", NULL); }
static void save_cb(lv_event_t *e) { (void)e; ask("Save credentials to firmware?", "Writes this network to the firmware's wpa_supplicant.conf on userdata, replacing the networks saved there.", "Save", 0, save_yes); }
static char wifi_shown[16];
static void wifi_tick(lv_timer_t *t)	/* the mode changed under the screen (still connecting when it was opened) */
{
	char *st = run_file("nocry-wifi.state");
	(void)t;
	if (strcmp(kv(st, "mode"), wifi_shown) && !modal) show();
	free(st);
}
static void wifi_screen(void)
{
	char *st = run_file("nocry-wifi.state"), s[300]; const char *mode = kv(st, "mode");
	set_title("Wi-Fi");
	snprintf(wifi_shown, sizeof wifi_shown, "%s", mode);
	screen_timer = lv_timer_create(wifi_tick, 1000, NULL);
	if (!strcmp(mode, "client")) snprintf(s, sizeof s, "Connected to \"%s\"%s.\nWeb interface: http://%s", kv(st, "ssid"), !strcmp(kv(st, "addressing"), "static") ? " (static IP)" : "", kv(st, "address"));
	else if (!strcmp(mode, "ap")) snprintf(s, sizeof s, "Access point mode\nSSID: %s\n%s%s\nWeb interface: http://%s", kv(st, "ssid"), *kv(st, "password") ? "Password: " : "Open network", kv(st, "password"), kv(st, "address"));
	else if (!strcmp(mode, "joining")) snprintf(s, sizeof s, "Connecting to \"%s\"...", kv(st, "ssid"));
	else if (!strcmp(mode, "off")) snprintf(s, sizeof s, "Wi-Fi is off.");
	else snprintf(s, sizeof s, "Not connected.");
	if (!strcmp(mode, "ap")) {
		/* The status, the buttons, and the QR code a phone joins from, as large as there is room:
		   wide, the buttons go down the left and the code fills the right; upright, the code fills
		   what is left under the buttons.  Touching it opens the screen that explains it. */
		int wide_ = W >= 400, q = wide_ ? H - BAR - 16 : H - BAR - 16 - 54 - 3 * 48 - 6, i; lv_obj_t *left, *b, *bs[3];
		if (q > W - 16) q = W - 16;
		lv_obj_set_style_pad_row(content, 6, 0);
		if (wide_) {
			lv_obj_set_flex_flow(content, LV_FLEX_FLOW_ROW);
			lv_obj_set_style_pad_column(content, 8, 0);
			left = lv_obj_create(content); lv_obj_remove_style_all(left);
			lv_obj_set_size(left, W - 16 - 8 - q, q);
			lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN); lv_obj_set_style_pad_row(left, 6, 0);
		} else {
			left = content;
			lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
		}
		snprintf(s, sizeof s, "Access point: %s\n%s%s\nhttp://%s", kv(st, "ssid"), *kv(st, "password") ? "Password: " : "Open network", kv(st, "password"), kv(st, "address"));
		text(left, s);
		bs[0] = button(left, LV_SYMBOL_WIFI "  Connect to a network", scan_cb, NULL);
		bs[1] = grey(button(left, "Access point settings...", ap_cb, NULL));
		bs[2] = grey(button(left, "Turn access point off", ap_off_cb, NULL));
		for (i = 0; i < 3; i++) { lv_obj_set_width(bs[i], lv_pct(100)); lv_obj_set_height(bs[i], 42); }
		b = lv_btn_create(content); lv_obj_set_size(b, q, q); lv_obj_set_style_pad_all(b, 0, 0);
		lv_obj_set_style_bg_color(b, lv_color_hex(0xffffff), 0); lv_obj_set_style_shadow_width(b, 0, 0);
		lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
		b = qr_make(b, q - 4); lv_obj_center(b); lv_obj_clear_flag(b, LV_OBJ_FLAG_CLICKABLE);
		lv_obj_add_event_cb(lv_obj_get_parent(b), qr_cb, LV_EVENT_CLICKED, NULL);
		free(st);
		return;
	}
	text(content, s);
	{	/* the buttons: one under the other when upright, two by two when wide */
		lv_obj_t *r = row(content), *bs[4]; int n = 0, i; char d[80];
		bs[n++] = button(r, LV_SYMBOL_WIFI "  Connect to a network", scan_cb, NULL);
		if (!strcmp(mode, "client")) {
			snprintf(d, sizeof d, "Disconnect from %.40s", kv(st, "ssid"));
			bs[n++] = grey(button(r, d, disconnect_cb, NULL));
			bs[n++] = grey(button(r, "Access point mode...", ap_cb, NULL));
			bs[n++] = grey(button(r, "Save credentials to firmware", save_cb, NULL));
		} else if (!strcmp(mode, "ap")) {
			bs[n++] = grey(button(r, "Access point settings...", ap_cb, NULL));
			bs[n++] = grey(button(r, "Turn access point off", ap_off_cb, NULL));
		} else bs[n++] = grey(button(r, "Access point mode...", ap_cb, NULL));
		for (i = 0; i < n; i++) { wide(bs[i]); lv_obj_set_width(bs[i], W < 400 ? W - 16 : (W - 24) / 2); }
	}
	free(st);
}

/* ---- slots ---------------------------------------------------------------------------- */
static char slot_text[2][120], active_slot; static int slot_ok[2];
static int pick;	/* which slot a button was about */

static void switch_done(int rc, char *out) { modal_close(); if (rc) message("Active slot not changed", out); else show(); }
static void switch_yes(void) { busy("Setting the active slot..."); start(switch_done, NULL, "nocry-rescue-do", "switch", pick ? "b" : "a", NULL); }
static void switch_cb(lv_event_t *e)
{
	char q[200];
	pick = (int)(intptr_t)lv_event_get_user_data(e);
	snprintf(q, sizeof q, "%s will boot from now on. No data is changed; this can be reverted.", slot_text[pick]);
	ask(pick ? "Set slot B active?" : "Set slot A active?", q, "Set active", 0, switch_yes);
}
static void copy_yes(void) { job_next = NULL; busy("Starting..."); start(job_started, NULL, "nocry-rescue-do", "copy", pick ? "b" : "a", NULL); }
static void copy_cb(lv_event_t *e)
{
	char q[320];
	pick = (int)(intptr_t)lv_event_get_user_data(e);
	snprintf(q, sizeof q, "Slot %c (%s) will be overwritten with slot %c (%s).", pick ? 'A' : 'B', slot_text[!pick], pick ? 'B' : 'A', slot_text[pick]);
	ask(pick ? "Copy slot B to slot A?" : "Copy slot A to slot B?", q, "Overwrite", 1, copy_yes);
}
static void slots_done(int rc, char *out)
{
	int i;
	modal_close();
	if (rc) { text(content, out); return; }
	active_slot = kv(out, "active")[0];
	for (i = 0; i < 2; i++) {
		lv_obj_t *card = lv_obj_create(content), *h, *r; char s[160];
		snprintf(slot_text[i], sizeof slot_text[0], "%s", kv(out, i ? "b_firmware" : "a_firmware"));
		slot_ok[i] = !strcmp(kv(out, i ? "b_complete" : "a_complete"), "yes");
		lv_obj_set_size(card, lv_pct(100), LV_SIZE_CONTENT);
		lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN); lv_obj_set_style_pad_all(card, 8, 0); lv_obj_set_style_pad_row(card, 6, 0);
		snprintf(s, sizeof s, "Slot %c%s", 'A' + i, active_slot == 'a' + i ? "   (active)" : "");
		h = text(card, s); lv_obj_set_style_text_font(h, &lv_font_montserrat_18, 0);
		if (active_slot == 'a' + i) lv_obj_set_style_border_color(card, lv_palette_main(LV_PALETTE_BLUE), 0);
		text(card, slot_text[i]);
		r = row(card);
		if (slot_ok[i] && active_slot != 'a' + i) button(r, "Set active", switch_cb, (void *)(intptr_t)i);
		if (slot_ok[i]) grey(button(r, i ? "Copy to slot A" : "Copy to slot B", copy_cb, (void *)(intptr_t)i));
	}
}
static void slots_screen(void)
{
	set_title("Slots");
	busy("Reading the board...");
	start(slots_done, NULL, "nocry-rescue-do", "status", NULL);
}

/* ---- install firmware ------------------------------------------------------------------ */
static struct fw { char key[24], name[64], version[24], date[16]; long long bytes; } fws[40]; static int fw_count, fw_pick;
static char fw_mode[8];

static void install_yes(void) { job_next = NULL; job_home = 1; job_undo = NULL; busy("Starting..."); start(job_started, NULL, "nocry-rescue-do", "fw-install", NULL); }
static void install_cb(lv_event_t *e)
{
	(void)e;
	ask("Install now?", !strcmp(fw_mode, "slot") ? "The inactive slot will be overwritten. The active slot is not changed."
		: "Slot A will be overwritten and set active. userdata is preserved; see the notes for rootfs_data.", "Install", 1, install_yes);
}
static void discard_done(int rc, char *out) { (void)rc; (void)out; modal_close(); home(); }
static void discard_cb(lv_event_t *e) { (void)e; usb_used = 0; busy("One moment..."); start(discard_done, NULL, "nocry-rescue-do", "fw-discard", NULL); }
static void report_done(int rc, char *out)
{
	char s[400], *line, *save = NULL, *copy; lv_obj_t *r; const char *slot, *on;
	(void)rc;
	modal_close();
	if (!*kv(out, "firmware")) {	/* nothing verified is held: there must be a way out */
		text(content, "No pending install.");
		wide(button(content, "Back to home", discard_cb, NULL));
		return;
	}
	snprintf(fw_mode, sizeof fw_mode, "%s", kv(out, "mode"));
	snprintf(s, sizeof s, "%s", kv(out, "firmware"));
	lv_obj_set_style_text_font(text(content, s), &lv_font_montserrat_18, 0);
	slot = !strcmp(kv(out, "slot"), "a") ? "A" : !strcmp(kv(out, "slot"), "b") ? "B" : kv(out, "slot"); on = kv(out, "on_board");
	snprintf(s, sizeof s, "Installed now: %s\nTarget: %s%s\nTo write: %s", *on ? on : "unknown",
		*slot ? "slot " : "slot A (set active)", slot, amount(atoll(kv(out, "to_write"))));
	text(content, s);
	if (*kv(out, "converting")) { snprintf(s, sizeof s, "Changes the printer model: %s", kv(out, "converting")); text(content, s); }
	if (*kv(out, "need_choice")) {
		text(content, "The printer model must be selected first. Use nocry on a computer.");
		wide(grey(button(content, "Discard", discard_cb, NULL)));
		return;
	}
	copy = strdup(out);
	for (line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save))
		if (!strncmp(line, "note=", 5)) {
			char *h = strstr(line, " (--");		/* a hint for the command line means nothing here */
			if (h && strchr(h, ')')) memmove(h, strchr(h, ')') + 1, strlen(strchr(h, ')') + 1) + 1);
			dim(text(content, line + 5));
		}
	free(copy);
	if (!atoll(kv(out, "to_write"))) {
		text(content, "Nothing to write: this firmware is already installed there.");
		wide(grey(button(content, "Close", discard_cb, NULL)));
		return;
	}
	r = row(content);
	lv_obj_set_flex_grow(grey(button(r, "Discard", discard_cb, NULL)), 1);
	lv_obj_set_flex_grow(button(r, "Install", install_cb, NULL), 1);
}
static void report_screen(void)
{
	set_title("Review install");
	lv_obj_add_flag(back_btn, LV_OBJ_FLAG_HIDDEN);		/* install it or discard it */
	busy("One moment...");
	start(report_done, NULL, "nocry-rescue-do", "fw-report", NULL);
}
static char usb_fw[220];	/* a firmware file on the flash drive, chosen instead of one from the catalog; "" otherwise */
static void fetch(const char *mode)
{
	job_next = report_screen; job_home = 0;
	job_undo = "fw-discard";	/* a download that failed or was cancelled is not kept */
	busy("Starting...");
	if (usb_fw[0]) { usb_used = 1; start(job_started, NULL, "nocry-rescue-do", "fw-check-usb", usb_fw, mode, NULL); }
	else start(job_started, NULL, "nocry-rescue-do", "fw-fetch", fws[fw_pick].key, fws[fw_pick].version, mode, NULL);
}
static void keep_cb(lv_event_t *e) { (void)e; fetch("keep"); }
static void other_cb(lv_event_t *e) { (void)e; fetch("slot"); }
static void where_screen(void)
{
	char s[300]; struct fw *f = &fws[fw_pick];
	set_title("Target slot");
	if (usb_fw[0]) { const char *b = strrchr(usb_fw, '/'); snprintf(s, sizeof s, "%s\nFrom the flash drive", b ? b + 1 : usb_fw); }
	else snprintf(s, sizeof s, "%s %s\nDownload: %s", f->name, f->version, amount(f->bytes));
	text(content, s);
	wide(button(content, "Install to slot A, set active", keep_cb, NULL));
	dim(text(content, "Slot A is overwritten and becomes the active slot. userdata is preserved."));
	wide(grey(button(content, "Install to inactive slot", other_cb, NULL)));
	dim(text(content, "The active slot is not changed. Set the new slot active afterwards."));
	dim(text(content, usb_fw[0] ? "The file is verified first. Nothing is written until you confirm." : "The image is downloaded and verified first. Nothing is written until you confirm."));
}
static void fw_cb(lv_event_t *e) { fw_pick = (int)(intptr_t)lv_event_get_user_data(e); usb_fw[0] = 0; go(where_screen); }
/* the other source: the firmware files on a flash drive if one is mounted; if none is plugged in, say so */
static int usb_mounted(void);
static void usb_fw_screen(void);
static void usb_screen(void);
static void usb_instead_cb(lv_event_t *e)
{
	char *st = run_file("nocry-usb.state"); int disk = *kv(st, "disk") != 0;
	(void)e;
	free(st);
	if (usb_mounted()) go(usb_fw_screen);
	else if (disk) go(usb_screen);		/* a drive is there but not mounted: that screen says why, and can format it */
	else remark("No flash drive", "Insert a USB flash drive (FAT32, exFAT or NTFS) with the firmware file on it, then tap this again. The USB-A sockets are off while a computer is connected to the Micro-USB socket.");
}
static void fw_list_done(int rc, char *out);
static void refresh_cb(lv_event_t *e)
{
	(void)e;
	busy("Fetching the firmware catalog...");
	start(fw_list_done, NULL, "nocry-rescue-do", "fw-list", "refresh", NULL);
}
static void fw_list_done(int rc, char *out)
{
	char *line, *save = NULL; lv_obj_t *list;
	modal_close(); lv_obj_clean(content); fw_count = 0;
	if (rc || !*out) {
		text(content, rc && *out ? out : "No firmware is listed for this board's model. Refreshing the catalog needs internet access over Wi-Fi.");
		wide(button(content, LV_SYMBOL_REFRESH "  Fetch catalog", refresh_cb, NULL));
		wide(grey(button(content, LV_SYMBOL_USB "  Install from a flash drive", usb_instead_cb, NULL)));
		return;
	}
	list = scroll_list();
	entry(list, LV_SYMBOL_USB "  Install from a flash drive", "firmware files on the USB drive", usb_instead_cb, NULL);
	for (line = strtok_r(out, "\n", &save); line && fw_count < 40; line = strtok_r(NULL, "\n", &save)) {
		struct fw *f = &fws[fw_count]; char *c[6], l1[100], l2[100]; int n = 0;
		for (c[n++] = line; n < 6 && (line = strchr(line, '\t')); c[n++] = line) *line++ = 0;
		if (n < 5) continue;
		snprintf(f->key, sizeof f->key, "%s", c[0]); snprintf(f->name, sizeof f->name, "%s", c[1]);
		snprintf(f->version, sizeof f->version, "%s", c[2]); snprintf(f->date, sizeof f->date, "%s", c[3]); f->bytes = atoll(c[4]);
		snprintf(l1, sizeof l1, "%s %s", f->name, f->version);
		snprintf(l2, sizeof l2, "%s   %s", f->date, amount(f->bytes));
		entry(list, l1, l2, fw_cb, (void *)(intptr_t)fw_count);
		fw_count++;
	}
	entry(list, LV_SYMBOL_REFRESH "  Refresh catalog", "fetch the current list", refresh_cb, NULL);
}
/* a verified firmware still held from before (the interface was restarted, or the install failed) comes first */
static void pending_done(int rc, char *out)
{
	(void)rc;
	if (*kv(out, "firmware")) { modal_close(); stack[depth - 1] = report_screen; show(); return; }
	start(fw_list_done, NULL, "nocry-rescue-do", "fw-list", NULL);
}
static void install_screen(void)
{
	set_title(W < 400 ? "From internet" : "Install from internet");	/* the upright bar is short */
	busy("One moment...");
	start(pending_done, NULL, "nocry-rescue-do", "fw-report", NULL);
}

/* ---- USB flash drive ------------------------------------------------------------------- */
/* nocry-usb mounts a drive when one appears and says so in a file: no command needed to look */
static int usb_mounted(void) { char *st = run_file("nocry-usb.state"); int m = !strcmp(kv(st, "mounted"), "yes"); free(st); return m; }
static void eject_done(int rc, char *out)
{
	modal_close();
	if (rc) message("Not ejected", out); else message("Flash drive ejected", "It can be removed now.");
}
static void eject_now(void) { busy("Ejecting..."); start(eject_done, NULL, "nocry-rescue-do", "usb-eject", NULL); }
static void eject_cb(lv_event_t *e) { (void)e; eject_now(); }
static void eject_yes_cb(lv_event_t *e) { (void)e; modal_close(); eject_now(); }
static void eject_never_cb(lv_event_t *e) { (void)e; modal_close(); settings_refresh(); eject_ask = 0; settings_store(NULL); }
/* after the drive was used: eject it?  "Don't ask again" is kept with the other settings. */
static void eject_prompt(void)
{
	lv_obj_t *box, *h;
	if (!eject_ask || !usb_mounted()) return;
	box = modal_box(); h = text(box, "Eject the flash drive?");
	lv_obj_set_style_text_font(h, &lv_font_montserrat_18, 0);
	text(box, "Everything is written out and the drive is unmounted, so it can be removed.");
	wide(button(box, "Eject", eject_yes_cb, NULL));
	wide(grey(button(box, "Not now", modal_close_cb, NULL)));
	wide(grey(button(box, "Don't ask again", eject_never_cb, NULL)));
}

/* back up: the whole storage or one partition */
static struct target { char name[40]; long long bytes; } targets[40]; static int target_count, target_pick;
static void backup_yes(void)
{
	usb_used = 1; job_next = NULL; job_home = 0; job_undo = NULL; job_pop = 1;	/* afterwards: the drive's screen */
	busy("Starting...");
	start(job_started, NULL, "nocry-rescue-do", "usb-backup", targets[target_pick].name, NULL);
}
static void target_cb(lv_event_t *e)
{
	char head[80], body[300]; struct target *t;
	target_pick = (int)(intptr_t)lv_event_get_user_data(e); t = &targets[target_pick];
	if (!strcmp(t->name, "full")) snprintf(head, sizeof head, "Back up the entire eMMC?");
	else snprintf(head, sizeof head, "Back up %s?", t->name);
	snprintf(body, sizeof body, "Writes an image (%s) with its SHA-256 to the folder nocry-backups on the flash drive, then reads it back and verifies it. Nothing on the board is changed.", amount(t->bytes));
	ask(head, body, "Back up", 0, backup_yes);
}
static void targets_done(int rc, char *out)
{
	char *line, *save = NULL, l2[80]; lv_obj_t *list;
	modal_close(); target_count = 0;
	if (rc || !*out) { text(content, *out ? out : "The board's storage could not be read."); return; }
	list = scroll_list();
	for (line = strtok_r(out, "\n", &save); line && target_count < 40; line = strtok_r(NULL, "\n", &save)) {
		struct target *t = &targets[target_count]; char *tab = strchr(line, '\t');
		if (!tab) continue;
		*tab = 0; snprintf(t->name, sizeof t->name, "%s", line); t->bytes = atoll(tab + 1);
		if (!strcmp(t->name, "full")) { snprintf(l2, sizeof l2, "%s   everything: boot loader, both slots, data", amount(t->bytes)); entry(list, "Entire eMMC", l2, target_cb, (void *)(intptr_t)target_count); }
		else { snprintf(l2, sizeof l2, "%s   partition", amount(t->bytes)); entry(list, t->name, l2, target_cb, (void *)(intptr_t)target_count); }
		target_count++;
	}
}
static void backup_screen(void)
{
	set_title("Back up to drive");
	busy("Reading...");
	start(targets_done, NULL, "nocry-rescue-do", "usb-targets", NULL);
}

/* restore: the images on the drive that fit this board */
static struct image { char kind[8], part[40], path[220]; long long bytes; int sum; } images[40]; static int image_count, image_pick;
static void restore_yes(void)
{
	usb_used = 1; job_next = NULL; job_home = 0; job_undo = NULL; job_pop = 1;
	busy("Starting...");
	start(job_started, NULL, "nocry-rescue-do", "usb-restore", images[image_pick].path, NULL);
}
static void image_info(int rc, char *out)
{
	char head[80], body[500]; struct image *m = &images[image_pick]; const char *b = strrchr(m->path, '/');
	const char *nosum = strcmp(kv(out, "checksum"), "yes") ? " There is no checksum file beside it: the image cannot be verified before it is written." : "";
	modal_close();
	b = b ? b + 1 : m->path;
	if (rc) { message("Cannot restore this file", out); return; }
	if (!strcmp(kv(out, "kind"), "full")) {
		if (strcmp(kv(out, "identity"), "same")) { message("Backup of another board", "The identity record in this image differs from this board's. Restoring it would replace the serial number. Use nocry on a computer for that."); return; }
		snprintf(head, sizeof head, "Restore the entire eMMC?");
		snprintf(body, sizeof body, "Overwrites the whole storage with %s: boot loader, both slots, settings and files. Only what differs is written; all of it is verified. Intact copies of the rescue image are kept.%s", b, nosum);
	} else {
		snprintf(head, sizeof head, "Restore %s?", kv(out, "part"));
		snprintf(body, sizeof body, "Overwrites the partition %s with %s. Only what differs is written; all of it is verified.%s", kv(out, "part"), b, nosum);
	}
	ask(head, body, "Restore", 1, restore_yes);
}
static void image_cb(lv_event_t *e)
{
	image_pick = (int)(intptr_t)lv_event_get_user_data(e);
	busy("Reading...");
	start(image_info, NULL, "nocry-rescue-do", "usb-info", images[image_pick].path, NULL);
}
static void images_done(int rc, char *out)
{
	char *line, *save = NULL, l2[120]; lv_obj_t *list;
	modal_close(); image_count = 0;
	if (rc) { text(content, out); return; }
	if (!*out) { text(content, "No backup of this board on the drive."); dim(text(content, "Listed are .img files (or .img.001 sets) up to three folders deep whose size is that of the eMMC, or of the partition their name ends in.")); return; }
	list = scroll_list();
	for (line = strtok_r(out, "\n", &save); line && image_count < 40; line = strtok_r(NULL, "\n", &save)) {
		struct image *m = &images[image_count]; char *c[5]; const char *b; int n = 0;
		for (c[n++] = line; n < 5 && (line = strchr(line, '\t')); c[n++] = line) *line++ = 0;
		if (n < 5) continue;
		snprintf(m->kind, sizeof m->kind, "%s", c[0]); snprintf(m->part, sizeof m->part, "%s", c[1]); m->bytes = atoll(c[2]); m->sum = !strcmp(c[3], "yes");
		snprintf(m->path, sizeof m->path, "%s", c[4]);
		b = strrchr(m->path, '/'); b = b ? b + 1 : m->path;
		if (!strcmp(m->kind, "full")) snprintf(l2, sizeof l2, "Entire eMMC   %s%s", amount(m->bytes), m->sum ? "" : "   no checksum");
		else snprintf(l2, sizeof l2, "Partition %s   %s%s", m->part, amount(m->bytes), m->sum ? "" : "   no checksum");
		entry(list, b, l2, image_cb, (void *)(intptr_t)image_count);
		image_count++;
	}
}
static void restore_screen(void)
{
	set_title("Restore from drive");
	busy("Searching the drive...");
	start(images_done, NULL, "nocry-rescue-do", "usb-list", "backups", NULL);
}

/* install a firmware file that is on the drive */
static char usb_fws[40][220]; static int usb_fw_count;
static void usb_fw_cb(lv_event_t *e) { snprintf(usb_fw, sizeof usb_fw, "%s", usb_fws[(int)(intptr_t)lv_event_get_user_data(e)]); go(where_screen); }
static void usb_fws_done(int rc, char *out)
{
	char *line, *save = NULL, l2[260]; lv_obj_t *list;
	modal_close(); usb_fw_count = 0;
	if (rc) { text(content, out); return; }
	if (!*out) { text(content, "No firmware file on the drive."); dim(text(content, "Listed are Creality update files (.img) and factory packages (.ingenic) up to three folders deep.")); return; }
	list = scroll_list();
	for (line = strtok_r(out, "\n", &save); line && usb_fw_count < 40; line = strtok_r(NULL, "\n", &save)) {
		char *tab = strchr(line, '\t'), *b, *dir;
		if (!tab) continue;
		*tab = 0; snprintf(usb_fws[usb_fw_count], sizeof usb_fws[0], "%s", tab + 1);
		b = strrchr(tab + 1, '/'); dir = b ? tab + 1 : NULL; if (b) *b++ = 0; else b = tab + 1;
		if (dir) snprintf(l2, sizeof l2, "%s   in %s", amount(atoll(line)), dir); else snprintf(l2, sizeof l2, "%s", amount(atoll(line)));
		entry(list, b, l2, usb_fw_cb, (void *)(intptr_t)usb_fw_count);
		usb_fw_count++;
	}
}
static void usb_fw_screen(void)
{
	set_title("Firmware on drive");
	busy("Searching the drive...");
	start(usb_fws_done, NULL, "nocry-rescue-do", "usb-list", "fw", NULL);
}

/* format: erase the drive that is plugged in, mounted or not, and put one file system on it */
static const char *format_fs;
static void format_done(int rc, char *out)
{
	modal_close();
	if (rc) { message("Format failed", out); return; }
	back();
	message("Drive formatted", "The new file system is mounted in a moment.");
}
static void format_yes(void) { busy("Formatting..."); start(format_done, NULL, "nocry-rescue-do", "usb-format", format_fs, NULL); }
static void format_cb(lv_event_t *e)
{
	char *st = run_file("nocry-usb.state"), body[400];
	format_fs = lv_event_get_user_data(e);
	snprintf(body, sizeof body, "ALL DATA on this drive is destroyed: every partition and every file on %s%s%s. This cannot be undone.\n\nIt gets one %s partition.",
		kv(st, "disk_model"), *kv(st, "disk_model") ? ", " : "", amount(atoll(kv(st, "disk_bytes"))), !strcmp(format_fs, "exfat") ? "exFAT" : "FAT32");
	ask("Erase the flash drive?", body, "Erase and format", 1, format_yes);
	free(st);
}
static void format_screen(void)
{
	char *st = run_file("nocry-usb.state"), s[300];
	set_title("Format drive");
	if (!*kv(st, "disk")) { text(content, "No flash drive."); free(st); return; }
	snprintf(s, sizeof s, "%s%s%s", kv(st, "disk_model"), *kv(st, "disk_model") ? "   " : "", amount(atoll(kv(st, "disk_bytes"))));
	lv_obj_set_style_text_font(text(content, s), &lv_font_montserrat_18, 0);
	lv_obj_set_style_text_color(text(content, "Formatting erases everything on the drive."), lv_palette_lighten(LV_PALETTE_RED, 1), 0);
	wide(red(button(content, "Format as exFAT", format_cb, (void *)"exfat")));
	dim(text(content, "Files of any size. Windows, macOS, Linux."));
	wide(red(button(content, "Format as FAT32", format_cb, (void *)"fat32")));
	dim(text(content, "Read by everything, printers included. No file over 4 GiB (backups are split)."));
	free(st);
}

static char usb_shown[400];
static void go_cb(lv_event_t *e);
static void usb_draw(void)
{
	char *st = run_file("nocry-usb.state"), s[300]; lv_obj_t *b;
	lv_obj_clean(content);
	snprintf(usb_shown, sizeof usb_shown, "%s", st);
	if (strcmp(kv(st, "mounted"), "yes")) {
		if (*kv(st, "disk")) {		/* a drive is there, but nothing of it is mounted */
			snprintf(s, sizeof s, "%s%s%s\n%s", kv(st, "disk_model"), *kv(st, "disk_model") ? "   " : "", amount(atoll(kv(st, "disk_bytes"))),
				!strcmp(kv(st, "ejected"), "yes") ? "Ejected. The flash drive can be removed." : "No file system on it that can be mounted (FAT32, exFAT, NTFS).");
			text(content, s);
			wide(red(button(content, "Format drive...", go_cb, (void *)format_screen)));
		} else {
			text(content, "No flash drive.");
			dim(text(content, "Supported: FAT32, exFAT, NTFS. The USB-A sockets are off while a computer is connected to the Micro-USB socket."));
		}
		free(st); return;
	}
	snprintf(s, sizeof s, "%s%s%s\n%s, %s free%s", kv(st, "label"), *kv(st, "label") ? "   " : "", kv(st, "fs"), amount(atoll(kv(st, "size"))), amount(atoll(kv(st, "free"))),
		!strcmp(kv(st, "readonly"), "yes") ? "\nMounted read-only: backups cannot be written." : "");
	text(content, s);
	{	/* four buttons: one under the other when upright, two by two when wide */
		lv_obj_t *r = row(content), *bs[5]; int i;
		bs[0] = b = button(r, "Back up to drive", go_cb, (void *)backup_screen);
		if (!strcmp(kv(st, "readonly"), "yes")) lv_obj_add_state(b, LV_STATE_DISABLED);
		bs[1] = button(r, "Restore from drive", go_cb, (void *)restore_screen);
		bs[2] = button(r, "Install firmware", go_cb, (void *)usb_fw_screen);
		bs[3] = grey(button(r, LV_SYMBOL_EJECT "  Eject", eject_cb, NULL));
		bs[4] = grey(button(r, "Format drive...", go_cb, (void *)format_screen));
		for (i = 0; i < 5; i++) lv_obj_set_width(bs[i], W < 400 ? W - 16 : (W - 24) / 2);
	}
	free(st);
}
static void usb_tick(lv_timer_t *t)
{
	char *st = run_file("nocry-usb.state");
	(void)t;
	/* the free space moves while nothing else does: only what the screen says differently redraws it */
	if (!modal && (strcmp(kv(st, "mounted"), kv(usb_shown, "mounted")) || strcmp(kv(st, "ejected"), kv(usb_shown, "ejected")) || strcmp(kv(st, "device"), kv(usb_shown, "device")) || strcmp(kv(st, "disk"), kv(usb_shown, "disk")))) usb_draw();
	free(st);
}
static void usb_screen(void)
{
	set_title("USB drive");
	usb_draw();
	screen_timer = lv_timer_create(usb_tick, 1000, NULL);
}

/* ---- repair ---------------------------------------------------------------------------- */
static const char *repair_arg[2];
static void repair_yes(void) { job_next = NULL; busy("Starting..."); start(job_started, NULL, "nocry-rescue-do", repair_arg[0], repair_arg[1], NULL); }
static void mods_cb(lv_event_t *e)
{
	(void)e; repair_arg[0] = "reset"; repair_arg[1] = "mods";
	ask("Wipe system overlay?", "Erases rootfs_data: all changes to the root file system (installed packages, root access tools, system configuration). User data (print files, printer configuration) is preserved.", "Wipe overlay", 1, repair_yes);
}
static void all_cb(lv_event_t *e)
{
	(void)e; repair_arg[0] = "reset"; repair_arg[1] = "all";
	ask("Factory reset?", "Erases rootfs_data and userdata: all settings and files.", "Erase all", 1, repair_yes);
}
static void fsck_data_cb(lv_event_t *e) { (void)e; repair_arg[0] = "check"; repair_arg[1] = "data"; ask("Check userdata?", "Runs e2fsck on the userdata partition and repairs errors.", "Run fsck", 0, repair_yes); }
static void fsck_set_cb(lv_event_t *e) { (void)e; repair_arg[0] = "check"; repair_arg[1] = "settings"; ask("Check rootfs_data?", "Runs e2fsck on the system overlay partition and repairs errors.", "Run fsck", 0, repair_yes); }
static void health_done(int rc, char *out)
{
	char s[400];
	modal_close();
	if (rc) { message("Read failed", out); return; }
	snprintf(s, sizeof s, "Device: %s\nWear (type A / B): %s / %s\nSpare blocks: %s\nuserdata: %s", kv(out, "chip"), kv(out, "wear_a"), kv(out, "wear_b"), kv(out, "spare"), kv(out, "data_state"));
	message("eMMC health", s);
}
static void health_cb(lv_event_t *e) { (void)e; busy("Reading..."); start(health_done, NULL, "nocry-rescue-do", "health", NULL); }
static void repair_screen(void)
{
	set_title("Repair");
	wide(grey(button(content, "Check userdata (fsck)", fsck_data_cb, NULL)));
	wide(grey(button(content, "Check rootfs_data (fsck)", fsck_set_cb, NULL)));
	wide(grey(button(content, "eMMC health", health_cb, NULL)));
	wide(red(button(content, "Wipe system overlay", mods_cb, NULL)));
	wide(red(button(content, "Factory reset", all_cb, NULL)));
}

/* ---- advanced: repair, and reaching the board over the network --------------------------- */
static void remote_draw(void);
static void remote_applied(int rc, char *out) { (void)rc; (void)out; modal_close(); remote_draw(); }
static void remote_stored(int rc, char *out) { (void)rc; (void)out; start(remote_applied, NULL, "nocry-remote", "apply", NULL); }
static void remote_apply(void) { busy("One moment..."); settings_store(remote_stored); }	/* kept, then started or stopped */
static void lan_on(void) { remote_lan = 1; remote_apply(); }
static void adb_on(void) { remote_adb = 1; remote_apply(); }
static void ssh_on(void) { remote_ssh = 1; remote_apply(); }
static void lan_cb(lv_event_t *e)
{
	(void)e;
	if (remote_lan) { remote_lan = 0; remote_apply(); }
	else ask("Turn on nocry over the network?", "nocry on a computer can then work on this board over the network (port 5560): back up, restore, install. It needs the pairing code shown on this screen.", "Turn on", 0, lan_on);
}
static void adb_cb(lv_event_t *e)
{
	(void)e;
	if (remote_adb) { remote_adb = 0; remote_apply(); }
	else ask("Turn on ADB over the network?", "ADB here has no password and no pairing: anyone who can reach this board on the network gets a root shell (port 5555). Only on a network you trust.", "Turn on", 1, adb_on);
}
static void ssh_cb(lv_event_t *e)
{
	char body[300];
	(void)e;
	if (remote_ssh) { remote_ssh = 0; remote_apply(); return; }
	snprintf(body, sizeof body, "SSH on port 22, user root, password %s. Anyone on the network who knows the password gets a root shell. Change the password first if this network is not yours alone.", ssh_pass[0] ? ssh_pass : "Creality2023");
	ask("Turn on SSH?", body, "Turn on", 0, ssh_on);
}
static void sshpw_key(lv_event_t *e)
{
	const char *p = lv_textarea_get_text(form_f[0]);
	if (lv_event_get_code(e) == LV_EVENT_CANCEL) { back(); return; }
	if (strlen(p) < 6 || strchr(p, ' ')) { remark("Password not usable", "At least 6 characters, no spaces."); return; }
	snprintf(ssh_pass, sizeof ssh_pass, "%s", p);
	depth--;				/* back to the remote access screen, which is drawn when it is applied */
	show();
	remote_apply();
}
static void sshpw_screen(void)
{
	settings_refresh();
	set_title("SSH password");
	form_begin();
	form_field("Password", ssh_pass[0] ? ssh_pass : "Creality2023", NULL, 63, 1);
	form_end(0, 1, sshpw_key);
}
static void go_cb(lv_event_t *e);
static void remote_item(const char *name, const char *detail, int on, lv_event_cb_t cb)
{
	lv_obj_t *r = row(content), *col = lv_obj_create(r), *l, *b;
	lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW); lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_remove_style_all(col); lv_obj_set_height(col, LV_SIZE_CONTENT); lv_obj_set_flex_grow(col, 1);
	lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
	l = lv_label_create(col); lv_label_set_text(l, name);
	l = dim(lv_label_create(col)); lv_obj_set_width(l, lv_pct(100)); lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP); lv_label_set_text(l, detail);
	b = button(r, on ? "On" : "Off", cb, NULL);
	lv_obj_set_width(b, 64); lv_obj_set_height(b, 40);
	if (!on) grey(b); else lv_obj_set_style_bg_color(b, lv_palette_darken(LV_PALETTE_GREEN, 2), 0);
}
static void remote_draw(void)
{
	char *st = run_file("nocry-wifi.state"), s[200]; const char *a = kv(st, "address");
	lv_obj_clean(content);
	lv_obj_set_style_pad_row(content, 6, 0);
	snprintf(s, sizeof s, *a ? "This board: %s" : "This board is not on a network.", a);
	dim(text(content, s));
	remote_item("nocry over the network", "the nocry program on a computer; port 5560, pairing code", remote_lan, lan_cb);
	snprintf(s, sizeof s, "adb connect %s; port 5555, no password", *a ? a : "<address>");
	remote_item("ADB over the network", s, remote_adb, adb_cb);
	snprintf(s, sizeof s, "ssh root@%s; password %s", *a ? a : "<address>", ssh_pass[0] ? ssh_pass : "Creality2023");
	remote_item("SSH", s, remote_ssh, ssh_cb);
	wide(grey(button(content, "SSH password...", go_cb, (void *)sshpw_screen)));
	dim(text(content, "All off unless turned on here; what is on stays on after a restart. The web interface is always available with the pairing code."));
	free(st);
}
static void remote_tick(lv_timer_t *t)	/* the web pages may switch these too */
{
	int l = remote_lan, a = remote_adb, h = remote_ssh;
	(void)t;
	if (modal) return;
	settings_refresh();
	if (l != remote_lan || a != remote_adb || h != remote_ssh) remote_draw();
}
static void remote_screen(void) { set_title("Remote access"); settings_refresh(); remote_draw(); screen_timer = lv_timer_create(remote_tick, 2000, NULL); }
static void repair_screen(void);
static void advanced_screen(void)
{
	set_title("Advanced");
	wide(button(content, LV_SYMBOL_SETTINGS "  Repair", go_cb, (void *)repair_screen));
	dim(text(content, "Check file systems, eMMC health, wipe the system overlay, factory reset."));
	wide(button(content, LV_SYMBOL_WIFI "  Remote access", go_cb, (void *)remote_screen));
	dim(text(content, "nocry over the network, ADB over the network, SSH. All off unless turned on."));
}

/* ---- about ----------------------------------------------------------------------------- */
static lv_obj_t *about_lbl; static char about[900];
static void about_add(const char *fmt, ...)
{
	va_list ap; size_t l = strlen(about);
	va_start(ap, fmt); vsnprintf(about + l, sizeof about - l, fmt, ap); va_end(ap);
	lv_label_set_text(about_lbl, about);
}
static void about_rescue(int rc, char *out)
{
	const char *s = kv(out, "stored");
	(void)rc;
	about_add("Rescue image: %s\n", !strcmp(s, "yes") ? "installed, verified" : !strcmp(s, "damaged") ? "checksum mismatch, reinstall" : "not installed");
}
static void about_loader(int rc, char *out)
{
	const char *l = strstr(out, "loader: ");
	(void)rc;
	if (l) about_add("Boot loader: %.*s\n", (int)strcspn(l + 8, "\n"), l + 8);
	start(about_rescue, NULL, "nocry-rescue-do", "rescue-status", NULL);
}
static void about_status(int rc, char *out)
{
	const char *note = kv(out, "note");
	modal_close();
	if (rc) { about_add("%s\n", out); return; }
	about_add("Model: %s\nMainboard: %s\nSlot A: %s\nSlot B: %s\nActive slot: %s\n", kv(out, "model"), kv(out, "mainboard"), kv(out, "a_firmware"), kv(out, "b_firmware"), kv(out, "active")[0] == 'b' ? "B" : kv(out, "active")[0] == 'a' ? "A" : "unknown");
	if (*note) about_add("Boot record: %s\n", note);
	start(about_loader, NULL, "nocry-rescue-do", "loader", NULL);
}
/* ---- updates: a newer rescue image or boot loader in this project's releases on GitHub ---- */
static char upd[700]; static int upd_asked;	/* the last answer of update-check; asked about once */
static void update_draw(void);
static void update_done(int rc, char *out)
{
	modal_close();
	snprintf(upd, sizeof upd, "%s", rc && !*kv(out, "error") ? "error=the check did not run" : out);
	update_draw();
}
static void update_check(const char *again)
{
	busy("Asking GitHub...");
	start(update_done, NULL, "nocry-rescue-do", "update-check", again, NULL);
}
static void update_again_cb(lv_event_t *e) { (void)e; update_check("again"); }
static void update_rescue_yes(void) { job_next = NULL; job_home = 0; job_undo = NULL; busy("Starting..."); start(job_started, NULL, "nocry-rescue-do", "update-rescue", NULL); }
static void update_loader_yes(void) { job_next = NULL; job_home = 0; job_undo = NULL; busy("Starting..."); start(job_started, NULL, "nocry-rescue-do", "update-loader", NULL); }
static void update_rescue_cb(lv_event_t *e)
{
	char b[400];
	(void)e;
	snprintf(b, sizeof b, "Downloads rescue %s from GitHub, checks its size, SHA-256 and the image itself, then replaces both stored copies one after the other, each read back. The rescue system running now is not affected; the new one runs from the next start.", kv(upd, "rescue_latest"));
	ask("Update the rescue image?", b, "Update", 0, update_rescue_yes);
}
static void update_loader_cb(lv_event_t *e)
{
	char b[500];
	(void)e;
	snprintf(b, sizeof b, "Downloads nocryboot %s from GitHub and checks its size, SHA-256, board family and version before anything is written. Writing takes a few seconds and is read back: do not power off. The factory loader stays kept on the board. If the board ever fails to start, USB boot mode (BOOT button) and nocry on a computer bring it back.", kv(upd, "loader_latest"));
	ask("Update the boot loader?", b, "Update", 1, update_loader_yes);
}
static void update_draw(void)
{
	char s[400]; int r = !strcmp(kv(upd, "rescue_update"), "yes"), l = !strcmp(kv(upd, "loader_update"), "yes");
	lv_obj_clean(content);
	if (*kv(upd, "error") || !*kv(upd, "release")) {
		text(content, "The update check needs an internet connection.");
		if (*kv(upd, "error")) dim(text(content, kv(upd, "error")));
		wide(button(content, "Check again", update_again_cb, NULL));
		return;
	}
	snprintf(s, sizeof s, "Newest release: %s\nRescue image: %s%s\nBoot loader: %s%s%s", kv(upd, "release"),
		kv(upd, "rescue_installed"), r ? "  (update available)" : "  (up to date)",
		!strcmp(kv(upd, "loader_installed"), "factory") ? "factory loader" : "nocryboot ", !strcmp(kv(upd, "loader_installed"), "factory") ? "" : kv(upd, "loader_installed"),
		!strcmp(kv(upd, "loader_installed"), "factory") ? "" : l ? "  (update available)" : "  (up to date)");
	text(content, s);
	if (r) { snprintf(s, sizeof s, "Update rescue image to %s", kv(upd, "rescue_latest")); wide(button(content, s, update_rescue_cb, NULL)); }
	if (l) {
		lv_obj_t *b;
		snprintf(s, sizeof s, "Update boot loader to %s", kv(upd, "loader_latest")); b = wide(button(content, s, update_loader_cb, NULL));
		/* the new loader has to find the rescue image where the new rescue image is kept: that one first */
		if (r) { lv_obj_add_state(b, LV_STATE_DISABLED); dim(text(content, "Update the rescue image first; the boot loader after it.")); }
	}
	if (!r && !l) dim(text(content, "Nothing to update."));
	wide(grey(button(content, "Check again", update_again_cb, NULL)));
}
static void update_screen(void)
{
	set_title("Updates");
	update_check(NULL);
}
/* once, when the board is on a network: is there something newer?  A box says so; nothing is
   downloaded or changed without the owner saying yes on the Updates screen. */
static void update_show(void) { go(update_screen); }
static void update_quiet(int rc, char *out)
{
	char b[300]; int r, l;
	if (rc || modal || depth != 1) return;
	snprintf(upd, sizeof upd, "%s", out);
	r = !strcmp(kv(upd, "rescue_update"), "yes"); l = !strcmp(kv(upd, "loader_update"), "yes");
	if (!r && !l) return;
	snprintf(b, sizeof b, "%s%s%s%s%s is available from the nocry project on GitHub.", r ? "Rescue " : "", r ? kv(upd, "rescue_latest") : "", r && l ? " and " : "", l ? "boot loader " : "", l ? kv(upd, "loader_latest") : "");
	ask2("Update available", b, "Later", "Show", 0, update_show);
}
void calibrate_screen(void);
static void cal_cb(lv_event_t *e) { (void)e; go(calibrate_screen); }
static void about_screen(void)
{
	set_title("System info");
	about[0] = 0;
	about_lbl = text(content, "");
	wide(button(content, "Updates", go_cb, (void *)update_screen));
	wide(grey(button(content, "Calibrate touch screen", cal_cb, NULL)));
	dim(text(content, "Touch offset? Hold anywhere for 10 seconds to recalibrate."));
	busy("Reading the board...");
	start(about_status, NULL, "nocry-rescue-do", "status", NULL);
}

/* ---- calibration: three crosses -------------------------------------------------------- */
static lv_point_t cal_at[3];
static int cal_step, cal_wait_up; static long cal_sx, cal_sy, cal_n; static int cal_rx[3], cal_ry[3];
static lv_obj_t *cal_cross, *cal_text;

static void cal_store(void) { settings_refresh(); settings_store(NULL); }
static int cal_solve(void)
{
	double x0 = cal_rx[0], y0 = cal_ry[0], x1 = cal_rx[1], y1 = cal_ry[1], x2 = cal_rx[2], y2 = cal_ry[2];
	double det = (x0 - x2) * (y1 - y2) - (x1 - x2) * (y0 - y2); int i;
	if (det > -200000 && det < 200000) return -1;	/* the three touches were nearly in one place or one line */
	for (i = 0; i < 2; i++) {
		double s[3]; int k, fx, fy;
		for (k = 0; k < 3; k++) { to_panel(cal_at[k].x, cal_at[k].y, &fx, &fy); s[k] = i ? fy : fx; }
		double s0 = s[0], s1 = s[1], s2 = s[2];
		cal[i * 3] = ((s0 - s2) * (y1 - y2) - (s1 - s2) * (y0 - y2)) / det;
		cal[i * 3 + 1] = ((x0 - x2) * (s1 - s2) - (x1 - x2) * (s0 - s2)) / det;
		cal[i * 3 + 2] = s2 - cal[i * 3] * x2 - cal[i * 3 + 1] * y2;
	}
	return 0;
}
static void cal_place(void)
{
	char s[80];
	lv_obj_set_pos(cal_cross, cal_at[cal_step].x - 20, cal_at[cal_step].y - 20);
	snprintf(s, sizeof s, "Press and hold the cross\n(%d of 3)", cal_step + 1);
	lv_label_set_text(cal_text, s);
	cal_sx = cal_sy = cal_n = 0; cal_wait_up = 1;
}
static void cal_tick(lv_timer_t *t)
{
	(void)t;
	if (!raw_down) {
		cal_wait_up = 0;
		if (cal_n < 12) { cal_sx = cal_sy = cal_n = 0; return; }	/* too short: again */
		cal_rx[cal_step] = cal_sx / cal_n; cal_ry[cal_step] = cal_sy / cal_n;
		if (++cal_step < 3) { cal_place(); cal_wait_up = 0; return; }
		lv_timer_del(screen_timer); screen_timer = NULL;
		if (cal_solve()) { cal_ok = 0; cal_step = 0; show(); return; }
		cal_ok = 1; cal_store();
		back();			/* to whatever was on the screen, a running job included */
		return;
	}
	if (cal_wait_up) return;
	if (cal_n < 40) { cal_sx += raw_x; cal_sy += raw_y; cal_n++; }
	if (cal_n == 12) lv_label_set_text(cal_text, "OK. Release.");
}
void calibrate_screen(void)
{
	lv_obj_t *l;
	set_title("Touch calibration");
	cal_ok = 0;				/* nothing can be pressed by accident meanwhile */
	cal_step = 0;
	cal_at[0].x = 40; cal_at[0].y = 40; cal_at[1].x = W - 40; cal_at[1].y = H / 2; cal_at[2].x = W / 2; cal_at[2].y = H - 40;
	modal_close();
	modal = lv_obj_create(lv_layer_top());		/* over everything; removed with the screen */
	lv_obj_remove_style_all(modal); lv_obj_set_size(modal, W, H);
	lv_obj_set_style_bg_color(modal, lv_color_hex(0x101418), 0); lv_obj_set_style_bg_opa(modal, LV_OPA_COVER, 0);
	cal_text = lv_label_create(modal); lv_obj_align(cal_text, LV_ALIGN_CENTER, 0, -34);
	lv_obj_set_style_text_color(cal_text, lv_color_hex(0xffffff), 0);
	lv_obj_set_width(cal_text, W - 100); lv_label_set_long_mode(cal_text, LV_LABEL_LONG_WRAP);
	lv_obj_set_style_text_align(cal_text, LV_TEXT_ALIGN_CENTER, 0);
	cal_cross = lv_obj_create(modal); lv_obj_remove_style_all(cal_cross); lv_obj_set_size(cal_cross, 40, 40);
	l = lv_label_create(cal_cross); lv_label_set_text(l, LV_SYMBOL_PLUS); lv_obj_set_style_text_font(l, &lv_font_montserrat_28, 0);
	lv_obj_set_style_text_color(l, lv_palette_main(LV_PALETTE_YELLOW), 0); lv_obj_center(l);
	cal_place();
	screen_timer = lv_timer_create(cal_tick, 25, NULL);
}

/* ---- home ------------------------------------------------------------------------------ */
static lv_obj_t *home_net, *home_eject;
static void home_tick(lv_timer_t *t)
{
	char *st = run_file("nocry-wifi.state"), s[200]; const char *mode = kv(st, "mode");
	(void)t;
	const char *gap = W < 400 ? "\n" : "      ";
	if (!strcmp(mode, "client")) snprintf(s, sizeof s, LV_SYMBOL_WIFI "  %s%shttp://%s", kv(st, "ssid"), gap, kv(st, "address"));
	else if (!strcmp(mode, "ap")) snprintf(s, sizeof s, LV_SYMBOL_WIFI "  AP \"%s\", %s%s%shttp://%s", kv(st, "ssid"), *kv(st, "password") ? "password " : "open", kv(st, "password"), gap, kv(st, "address"));
	else if (!strcmp(mode, "joining")) snprintf(s, sizeof s, LV_SYMBOL_WIFI "  connecting to %s...", kv(st, "ssid"));
	else if (!strcmp(mode, "off")) snprintf(s, sizeof s, LV_SYMBOL_WIFI "  off");
	else snprintf(s, sizeof s, LV_SYMBOL_WIFI "  not connected");
	if (W < 400) {		/* the bar has no room for the pairing code here */
		char *c = run_file("nocry-pair-code"); size_t l = strlen(s);
		c[strcspn(c, "\r\n")] = 0;
		if (strlen(c) == 6) snprintf(s + l, sizeof s - l, "\npairing code   %.3s %s", c, c + 3);
		free(c);
	}
	if (strcmp(lv_label_get_text(home_net), s)) lv_label_set_text(home_net, s);
	free(st);
	/* on a network for the first time since the start: ask once, quietly, whether there is an update */
	if (!upd_asked && !strcmp(mode, "client") && !modal && depth == 1 && !sim) {
		upd_asked = 1;
		start(update_quiet, NULL, "nocry-rescue-do", "update-check", NULL);
	}
	/* Eject is there to be seen, and greyed out while no drive is mounted */
	if (usb_mounted()) lv_obj_clear_state(home_eject, LV_STATE_DISABLED); else lv_obj_add_state(home_eject, LV_STATE_DISABLED);
}
/* Started by the loader because no firmware slot could be started (nobody held the screen)?
   Say so once: otherwise the owner switches on a printer and finds this instead, unexplained. */
static int slots_told;
static void home_slots(int rc, char *out)
{
	if (rc || modal || depth != 1) return;
	if (!strcmp(kv(out, "a_complete"), "yes") || !strcmp(kv(out, "b_complete"), "yes")) return;
	message("No firmware can be started", "Neither slot holds a complete firmware, so the rescue system started instead. Install firmware (Install, or USB drive), or restore a backup.");
}
static void home_job(int rc, char *out)
{
	if (!rc && !strcmp(kv(out, "state"), "running")) { job_next = NULL; job_home = 0; job_undo = NULL; go(job_screen); }
}
static void restart_done(int rc, char *out) { if (rc) { modal_close(); message("Not rebooting", out); } else busy("Rebooting..."); }
static void restart_yes(void) { busy("Rebooting..."); start(restart_done, NULL, "nocry-rescue-do", "restart", NULL); }
static void restart_cb(lv_event_t *e) { (void)e; ask("Reboot?", "Leaves rescue and boots the active slot.", "Reboot", 0, restart_yes); }
static void go_cb(lv_event_t *e) { go((screen_fn)lv_event_get_user_data(e)); }
static lv_obj_t *tile(lv_obj_t *grid, const char *icon, const char *label, lv_event_cb_t cb, void *arg)
{
	lv_obj_t *b = lv_btn_create(grid), *i = lv_label_create(b), *l = lv_label_create(b);
	lv_obj_set_size(b, W < 400 ? (W - 20) / 2 : (W - 32) / 4, W < 400 ? 84 : 74);	/* eight: two by four, or four by two */
	lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_label_set_text(i, icon); lv_obj_set_style_text_font(i, &lv_font_montserrat_28, 0);
	lv_label_set_text(l, label);
	lv_obj_set_style_bg_color(b, lv_color_hex(0x2c3440), 0);
	lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, arg);
	return b;
}
static void home_screen(void)
{
	lv_obj_t *grid = row(content);
	set_title("nocry rescue");
	lv_obj_set_style_pad_all(content, 6, 0);
	lv_obj_set_style_pad_column(grid, 6, 0); lv_obj_set_style_pad_row(grid, 6, 0);
	lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	tile(grid, LV_SYMBOL_WIFI, "Wi-Fi", go_cb, (void *)wifi_screen);
	tile(grid, LV_SYMBOL_SHUFFLE, "Slots", go_cb, (void *)slots_screen);
	tile(grid, LV_SYMBOL_DOWNLOAD, W < 400 ? "Install firmware" : "Install", go_cb, (void *)install_screen);
	tile(grid, LV_SYMBOL_USB, "USB drive", go_cb, (void *)usb_screen);
	tile(grid, LV_SYMBOL_SETTINGS, "Advanced", go_cb, (void *)advanced_screen);
	tile(grid, LV_SYMBOL_LIST, "System info", go_cb, (void *)about_screen);
	home_eject = tile(grid, LV_SYMBOL_EJECT, "Eject drive", eject_cb, NULL);
	tile(grid, LV_SYMBOL_POWER, "Reboot", restart_cb, NULL);
	home_net = dim(text(content, ""));
	lv_obj_set_style_pad_left(home_net, 4, 0);
	if (W < 400) lv_obj_set_width(home_net, W - 72);	/* the turn button is beside it */
	home_tick(NULL);
	screen_timer = lv_timer_create(home_tick, 1000, NULL);
	start(home_job, NULL, "nocry-rescue-do", "job", NULL);
	if (!slots_told) { slots_told = 1; start(home_slots, NULL, "nocry-rescue-do", "status", NULL); }
}

/* ---- start ----------------------------------------------------------------------------- */
static const char *self, *fbdev = "/dev/fb0", *i2cdev = "/dev/i2c-4";
/* Turning: the new turn is kept, and the program starts again with it.  Everything it shows
   is read from the board again, and a running job is found again by the home screen. */
static void rot_done(int rc, char *out)
{
	char t[8];
	(void)rc; (void)out;
	snprintf(t, sizeof t, "%d", turn);
	if (sim) { modal_close(); return; }
	if (fb) memset(fb, 0, (size_t)fb_stride * PH);
	execlp(self, self, "--fb", fbdev, "--i2c", i2cdev, "--turn", t, (char *)NULL);
	_exit(3);		/* the start-up script starts it again */
}
/* The picture on the turn button, after the usual "rotate right" sign: three quarters of a
   circle, and at its upper right end a solid arrow head, centred on the line, pointing the way
   each press turns the interface (a quarter turn clockwise: one press from wide to the usual
   upright).  Drawn here: the symbol
   fonts have only the two-arrow "refresh" circle. */
#define ICON 28
static void turn_icon(lv_obj_t *parent)
{
	static lv_color_t buf[LV_CANVAS_BUF_SIZE_TRUE_COLOR_ALPHA(ICON, ICON)];
	const double R = 8.5, C = ICON / 2.0, END = 30 * M_PI / 180;	/* the head sits at 30 degrees (upper right) */
	double ex = C + R * cos(END), ey = C - R * sin(END);		/* where the line ends: the middle of the head's base */
	double tx = sin(END), ty = cos(END);				/* the way the line is going there (on the screen): clockwise */
	double nx = cos(END), ny = -sin(END);				/* across the line */
	lv_obj_t *cv = lv_canvas_create(parent); lv_draw_arc_dsc_t arc; lv_draw_rect_dsc_t fill; lv_point_t head[3];
	lv_canvas_set_buffer(cv, buf, ICON, ICON, LV_IMG_CF_TRUE_COLOR_ALPHA);
	lv_canvas_fill_bg(cv, lv_color_hex(0xffffff), LV_OPA_TRANSP);
	lv_obj_center(cv); lv_obj_clear_flag(cv, LV_OBJ_FLAG_CLICKABLE);
	lv_draw_arc_dsc_init(&arc); arc.color = lv_color_hex(0xffffff); arc.width = 3; arc.rounded = 1;
	/* angles here run clockwise from three o'clock: from the lower right round by the left and the top to the head's base */
	lv_canvas_draw_arc(cv, (lv_coord_t)C, (lv_coord_t)C, (lv_coord_t)(R + 1.5), 60, 328, &arc);
	lv_draw_rect_dsc_init(&fill); fill.bg_color = lv_color_hex(0xffffff); fill.bg_opa = LV_OPA_COVER;
	head[0].x = (lv_coord_t)(ex + 7 * tx + 0.5); head[0].y = (lv_coord_t)(ey + 7 * ty + 0.5);	/* the tip */
	head[1].x = (lv_coord_t)(ex + 5.5 * nx - tx + 0.5); head[1].y = (lv_coord_t)(ey + 5.5 * ny - ty + 0.5);
	head[2].x = (lv_coord_t)(ex - 5.5 * nx - tx + 0.5); head[2].y = (lv_coord_t)(ey - 5.5 * ny - ty + 0.5);
	lv_canvas_draw_polygon(cv, head, 3, &fill);
}
static void rot_cb(lv_event_t *e)
{
	(void)e;
	settings_refresh();
	turn = (turn + 270) % 360;		/* a quarter turn clockwise */
	busy("Rotating...");
	settings_store(rot_done);
}
static void frame_build(void)
{
	lv_obj_t *scr = lv_scr_act(), *bar = lv_obj_create(scr);
	lv_obj_set_style_bg_color(scr, lv_color_hex(0x101418), 0);
	lv_obj_remove_style_all(bar); lv_obj_set_size(bar, W, BAR); lv_obj_set_pos(bar, 0, 0);
	lv_obj_set_style_bg_color(bar, lv_color_hex(0x1c232c), 0); lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
	back_btn = lv_btn_create(bar); lv_obj_set_size(back_btn, 76, BAR); lv_obj_set_pos(back_btn, 0, 0);
	lv_obj_set_style_radius(back_btn, 0, 0); lv_obj_set_style_bg_color(back_btn, lv_color_hex(0x38424e), 0);
	lv_obj_set_style_shadow_width(back_btn, 0, 0);
	{ lv_obj_t *l = lv_label_create(back_btn); lv_label_set_text(l, LV_SYMBOL_LEFT); lv_obj_center(l); }
	lv_obj_add_event_cb(back_btn, back_cb, LV_EVENT_CLICKED, NULL);
	title = lv_label_create(bar); lv_obj_set_style_text_font(title, &lv_font_montserrat_18, 0);
	lv_obj_align(title, LV_ALIGN_LEFT_MID, 86, 0);
	lv_label_set_long_mode(title, LV_LABEL_LONG_DOT); lv_obj_set_width(title, W < 400 ? W - 92 : W - 200);
	code_lbl = lv_label_create(bar); lv_label_set_text(code_lbl, "");
	lv_obj_align(code_lbl, LV_ALIGN_RIGHT_MID, -8, 0);
	lv_obj_set_style_text_color(code_lbl, lv_palette_lighten(LV_PALETTE_YELLOW, 1), 0);
	content = lv_obj_create(scr);
	lv_obj_remove_style_all(content);
	lv_obj_set_size(content, W, H - BAR); lv_obj_set_pos(content, 0, BAR);
	/* the turn button: one corner of the glass, top right when wide, bottom right when upright */
	rot_btn = lv_btn_create(scr);
	if (W > H) { lv_obj_set_size(rot_btn, 52, BAR); lv_obj_set_pos(rot_btn, W - 52, 0); lv_obj_set_style_radius(rot_btn, 0, 0); lv_obj_align(code_lbl, LV_ALIGN_RIGHT_MID, -60, 0); }
	else { lv_obj_set_size(rot_btn, 52, 48); lv_obj_set_pos(rot_btn, W - 56, H - 52); }
	lv_obj_set_style_bg_color(rot_btn, lv_color_hex(0x38424e), 0); lv_obj_set_style_shadow_width(rot_btn, 0, 0);
	turn_icon(rot_btn);
	lv_obj_add_event_cb(rot_btn, rot_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_set_style_pad_all(content, 8, 0); lv_obj_set_style_pad_row(content, 8, 0);
	lv_obj_add_flag(content, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_scroll_dir(content, LV_DIR_VER);
}
/* the kept settings: the calibration (made for this panel, or it is of no use), the turn,
   whether to ask about ejecting.  Returns the kept turn, or -1. */
static int settings_load(void)
{
	char *s = run_file("nocry-touch.cal"), *p; double c[6];
	int w, h, t = -1;
	if (sscanf(s, "fb %dx%d %lf %lf %lf %lf %lf %lf", &w, &h, &cal[0], &cal[1], &cal[2], &cal[3], &cal[4], &cal[5]) == 8 && w == PW && h == PH) cal_ok = 1;
	else if (sscanf(s, "%dx%d %lf %lf %lf %lf %lf %lf", &w, &h, &c[0], &c[1], &c[2], &c[3], &c[4], &c[5]) == 8 && w == PH && h == PW) {
		/* as kept before the interface could be turned: made for the upright interface (turn 90) */
		cal[0] = c[3]; cal[1] = c[4]; cal[2] = c[5]; cal[3] = -c[0]; cal[4] = -c[1]; cal[5] = PH - 1 - c[2];
		cal_ok = 1;
	}
	if ((p = strstr(s, "\nturn "))) t = atoi(p + 6);
	if (strstr(s, "\neject never")) eject_ask = 0;
	remote_lan = strstr(s, "\nremote_lan on") != NULL; remote_adb = strstr(s, "\nremote_adb on") != NULL; remote_ssh = strstr(s, "\nremote_ssh on") != NULL;
	if ((p = strstr(s, "\nssh_pass "))) snprintf(ssh_pass, sizeof ssh_pass, "%.*s", (int)strcspn(p + 10, "\n"), p + 10);
	if ((p = strstr(s, "\nap_ssid ")) && p[9] != '\n' && p[9]) {
		snprintf(ap_ssid, sizeof ap_ssid, "%.*s", (int)strcspn(p + 9, "\n"), p + 9); ap_own = 1;
		if ((p = strstr(s, "\nap_pass "))) snprintf(ap_pass, sizeof ap_pass, "%.*s", (int)strcspn(p + 9, "\n"), p + 9);
	}
	free(s);
	return t;
}

static void shot(const char *path)
{
	FILE *f = fopen(path, "wb"); int i;
	if (!f) { perror(path); return; }
	fprintf(f, "P6\n%d %d\n255\n", W, H);
	for (i = 0; i < W * H; i++) { fputc(frame[i].ch.red, f); fputc(frame[i].ch.green, f); fputc(frame[i].ch.blue, f); }
	fclose(f);
}
static void sim_run(int ms) { for (; ms > 0; ms -= 5) { sim_ms += 5; lv_timer_handler(); usleep(300); } }

int main(int argc, char **argv)
{
	static lv_disp_draw_buf_t db; static lv_disp_drv_t dd; static lv_indev_drv_t id;
	const char *script = NULL; int i, want_turn = -1, kept;

	for (i = 1; i + 1 < argc; i += 2) {
		if (!strcmp(argv[i], "--fb")) fbdev = argv[i + 1];
		else if (!strcmp(argv[i], "--i2c")) i2cdev = argv[i + 1];
		else if (!strcmp(argv[i], "--sim")) { script = argv[i + 1]; sim = 1; }
		else if (!strcmp(argv[i], "--turn")) want_turn = atoi(argv[i + 1]);
		else break;
	}
	if (i < argc) { fprintf(stderr, "usage: nocry-ui [--fb DEV] [--i2c DEV] [--turn 0|90|180|270] | nocry-ui --sim SCRIPT\n"); return 2; }
	self = argv[0];
	if (getenv("NOCRY_RUN")) RUN = getenv("NOCRY_RUN");
	signal(SIGPIPE, SIG_IGN);
	if (!sim) {
		if (screen_open(fbdev)) return 1;
		i2c = open(i2cdev, O_RDWR | O_CLOEXEC);
		if (i2c < 0 || ioctl(i2c, I2C_SLAVE, 0x48) < 0) { perror(i2cdev); i2c = -1; }
	}
	kept = settings_load();
	if (sim) cal_ok = 1;
	turn = want_turn == 0 || want_turn == 90 || want_turn == 180 || want_turn == 270 ? want_turn : kept == 90 || kept == 180 || kept == 270 ? kept : 0;
	if (turn == 90 || turn == 270) { W = PH; H = PW; } else { W = PW; H = PH; }
	touch_log = getenv("NOCRY_UI_LOG") != NULL;

	lv_init();
	lv_disp_draw_buf_init(&db, frame, NULL, MAXPIX);
	lv_disp_drv_init(&dd);
	dd.hor_res = W; dd.ver_res = H; dd.flush_cb = flush; dd.draw_buf = &db; dd.direct_mode = 1;	/* "frame" always holds the whole picture */
	lv_disp_drv_register(&dd);
	lv_indev_drv_init(&id);
	id.type = LV_INDEV_TYPE_POINTER; id.read_cb = touch_read;
	lv_indev_drv_register(&id);

	frame_build();
	lv_timer_create(reap, 100, NULL);
	lv_timer_create(code_tick, 1000, NULL);
	code_tick(NULL);
	stack[depth++] = home_screen;
	if (cal_ok) show(); else go(calibrate_screen);

	if (sim) {
		FILE *f = fopen(script, "r"); char line[200], a[120]; int x, y, ms;
		if (!f) { perror(script); return 1; }
		sim_run(200);
		while (fgets(line, sizeof line, f)) {
			if (sscanf(line, "tap %d %d", &x, &y) == 2) { sim_x = x; sim_y = y; sim_down = 1; sim_run(120); sim_down = 0; sim_run(200); }
			else if (sscanf(line, "hold %d %d %d", &x, &y, &ms) == 3) { sim_x = x; sim_y = y; sim_down = 1; sim_run(ms); sim_down = 0; sim_run(200); }
			else if (sscanf(line, "wait %d", &ms) == 1) sim_run(ms);
			else if (sscanf(line, "shot %119s", a) == 1) { sim_run(60); shot(a); }
		}
		return 0;
	}
	for (;;) { lv_timer_handler(); usleep(5000); }
}
