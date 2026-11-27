/*
 * nocry-webd: the web pages of nocry-rescue (docs/nocry-rescue.md).
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * For someone with a phone or laptop and no nocry on it: join the board's own Wi-Fi network
 * "nocry" (the access point resolves every name to the board, so most devices show the page
 * by themselves), or open the board's address on the home network.
 *
 *   1. The pairing code from the board's screen, once. The browser then holds a session until
 *      the board restarts. Wrong codes cost two seconds; five in a row lock the door a minute.
 *   2. Wi-Fi: join a network (first and largest while the board is on its own network).
 *   3. The board: what each firmware slot holds, switch the slot that starts, copy a slot,
 *      remove system modifications, factory reset. Each change asks first and says what it
 *      costs; long ones run as a job with progress and a Cancel that says what stopping leaves.
 *
 * Plain HTTP: on a network shared with others the code and the session can be overheard.
 * A browser would not accept an encrypted page from a board without warnings on every visit;
 * nocry on a computer uses its own encrypted link instead.
 *
 * No JavaScript, no files, one request per connection. The work is done by
 * /usr/sbin/nocry-wifi and /usr/sbin/nocry-rescue-do, started with their arguments as they are
 * (never through a shell). Actions are POST only and the session cookie is SameSite=Strict.
 *
 * usage: nocry-webd [--address A] [--port N] [--wifi PATH] [--do PATH] [--code-file F] [--state F]
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static const char *wifi = "/usr/sbin/nocry-wifi", *doer = "/usr/sbin/nocry-rescue-do";
static const char *codefile = "/run/nocry-pair-code", *statefile = "/run/nocry-wifi.state";

struct buf { char *p; size_t n, cap; };
static void put(struct buf *b, const char *s, size_t n)
{
	if (b->n + n + 1 > b->cap) {
		b->cap = (b->n + n + 1) * 2;
		b->p = realloc(b->p, b->cap);
		if (!b->p) exit(1);
	}
	memcpy(b->p + b->n, s, n); b->n += n; b->p[b->n] = 0;
}
static void puts_(struct buf *b, const char *s) { put(b, s, strlen(s)); }
static void esc(struct buf *b, const char *s)		/* text into HTML, also inside attributes */
{
	for (; *s; s++) {
		switch (*s) {
		case '&': puts_(b, "&amp;"); break;
		case '<': puts_(b, "&lt;"); break;
		case '>': puts_(b, "&gt;"); break;
		case '"': puts_(b, "&quot;"); break;
		case '\'': puts_(b, "&#39;"); break;
		default: put(b, s, 1);
		}
	}
}


/* Runs a tool with up to three arguments; its output (both streams, at most max bytes) and
 * exit status. */
static char *run(const char *tool, const char *a1, const char *a2, const char *a3, size_t max, int *status)
{
	int fd[2], st = 0; pid_t pid; char *out = calloc(1, max + 1); size_t n = 0; ssize_t k;
	if (status) *status = -1;
	if (!out || pipe(fd)) return out;
	pid = fork();
	if (pid == 0) {
		dup2(fd[1], 1); dup2(fd[1], 2); close(fd[0]); close(fd[1]);
		execl(tool, tool, a1, a2, a3, (char *)NULL);
		_exit(127);
	}
	close(fd[1]);
	while (pid > 0 && n < max && (k = read(fd[0], out + n, max - n)) > 0) n += k;
	close(fd[0]);
	if (pid > 0) { waitpid(pid, &st, 0); if (status) *status = WIFEXITED(st) ? WEXITSTATUS(st) : -1; }
	out[n] = 0;
	while (n && (out[n - 1] == '\n' || out[n - 1] == '\r')) out[--n] = 0;
	return out;
}

/* Starts a tool on its own after `delay` seconds: it must outlive this server's connection
 * (a Wi-Fi change takes the network the request came through away). */
static void launch(int delay, const char *tool, const char *a1, const char *a2, const char *a3)
{
	pid_t pid = fork();
	if (pid == 0) {
		setsid();
		if (fork() == 0) {
			int fd; for (fd = 0; fd < 64; fd++) close(fd);
			if (open("/dev/null", O_RDWR) == 0 && dup(0) == 1 && dup(0) == 2) { /* quiet */ }
			if (delay) sleep(delay);
			execl(tool, tool, a1, a2, a3, (char *)NULL);
		}
		_exit(0);
	}
	if (pid > 0) waitpid(pid, NULL, 0);
}

static void field(const char *text, const char *key, char *out, size_t max)	/* key=value lines */
{
	size_t kl = strlen(key); const char *p = text;
	out[0] = 0;
	while (p && *p) {
		if (!strncmp(p, key, kl) && p[kl] == '=') {
			size_t n = strcspn(p + kl + 1, "\n");
			if (n >= max) n = max - 1;
			memcpy(out, p + kl + 1, n); out[n] = 0;
			return;
		}
		p = strchr(p, '\n'); if (p) p++;
	}
}

static char *slurp(const char *path, size_t max)
{
	FILE *f = fopen(path, "r"); char *b = calloc(1, max + 1);
	if (f && b) { size_t n = fread(b, 1, max, f); b[n] = 0; }
	if (f) fclose(f);
	return b;
}

static int hexv(int c) { return isdigit(c) ? c - '0' : isxdigit(c) ? (tolower(c) - 'a' + 10) : -1; }
/* value of `key` in an application/x-www-form-urlencoded body; 0 if absent or too long */
static int form(const char *body, const char *key, char *out, size_t max)
{
	size_t kl = strlen(key), n = 0; const char *p = body;
	out[0] = 0;
	while (p && *p) {
		if (!strncmp(p, key, kl) && p[kl] == '=') {
			for (p += kl + 1; *p && *p != '&'; p++) {
				int c = *p;
				if (c == '+') c = ' ';
				else if (c == '%' && hexv(p[1]) >= 0 && hexv(p[2]) >= 0) { c = hexv(p[1]) * 16 + hexv(p[2]); p += 2; }
				if (c == 0 || n + 1 >= max) { out[0] = 0; return 0; }
				out[n++] = c;
			}
			out[n] = 0;
			return 1;
		}
		p = strchr(p, '&'); if (p) p++;
	}
	return 0;
}

static int code_ok(const char *given)
{
	char *want = slurp(codefile, 32), a[16] = "", b_[16] = ""; size_t i, n = 0, m = 0; unsigned diff;
	for (i = 0; given[i] && n < 15; i++) if (isdigit((unsigned char)given[i])) a[n++] = given[i];
	for (i = 0; want && want[i] && m < 15; i++) if (isdigit((unsigned char)want[i])) b_[m++] = want[i];
	free(want);
	diff = (n != m) | (m < 4);
	for (i = 0; i < 15; i++) diff |= (unsigned char)(a[i] ^ b_[i]);
	return !diff;
}


/* Sends a tool's standard output as a download (no length known beforehand: the connection's
 * end is the file's end). */
static void stream(int s, const char *filename, const char *type, const char *tool, const char *a1, const char *a2)
{
	int fd[2]; pid_t pid; char h[400], buf[16384]; ssize_t k; int n;
	struct timeval tv = { 120, 0 };
	if (pipe(fd)) return;
	pid = fork();
	if (pid == 0) {
		int nul = open("/dev/null", O_WRONLY);
		dup2(fd[1], 1); if (nul >= 0) dup2(nul, 2); close(fd[0]); close(fd[1]);
		execl(tool, tool, a1, a2, (char *)NULL);
		_exit(127);
	}
	close(fd[1]);
	setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);	/* a slow link is not an error */
	n = snprintf(h, sizeof h, "HTTP/1.0 200 OK\r\nContent-Type: %s\r\nContent-Disposition: attachment; filename=\"%s\"\r\n"
		     "Cache-Control: no-store\r\nConnection: close\r\n\r\n", type, filename);
	if (write(s, h, n) == n)
		while ((k = read(fd[0], buf, sizeof buf)) > 0) {
			ssize_t off = 0, w;
			while (off < k && (w = write(s, buf + off, k - off)) > 0) off += w;
			if (off < k) break;
		}
	close(fd[0]);
	if (pid > 0) { kill(pid, SIGTERM); waitpid(pid, NULL, 0); }
}

