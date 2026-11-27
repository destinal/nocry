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
 *   nocry-ui [--fb DEV] [--i2c DEV]
 *   nocry-ui --sim SCRIPT        no screen and no touch chip: follow a script of
 *                                "tap X Y", "hold X Y MS", "wait MS", "shot FILE.ppm" lines
 *                                (testing on a PC; commands come from $PATH)
 *
 * The touch position needs calibrating once (three crosses); it is kept on the data partition
 * next to the rescue image.  Holding a finger anywhere for ten seconds starts the calibration
 * again, so a wrong one cannot lock anybody out.
 */
#include <errno.h>
#include <fcntl.h>
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

#define W 480
#define H 272
#define BAR 34			/* the title bar */

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
static lv_color_t frame[W * H];

static void flush(lv_disp_drv_t *d, const lv_area_t *a, lv_color_t *c)
{
	int x, y, w = a->x2 - a->x1 + 1;
	for (y = a->y1; y <= a->y2; y++, c += w) {
		if (!fb) break;
		if (fb_bpp == 4) memcpy(fb + y * fb_stride + a->x1 * 4, c, w * 4);
		else for (x = 0; x < w; x++) {
			uint16_t v = (c[x].ch.red >> 3) << 11 | (c[x].ch.green >> 2) << 5 | c[x].ch.blue >> 3;
			memcpy(fb + y * fb_stride + (a->x1 + x) * 2, &v, 2);
		}
	}
	lv_disp_flush_ready(d);
}

static int screen_open(const char *dev)
{
	struct fb_var_screeninfo v; struct fb_fix_screeninfo f; int fd = open(dev, O_RDWR); size_t size;
	if (fd < 0 || ioctl(fd, FBIOGET_VSCREENINFO, &v) || ioctl(fd, FBIOGET_FSCREENINFO, &f)) { perror(dev); return -1; }
	fb_bpp = v.bits_per_pixel / 8; fb_stride = f.line_length; size = (size_t)fb_stride * v.yres_virtual;
	if ((fb_bpp != 4 && fb_bpp != 2) || v.xres < W || v.yres < H) { fprintf(stderr, "nocry-ui: this screen is not supported\n"); return -1; }
	fb = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (fb == MAP_FAILED) { perror("mmap"); fb = NULL; return -1; }
	memset(fb, 0, size);
	v.yoffset = 0; ioctl(fd, FBIOPAN_DISPLAY, &v);
	return 0;
}

