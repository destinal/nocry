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
	puts_(b, "<p>This printer board is running its rescue system. Type the six digits shown on the board's screen.</p>"
		 "<form method=post action=/login><input type=text name=code inputmode=numeric maxlength=7 autocomplete=off autofocus "
		 "style=\"font-size:1.6em;letter-spacing:.2em;text-align:center\">"
		 "<button class=w>Continue</button></form>"
		 "<p class=t>The code proves you are at the printer. It is asked once; it changes when the board restarts.</p></div>");
}

static void wifi_card(struct buf *b, const char *st)
{
	char mode[32], ssid[128], addr[64], src[160], hint[32];
	field(st, "mode", mode, sizeof mode); field(st, "ssid", ssid, sizeof ssid); field(st, "address", addr, sizeof addr);
	field(st, "source", src, sizeof src); field(st, "hint", hint, sizeof hint);
	if (!strcmp(mode, "ap")) {
		puts_(b, "<div class=\"c hi\"><h2>Put the board on your Wi-Fi</h2>");
		if (!strncmp(src, "failed:", 7)) {
			puts_(b, "<p class=e>The board could not join <b>"); esc(b, src + 7);
			puts_(b, "</b>: wrong password, or the network is out of range.");
			if (!strncmp(hint, "nonascii", 8))
				puts_(b, " <b>The password contained a character that is not on a plain keyboard</b>, such as a curly apostrophe "
					 "(&rsquo;) instead of a straight one ('). Phones swap these in by themselves: hold the apostrophe key and pick the straight one.");
			puts_(b, "</p>");
		}
		puts_(b, "<p>Right now it runs its own small network, which is slow and has no internet. On your Wi-Fi a computer "
			 "with nocry can reach it, and it is many times faster.</p>"
			 "<a class=\"b w\" href=/wifi>Join your Wi-Fi network</a></div>");
	} else if (!strcmp(mode, "client")) {
		puts_(b, "<div class=c><h2>Wi-Fi</h2><p>On <b>"); esc(b, ssid); puts_(b, "</b>, address "); esc(b, addr);
		puts_(b, ".</p><a class=\"b q\" href=/wifi>Change network</a></div>");
	} else {
		puts_(b, "<div class=c><h2>Wi-Fi</h2><p>"); esc(b, !strcmp(mode, "joining") ? "Joining a network\xe2\x80\xa6" : "No working Wi-Fi.");
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
		puts_(b, "</p><p class=t>This page updates by itself. Leave the board switched on.</p>"
			 "<form method=post action=/job/cancel><button class=q>Cancel</button></form></div>");
		return 1;
	}
	puts_(b, "<div class=c><h2>"); esc(b, what); puts_(b, "</h2><p class=");
	puts_(b, !strcmp(state, "done") ? "ok>Finished: " : !strcmp(state, "cancelled") ? "e>Stopped: " : "e>Failed: ");
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
	puts_(b, "<p><b>Slot "); puts_(b, L); puts_(b, "</b>"); if (active) puts_(b, " &middot; <b>starts</b>");
	puts_(b, "<br>"); esc(b, fw[0] ? fw : "unknown"); puts_(b, "</p>");
	if (busy || strcmp(ok, "yes")) return;
	if (!active) {
		puts_(b, "<form method=post action=/slot/switch><input type=hidden name=slot value="); puts_(b, l);
		puts_(b, "><button>Start slot "); puts_(b, L); puts_(b, "</button></form>");
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
	if (model[0]) { puts_(b, "<p class=t>Board set up as: "); esc(b, model); puts_(b, "</p>"); }
	if (!strncmp(notev, "nocryboot:failed=", 17) || !strncmp(notev, "nocryboot:incomplete=", 21) || !strncmp(notev, "nocryboot:unconfirmed=", 22)) {
		puts_(b, "<p class=e>The boot loader noted a problem with slot "); esc(b, strchr(notev, '=') + 1);
		puts_(b, !strncmp(notev, "nocryboot:incomplete", 20) ? ": its firmware is incomplete." : ": it did not start properly, so the other slot was used.");
		puts_(b, "</p>");
	}
	slot_row(b, bs, 'a', active[0] == 'a', running);
	puts_(b, "<hr style=\"border:0;border-top:1px solid #8884\">");
	slot_row(b, bs, 'b', active[0] == 'b', running);
	puts_(b, "<p class=t>The board holds two firmware slots and starts one of them. Settings and files are shared by both.</p></div>");
	if (!running)
		puts_(b, "<div class=c><h2>Reset</h2>"
			 "<p><b>Remove system modifications.</b> Clears changes made to the system itself (installed mods, helper scripts, "
			 "changed system settings). Your files, print history and printer configuration stay. Try this first when the printer stopped starting after a mod.</p>"
			 "<form method=post action=/reset><input type=hidden name=kind value=mods><button class=q>Remove system modifications</button></form>"
			 "<p><b>Factory reset.</b> Erases all settings and files. The firmware, the slots and the board's serial number stay.</p>"
			 "<form method=post action=/reset><input type=hidden name=kind value=all><button class=d>Factory reset</button></form></div>");
	puts_(b, "<p class=t>Installing firmware, backups and the boot loader need nocry on a computer for now "
		 "(github.com/destinal/nocry): it finds this board on the network by itself.</p>");
	free(st); free(job); free(bs);
}

static void page_wifi(struct buf *b, const char *error, const char *keep_ssid)
{
	char *st = slurp(statefile, 1024), *scan = run(wifi, "scan", NULL, NULL, 16384, NULL), *line, *save, mode[32]; int any = 0;
	field(st, "mode", mode, sizeof mode);
	head(b, NULL);
	puts_(b, "<div class=c><h2>Join a Wi-Fi network</h2>");
	note(b, "e", error);
	puts_(b, "<p class=t>Only 2.4&nbsp;GHz networks are in reach of this board.</p><form method=post action=/wifi/join><label>Network</label>");
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
	if (!any) puts_(b, "<p>No networks were found. Type the name below.</p>");
	puts_(b, "<label for=o>Or another name (hidden network)</label><input id=o type=text name=other maxlength=32 autocapitalize=none autocorrect=off>"
		 "<label for=p>Wi-Fi password</label><input id=p type=password name=pass maxlength=63 autocomplete=off>"
		 "<button class=w>Join this network</button></form>"
		 "<p class=t>The board then leaves the network it is on. If it cannot join, its own network \"nocry\" comes back within a minute.</p></div>");
	if (!strcmp(mode, "client"))
		puts_(b, "<div class=c><form method=post action=/wifi/own><button class=q>Go back to the board's own network</button></form></div>");
	puts_(b, "<a class=\"b q\" href=/>Back</a>");
	free(st); free(scan);
}

static void page_moving(struct buf *b, const char *ssid)
{
	head(b, NULL);
	puts_(b, "<div class=c><h2>Changing network</h2>");
	if (ssid) {
		puts_(b, "<p class=ok>The board is joining <b>"); esc(b, ssid); puts_(b, "</b>.</p>"
			 "<p><b>If it works,</b> the board's screen shows the network and its new address there. Put this device on that network "
			 "and open that address; nocry on a computer finds the board by itself. You stay signed in.</p>"
			 "<p><b>If it does not,</b> the network \"nocry\" comes back within a minute: join it again and check the password.</p>");
	} else {
		puts_(b, "<p class=ok>The board is opening its own network \"nocry\" (password nocry111).</p>"
			 "<p>Join it, then open http://192.168.77.1</p>");
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
	puts_(b, danger ? "<button class=d>" : "<button>"); esc(b, go); puts_(b, "</button></form><a class=\"b q\" href=/>Keep things as they are</a></div>");
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

static int is(const char *req, const char *method_path)	/* exact request line prefix followed by space or ? */
{
	size_t n = strlen(method_path);
	return !strncmp(req, method_path, n) && (req[n] == ' ' || req[n] == '?');
}

static void serve(int s)
{
	static char req[8192]; size_t n = 0, clen = 0; ssize_t k; char *body, *cl; struct buf b = { 0 };
	char v1[80], v2[80], v3[80], yes[4]; int st;
	while (n < sizeof req - 1 && (k = read(s, req + n, sizeof req - 1 - n)) > 0) {
		n += k; req[n] = 0;
		if ((body = strstr(req, "\r\n\r\n"))) {
			for (cl = req; cl < body; cl++) if (!strncasecmp(cl, "\nContent-Length:", 16)) clen = strtoul(cl + 16, NULL, 10);
			if (n >= (size_t)(body + 4 - req) + clen) break;
		}
	}
	req[n] = 0;
	body = strstr(req, "\r\n\r\n");
	if (!body) return;
	*body = 0; body += 4;		/* req: request line and headers; body: the form */

	if (is(req, "POST /login")) {
		time_t now = time(NULL);
		form(body, "code", v1, sizeof v1);
		if (now < locked_until) page_login(&b, "Too many wrong codes. Wait a minute, then try again.");
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
			page_login(&b, "That is not the code on the board's screen.");
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
		if (!v1[0] || strlen(v1) > 32) page_wifi(&b, "Choose a network, or type its name.", NULL);
		else if (pl && (pl < 8 || pl > 63)) page_wifi(&b, "A Wi-Fi password has 8 to 63 characters. Leave it empty only for an open network.", v1);
		else if (strchr(v3, '"')) page_wifi(&b, "A password with a double quote (\") cannot be entered here. Use nocry on a computer instead.", v1);
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
			page_confirm(&b, v1[0] == 'a' ? "Start slot A?" : "Start slot B?",
				     "The board then starts the firmware in this slot the next time it is switched on. Nothing is erased; settings and files are shared by both slots. "
				     "If the two slots hold different kinds of firmware, the printer's mainboard firmware may be changed by the one that starts.",
				     "/slot/switch", "slot", v1, "Start this slot", 0);
		} else {
			char *out = run(doer, "switch", v1, NULL, 512, &st);
			if (st == 0) page_home(&b, v1[0] == 'a' ? "Slot A starts at the next power-on." : "Slot B starts at the next power-on.", 1);
			else page_home(&b, out && *out ? out : "The slot could not be switched.", 0);
			free(out);
		}
		reply(s, "200 OK", NULL, &b);
	} else if (is(req, "POST /slot/copy")) {
		form(body, "from", v1, sizeof v1); form(body, "yes", yes, sizeof yes);
		if (strcmp(v1, "a") && strcmp(v1, "b")) { redirect(s, "/"); free(b.p); return; }
		if (!yes[0]) {
			page_confirm(&b, v1[0] == 'a' ? "Copy slot A to slot B?" : "Copy slot B to slot A?",
				     "The other slot gets an exact, verified copy of this slot's firmware; what it holds now is replaced. "
				     "Which slot starts, and your settings and files, are not changed. It takes a few minutes.",
				     "/slot/copy", "from", v1, "Copy", 1);
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
			page_confirm(&b, all ? "Factory reset?" : "Remove system modifications?",
				     all ? "All settings and files on the printer are erased: Wi-Fi settings, print history, uploaded files, calibration, the printer configuration. "
					   "This cannot be undone. The firmware in both slots and the board's serial number stay."
					 : "Everything changed in the system itself is removed: installed mods and helper scripts, changed system files. "
					   "This cannot be undone. Your files, print history and printer configuration stay.",
				     "/reset", "kind", v1, all ? "Erase everything" : "Remove modifications", 1);
			reply(s, "200 OK", NULL, &b);
		} else {
			char *out = run(doer, "reset", v1, NULL, 512, &st);
			if (st == 0) redirect(s, "/");
			else { page_home(&b, out && *out ? out : "The reset could not be started.", 0); reply(s, "200 OK", NULL, &b); }
			free(out);
		}
	} else if (is(req, "POST /job/cancel")) {
		char *job = run(doer, "job", NULL, NULL, 2048, NULL), risk[220], state[16];
		form(body, "yes", yes, sizeof yes);
		field(job, "risk", risk, sizeof risk); field(job, "state", state, sizeof state);
		if (strcmp(state, "running")) redirect(s, "/");
		else if (risk[0] && !yes[0]) {
			head(&b, NULL);
			puts_(&b, "<div class=c><h2>Stop before this is finished?</h2><p>"); esc(&b, risk);
			puts_(&b, "</p><a class=b href=/>Keep going</a><form method=post action=/job/cancel><input type=hidden name=yes value=1>"
				  "<button class=d>Stop anyway</button></form></div>");
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