/* Runs a tool with `data` as its standard input; its messages and exit status. */
static char *feed(const char *tool, const char *a1, const char *a2, const char *data, size_t len, int *status)
{
	int in[2], out[2], st = 0; pid_t pid; char *msg = calloc(1, 513); size_t n = 0, off = 0; ssize_t k;
	*status = -1;
	if (!msg || pipe(in) || pipe(out)) return msg;
	pid = fork();
	if (pid == 0) {
		dup2(in[0], 0); dup2(out[1], 1); dup2(out[1], 2);
		close(in[0]); close(in[1]); close(out[0]); close(out[1]);
		execl(tool, tool, a1, a2, (char *)NULL);
		_exit(127);
	}
	close(in[0]); close(out[1]);
	while (off < len && (k = write(in[1], data + off, len - off)) > 0) off += k;
	close(in[1]);
	while (n < 512 && (k = read(out[0], msg + n, 512 - n)) > 0) n += k;
	close(out[0]);
	if (pid > 0) { waitpid(pid, &st, 0); *status = (WIFEXITED(st) && off == len) ? WEXITSTATUS(st) : -1; }
	msg[n] = 0;
	while (n && (msg[n - 1] == '\n' || msg[n - 1] == '\r')) msg[--n] = 0;
	return msg;
}

/* text for a URL query value */
static void urlenc(struct buf *b, const char *s)
{
	char t[4];
	for (; *s; s++) {
		if (isalnum((unsigned char)*s) || strchr("-_.~/", *s)) put(b, s, 1);
		else { snprintf(t, sizeof t, "%%%02X", (unsigned char)*s); puts_(b, t); }
	}
}

/* ------------------------------------------------------------------ sessions -- */

#define NSESS 8
static char sessions[NSESS][33];
static int next_session, wrong_codes;
static time_t locked_until;

static const char *new_session(void)
{
	unsigned char r[16]; int fd = open("/dev/urandom", O_RDONLY), i; char *s = sessions[next_session++ % NSESS];
	if (fd < 0 || read(fd, r, sizeof r) != sizeof r) { if (fd >= 0) close(fd); return NULL; }
	close(fd);
	for (i = 0; i < 16; i++) sprintf(s + 2 * i, "%02x", r[i]);
	return s;
}

static int signed_in(const char *req)
{
	const char *c = req; int i;
	while ((c = strstr(c, "nocry="))) {
		c += 6;
		for (i = 0; i < NSESS; i++)
			if (sessions[i][0] && !strncmp(c, sessions[i], 32) && !isxdigit((unsigned char)c[32])) return 1;
	}
	return 0;
}

/* ------------------------------------------------------------------ pages -- */

static const char *HEAD =
	"<!doctype html><html lang=en><meta charset=utf-8>"
	"<meta name=viewport content=\"width=device-width,initial-scale=1\">"
	"<title>nocry rescue</title><style>"
	"body{font:16px/1.45 system-ui,sans-serif;margin:0 auto;max-width:34em;padding:1em;color:#111;background:#f4f5f7}"
	"h1{font-size:1.25em;margin:.2em 0 .6em}h2{font-size:1.05em;margin:0 0 .4em}p{margin:.5em 0}"
	".c{background:#fff;border:1px solid #d5d8dd;border-radius:8px;padding:.9em 1em;margin:0 0 .9em}"
	".hi{border:2px solid #1463c2}"
	"label{display:block;margin:.7em 0 .2em;font-weight:600}"
	"input[type=text],input[type=password]{width:100%;box-sizing:border-box;font:inherit;padding:.55em;border:1px solid #888;border-radius:5px;background:#fff;color:#111}"
	".n{display:flex;align-items:center;gap:.6em;padding:.5em .2em;border-bottom:1px solid #e3e5e8;font-weight:400;margin:0}"
	".n small{margin-left:auto;color:#555;white-space:nowrap}"
	"button,.b{display:inline-block;font:inherit;font-weight:600;padding:.6em 1em;margin:.5em .4em 0 0;border:0;border-radius:5px;background:#1463c2;color:#fff;text-decoration:none;cursor:pointer}"
	".w{width:100%;margin-right:0;padding:.8em;text-align:center;box-sizing:border-box}"
	".q{background:#e6e8ec;color:#111}.d{background:#b3261e}"
	".e{background:#fde8e8;border:1px solid #c33;padding:.6em;border-radius:5px}.ok{background:#e7f5ea;border:1px solid #3a8a4d;padding:.6em;border-radius:5px}"
	".t{color:#555;font-size:.9em}form{display:inline}"
	".bar{height:.9em;background:#e3e5e8;border-radius:5px;overflow:hidden}.bar i{display:block;height:100%;background:#1463c2}"
	"@media(prefers-color-scheme:dark){body{background:#0f1114;color:#eee}.c{background:#1a1d21;border-color:#33373d}"
	"input[type=text],input[type=password]{background:#0f1114;color:#eee;border-color:#555}.q{background:#33373d;color:#eee}"
	".n{border-color:#33373d}.n small,.t{color:#aaa}.e{background:#3a1616}.ok{background:#16301c}.bar{background:#33373d}}"
	"</style>";

static void head(struct buf *b, const char *extra)
{
	puts_(b, HEAD);
	if (extra) puts_(b, extra);
	puts_(b, "<h1>nocry rescue</h1>");
}
static void note(struct buf *b, const char *cls, const char *text)
{
	if (!text || !*text) return;
	puts_(b, "<p class="); puts_(b, cls); puts_(b, ">"); esc(b, text); puts_(b, "</p>");
}

static void page_login(struct buf *b, const char *error)
{
	head(b, NULL);
	puts_(b, "<div class=c><h2>Pairing code</h2>");
	note(b, "e", error);
	puts_(b, "<p>Rescue system active. Enter the six-digit pairing code shown on the board's screen.</p>"
		 "<form method=post action=/login><input type=text name=code inputmode=numeric maxlength=7 autocomplete=off autofocus "
		 "style=\"font-size:1.6em;letter-spacing:.2em;text-align:center\">"
		 "<button class=w>Continue</button></form>"
		 "<p class=t>The pairing code confirms physical access. Requested once per session; it changes at every boot of the board.</p></div>");
}

static void wifi_card(struct buf *b, const char *st)
{
	char mode[32], ssid[128], addr[64], src[160], hint[32];
	field(st, "mode", mode, sizeof mode); field(st, "ssid", ssid, sizeof ssid); field(st, "address", addr, sizeof addr);
	field(st, "source", src, sizeof src); field(st, "hint", hint, sizeof hint);
	if (!strcmp(mode, "ap")) {
		puts_(b, "<div class=\"c hi\"><h2>Access point mode</h2>");
		if (!strncmp(src, "failed:", 7)) {
			puts_(b, "<p class=e>Connection to <b>"); esc(b, src + 7);
			puts_(b, "</b> failed: authentication failed or the network is out of range.");
			if (!strncmp(hint, "nonascii", 8))
				puts_(b, " <b>The password contained a non-ASCII character</b>, such as a typographic apostrophe "
					 "(&rsquo;) instead of a straight one ('). Phone keyboards substitute these automatically: hold the apostrophe key and select the straight one.");
			puts_(b, "</p>");
		}
		puts_(b, "<p>The board is running its own access point: slow, no internet access. On a Wi-Fi network it is reachable by "
			 "nocry on a computer, and transfers are many times faster.</p>"
			 "<a class=\"b w\" href=/wifi>Connect to a Wi-Fi network</a></div>");
	} else if (!strcmp(mode, "client")) {
		puts_(b, "<div class=c><h2>Wi-Fi</h2><p>Connected to <b>"); esc(b, ssid); puts_(b, "</b>, address "); esc(b, addr);
		puts_(b, ".</p><a class=\"b q\" href=/wifi>Change network</a></div>");
	} else {
		puts_(b, "<div class=c><h2>Wi-Fi</h2><p>"); esc(b, !strcmp(mode, "joining") ? "Connecting\xe2\x80\xa6" : "Not connected.");
		puts_(b, "</p><a class=\"b q\" href=/wifi>Networks</a></div>");
	}
}