/* ---- the touch chip ---------------------------------------------------------------- */
static int i2c = -1;
static double cal[6]; static int cal_ok;	/* screen x = c0*rx + c1*ry + c2, y = c3*rx + c4*ry + c5 */
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
	static int px, py, have, down, last_x, last_y;
	int z, x, y, x2, y2;
	(void)d;
	if (sim) {
		raw_down = sim_down; raw_x = sim_x; raw_y = sim_y;
		data->point.x = sim_x; data->point.y = sim_y; data->state = sim_down ? LV_INDEV_STATE_PR : LV_INDEV_STATE_REL;
		return;
	}
	z = i2c < 0 ? -1 : ns_read(0xe0);
	/* pressed from 500 up, let go below 250: a finger that is leaving wobbles in between */
	if (z >= (down ? 250 : 500)) {
		x = ns_read(0xc0); y = ns_read(0xd0); x2 = ns_read(0xc0); y2 = ns_read(0xd0);
		/* two readings that agree, or the reading is taken while the finger settles */
		if (x >= 0 && y >= 0 && abs(x - x2) < 40 && abs(y - y2) < 40) {
			x = (x + x2) / 2; y = (y + y2) / 2;
			if (!down) { down = 1; have = 0; held_since = nocry_ui_ms(); }
			/* report one reading late: the last one before the finger lifts is the worst */
			if (have) { raw_x = px; raw_y = py; raw_down = 1; }
			px = x; py = y; have = 1;
		}
	} else if (z >= 0 || i2c < 0) {
		down = 0; have = 0; raw_down = 0;
	}
	if (raw_down && cal_ok) {
		last_x = (int)(cal[0] * raw_x + cal[1] * raw_y + cal[2]); last_y = (int)(cal[3] * raw_x + cal[4] * raw_y + cal[5]);
		if (last_x < 0) last_x = 0;
		if (last_x >= W) last_x = W - 1;
		if (last_y < 0) last_y = 0;
		if (last_y >= H) last_y = H - 1;
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
	if (!p) { if (fn) fn(127, strdup("too much at once")); return; }
	snprintf(p->out, sizeof p->out, "%s/nocry-ui.%d.%d", RUN, (int)getpid(), serial++);
	p->fn = fn; p->gen = gen;
	p->pid = fork();
	if (p->pid == 0) {
		int o = open(p->out, O_WRONLY | O_CREAT | O_TRUNC, 0600), in = open(input ? input : "/dev/null", O_RDONLY);
		dup2(in, 0); dup2(o, 1); dup2(o, 2);
		execvp(argv[0], argv);
		_exit(127);
	}
	if (p->pid < 0) { p->pid = 0; if (fn) fn(127, strdup("could not start it")); }
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

/* ---- the frame every screen shares ------------------------------------------------- */
typedef void (*screen_fn)(void);
static screen_fn stack[10]; static int depth;
static lv_obj_t *title, *back_btn, *code_lbl, *content, *modal;
static lv_timer_t *screen_timer;

static void modal_close(void) { if (modal) { lv_obj_del(modal); modal = NULL; } }

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
static lv_obj_t *wide(lv_obj_t *b) { lv_obj_set_width(b, lv_pct(100)); return b; }
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
	lv_obj_set_size(box, W - 40, LV_SIZE_CONTENT);
	lv_obj_set_style_max_height(box, H - 16, 0);
	lv_obj_center(box);
	lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(box, 10, 0);
	return box;
}
static void busy(const char *what)
{
	lv_obj_t *box = modal_box(), *s;
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
static void ask(const char *head, const char *body, const char *yes, int danger, void (*fn)(void))
{
	lv_obj_t *box = modal_box(), *h = text(box, head), *r, *b;
	lv_obj_set_style_text_font(h, &lv_font_montserrat_18, 0);
	if (body && *body) text(box, body);
	yes_fn = fn;
	r = row(box);
	b = grey(button(r, "No, go back", modal_close_cb, NULL)); lv_obj_set_flex_grow(b, 1);
	b = button(r, yes, yes_cb, NULL); lv_obj_set_flex_grow(b, 1);
	if (danger) red(b);
}

/* the pairing code, top right on every screen */
static void code_tick(lv_timer_t *t)
{
	char *c = run_file("nocry-pair-code"), s[32];
	(void)t;
	c[strcspn(c, "\r\n")] = 0;
	if (strlen(c) == 6) snprintf(s, sizeof s, "code  %.3s %s", c, c + 3); else s[0] = 0;
	if (strcmp(lv_label_get_text(code_lbl), s)) lv_label_set_text(code_lbl, s);
	free(c);
	if (want_calibration) { extern void calibrate_screen(void); want_calibration = 0; depth = 1; go(calibrate_screen); }
}

/* ---- a job: something long that runs on the board, with progress and a way to stop -- */
static lv_obj_t *job_what, *job_bar, *job_msg, *job_btns;
static char job_risk[400]; static int job_over; static screen_fn job_next;

static void job_cancel_yes(void) { start(NULL, NULL, "nocry-rescue-do", "cancel", NULL); lv_label_set_text(job_msg, "stopping at the next safe point..."); }
static void job_cancel_cb(lv_event_t *e)
{
	(void)e;
	ask("Stop this?", job_risk[0] ? job_risk : "It stops at the next safe point.", "Stop it", 1, job_cancel_yes);
}
static void job_done_cb(lv_event_t *e)
{
	screen_fn n = job_next;
	(void)e;
	depth--;			/* the job screen itself */
	if (n && lv_event_get_user_data(e)) go(n); else show();
}
static void job_answer(int rc, char *out)
{
	const char *st = kv(out, "state"); long long done = atoll(kv(out, "done")), total = atoll(kv(out, "total")); char line[200];
	(void)rc;
	if (job_over) return;
	lv_label_set_text(job_what, kv(out, "what"));
	snprintf(job_risk, sizeof job_risk, "%s", kv(out, "risk"));
	if (total > 1) {
		lv_bar_set_value(job_bar, (int)(done * 1000 / total), LV_ANIM_OFF);
		snprintf(line, sizeof line, "%lld of %lld MiB    %s", done >> 20, total >> 20, kv(out, "message"));
	} else snprintf(line, sizeof line, "%s", kv(out, "message"));
	if (!strcmp(st, "running")) { lv_label_set_text(job_msg, line); return; }
	job_over = 1;
	modal_close();			/* a "Stop this?" that is still open has nothing left to stop */
	lv_obj_clean(job_btns);
	if (!strcmp(st, "done")) {
		lv_bar_set_value(job_bar, 1000, LV_ANIM_OFF);
		snprintf(line, sizeof line, "Done.  %s", kv(out, "message"));
		wide(button(job_btns, job_next ? "Continue" : "OK", job_done_cb, (void *)1));
	} else {
		snprintf(line, sizeof line, "%s  %s", !strcmp(st, "cancelled") ? "Stopped." : "It did not work.", kv(out, "message"));
		if (job_risk[0] && strlen(line) + strlen(job_risk) + 3 < sizeof line) { strcat(line, "\n"); strcat(line, job_risk); }
		wide(button(job_btns, "OK", job_done_cb, NULL));
	}
	lv_label_set_text(job_msg, line);
}
static void job_tick(lv_timer_t *t) { (void)t; if (!job_over) start(job_answer, NULL, "nocry-rescue-do", "job", NULL); }
static void job_screen(void)
{
	set_title("Working");
	lv_obj_add_flag(back_btn, LV_OBJ_FLAG_HIDDEN);		/* it runs whether anyone watches or not */
	job_over = 0; job_risk[0] = 0;
	job_what = text(content, "Starting...");
	lv_obj_set_style_text_font(job_what, &lv_font_montserrat_18, 0);
	job_bar = lv_bar_create(content); lv_obj_set_size(job_bar, lv_pct(100), 18); lv_bar_set_range(job_bar, 0, 1000);
	job_msg = text(content, "");
	job_btns = row(content);
	wide(grey(button(job_btns, "Stop", job_cancel_cb, NULL)));
	screen_timer = lv_timer_create(job_tick, 1000, NULL);
	job_tick(NULL);
}
/* the answer of a command that starts a job: watch it, or say why it did not start */
static void job_started(int rc, char *out)
{
	modal_close();
	if (rc) { job_next = NULL; message("That could not be started", out); return; }
	go(job_screen);
}

/* ---- Wi-Fi --------------------------------------------------------------------------- */
static char nets[24][72]; static int net_lock[24], net_count; static char chosen[72];
static lv_obj_t *pw_area;

static void wifi_done(int rc, char *out)
{
	char *st = run_file("nocry-wifi.state"), body[300];
	(void)out;
	modal_close();
	if (!rc && !strcmp(kv(st, "mode"), "client")) {
		snprintf(body, sizeof body, "The board is on \"%s\".\nIts pages: http://%s", kv(st, "ssid"), kv(st, "address"));
		depth = 1; show(); message("Joined", body);
	} else if (!strcmp(kv(st, "mode"), "ap") && rc) {
		snprintf(body, sizeof body, "Wrong password, or the network is out of reach. The board is back on its own network \"%s\".", kv(st, "ssid"));
		message("Could not join", body);
	} else if (rc) message("That did not work", out);
	else { depth = 1; show(); }
	free(st);
}
static void wifi_join(const char *pw)
{
	char what[120];
	snprintf(what, sizeof what, "Joining \"%s\"...\nThis takes up to a minute.", chosen);
	busy(what);
	if (pw && *pw) start(wifi_done, NULL, "nocry-wifi", "join", chosen, pw, NULL);
	else start(wifi_done, NULL, "nocry-wifi", "join", chosen, NULL);
}
static void pw_key(lv_event_t *e)
{
	if (lv_event_get_code(e) == LV_EVENT_CANCEL) { back(); return; }
	if (strlen(lv_textarea_get_text(pw_area)) < 8) { remark("Too short", "A Wi-Fi password has at least 8 characters."); return; }
	{ char pw[80]; snprintf(pw, sizeof pw, "%s", lv_textarea_get_text(pw_area)); wifi_join(pw); }
}
static void password_screen(void)
{
	lv_obj_t *kb; char t[100];
	snprintf(t, sizeof t, "Password for %s", chosen); set_title(t);
	lv_obj_set_style_pad_all(content, 4, 0);
	pw_area = lv_textarea_create(content);
	lv_textarea_set_one_line(pw_area, true); lv_textarea_set_max_length(pw_area, 63);
	lv_textarea_set_placeholder_text(pw_area, "password");
	lv_obj_set_width(pw_area, lv_pct(100));
	kb = lv_keyboard_create(content);
	lv_obj_set_size(kb, lv_pct(100), H - BAR - 52);
	lv_keyboard_set_textarea(kb, pw_area);
	lv_obj_add_event_cb(kb, pw_key, LV_EVENT_READY, NULL);
	lv_obj_add_event_cb(kb, pw_key, LV_EVENT_CANCEL, NULL);
}
static void net_cb(lv_event_t *e)
{
	int i = (int)(intptr_t)lv_event_get_user_data(e);
	snprintf(chosen, sizeof chosen, "%s", nets[i]);
	if (net_lock[i]) go(password_screen); else wifi_join(NULL);
}
static void scan_done(int rc, char *out)
{
	char *line, *save = NULL; lv_obj_t *list;
	modal_close(); net_count = 0;
	lv_obj_clean(content);
	if (rc || !*out) { text(content, "No networks found. The radio may need a moment: go back and try again."); return; }
	lv_obj_set_style_pad_all(content, 0, 0);
	list = lv_list_create(content); lv_obj_set_size(list, lv_pct(100), lv_pct(100));
	for (line = strtok_r(out, "\n", &save); line && net_count < 24; line = strtok_r(NULL, "\n", &save)) {
		char *lock = strchr(line, '\t'), *name = lock ? strchr(lock + 1, '\t') : NULL, label[110]; lv_obj_t *b;
		if (!name || !name[1]) continue;
		*lock = 0; *name++ = 0;
		snprintf(nets[net_count], sizeof nets[0], "%s", name);
		net_lock[net_count] = lock[1] && lock[1] != '-' && lock[1] != '0' && strcmp(lock + 1, "open");
		snprintf(label, sizeof label, "%s%s", name, net_lock[net_count] ? "" : "   (open)");
		b = lv_list_add_btn(list, LV_SYMBOL_WIFI, label);
		lv_obj_set_height(b, 42);
		lv_obj_add_event_cb(b, net_cb, LV_EVENT_CLICKED, (void *)(intptr_t)net_count);
		net_count++;
	}
}
static void scan_screen(void)
{
	set_title("Choose a network");
	busy("Looking for networks...");
	start(scan_done, NULL, "nocry-wifi", "scan", NULL);
}
static void scan_cb(lv_event_t *e) { (void)e; go(scan_screen); }
static void ap_yes(void) { busy("Opening the board's own network..."); start(wifi_done, NULL, "nocry-wifi", "ap", NULL); }
static void ap_cb(lv_event_t *e) { (void)e; ask("Use the board's own network?", "The board leaves the Wi-Fi network it is on and opens its own, \"nocry\".", "Yes", 0, ap_yes); }
static void save_done(int rc, char *out) { modal_close(); message(rc ? "Not saved" : "Saved", rc ? out : "The installed firmware will join this network by itself."); }
static void save_cb(lv_event_t *e) { (void)e; busy("Saving..."); start(save_done, NULL, "nocry-rescue-do", "wifi-save", NULL); }
static void wifi_screen(void)
{
	char *st = run_file("nocry-wifi.state"), s[300]; const char *mode = kv(st, "mode");
	set_title("Wi-Fi");
	if (!strcmp(mode, "client")) snprintf(s, sizeof s, "On the network \"%s\".\nFrom a phone or computer on it, open  http://%s", kv(st, "ssid"), kv(st, "address"));
	else if (!strcmp(mode, "ap")) snprintf(s, sizeof s, "The board has its own network:\n    name  %s      password  %s\nJoin it with a phone or computer and open  http://%s", kv(st, "ssid"), kv(st, "password"), kv(st, "address"));
	else if (!strcmp(mode, "joining")) snprintf(s, sizeof s, "Joining \"%s\"...", kv(st, "ssid"));
	else snprintf(s, sizeof s, "No Wi-Fi yet.");
	text(content, s);
	wide(button(content, LV_SYMBOL_WIFI "  Join a Wi-Fi network", scan_cb, NULL));
	if (strcmp(mode, "ap")) wide(grey(button(content, "Use the board's own network instead", ap_cb, NULL)));
	if (!strcmp(mode, "client")) wide(grey(button(content, "Save this network for the firmware", save_cb, NULL)));
	free(st);
}

/* ---- slots ---------------------------------------------------------------------------- */
static char slot_text[2][120], active_slot; static int slot_ok[2];
static int pick;	/* which slot a button was about */

static void switch_done(int rc, char *out) { modal_close(); if (rc) message("Not changed", out); else show(); }
static void switch_yes(void) { busy("Changing..."); start(switch_done, NULL, "nocry-rescue-do", "switch", pick ? "b" : "a", NULL); }
static void switch_cb(lv_event_t *e)
{
	char q[200];
	pick = (int)(intptr_t)lv_event_get_user_data(e);
	snprintf(q, sizeof q, "The printer will start %s from now on. Nothing is erased; you can change back.", slot_text[pick]);
	ask(pick ? "Start from slot B?" : "Start from slot A?", q, "Yes", 0, switch_yes);
}
static void copy_yes(void) { job_next = NULL; busy("Starting..."); start(job_started, NULL, "nocry-rescue-do", "copy", pick ? "b" : "a", NULL); }
static void copy_cb(lv_event_t *e)
{
	char q[320];
	pick = (int)(intptr_t)lv_event_get_user_data(e);
	snprintf(q, sizeof q, "Slot %c (%s) is replaced by a copy of slot %c (%s).", pick ? 'A' : 'B', slot_text[!pick], pick ? 'B' : 'A', slot_text[pick]);
	ask(pick ? "Copy slot B over slot A?" : "Copy slot A over slot B?", q, "Copy", 1, copy_yes);
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
		snprintf(s, sizeof s, "Slot %c%s", 'A' + i, active_slot == 'a' + i ? "   -   the printer starts this one" : "");
		h = text(card, s); lv_obj_set_style_text_font(h, &lv_font_montserrat_18, 0);
		if (active_slot == 'a' + i) lv_obj_set_style_border_color(card, lv_palette_main(LV_PALETTE_BLUE), 0);
		text(card, slot_text[i]);
		r = row(card);
		if (slot_ok[i] && active_slot != 'a' + i) button(r, "Start this one", switch_cb, (void *)(intptr_t)i);
		if (slot_ok[i]) grey(button(r, i ? "Copy to slot A" : "Copy to slot B", copy_cb, (void *)(intptr_t)i));
	}
}
static void slots_screen(void)
{
	set_title("Firmware slots");
	busy("Reading the board...");
	start(slots_done, NULL, "nocry-rescue-do", "status", NULL);
}