/* the job card; returns 1 while a job runs (the page then reloads by itself) */
static int job_card(struct buf *b, const char *job)
{
	char state[16], what[96], done[24], total[24], msg[160];
	unsigned long long d, t; int pct;
	field(job, "state", state, sizeof state);
	if (!state[0] || !strcmp(state, "none")) return 0;
	field(job, "what", what, sizeof what); field(job, "done", done, sizeof done); field(job, "total", total, sizeof total);
	field(job, "message", msg, sizeof msg);
	d = strtoull(done, NULL, 10); t = strtoull(total, NULL, 10); pct = t ? (int)(d * 100 / t) : 0;
	if (!strcmp(state, "running")) {
		char n[160];
		puts_(b, "<div class=\"c hi\"><h2>"); esc(b, what); puts_(b, "</h2>");
		snprintf(n, sizeof n, "<div class=bar><i style=\"width:%d%%\"></i></div><p>%d%% &middot; %llu of %llu MB", pct, pct, d >> 20, t >> 20);
		puts_(b, n);
		if (msg[0]) { puts_(b, " &middot; "); esc(b, msg); }
		puts_(b, "</p><p class=t>This page refreshes automatically. Keep the board powered.</p>"
			 "<form method=post action=/job/cancel><button class=q>Cancel job</button></form></div>");
		return 1;
	}
	puts_(b, "<div class=c><h2>"); esc(b, what); puts_(b, "</h2><p class=");
	puts_(b, !strcmp(state, "done") ? "ok>Completed: " : !strcmp(state, "cancelled") ? "e>Cancelled: " : "e>Failed: ");
	esc(b, msg[0] ? msg : state);
	if (strcmp(state, "done")) {
		char risk[220]; field(job, "risk", risk, sizeof risk);
		if (risk[0]) { puts_(b, ". "); esc(b, risk); }
	}
	puts_(b, "</p></div>");
	return 0;
}

static void slot_row(struct buf *b, const char *st, char letter, int active, int busy)
{
	char key[16], fw[128], ok[8], L[2] = { (char)toupper(letter), 0 }, other[2] = { letter == 'a' ? 'B' : 'A', 0 }, l[2] = { letter, 0 };
	snprintf(key, sizeof key, "%c_firmware", letter); field(st, key, fw, sizeof fw);
	snprintf(key, sizeof key, "%c_complete", letter); field(st, key, ok, sizeof ok);
	puts_(b, "<p><b>Slot "); puts_(b, L); puts_(b, "</b>"); if (active) puts_(b, " &middot; <b>active</b>");
	puts_(b, "<br>"); esc(b, fw[0] ? fw : "unknown"); puts_(b, "</p>");
	if (busy || strcmp(ok, "yes")) return;
	if (!active) {
		puts_(b, "<form method=post action=/slot/switch><input type=hidden name=slot value="); puts_(b, l);
		puts_(b, "><button>Set slot "); puts_(b, L); puts_(b, " active</button></form>");
	}
	puts_(b, "<form method=post action=/slot/copy><input type=hidden name=from value="); puts_(b, l);
	puts_(b, "><button class=q>Copy to slot "); puts_(b, other); puts_(b, "</button></form>");
}

static void page_home(struct buf *b, const char *flash, int flash_ok)
{
	char *st = slurp(statefile, 1024), *job = run(doer, "job", NULL, NULL, 2048, NULL), *bs = run(doer, "status", NULL, NULL, 4096, NULL);
	char state[16], active[8], model[64], notev[64]; int running;
	field(job, "state", state, sizeof state); running = !strcmp(state, "running");
	head(b, running ? "<meta http-equiv=refresh content=3>" : NULL);
	note(b, flash_ok ? "ok" : "e", flash);
	job_card(b, job);
	wifi_card(b, st);
	field(bs, "active", active, sizeof active); field(bs, "model", model, sizeof model); field(bs, "note", notev, sizeof notev);
	puts_(b, "<div class=c><h2>Firmware</h2>");
	if (model[0]) { puts_(b, "<p class=t>Model: "); esc(b, model); puts_(b, "</p>"); }
	if (!strncmp(notev, "nocryboot:failed=", 17) || !strncmp(notev, "nocryboot:incomplete=", 21) || !strncmp(notev, "nocryboot:unconfirmed=", 22)) {
		puts_(b, "<p class=e>Boot loader record, slot "); esc(b, strchr(notev, '=') + 1);
		puts_(b, !strncmp(notev, "nocryboot:incomplete", 20) ? ": firmware incomplete." : ": boot failed; the other slot was booted.");
		puts_(b, "</p>");
	}
	slot_row(b, bs, 'a', active[0] == 'a', running);
	puts_(b, "<hr style=\"border:0;border-top:1px solid #8884\">");
	slot_row(b, bs, 'b', active[0] == 'b', running);
	puts_(b, "<p class=t>Two firmware slots; the active slot boots. Settings and files (rootfs_data, userdata) are shared by both.</p>"
		 "<a class=\"b w\" href=/install>Install firmware</a></div>");
	puts_(b, "<div class=c><h2>More</h2>"
		 "<a class=\"b q w\" href=/repair>Repair: fsck, wipe system overlay, factory reset</a>"
		 "<a class=\"b q w\" href=/files>Files: browse, download, delete, upload</a>"
		 "<a class=\"b q w\" href=/board>Board: eMMC health, backup, logs, identity, reboot</a>"
		 "<a class=\"b q w\" href=/boot>Boot loader and boot logo</a></div>");
	free(st); free(job); free(bs);
}

static void back(struct buf *b) { puts_(b, "<a class=\"b q\" href=/>Back</a>"); }

static void page_repair(struct buf *b)
{
	char *job = run(doer, "job", NULL, NULL, 2048, NULL), state[16]; int running;
	field(job, "state", state, sizeof state); running = !strcmp(state, "running");
	head(b, running ? "<meta http-equiv=refresh content=3>" : NULL);
	job_card(b, job);
	if (!running) {
		puts_(b, "<div class=c><h2>Wipe system overlay</h2>"
			 "<p>Erases rootfs_data: all changes to the root file system (installed mods, helper scripts, system configuration). "
			 "userdata (files, print history, printer configuration) is preserved. First step when the firmware no longer boots after a modification. "
			 "(Creality firmware keeps these changes in rootfs_data; NebulaOS does not use it.)</p>"
			 "<form method=post action=/reset><input type=hidden name=kind value=mods><button class=q>Wipe system overlay</button></form></div>"
			 "<div class=c><h2>File system check (fsck)</h2>"
			 "<p>Power loss can corrupt the userdata or rootfs_data file system; the printer then hangs at the boot logo. "
			 "Runs e2fsck and repairs errors.</p>"
			 "<form method=post action=/check><input type=hidden name=which value=data><button class=q>Check userdata (fsck)</button></form>"
			 "<form method=post action=/check><input type=hidden name=which value=settings><button class=q>Check rootfs_data (fsck)</button></form></div>"
			 "<div class=c><h2>Factory reset</h2>"
			 "<p>Erases rootfs_data and userdata: all settings and files. The firmware in both slots and the serial number are preserved. "
			 "Back up first: <a href=/files>Files</a>.</p>"
			 "<form method=post action=/reset><input type=hidden name=kind value=all><button class=d>Factory reset</button></form></div>");
	}
	back(b);
	free(job);
}

static void page_board(struct buf *b, const char *flash, int flash_ok)
{
	char *h = run(doer, "health", NULL, NULL, 2048, NULL), *bs = run(doer, "status", NULL, NULL, 4096, NULL), *st = slurp(statefile, 1024);
	char v[96], w[96], mode[24];
	head(b, NULL);
	note(b, flash_ok ? "ok" : "e", flash);
	puts_(b, "<div class=c><h2>eMMC health</h2>");
	field(h, "wear_a", v, sizeof v); field(h, "wear_b", w, sizeof w);
	puts_(b, "<p>Wear (device life-time estimate, used): <b>"); esc(b, v); puts_(b, "&nbsp;%</b>");
	if (strcmp(v, w)) { puts_(b, " and <b>"); esc(b, w); puts_(b, "&nbsp;%</b> (type A / B)"); }
	field(h, "spare", v, sizeof v); puts_(b, ". Spare blocks: "); esc(b, v); puts_(b, ".");
	field(h, "chip", v, sizeof v); if (v[0]) { puts_(b, " <span class=t>(device "); esc(b, v); puts_(b, ")</span>"); }
	puts_(b, "</p><p class=t>Wear near or above 100&nbsp;% explains recurring file system corruption.</p>");
	field(h, "data_state", v, sizeof v);
	if (!strcmp(v, "readable")) {
		char t[24], u[24], f[24];
		field(h, "data_total_mb", t, sizeof t); field(h, "data_used_mb", u, sizeof u); field(h, "data_free_mb", f, sizeof f);
		puts_(b, "<p>userdata: "); esc(b, u); puts_(b, " MB used of "); esc(b, t); puts_(b, " MB, "); esc(b, f); puts_(b, " MB free.");
		if (atol(f) < 50) puts_(b, " <b>Nearly full: the firmware may fail to boot without free space. Delete files under <a href=/files>Files</a>.</b>");
		puts_(b, "</p>");
	} else puts_(b, "<p>userdata: no readable file system (blank after a reset, or corrupted; see <a href=/repair>Repair</a>).</p>");
	puts_(b, "</div><div class=c><h2>Downloads</h2>"
		 "<p><a class=\"b q\" href=/dl/logs>Logs</a> Printer and rescue system logs in one archive. Attach it to support requests.</p>"
		 "<p><a class=\"b q\" href=/dl/config>Printer configuration</a> The printer's configuration files. Download before a reset.</p>"
		 "<p><a class=\"b q\" href=/dl/identity>Identity record</a> The serial number and MAC address record as stored on the board. Keep it private.</p>"
		 "<p><a class=\"b q\" href=/dl/backup>Full backup</a> Full eMMC image, compressed (includes the identity record). "
		 "Several gigabytes are read and sent: an hour or more over Wi-Fi; keep this page open. "
		 "To restore: decompress it and use Restore backup in nocry on a computer.</p></div>");
	field(bs, "model", v, sizeof v); field(bs, "mainboard", w, sizeof w);
	puts_(b, "<div class=c><h2>Board</h2><p>Model: "); esc(b, v[0] ? v : "unknown"); puts_(b, ", mainboard "); esc(b, w[0] ? w : "unknown"); puts_(b, ".</p></div>");
	field(st, "mode", mode, sizeof mode);
	if (!strcmp(mode, "client")) {
		field(st, "ssid", v, sizeof v);
		puts_(b, "<div class=c><h2>Save Wi-Fi credentials to firmware</h2><p>Writes <b>"); esc(b, v);
		puts_(b, "</b> and its password to the installed firmware's Wi-Fi configuration; the printer then connects to it on a normal boot. "
			 "For a printer whose screen is unusable.</p><form method=post action=/wifi/save><button class=q>Save credentials to firmware</button></form></div>");
	}
	puts_(b, "<div class=c><h2>Reboot</h2><p>Leaves rescue and boots the active slot.</p>"
		 "<form method=post action=/restart><button>Reboot</button></form></div>");
	back(b);
	free(h); free(bs); free(st);
}