/* ---- install firmware ------------------------------------------------------------------ */
static struct fw { char key[24], name[64], version[24], date[16]; long long bytes; } fws[40]; static int fw_count, fw_pick;
static char fw_mode[8];

static void install_yes(void) { job_next = NULL; busy("Starting..."); start(job_started, NULL, "nocry-rescue-do", "fw-install", NULL); }
static void install_cb(lv_event_t *e)
{
	(void)e;
	ask("Install it now?", !strcmp(fw_mode, "slot") ? "The slot that is not in use is replaced. The printer keeps starting what it starts now."
		: "The firmware the printer starts is replaced. Settings and files are kept.", "Install", 1, install_yes);
}
static void discard_done(int rc, char *out) { (void)rc; (void)out; modal_close(); home(); }
static void discard_cb(lv_event_t *e) { (void)e; busy("One moment..."); start(discard_done, NULL, "nocry-rescue-do", "fw-discard", NULL); }
static void report_done(int rc, char *out)
{
	char s[300], *line, *save = NULL, *copy; lv_obj_t *r;
	(void)rc;
	modal_close();
	if (!*kv(out, "firmware")) { text(content, "Nothing is waiting to be installed."); return; }
	snprintf(fw_mode, sizeof fw_mode, "%s", kv(out, "mode"));
	snprintf(s, sizeof s, "%s", kv(out, "firmware"));
	lv_obj_set_style_text_font(text(content, s), &lv_font_montserrat_18, 0);
	snprintf(s, sizeof s, "On the board now: %s\nGoes into: slot %s\nTo write: %lld MiB", kv(out, "on_board"), kv(out, "slot"), atoll(kv(out, "to_write")) >> 20);
	text(content, s);
	if (*kv(out, "need_choice")) {
		text(content, "This board's printer type has to be chosen first; that is done on the rescue pages or with nocry on a computer.");
		wide(grey(button(content, "Do not install", discard_cb, NULL)));
		return;
	}
	copy = strdup(out);
	for (line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save))
		if (!strncmp(line, "note=", 5)) dim(text(content, line + 5));
	free(copy);
	r = row(content);
	lv_obj_set_flex_grow(grey(button(r, "Do not install", discard_cb, NULL)), 1);
	lv_obj_set_flex_grow(button(r, "Install", install_cb, NULL), 1);
}
static void report_screen(void)
{
	set_title("Check before installing");
	lv_obj_add_flag(back_btn, LV_OBJ_FLAG_HIDDEN);		/* install it or discard it */
	busy("One moment...");
	start(report_done, NULL, "nocry-rescue-do", "fw-report", NULL);
}
static void fetch(const char *mode)
{
	job_next = report_screen; depth = 1;
	busy("Starting...");
	start(job_started, NULL, "nocry-rescue-do", "fw-fetch", fws[fw_pick].key, fws[fw_pick].version, mode, NULL);
}
static void keep_cb(lv_event_t *e) { (void)e; fetch("keep"); }
static void other_cb(lv_event_t *e) { (void)e; fetch("slot"); }
static void where_screen(void)
{
	char s[200]; struct fw *f = &fws[fw_pick];
	set_title("Where should it go?");
	snprintf(s, sizeof s, "%s %s   (%lld MiB to download)", f->name, f->version, f->bytes >> 20);
	text(content, s);
	wide(button(content, "Replace the firmware that starts", keep_cb, NULL));
	dim(text(content, "Settings and files are kept."));
	wide(grey(button(content, "Into the other slot", other_cb, NULL)));
	dim(text(content, "The printer keeps starting what it starts now; switch when you are ready."));
	dim(text(content, "It is downloaded and checked first. Nothing is written until you confirm."));
}
static void fw_cb(lv_event_t *e) { fw_pick = (int)(intptr_t)lv_event_get_user_data(e); go(where_screen); }
static void fw_list_done(int rc, char *out);
static void refresh_cb(lv_event_t *e)
{
	(void)e;
	busy("Asking the internet for the current list...");
	start(fw_list_done, NULL, "nocry-rescue-do", "fw-list", "refresh", NULL);
}
static void fw_list_done(int rc, char *out)
{
	char *line, *save = NULL; lv_obj_t *list;
	modal_close(); lv_obj_clean(content); fw_count = 0;
	if (rc || !*out) {
		text(content, rc && *out ? out : "No list of firmware yet. The board needs to be on a Wi-Fi network with internet.");
		wide(button(content, LV_SYMBOL_REFRESH "  Get the list", refresh_cb, NULL));
		return;
	}
	lv_obj_set_style_pad_all(content, 0, 0);
	list = lv_list_create(content); lv_obj_set_size(list, lv_pct(100), lv_pct(100));
	for (line = strtok_r(out, "\n", &save); line && fw_count < 40; line = strtok_r(NULL, "\n", &save)) {
		struct fw *f = &fws[fw_count]; char *c[6], label[140]; int n = 0; lv_obj_t *b;
		for (c[n++] = line; n < 6 && (line = strchr(line, '\t')); c[n++] = line) *line++ = 0;
		if (n < 5) continue;
		snprintf(f->key, sizeof f->key, "%s", c[0]); snprintf(f->name, sizeof f->name, "%s", c[1]);
		snprintf(f->version, sizeof f->version, "%s", c[2]); snprintf(f->date, sizeof f->date, "%s", c[3]); f->bytes = atoll(c[4]);
		snprintf(label, sizeof label, "%s  %s    %s", f->name, f->version, f->date);
		b = lv_list_add_btn(list, NULL, label); lv_obj_set_height(b, 42);
		lv_obj_add_event_cb(b, fw_cb, LV_EVENT_CLICKED, (void *)(intptr_t)fw_count);
		fw_count++;
	}
	lv_obj_set_height(lv_list_add_btn(list, LV_SYMBOL_REFRESH, "Get the current list from the internet"), 42);
	lv_obj_add_event_cb(lv_obj_get_child(list, -1), refresh_cb, LV_EVENT_CLICKED, NULL);
}
static void install_screen(void)
{
	set_title("Install firmware");
	busy("One moment...");
	start(fw_list_done, NULL, "nocry-rescue-do", "fw-list", NULL);
}

/* ---- repair ---------------------------------------------------------------------------- */
static const char *repair_arg[2];
static void repair_yes(void) { job_next = NULL; busy("Starting..."); start(job_started, NULL, "nocry-rescue-do", repair_arg[0], repair_arg[1], NULL); }
static void mods_cb(lv_event_t *e)
{
	(void)e; repair_arg[0] = "reset"; repair_arg[1] = "mods";
	ask("Remove system modifications?", "Everything changed in the firmware's own files is undone: added programs, root access tools, changed settings of the system. Your print files and printer settings stay.", "Remove them", 1, repair_yes);
}
static void all_cb(lv_event_t *e)
{
	(void)e; repair_arg[0] = "reset"; repair_arg[1] = "all";
	ask("Factory reset?", "All settings and all files on the printer are erased, as when it was new. The rescue system stored on the board is erased with them and has to be stored again from a computer.", "Erase everything", 1, repair_yes);
}
static void fsck_data_cb(lv_event_t *e) { (void)e; repair_arg[0] = "check"; repair_arg[1] = "data"; ask("Check the files?", "The file system with your print files is checked and repaired where needed.", "Check", 0, repair_yes); }
static void fsck_set_cb(lv_event_t *e) { (void)e; repair_arg[0] = "check"; repair_arg[1] = "settings"; ask("Check the settings?", "The file system with the firmware's settings is checked and repaired where needed.", "Check", 0, repair_yes); }
static void health_done(int rc, char *out)
{
	char s[400];
	modal_close();
	if (rc) { message("Could not read it", out); return; }
	snprintf(s, sizeof s, "Chip: %s\nWear: %s and %s\nSpare blocks: %s\nFiles: %s", kv(out, "chip"), kv(out, "wear_a"), kv(out, "wear_b"), kv(out, "spare"), kv(out, "data_state"));
	message("Storage", s);
}
static void health_cb(lv_event_t *e) { (void)e; busy("Reading..."); start(health_done, NULL, "nocry-rescue-do", "health", NULL); }
static void repair_screen(void)
{
	set_title("Repair");
	wide(grey(button(content, "Check the file system with your files", fsck_data_cb, NULL)));
	wide(grey(button(content, "Check the file system with the settings", fsck_set_cb, NULL)));
	wide(grey(button(content, "Storage health", health_cb, NULL)));
	wide(red(button(content, "Remove system modifications", mods_cb, NULL)));
	wide(red(button(content, "Factory reset", all_cb, NULL)));
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
	about_add("Rescue system on the board: %s\n", !strcmp(s, "yes") ? "stored and intact" : !strcmp(s, "damaged") ? "stored, but damaged: store it again" : "not stored");
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
	about_add("Model: %s\nMainboard: %s\nSlot A: %s\nSlot B: %s\nStarts: slot %c\n", kv(out, "model"), kv(out, "mainboard"), kv(out, "a_firmware"), kv(out, "b_firmware"), kv(out, "active")[0] == 'b' ? 'B' : 'A');
	if (*note) about_add("Loader's note: %s\n", note);
	start(about_loader, NULL, "nocry-rescue-do", "loader", NULL);
}
void calibrate_screen(void);
static void cal_cb(lv_event_t *e) { (void)e; go(calibrate_screen); }
static void about_screen(void)
{
	set_title("About this board");
	about[0] = 0;
	about_lbl = text(content, "");
	wide(grey(button(content, "Calibrate the touch screen", cal_cb, NULL)));
	dim(text(content, "If touches land in the wrong place: hold a finger anywhere for ten seconds."));
	busy("Reading the board...");
	start(about_status, NULL, "nocry-rescue-do", "status", NULL);
}