/* the folder `dir` of the data partition */
static void page_files(struct buf *b, const char *dir, const char *flash, int flash_ok)
{
	int st; char *ls = run(doer, "ls", dir[0] ? dir : "/", NULL, 65536, &st), *line, *save; int any = 0;
	head(b, NULL);
	note(b, flash_ok ? "ok" : "e", flash);
	puts_(b, "<div class=c><h2>Files: /"); esc(b, dir); puts_(b, "</h2>");
	if (st != 0) { note(b, "e", ls && *ls ? ls : "The folder cannot be read."); }
	else {
		puts_(b, "<p class=t>userdata (/usr/data in the firmware). Deletion is permanent.</p>");
		if (dir[0]) {
			char up[512]; char *sl;
			snprintf(up, sizeof up, "%s", dir); sl = strrchr(up, '/'); if (sl) *sl = 0; else up[0] = 0;
			puts_(b, "<div class=n><a href=\"/files?p="); urlenc(b, up); puts_(b, "\">&larr; up</a></div>");
		}
		for (line = strtok_r(ls, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
			char *t1 = strchr(line, '\t'), *t2 = t1 ? strchr(t1 + 1, '\t') : NULL, path[700], sz[32];
			unsigned long long bytes;
			if (!t2 || !t2[1]) continue;
			snprintf(path, sizeof path, "%s%s%s", dir, dir[0] ? "/" : "", t2 + 1);
			bytes = strtoull(t1 + 1, NULL, 10);
			if (bytes >= 10 << 20) snprintf(sz, sizeof sz, "%llu MB", bytes >> 20); else if (bytes >= 10 << 10) snprintf(sz, sizeof sz, "%llu KB", bytes >> 10); else snprintf(sz, sizeof sz, "%llu B", bytes);
			puts_(b, "<div class=n>");
			if (line[0] == 'd') { puts_(b, "<a href=\"/files?p="); urlenc(b, path); puts_(b, "\"><b>"); esc(b, t2 + 1); puts_(b, "/</b></a>"); }
			else { puts_(b, "<a href=\"/files/get?p="); urlenc(b, path); puts_(b, "\">"); esc(b, t2 + 1); puts_(b, "</a><small>"); puts_(b, sz); puts_(b, "</small>"); }
			puts_(b, "<form method=post action=/files/delete style=\"margin-left:"); puts_(b, line[0] == 'd' ? "auto" : ".6em");
			puts_(b, "\"><input type=hidden name=p value=\""); esc(b, path); puts_(b, "\"><button class=q style=\"margin:0;padding:.2em .6em\">Delete</button></form></div>");
			any = 1;
		}
		if (!any) puts_(b, "<p>Empty folder.</p>");
		puts_(b, "</div><div class=c><h2>Upload a file</h2><p class=t>E.g. a corrected printer.cfg. An existing file of the same name is overwritten. 8 MB maximum.</p>"
			 "<form method=post action=\"/files/upload?p="); urlenc(b, dir);
		puts_(b, "\" enctype=multipart/form-data><input type=file name=file><button class=q>Upload</button></form>");
	}
	puts_(b, "</div>"); back(b);
	free(ls);
}

static void mb(struct buf *b, const char *bytes)
{
	char t[32]; snprintf(t, sizeof t, "%llu MB", strtoull(bytes, NULL, 10) >> 20); puts_(b, t);
}

/* Install firmware: choose (the list for this board, or a file), check, then install. */
static void page_install(struct buf *b, const char *flash, int flash_ok)
{
	char *job = run(doer, "job", NULL, NULL, 2048, NULL), *rep = run(doer, "fw-report", NULL, NULL, 8192, NULL), state[16], what[96];
	char *st = slurp(statefile, 1024), mode[24]; int running;
	field(job, "state", state, sizeof state); field(job, "what", what, sizeof what); running = !strcmp(state, "running");
	field(st, "mode", mode, sizeof mode);
	head(b, running ? "<meta http-equiv=refresh content=3>" : NULL);
	note(b, flash_ok ? "ok" : "e", flash);
	if (running || strstr(what, "firmware")) job_card(b, job);
	if (running) { back(b); free(job); free(rep); free(st); return; }
	if (rep && strstr(rep, "firmware=")) {
		char fw[128], on[128], slot[8], tw[32], conv[160], need[32], m[16], *line, *save, *copy = strdup(rep);
		field(rep, "firmware", fw, sizeof fw); field(rep, "on_board", on, sizeof on); field(rep, "slot", slot, sizeof slot);
		field(rep, "to_write", tw, sizeof tw); field(rep, "converting", conv, sizeof conv); field(rep, "need_choice", need, sizeof need);
		field(rep, "mode", m, sizeof m);
		puts_(b, "<div class=\"c hi\"><h2>Review install</h2><p><b>"); esc(b, fw); puts_(b, "</b>");
		if (!strcmp(m, "slot")) { puts_(b, " into slot <b>"); esc(b, slot[0] == 'b' ? "B" : "A"); puts_(b, "</b>, the inactive slot. The active slot is not changed."); }
		else puts_(b, ", overwriting the active slot. Settings and files are preserved.");
		puts_(b, "</p><p>Installed: "); esc(b, on[0] > ' ' ? on : "no readable firmware"); puts_(b, ". To write: "); mb(b, tw); puts_(b, ".</p>");
		if (conv[0]) { puts_(b, "<p class=e>This changes the board's configured printer model: "); esc(b, conv); puts_(b, ".</p>"); }
		puts_(b, "<p class=t>");
		for (line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save))
			if (!strncmp(line, "note=", 5)) { esc(b, line + 5); puts_(b, "<br>"); }
		puts_(b, "</p>");
		if (need[0]) puts_(b, "<p class=e>This firmware supports several printer models and the board's model is not set. Use nocry on a computer for this install.</p>");
		else if (!strtoull(tw, NULL, 10)) puts_(b, "<p class=ok>This exact firmware is already installed. Nothing to write.</p>");
		else puts_(b, "<form method=post action=/install/go><button>Install</button></form>");
		puts_(b, "<form method=post action=/install/discard><button class=q>Discard</button></form></div>");
		free(copy);
	} else {
		int stl; char *ls = run(doer, "fw-list", NULL, NULL, 32768, &stl), *line, *save; int any = 0;
		puts_(b, "<div class=c><h2>Install firmware</h2>");
		if (!strcmp(mode, "ap"))
			puts_(b, "<p class=e>Access point mode: no internet access. To download firmware, <a href=/wifi>connect to a Wi-Fi network</a> first. "
				 "Upload from this device works in either mode.</p>");
		puts_(b, "<form method=post action=/install/fetch><label>Firmware catalog</label>");
		for (line = ls ? strtok_r(ls, "\n", &save) : NULL; line; line = strtok_r(NULL, "\n", &save)) {
			char *f[6]; int i;
			for (i = 0, f[0] = line; i < 5 && f[i]; i++) { f[i + 1] = strchr(f[i], '\t'); if (f[i + 1]) *f[i + 1]++ = 0; }
			if (i < 5 || !f[5]) continue;
			puts_(b, "<label class=n><input type=radio name=fw value=\""); esc(b, f[0]); puts_(b, "|"); esc(b, f[2]); puts_(b, "\"");
			if (!any) puts_(b, " checked");
			puts_(b, "> "); esc(b, f[1]); puts_(b, " "); esc(b, f[2]); puts_(b, "<small>"); esc(b, f[3]); puts_(b, ", "); mb(b, f[4]); puts_(b, "</small></label>");
			any = 1;
		}
		if (!any) puts_(b, "<p>No firmware catalog for this board (printer model not set, or this rescue system has no installer).</p>");
		puts_(b, "<label>Target slot</label>"
			 "<label class=n><input type=radio name=mode value=keep checked> Active slot: overwrites the active firmware; settings and files are preserved</label>"
			 "<label class=n><input type=radio name=mode value=slot> Inactive slot: the active slot is not changed</label>");
		if (any) puts_(b, "<button class=w>Download and verify</button>");
		puts_(b, "</form><form method=post action=/install/refresh><button class=q>Refresh catalog</button></form>"
			 "<p class=t>Nothing is written yet: the image is downloaded, verified and checked against this board, then shown for confirmation. "
			 "For a clean install, install first, then run Factory reset under Repair.</p></div>"
			 "<div class=c><h2>Upload firmware</h2><p class=t>A Creality update (.img), a .ingenic package, or a NebulaOS .img. 700 MB maximum.</p>");
		puts_(b, "<form method=post action=\"/install/upload?mode=keep\" enctype=multipart/form-data><input type=file name=file><button class=q>Upload and verify (active slot)</button></form>"
			 "<form method=post action=\"/install/upload?mode=slot\" enctype=multipart/form-data><input type=file name=file><button class=q>Upload and verify (inactive slot)</button></form></div>");
		free(ls);
	}
	back(b);
	free(job); free(rep); free(st);
}

static void pre(struct buf *b, const char *text)
{
	puts_(b, "<pre style=\"white-space:pre-wrap;font:inherit;margin:.4em 0\">"); esc(b, text ? text : ""); puts_(b, "</pre>");
}

static void page_boot(struct buf *b, const char *flash, int flash_ok)
{
	int s1, s2; char *lo = run(doer, "loader", NULL, NULL, 4096, &s1), *lg = run(doer, "logo", NULL, NULL, 4096, &s2);
	head(b, NULL);
	if (flash) { puts_(b, flash_ok ? "<div class=ok>" : "<div class=e>"); pre(b, flash); puts_(b, "</div><p></p>"); }
	puts_(b, "<div class=c><h2>Boot loader</h2><p class=t>The first code the board executes from eMMC. nocryboot is an open boot loader: it falls back to the other "
		 "firmware slot when one fails to boot, and enters USB boot mode when neither boots. The factory loader is kept on the board when replaced.</p>");
	pre(b, lo);
	if (s1 == 0) puts_(b, "<form method=post action=/boot/loader><input type=hidden name=what value=install><button class=q>Install nocryboot</button></form>"
			      "<form method=post action=/boot/loader><input type=hidden name=what value=factory><button class=q>Restore factory loader</button></form>");
	puts_(b, "</div><div class=c><h2>Boot logo</h2><p class=t>Shown for the first seconds after power-on.</p>");
	pre(b, lg);
	if (s2 == 0) puts_(b, "<form method=post action=/boot/logo><input type=hidden name=what value=nocry><button class=q>Use nocry logo</button></form>"
			      "<form method=post action=/boot/logo><input type=hidden name=what value=firmware><button class=q>Use firmware logo</button></form>");
	puts_(b, "</div>"); back(b);
	free(lo); free(lg);
}

static void page_wifi(struct buf *b, const char *error, const char *keep_ssid)
{
	char *st = slurp(statefile, 1024), *scan = run(wifi, "scan", NULL, NULL, 16384, NULL), *line, *save, mode[32]; int any = 0;
	field(st, "mode", mode, sizeof mode);
	head(b, NULL);
	puts_(b, "<div class=c><h2>Connect to a Wi-Fi network</h2>");
	note(b, "e", error);
	puts_(b, "<p class=t>This board supports 2.4&nbsp;GHz networks only.</p><form method=post action=/wifi/join><label>Network</label>");
	for (line = strtok_r(scan, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		char *t1 = strchr(line, '\t'), *t2 = t1 ? strchr(t1 + 1, '\t') : NULL; int sig;
		if (!t2 || !t2[1]) continue;
		sig = atoi(line);
		puts_(b, "<label class=n><input type=radio name=ssid value=\""); esc(b, t2 + 1); puts_(b, "\"");
		if ((keep_ssid && !strcmp(keep_ssid, t2 + 1)) || (!keep_ssid && !any)) puts_(b, " checked");
		puts_(b, "> "); esc(b, t2 + 1);
		puts_(b, "<small>"); puts_(b, !strncmp(t1 + 1, "open", 4) ? "open, " : "");
		puts_(b, sig >= -60 ? "strong" : sig >= -72 ? "medium" : "weak"); puts_(b, "</small></label>");
		any = 1;
	}
	if (!any) puts_(b, "<p>No networks found. Enter the SSID below.</p>");
	puts_(b, "<label for=o>Other SSID (hidden network)</label><input id=o type=text name=other maxlength=32 autocapitalize=none autocorrect=off>"
		 "<label for=p>Password</label><input id=p type=password name=pass maxlength=63 autocomplete=off>"
		 "<button class=w>Connect</button></form>"
		 "<p class=t>The board disconnects from its current network. If the connection fails, the access point \"nocry\" resumes within a minute.</p></div>");
	if (!strcmp(mode, "client"))
		puts_(b, "<div class=c><form method=post action=/wifi/own><button class=q>Switch to access point mode</button></form></div>");
	puts_(b, "<a class=\"b q\" href=/>Back</a>");
	free(st); free(scan);
}

static void page_moving(struct buf *b, const char *ssid)
{
	head(b, NULL);
	puts_(b, "<div class=c><h2>Changing network</h2>");
	if (ssid) {
		puts_(b, "<p class=ok>Connecting to <b>"); esc(b, ssid); puts_(b, "</b>.</p>"
			 "<p><b>On success,</b> the board's screen shows the SSID and the new address. Connect this device to that network "
			 "and open that address; nocry on a computer discovers the board automatically. The session stays valid.</p>"
			 "<p><b>On failure,</b> the access point \"nocry\" resumes within a minute: reconnect to it and check the password.</p>");
	} else {
		puts_(b, "<p class=ok>Starting the access point \"nocry\" (password nocry111).</p>"
			 "<p>Connect to it, then open http://192.168.77.1</p>");
	}
	puts_(b, "</div>");
}

/* a question before a change: the form is sent again with yes=1 */
static void page_confirm(struct buf *b, const char *title, const char *text, const char *action, const char *name, const char *value, const char *go, int danger)
{
	head(b, NULL);
	puts_(b, "<div class=c><h2>"); esc(b, title); puts_(b, "</h2><p>"); esc(b, text);
	puts_(b, "</p><form method=post action="); puts_(b, action); puts_(b, "><input type=hidden name=yes value=1>");
	if (name) { puts_(b, "<input type=hidden name="); puts_(b, name); puts_(b, " value=\""); esc(b, value); puts_(b, "\">"); }
	puts_(b, danger ? "<button class=d>" : "<button>"); esc(b, go); puts_(b, "</button></form><a class=\"b q\" href=/>Cancel</a></div>");
}

/* ------------------------------------------------------------------ requests -- */

static void reply(int s, const char *status, const char *extra, struct buf *b)
{
	char h[512]; size_t off = 0; ssize_t k;
	int n = snprintf(h, sizeof h, "HTTP/1.1 %s\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: %zu\r\n"
			 "Cache-Control: no-store\r\n%sConnection: close\r\n\r\n", status, b->n, extra ? extra : "");
	if (write(s, h, n) != n) return;
	while (off < b->n && (k = write(s, b->p + off, b->n - off)) > 0) off += k;
}
static void redirect(int s, const char *where)
{
	char h[160]; struct buf b = { 0 };
	snprintf(h, sizeof h, "Location: %s\r\n", where);
	puts_(&b, "<a href=/>continue</a>");
	reply(s, "303 See Other", h, &b);
	free(b.p);
}

#define UPLOAD_MAX (8u << 20)

/* The file in a multipart/form-data body: its bytes and its name. 0 if there is none. */
static int upload(char *headers, char *body, size_t blen, char **data, size_t *len, char *name, size_t namemax)
{
	char *ct = strcasestr(headers, "\nContent-Type: multipart/form-data"), *bd, delim[128], *p, *end, *fn, *e;
	size_t dl;
	if (!ct || !(bd = strstr(ct, "boundary="))) return 0;
	bd += 9; if (*bd == '"') bd++;
	dl = snprintf(delim, sizeof delim, "\r\n--%.*s", (int)strcspn(bd, "\"\r\n;"), bd);
	p = body;
	while (p && p < body + blen) {
		char *hend = strstr(p, "\r\n\r\n");
		if (!hend) return 0;
		*hend = 0;
		fn = strstr(p, "filename=\"");
		end = memmem(hend + 4, body + blen - (hend + 4), delim, dl);
		if (!end) return 0;
		if (fn && strstr(p, "name=\"file\"")) {
			fn += 10; e = strchr(fn, '"'); if (!e) return 0; *e = 0;
			if (strrchr(fn, '/')) fn = strrchr(fn, '/') + 1;
			if (strrchr(fn, '\\')) fn = strrchr(fn, '\\') + 1;
			if (!fn[0] || !strcmp(fn, ".") || !strcmp(fn, "..") || strlen(fn) >= namemax) return 0;
			snprintf(name, namemax, "%s", fn);
			*data = hend + 4; *len = end - (hend + 4);
			return 1;
		}
		p = end + dl;
	}
	return 0;
}

#define FW_MAX (700ull << 20)

/* A firmware file from the browser, straight into the file `path` (it is far too large for
 * memory). `have` bytes of the body are already in `body`; the rest comes from the socket.
 * The form has one field, the file, so everything after its headers up to the closing line is
 * the file. Returns 0 and the file's name on success. */
static int upload_to_file(int s, char *headers, char *body, size_t have, size_t clen, const char *path, char *name, size_t namemax)
{
	char *ct = strcasestr(headers, "\nContent-Type: multipart/form-data"), *bd, tail[160], *hend, *fn, *e, buf[65536];
	size_t tl, left; int fd; ssize_t k; off_t size; struct timeval tv = { 60, 0 };
	if (!ct || !(bd = strstr(ct, "boundary="))) return -1;
	bd += 9; if (*bd == '"') bd++;
	tl = snprintf(tail, sizeof tail, "\r\n--%.*s--\r\n", (int)strcspn(bd, "\"\r\n;"), bd);
	body[have] = 0;
	hend = strstr(body, "\r\n\r\n");		/* the part's own headers fit in what was read with the request */
	if (!hend || !(fn = strstr(body, "filename=\"")) || fn > hend) return -1;
	fn += 10; e = strchr(fn, '"'); if (!e || e > hend) return -1;
	*e = 0;
	if (strrchr(fn, '/')) fn = strrchr(fn, '/') + 1;
	if (strrchr(fn, '\\')) fn = strrchr(fn, '\\') + 1;
	if (!fn[0]) return -1;
	snprintf(name, namemax, "%s", fn);
	hend += 4;
	fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);	/* read back at the end to check the closing line */
	if (fd < 0) return -1;
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
	left = clen - have;
	if (write(fd, hend, have - (hend - body)) < 0) { close(fd); return -1; }
	while (left) {
		k = read(s, buf, left < sizeof buf ? left : sizeof buf);
		if (k <= 0 || write(fd, buf, k) != k) { close(fd); unlink(path); return -1; }
		left -= k;
	}
	/* cut the closing line off the end, after checking that it is there */
	size = lseek(fd, 0, SEEK_END);
	if (size < (off_t)tl || pread(fd, buf, tl, size - tl) != (ssize_t)tl || memcmp(buf, tail, tl) || ftruncate(fd, size - tl)) { close(fd); unlink(path); return -1; }
	fsync(fd); close(fd);
	return 0;
}