/* ---- calibration: three crosses -------------------------------------------------------- */
static const lv_point_t cal_at[3] = { { 48, 40 }, { W - 48, H / 2 }, { W / 2, H - 40 } };
static int cal_step, cal_wait_up; static long cal_sx, cal_sy, cal_n; static int cal_rx[3], cal_ry[3];
static lv_obj_t *cal_cross, *cal_text;

static void cal_store(void)
{
	char p[128], d[160]; FILE *f;
	snprintf(p, sizeof p, "%s/nocry-touch.cal", RUN);
	f = fopen(p, "w"); if (!f) return;
	fprintf(f, "%.9g %.9g %.9g %.9g %.9g %.9g\n", cal[0], cal[1], cal[2], cal[3], cal[4], cal[5]); fclose(f);
	snprintf(d, sizeof d, "nocry-rescue/touch.cal");
	start(NULL, p, "nocry-rescue-do", "put", d, NULL);	/* kept if the rescue system is stored on the board */
}
static int cal_solve(void)
{
	double x0 = cal_rx[0], y0 = cal_ry[0], x1 = cal_rx[1], y1 = cal_ry[1], x2 = cal_rx[2], y2 = cal_ry[2];
	double det = (x0 - x2) * (y1 - y2) - (x1 - x2) * (y0 - y2); int i;
	if (det > -1000 && det < 1000) return -1;		/* the three touches were nearly in one place or one line */
	for (i = 0; i < 2; i++) {
		double s0 = i ? cal_at[0].y : cal_at[0].x, s1 = i ? cal_at[1].y : cal_at[1].x, s2 = i ? cal_at[2].y : cal_at[2].x;
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
	snprintf(s, sizeof s, "Press the cross and hold it a moment   (%d of 3)", cal_step + 1);
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
		home();
		return;
	}
	if (cal_wait_up) return;
	if (cal_n < 40) { cal_sx += raw_x; cal_sy += raw_y; cal_n++; }
	if (cal_n == 12) lv_label_set_text(cal_text, "Good. Let go.");
}
void calibrate_screen(void)
{
	lv_obj_t *l;
	set_title("Touch calibration");
	cal_ok = 0;				/* nothing can be pressed by accident meanwhile */
	cal_step = 0;
	modal_close();
	modal = lv_obj_create(lv_layer_top());		/* over everything; removed with the screen */
	lv_obj_remove_style_all(modal); lv_obj_set_size(modal, W, H);
	lv_obj_set_style_bg_color(modal, lv_color_hex(0x101418), 0); lv_obj_set_style_bg_opa(modal, LV_OPA_COVER, 0);
	cal_text = lv_label_create(modal); lv_obj_align(cal_text, LV_ALIGN_CENTER, 0, -34);
	cal_cross = lv_obj_create(modal); lv_obj_remove_style_all(cal_cross); lv_obj_set_size(cal_cross, 40, 40);
	l = lv_label_create(cal_cross); lv_label_set_text(l, LV_SYMBOL_PLUS); lv_obj_set_style_text_font(l, &lv_font_montserrat_28, 0);
	lv_obj_set_style_text_color(l, lv_palette_main(LV_PALETTE_YELLOW), 0); lv_obj_center(l);
	cal_place();
	screen_timer = lv_timer_create(cal_tick, 25, NULL);
}

/* ---- home ------------------------------------------------------------------------------ */
static lv_obj_t *home_net;
static void home_tick(lv_timer_t *t)
{
	char *st = run_file("nocry-wifi.state"), s[200]; const char *mode = kv(st, "mode");
	(void)t;
	if (!strcmp(mode, "client")) snprintf(s, sizeof s, LV_SYMBOL_WIFI "  %s      http://%s", kv(st, "ssid"), kv(st, "address"));
	else if (!strcmp(mode, "ap")) snprintf(s, sizeof s, LV_SYMBOL_WIFI "  own network \"%s\", password %s      http://%s", kv(st, "ssid"), kv(st, "password"), kv(st, "address"));
	else if (!strcmp(mode, "joining")) snprintf(s, sizeof s, LV_SYMBOL_WIFI "  joining %s...", kv(st, "ssid"));
	else snprintf(s, sizeof s, LV_SYMBOL_WIFI "  no Wi-Fi yet");
	if (strcmp(lv_label_get_text(home_net), s)) lv_label_set_text(home_net, s);
	free(st);
}
static void restart_done(int rc, char *out) { (void)rc; (void)out; busy("Starting the printer..."); }
static void restart_yes(void) { busy("Starting the printer..."); start(restart_done, NULL, "nocry-rescue-do", "restart", NULL); }
static void restart_cb(lv_event_t *e) { (void)e; ask("Leave the rescue system?", "The printer restarts and starts its firmware.", "Start the printer", 0, restart_yes); }
static void go_cb(lv_event_t *e) { go((screen_fn)lv_event_get_user_data(e)); }
static void tile(lv_obj_t *grid, const char *icon, const char *label, lv_event_cb_t cb, void *arg)
{
	lv_obj_t *b = lv_btn_create(grid), *i = lv_label_create(b), *l = lv_label_create(b);
	lv_obj_set_size(b, 148, 78);
	lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_label_set_text(i, icon); lv_obj_set_style_text_font(i, &lv_font_montserrat_28, 0);
	lv_label_set_text(l, label);
	lv_obj_set_style_bg_color(b, lv_color_hex(0x2c3440), 0);
	lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, arg);
}
static void home_screen(void)
{
	lv_obj_t *grid = row(content);
	set_title("nocry rescue");
	lv_obj_set_style_pad_all(content, 6, 0);
	lv_obj_set_style_pad_column(grid, 6, 0); lv_obj_set_style_pad_row(grid, 6, 0);
	lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	tile(grid, LV_SYMBOL_WIFI, "Wi-Fi", go_cb, (void *)wifi_screen);
	tile(grid, LV_SYMBOL_SHUFFLE, "Firmware slots", go_cb, (void *)slots_screen);
	tile(grid, LV_SYMBOL_DOWNLOAD, "Install firmware", go_cb, (void *)install_screen);
	tile(grid, LV_SYMBOL_SETTINGS, "Repair", go_cb, (void *)repair_screen);
	tile(grid, LV_SYMBOL_LIST, "About this board", go_cb, (void *)about_screen);
	tile(grid, LV_SYMBOL_POWER, "Start the printer", restart_cb, NULL);
	home_net = dim(text(content, ""));
	lv_obj_set_style_pad_left(home_net, 4, 0);
	home_tick(NULL);
	screen_timer = lv_timer_create(home_tick, 1000, NULL);
}