static int is(const char *req, const char *method_path)	/* exact request line prefix followed by space or ? */
{
	size_t n = strlen(method_path);
	return !strncmp(req, method_path, n) && (req[n] == ' ' || req[n] == '?');
}

static void serve(int s)
{
	static char *req; static size_t cap; size_t n = 0, clen = 0, blen; ssize_t k; char *body = NULL, *cl; struct buf b = { 0 };
	char v1[80], v2[80], v3[80], yes[4], query[600] = "", path[512], *q, *sp; int st;
	if (!req) { cap = 16384; req = malloc(cap); if (!req) return; }
	for (;;) {
		if (n + 1 >= cap) {
			/* only a signed-in upload may grow the buffer, and only to its stated size (8 MB at most) */
			if (!body || clen > UPLOAD_MAX || !signed_in(req) || cap >= (size_t)(body - req) + clen + 2) break;
			{ size_t off = body - req; char *r = realloc(req, off + clen + 2); if (!r) break; req = r; cap = off + clen + 2; body = req + off; }
		}
		k = read(s, req + n, cap - 1 - n);
		if (k <= 0) break;
		n += k; req[n] = 0;
		if (!body && (body = strstr(req, "\r\n\r\n"))) {
			char save = *body; *body = 0;
			for (cl = req; *cl; cl++) if (!strncasecmp(cl, "\nContent-Length:", 16)) clen = strtoul(cl + 16, NULL, 10);
			*body = save; body += 4;
		}
		if (body && n >= (size_t)(body - req) + clen) break;
	}
	if (!body) return;
	body[-4] = 0;			/* req: request line and headers; body: the form or the upload */
	blen = n - (body - req); if (blen > clen) blen = clen;
	body[blen] = 0;
	/* the part of the address after "?" */
	sp = strchr(req, ' '); q = sp ? strchr(sp + 1, '?') : NULL;
	if (q && q < strchr(sp + 1, ' ' ) ) { size_t l = strcspn(q + 1, " \r\n"); if (l >= sizeof query) l = sizeof query - 1; memcpy(query, q + 1, l); query[l] = 0; }

	if (is(req, "POST /login")) {
		time_t now = time(NULL);
		form(body, "code", v1, sizeof v1);
		if (now < locked_until) page_login(&b, "Too many wrong pairing codes. Locked for one minute.");
		else if (code_ok(v1)) {
			const char *t = new_session(); char h[160];
			wrong_codes = 0;
			if (t) {
				snprintf(h, sizeof h, "Location: /\r\nSet-Cookie: nocry=%s; Path=/; HttpOnly; SameSite=Strict\r\n", t);
				puts_(&b, "<a href=/>continue</a>");
				reply(s, "303 See Other", h, &b);
				free(b.p);
				return;
			}
			page_login(&b, "Could not start a session.");
		} else {
			sleep(2);
			if (++wrong_codes >= 5) { wrong_codes = 0; locked_until = time(NULL) + 60; }
			page_login(&b, "Wrong pairing code.");
		}
		reply(s, "200 OK", NULL, &b);
	} else if (!signed_in(req)) {
		/* every address gets this page: that is what makes phones offer it after joining */
		page_login(&b, NULL);
		reply(s, "200 OK", NULL, &b);
	} else if (is(req, "GET /wifi")) {
		page_wifi(&b, NULL, NULL);
		reply(s, "200 OK", NULL, &b);
	} else if (is(req, "POST /wifi/join")) {
		size_t pl;
		form(body, "ssid", v1, sizeof v1); form(body, "other", v2, sizeof v2); form(body, "pass", v3, sizeof v3);
		if (v2[0]) snprintf(v1, sizeof v1, "%s", v2);
		pl = strlen(v3);
		if (!v1[0] || strlen(v1) > 32) page_wifi(&b, "Select a network or enter an SSID.", NULL);
		else if (pl && (pl < 8 || pl > 63)) page_wifi(&b, "WPA passwords are 8 to 63 characters. Leave it empty only for an open network.", v1);
		else if (strchr(v3, '"')) page_wifi(&b, "A password containing a double quote (\") cannot be entered here. Use nocry on a computer.", v1);
		else {
			page_moving(&b, v1);
			reply(s, "200 OK", NULL, &b);
			shutdown(s, SHUT_WR);
			launch(2, wifi, "join", v1, pl ? v3 : NULL);	/* after the answer has reached the browser */
			free(b.p);
			return;
		}
		reply(s, "200 OK", NULL, &b);
	} else if (is(req, "POST /wifi/own")) {
		page_moving(&b, NULL);
		reply(s, "200 OK", NULL, &b);
		shutdown(s, SHUT_WR);
		launch(2, wifi, "ap", NULL, NULL);
	} else if (is(req, "POST /slot/switch")) {
		form(body, "slot", v1, sizeof v1); form(body, "yes", yes, sizeof yes);
		if (strcmp(v1, "a") && strcmp(v1, "b")) { redirect(s, "/"); free(b.p); return; }
		if (!yes[0]) {
			page_confirm(&b, v1[0] == 'a' ? "Set slot A active?" : "Set slot B active?",
				     "This slot boots from the next power-on. No data is changed; settings and files are shared by both slots. "
				     "If the two slots hold different kinds of firmware, the active firmware may change the printer's mainboard firmware.",
				     "/slot/switch", "slot", v1, "Set active", 0);
		} else {
			char *out = run(doer, "switch", v1, NULL, 512, &st);
			if (st == 0) page_home(&b, v1[0] == 'a' ? "Slot A is active from the next power-on." : "Slot B is active from the next power-on.", 1);
			else page_home(&b, out && *out ? out : "Active slot not changed.", 0);
			free(out);
		}
		reply(s, "200 OK", NULL, &b);
	} else if (is(req, "POST /slot/copy")) {
		form(body, "from", v1, sizeof v1); form(body, "yes", yes, sizeof yes);
		if (strcmp(v1, "a") && strcmp(v1, "b")) { redirect(s, "/"); free(b.p); return; }
		if (!yes[0]) {
			page_confirm(&b, v1[0] == 'a' ? "Copy slot A to slot B?" : "Copy slot B to slot A?",
				     "The other slot is overwritten with an exact, verified copy of this slot's firmware. "
				     "The active slot, settings and files are not changed. Takes a few minutes.",
				     "/slot/copy", "from", v1, "Overwrite", 1);
			reply(s, "200 OK", NULL, &b);
		} else {
			char *out = run(doer, "copy", v1, NULL, 512, &st);
			if (st == 0) redirect(s, "/");
			else { page_home(&b, out && *out ? out : "The copy could not be started.", 0); reply(s, "200 OK", NULL, &b); }
			free(out);
		}
	} else if (is(req, "POST /reset")) {
		int all;
		form(body, "kind", v1, sizeof v1); form(body, "yes", yes, sizeof yes);
		all = !strcmp(v1, "all");
		if (!all && strcmp(v1, "mods")) { redirect(s, "/"); free(b.p); return; }
		if (!yes[0]) {
			page_confirm(&b, all ? "Factory reset?" : "Wipe system overlay?",
				     all ? "Erases rootfs_data and userdata: all settings and files (Wi-Fi settings, print history, uploaded files, calibration, the printer configuration). "
					   "This cannot be undone. The firmware in both slots and the serial number are preserved."
					 : "Erases rootfs_data: all changes to the root file system (installed mods and helper scripts, changed system files). "
					   "This cannot be undone. userdata (files, print history, printer configuration) is preserved.",
				     "/reset", "kind", v1, all ? "Erase all" : "Wipe overlay", 1);
			reply(s, "200 OK", NULL, &b);
		} else {
			char *out = run(doer, "reset", v1, NULL, 512, &st);
			if (st == 0) redirect(s, "/");
			else { page_home(&b, out && *out ? out : "The reset could not be started.", 0); reply(s, "200 OK", NULL, &b); }
			free(out);
		}
	} else if (is(req, "GET /install")) {
		page_install(&b, NULL, 0);
		reply(s, "200 OK", NULL, &b);
	} else if (is(req, "POST /install/fetch")) {
		char *bar, *out;
		form(body, "fw", v1, sizeof v1); form(body, "mode", v2, sizeof v2);
		bar = strchr(v1, '|');
		if (!bar || (strcmp(v2, "keep") && strcmp(v2, "slot"))) { redirect(s, "/install"); free(b.p); return; }
		*bar++ = 0;
		{ /* doer takes at most three arguments through run(): key, version and mode travel as "fw-fetch" KEY VERSION, mode by a fourth */
		  int fd[2], stt = -1; pid_t pid; char *o = calloc(1, 513); size_t n = 0; ssize_t k2;
		  if (o && !pipe(fd)) {
			pid = fork();
			if (pid == 0) { dup2(fd[1], 1); dup2(fd[1], 2); close(fd[0]); close(fd[1]); execl(doer, doer, "fw-fetch", v1, bar, v2, (char *)NULL); _exit(127); }
			close(fd[1]);
			while (pid > 0 && n < 512 && (k2 = read(fd[0], o + n, 512 - n)) > 0) n += k2;
			close(fd[0]);
			if (pid > 0) { waitpid(pid, &stt, 0); stt = WIFEXITED(stt) ? WEXITSTATUS(stt) : -1; }
		  }
		  out = o; st = stt; }
		if (st == 0) redirect(s, "/install");
		else { page_install(&b, out && *out ? out : "The download could not be started.", 0); reply(s, "200 OK", NULL, &b); }
		free(out);
	} else if (is(req, "POST /install/refresh")) {
		char *out = run(doer, "fw-list", "refresh", NULL, 512, &st);
		page_install(&b, st == 0 ? "Catalog refreshed." : "The catalog could not be refreshed (no internet access?). The built-in catalog is still shown.", st == 0);
		free(out);
		reply(s, "200 OK", NULL, &b);
	} else if (is(req, "POST /install/upload")) {
		char name[160], *out, *pth;
		form(query, "mode", v2, sizeof v2);
		if (strcmp(v2, "keep") && strcmp(v2, "slot")) snprintf(v2, sizeof v2, "keep");
		if (clen > FW_MAX) page_install(&b, "File larger than 700 MB.", 0);
		else {
			pth = run(doer, "fw-upload-path", NULL, NULL, 512, &st);
			if (st != 0) page_install(&b, pth && *pth ? pth : "No storage available for the upload.", 0);
			else if (upload_to_file(s, req, body, n - (body - req), clen, pth, name, sizeof name)) {
				free(run(doer, "fw-discard", NULL, NULL, 64, NULL));
				page_install(&b, "Upload incomplete. Select the file and retry.", 0);
			} else {
				out = run(doer, "fw-check", name, v2, 512, &st);
				if (st == 0) { free(out); free(pth); redirect(s, "/install"); free(b.p); return; }
				page_install(&b, out && *out ? out : "The file could not be verified.", 0);
				free(out);
			}
			free(pth);
		}
		reply(s, "200 OK", NULL, &b);
	} else if (is(req, "POST /install/go")) {
		char *job = run(doer, "fw-install", NULL, NULL, 512, &st);
		if (st == 0) redirect(s, "/install");
		else { page_install(&b, job && *job ? job : "The install could not be started.", 0); reply(s, "200 OK", NULL, &b); }
		free(job);
	} else if (is(req, "POST /install/discard")) {
		free(run(doer, "fw-discard", NULL, NULL, 64, NULL));
		redirect(s, "/install");
	} else if (is(req, "GET /boot")) {
		page_boot(&b, NULL, 0);
		reply(s, "200 OK", NULL, &b);
	} else if (is(req, "POST /boot/loader") || is(req, "POST /boot/logo")) {
		int loader = is(req, "POST /boot/loader"); const char *verb = loader ? "loader" : "logo";
		form(body, "what", v1, sizeof v1); form(body, "yes", yes, sizeof yes);
		if (loader ? (strcmp(v1, "install") && strcmp(v1, "factory")) : (strcmp(v1, "nocry") && strcmp(v1, "firmware"))) { redirect(s, "/boot"); free(b.p); return; }
		if (!yes[0]) {
			page_confirm(&b, loader ? (v1[0] == 'i' ? "Install the nocryboot boot loader?" : "Restore the factory boot loader?")
						: (v1[0] == 'n' ? "Use the nocry boot logo?" : "Use the firmware's boot logo?"),
				     loader ? "The boot loader is the first code the board executes. If the write fails the board does not boot by itself; recovery is through USB boot mode: "
					      "hold BOOT, tap RESET, and use nocry on a computer. The replaced loader is kept on the board. Takes a few seconds; do not power off."
					    : (v1[0] == 'n' ? "Changes only the screen contents during the first seconds after power-on. The replaced logo is kept on the board. "
							      "The nocry logo is made for Creality firmware and is untested on a slot holding NebulaOS: "
							      "if the screen stays dark after power-on, return here and select the firmware's logo."
							    : "Restores each slot's original firmware boot logo from the copy kept on the board."),
				     loader ? "/boot/loader" : "/boot/logo", "what", v1, loader ? "Change boot loader" : "Change boot logo", loader);
		} else {
			char *out = run(doer, verb, v1, NULL, 4096, &st);
			page_boot(&b, out && *out ? out : (st == 0 ? "Completed." : "Failed."), st == 0);
			free(out);
		}
		reply(s, "200 OK", NULL, &b);
	} else if (is(req, "GET /dl/backup")) {
		stream(s, "nocry-backup.img.gz", "application/gzip", doer, "bundle", "backup");
	} else if (is(req, "GET /repair")) {
		page_repair(&b);
		reply(s, "200 OK", NULL, &b);
	} else if (is(req, "GET /board")) {
		page_board(&b, NULL, 0);
		reply(s, "200 OK", NULL, &b);
	} else if (is(req, "POST /check")) {
		form(body, "which", v1, sizeof v1); form(body, "yes", yes, sizeof yes);
		if (strcmp(v1, "data") && strcmp(v1, "settings")) { redirect(s, "/repair"); free(b.p); return; }
		if (!yes[0]) {
			page_confirm(&b, "Run fsck?",
				     "Runs e2fsck and repairs errors. Files damaged beyond repair may be lost or moved to lost+found. Usually takes under a minute. Do not interrupt it once started.",
				     "/check", "which", v1, "Run fsck", 0);
			reply(s, "200 OK", NULL, &b);
		} else {
			char *out = run(doer, "check", v1, NULL, 512, &st);
			if (st == 0) redirect(s, "/repair");
			else { page_home(&b, out && *out ? out : "The check could not be started.", 0); reply(s, "200 OK", NULL, &b); }
			free(out);
		}
	} else if (is(req, "GET /dl/logs")) {
		stream(s, "nocry-board-logs.tar.gz", "application/gzip", doer, "bundle", "logs");
	} else if (is(req, "GET /dl/config")) {
		stream(s, "nocry-printer-configuration.tar.gz", "application/gzip", doer, "bundle", "config");
	} else if (is(req, "GET /dl/identity")) {
		stream(s, "nocry-identity-record.bin", "application/octet-stream", doer, "bundle", "identity");
	} else if (is(req, "GET /files/get")) {
		const char *base;
		if (!form(query, "p", path, sizeof path)) { redirect(s, "/files"); free(b.p); return; }
		base = strrchr(path, '/'); base = base ? base + 1 : path;
		{ char safe[128]; size_t i; for (i = 0; base[i] && i < sizeof safe - 1; i++) safe[i] = (isalnum((unsigned char)base[i]) || strchr("._-", base[i])) ? base[i] : '_'; safe[i] = 0;
		  stream(s, safe[0] ? safe : "file", "application/octet-stream", doer, "get", path); }
	} else if (is(req, "GET /files")) {
		form(query, "p", path, sizeof path);
		page_files(&b, path, NULL, 0);
		reply(s, "200 OK", NULL, &b);
	} else if (is(req, "POST /files/delete")) {
		char *sl;
		form(body, "p", path, sizeof path); form(body, "yes", yes, sizeof yes);
		if (!path[0]) { redirect(s, "/files"); free(b.p); return; }
		if (!yes[0]) {
			char text[700];
			snprintf(text, sizeof text, "\"%s\" is deleted permanently. A folder must be empty first.", path);
			page_confirm(&b, "Delete?", text, "/files/delete", "p", path, "Delete", 1);
		} else {
			char *out = run(doer, "rm", path, NULL, 512, &st);
			sl = strrchr(path, '/'); if (sl) *sl = 0; else path[0] = 0;
			page_files(&b, path, st == 0 ? "Deleted." : (out && *out ? out : "Delete failed."), st == 0);
			free(out);
		}
		reply(s, "200 OK", NULL, &b);
	} else if (is(req, "POST /files/upload")) {
		char *data, name[160], dest[700], *out; size_t len;
		form(query, "p", path, sizeof path);
		if (clen > UPLOAD_MAX) page_files(&b, path, "File larger than 8 MB.", 0);
		else if (!upload(req, body, blen, &data, &len, name, sizeof name)) page_files(&b, path, "No file selected.", 0);
		else {
			snprintf(dest, sizeof dest, "%s%s%s", path, path[0] ? "/" : "", name);
			out = feed(doer, "put", dest, data, len, &st);
			page_files(&b, path, st == 0 ? "Uploaded." : (out && *out ? out : "The file could not be written."), st == 0);
			free(out);
		}
		reply(s, "200 OK", NULL, &b);
	} else if (is(req, "POST /wifi/save")) {
		char *out = run(doer, "wifi-save", NULL, NULL, 512, &st);
		page_board(&b, st == 0 ? "Credentials written to the installed firmware's Wi-Fi configuration." : (out && *out ? out : "Not saved."), st == 0);
		free(out);
		reply(s, "200 OK", NULL, &b);
	} else if (is(req, "POST /restart")) {
		form(body, "yes", yes, sizeof yes);
		if (!yes[0]) {
			page_confirm(&b, "Reboot?", "Leaves rescue and boots the active slot. These pages stop responding; the rescue system must be started again to return.",
				     "/restart", NULL, NULL, "Reboot", 0);
			reply(s, "200 OK", NULL, &b);
		} else {
			head(&b, NULL);
			puts_(&b, "<div class=c><h2>Rebooting</h2><p class=ok>Booting the active slot. This page will stop responding.</p></div>");
			reply(s, "200 OK", NULL, &b);
			shutdown(s, SHUT_WR);
			free(run(doer, "restart", NULL, NULL, 64, NULL));
		}
	} else if (is(req, "POST /job/cancel")) {
		char *job = run(doer, "job", NULL, NULL, 2048, NULL), risk[220], state[16];
		form(body, "yes", yes, sizeof yes);
		field(job, "risk", risk, sizeof risk); field(job, "state", state, sizeof state);
		if (strcmp(state, "running")) redirect(s, "/");
		else if (risk[0] && !yes[0]) {
			head(&b, NULL);
			puts_(&b, "<div class=c><h2>Cancel this job?</h2><p>"); esc(&b, risk);
			puts_(&b, "</p><a class=b href=/>Cancel</a><form method=post action=/job/cancel><input type=hidden name=yes value=1>"
				  "<button class=d>Cancel job</button></form></div>");
			reply(s, "200 OK", NULL, &b);
		} else {
			free(run(doer, "cancel", NULL, NULL, 64, NULL));
			redirect(s, "/");
		}
		free(job);
	} else if (is(req, "GET /") || !strncmp(req, "GET ", 4)) {
		page_home(&b, NULL, 0);
		reply(s, "200 OK", NULL, &b);
	} else {
		redirect(s, "/");
	}
	free(b.p);
}

int main(int argc, char **argv)
{
	const char *addr = "0.0.0.0"; int port = 80, i, ls, one = 1; struct sockaddr_in sa;
	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--address") && i + 1 < argc) addr = argv[++i];
		else if (!strcmp(argv[i], "--port") && i + 1 < argc) port = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--wifi") && i + 1 < argc) wifi = argv[++i];
		else if (!strcmp(argv[i], "--do") && i + 1 < argc) doer = argv[++i];
		else if (!strcmp(argv[i], "--code-file") && i + 1 < argc) codefile = argv[++i];
		else if (!strcmp(argv[i], "--state") && i + 1 < argc) statefile = argv[++i];
		else { fprintf(stderr, "usage: nocry-webd [--address A] [--port N] [--wifi PATH] [--do PATH] [--code-file F] [--state F]\n"); return 2; }
	}
	signal(SIGPIPE, SIG_IGN);
	ls = socket(AF_INET, SOCK_STREAM, 0);
	setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	memset(&sa, 0, sizeof sa);
	sa.sin_family = AF_INET; sa.sin_port = htons(port);
	if (!inet_aton(addr, &sa.sin_addr)) { fprintf(stderr, "nocry-webd: bad address %s\n", addr); return 2; }
	if (ls < 0 || bind(ls, (struct sockaddr *)&sa, sizeof sa) || listen(ls, 8)) { fprintf(stderr, "nocry-webd: %s:%d: %s\n", addr, port, strerror(errno)); return 1; }
	fprintf(stderr, "nocry-webd: pages at http://%s:%d\n", addr, port);
	for (;;) {
		struct timeval tv = { 5, 0 };
		int s = accept(ls, NULL, NULL);
		if (s < 0) { if (errno == EINTR) continue; sleep(1); continue; }
		setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
		setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
		serve(s);
		close(s);
	}
}