/* ---- start ----------------------------------------------------------------------------- */
static void frame_build(void)
{
	lv_obj_t *scr = lv_scr_act(), *bar = lv_obj_create(scr);
	lv_obj_set_style_bg_color(scr, lv_color_hex(0x101418), 0);
	lv_obj_remove_style_all(bar); lv_obj_set_size(bar, W, BAR); lv_obj_set_pos(bar, 0, 0);
	lv_obj_set_style_bg_color(bar, lv_color_hex(0x1c232c), 0); lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
	back_btn = lv_btn_create(bar); lv_obj_set_size(back_btn, 54, BAR); lv_obj_set_pos(back_btn, 0, 0);
	lv_obj_set_style_radius(back_btn, 0, 0); lv_obj_set_style_bg_color(back_btn, lv_color_hex(0x38424e), 0);
	lv_obj_set_style_shadow_width(back_btn, 0, 0);
	{ lv_obj_t *l = lv_label_create(back_btn); lv_label_set_text(l, LV_SYMBOL_LEFT); lv_obj_center(l); }
	lv_obj_add_event_cb(back_btn, back_cb, LV_EVENT_CLICKED, NULL);
	title = lv_label_create(bar); lv_obj_set_style_text_font(title, &lv_font_montserrat_18, 0);
	lv_obj_align(title, LV_ALIGN_LEFT_MID, 64, 0);
	lv_label_set_long_mode(title, LV_LABEL_LONG_DOT); lv_obj_set_width(title, 290);
	code_lbl = lv_label_create(bar); lv_label_set_text(code_lbl, "");
	lv_obj_align(code_lbl, LV_ALIGN_RIGHT_MID, -8, 0);
	lv_obj_set_style_text_color(code_lbl, lv_palette_lighten(LV_PALETTE_YELLOW, 1), 0);
	content = lv_obj_create(scr);
	lv_obj_remove_style_all(content);
	lv_obj_set_size(content, W, H - BAR); lv_obj_set_pos(content, 0, BAR);
	lv_obj_set_style_pad_all(content, 8, 0); lv_obj_set_style_pad_row(content, 8, 0);
	lv_obj_add_flag(content, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_scroll_dir(content, LV_DIR_VER);
}
static void cal_load(void)
{
	char *s = run_file("nocry-touch.cal");
	if (sscanf(s, "%lf %lf %lf %lf %lf %lf", &cal[0], &cal[1], &cal[2], &cal[3], &cal[4], &cal[5]) == 6) cal_ok = 1;
	free(s);
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
	const char *fbdev = "/dev/fb0", *i2cdev = "/dev/i2c-4", *script = NULL; int i;

	for (i = 1; i + 1 < argc; i += 2) {
		if (!strcmp(argv[i], "--fb")) fbdev = argv[i + 1];
		else if (!strcmp(argv[i], "--i2c")) i2cdev = argv[i + 1];
		else if (!strcmp(argv[i], "--sim")) { script = argv[i + 1]; sim = 1; }
		else break;
	}
	if (i < argc) { fprintf(stderr, "usage: nocry-ui [--fb DEV] [--i2c DEV] | nocry-ui --sim SCRIPT\n"); return 2; }
	if (getenv("NOCRY_RUN")) RUN = getenv("NOCRY_RUN");
	signal(SIGPIPE, SIG_IGN);
	if (!sim) {
		if (screen_open(fbdev)) return 1;
		i2c = open(i2cdev, O_RDWR);
		if (i2c < 0 || ioctl(i2c, I2C_SLAVE, 0x48) < 0) { perror(i2cdev); i2c = -1; }
		cal_load();
	} else cal_ok = 1;

	lv_init();
	lv_disp_draw_buf_init(&db, frame, NULL, W * H);
	lv_disp_drv_init(&dd);
	dd.hor_res = W; dd.ver_res = H; dd.flush_cb = flush; dd.draw_buf = &db; dd.direct_mode = 1;
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
