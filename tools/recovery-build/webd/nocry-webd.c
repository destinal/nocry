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
 *   2. Then everything the touch screen offers that makes sense from a browser: Wi-Fi,
 *      slots, installing firmware, the flash drive, files, repair, boot loader, remote access.
 *      Each change asks first and says what it costs; long ones run as a job with progress
 *      and a Cancel that says what stopping leaves.
 *
 * Plain HTTP: on a network shared with others the code and the session can be overheard.
 * A browser would not accept an encrypted page from a board without warnings on every visit;
 * nocry on a computer uses its own encrypted link instead.
 *
 * A signed-in browser gets the application at every address: web/index.html, one file (HTML,
 * CSS and JavaScript, nothing fetched from elsewhere). It asks /api/state about once a second,
 * /api/get for lists, sends every action to /api/do and files to /api/upload-*. Only the
 * pairing-code page is made here.
 *
 * One request per connection, one connection at a time. The work is done by
 * /usr/sbin/nocry-wifi, nocry-rescue-do, nocry-usb and nocry-remote, started with their arguments
 * as they are (never through a shell). Actions are POST only and the session cookie is
 * SameSite=Strict.
 *
 * usage: nocry-webd [--address A] [--port N] [--wifi PATH] [--do PATH] [--usb PATH] [--remote PATH]
 *                   [--web FILE] [--settings FILE] [--code-file F] [--state F]
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
static const char *usbtool = "/usr/sbin/nocry-usb", *remotetool = "/usr/sbin/nocry-remote";
static const char *webfile = "/usr/share/nocry/web/index.html", *settingsfile = "/run/nocry-touch.cal";
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

/* The same with any number of arguments (argv[0] is the tool). */
static char *runv(char *const argv[], size_t max, int *status)
{
	int fd[2], st = 0; pid_t pid; char *out = calloc(1, max + 1); size_t n = 0; ssize_t k;
	if (status) *status = -1;
	if (!out || pipe(fd)) return out;
	pid = fork();
	if (pid == 0) {
		dup2(fd[1], 1); dup2(fd[1], 2); close(fd[0]); close(fd[1]);
		execv(argv[0], argv);
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
static void launchv(int delay, char *const argv[])
{
	pid_t pid = fork();
	if (pid == 0) {
		setsid();
		if (fork() == 0) {
			int fd; for (fd = 0; fd < 64; fd++) close(fd);
			if (open("/dev/null", O_RDWR) == 0 && dup(0) == 1 && dup(0) == 2) { /* quiet */ }
			if (delay) sleep(delay);
			execv(argv[0], argv);
		}
		_exit(0);
	}
	if (pid > 0) waitpid(pid, NULL, 0);
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
	"<meta name=viewport content=\"width=device-width,initial-scale=1\"><meta name=color-scheme content=\"light dark\">"
	"<title>nocry rescue</title><style>"
	":root{--bg:#f3f4f7;--panel:#fff;--line:#dfe2e8;--text:#14171c;--dim:#5d6572;--accent:#1667d9;--at:#fff;--err:#c0312b}"
	"@media(prefers-color-scheme:dark){:root{--bg:#0e1116;--panel:#171b22;--line:#2a313c;--text:#e8ebf0;--dim:#98a2b1;--accent:#4c93ff;--at:#06101f;--err:#ef5b52}}"
	"body{font:15px/1.5 system-ui,-apple-system,Segoe UI,Roboto,sans-serif;margin:0;min-height:100vh;display:grid;place-items:center;padding:16px;box-sizing:border-box;background:var(--bg);color:var(--text)}"
	"h1{font-size:1.15rem;margin:0 0 14px;display:flex;align-items:center;gap:10px}"
	"h1:before{content:'nc';width:28px;height:28px;border-radius:8px;background:linear-gradient(135deg,var(--accent),#7a5cff);color:#fff;font-size:.8rem;display:grid;place-items:center}"
	"h2{font-size:1.05rem;margin:0 0 6px}p{margin:.5em 0}"
	".c{background:var(--panel);border:1px solid var(--line);border-radius:14px;padding:20px 22px;box-shadow:0 6px 24px rgba(0,0,0,.08)}"
	"input{width:100%;box-sizing:border-box;font:inherit;padding:10px;border:1px solid var(--line);border-radius:9px;background:var(--bg);color:var(--text)}"
	"button{font:inherit;font-weight:600;width:100%;margin-top:12px;padding:10px;border:0;border-radius:9px;background:var(--accent);color:var(--at);cursor:pointer}"
	".e{color:var(--err);font-weight:600}.t{color:var(--dim);font-size:.86rem}"
	"</style><div style=\"max-width:380px;width:100%\">";

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

/* ------------------------------------------------------------------ the application's side -- */

static void reply_as(int s, const char *type, struct buf *b)
{
	char h[512]; size_t off = 0; ssize_t k;
	int n = snprintf(h, sizeof h, "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
			 "Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nConnection: close\r\n\r\n", type, b->n);
	if (write(s, h, n) != n) return;
	while (off < b->n && (k = write(s, b->p + off, b->n - off)) > 0) off += k;
}
/* every line of `text` with "sect." in front: the state of several tools in one answer */
static void prefixed(struct buf *b, const char *sect, const char *text)
{
	const char *p = text;
	while (p && *p) {
		size_t l = strcspn(p, "\n");
		if (l && memchr(p, '=', l)) { puts_(b, sect); puts_(b, "."); put(b, p, l); puts_(b, "\n"); }
		p += l; if (*p) p++;
	}
}
/* What the application shows all the time, asked for about once a second: Wi-Fi, the running
 * job, the flash drive, remote access, and the settings the touch screen keeps. */
static void api_state(struct buf *b)
{
	char *t, *line, *save = NULL;
	t = slurp(statefile, 2048); prefixed(b, "wifi", t); free(t);
	t = run(doer, "job", NULL, NULL, 4096, NULL); prefixed(b, "job", t); free(t);
	t = run(usbtool, "status", NULL, NULL, 2048, NULL); prefixed(b, "usb", t); free(t);
	t = run(remotetool, "status", NULL, NULL, 512, NULL); prefixed(b, "remote", t); free(t);
	t = slurp(settingsfile, 4096);
	for (line = strtok_r(t, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		char *sp = strchr(line, ' ');
		if (!sp || (strncmp(line, "remote_", 7) && strncmp(line, "ap_", 3) && strncmp(line, "ssh_pass", 8))) continue;
		puts_(b, "set."); put(b, line, sp - line); puts_(b, "="); puts_(b, sp + 1); puts_(b, "\n");
	}
	free(t);
}
/* things read on request; `p` is the one argument some of them take */
static int api_get(struct buf *b, const char *what, const char *p)
{
	static const struct { const char *name; int tool; const char *a1, *a2; int arg; size_t max; } gets[] = {
		{ "status", 0, "status", NULL, 0, 4096 }, { "health", 0, "health", NULL, 0, 2048 },
		{ "loader", 0, "loader", NULL, 0, 4096 }, { "logo", 0, "logo", NULL, 0, 4096 }, { "rescue", 0, "rescue-status", NULL, 0, 1024 },
		{ "fw-list", 0, "fw-list", NULL, 0, 32768 }, { "fw-report", 0, "fw-report", NULL, 0, 8192 },
		{ "usb-targets", 0, "usb-targets", NULL, 0, 4096 }, { "usb-backups", 0, "usb-list", "backups", 0, 32768 },
		{ "usb-fw", 0, "usb-list", "fw", 0, 32768 }, { "usb-info", 0, "usb-info", NULL, 1, 2048 },
		{ "ls", 0, "ls", NULL, 1, 65536 }, { "scan", 1, "scan", NULL, 0, 16384 }, { "saved", 1, "saved", NULL, 0, 1024 },
	};
	size_t i; int st; char *t;
	for (i = 0; i < sizeof gets / sizeof gets[0]; i++) {
		if (strcmp(what, gets[i].name)) continue;
		t = run(gets[i].tool ? wifi : doer, gets[i].a1, gets[i].arg ? (gets[i].a2 ? gets[i].a2 : p) : gets[i].a2, gets[i].arg && gets[i].a2 ? p : NULL, gets[i].max, &st);
		{ char h[24]; snprintf(h, sizeof h, "rc=%d\n", st); puts_(b, h); }
		if (t) puts_(b, t);
		free(t);
		return 1;
	}
	return 0;
}
/* Every action of the application. The tools check their own arguments; this only decides which
 * tool and which fixed words. `away`: the answer goes out first and the tool starts a moment
 * later on its own, because it takes away the network the request came through (or the board). */
static int api_do(struct buf *b, const char *body, char **away_argv, int *away)
{
	static const struct { const char *name; int tool; const char *f1, *f2; int args, away; size_t max; } verbs[] = {
		{ "switch", 0, "switch", NULL, 1, 0, 512 }, { "copy", 0, "copy", NULL, 1, 0, 512 }, { "reset", 0, "reset", NULL, 1, 0, 512 },
		{ "check", 0, "check", NULL, 1, 0, 512 }, { "cancel", 0, "cancel", NULL, 0, 0, 256 }, { "restart", 0, "restart", NULL, 0, 1, 0 },
		{ "wifi-save", 0, "wifi-save", NULL, 0, 0, 512 }, { "rm", 0, "rm", NULL, 1, 0, 512 },
		{ "fw-refresh", 0, "fw-list", "refresh", 0, 0, 512 }, { "fw-fetch", 0, "fw-fetch", NULL, 3, 0, 512 },
		{ "fw-check-usb", 0, "fw-check-usb", NULL, 2, 0, 512 }, { "fw-install", 0, "fw-install", NULL, 0, 0, 512 }, { "fw-discard", 0, "fw-discard", NULL, 0, 0, 256 },
		{ "usb-eject", 0, "usb-eject", NULL, 0, 0, 512 }, { "usb-format", 0, "usb-format", NULL, 1, 0, 1024 },
		{ "usb-backup", 0, "usb-backup", NULL, 1, 0, 512 }, { "usb-restore", 0, "usb-restore", NULL, 1, 0, 1024 },
		{ "loader", 0, "loader", NULL, 1, 0, 4096 }, { "logo", 0, "logo", NULL, 1, 0, 4096 },
		{ "wifi-join", 1, "join", NULL, 7, 1, 0 }, { "wifi-ap", 1, "ap", NULL, 0, 1, 0 }, { "wifi-off", 1, "off", NULL, 0, 1, 0 }, { "wifi-wps", 1, "wps", NULL, 0, 1, 0 },
		{ "remote-set", 3, "set", NULL, 3, 0, 1024 },
	};
	static char verb[40], arg[7][300]; static char *argv[12];
	size_t i; int n = 0, k, last = 0, st; char key[4], *t;
	if (!form(body, "verb", verb, sizeof verb)) return 0;
	for (i = 0; i < sizeof verbs / sizeof verbs[0]; i++) {
		if (strcmp(verb, verbs[i].name)) continue;
		argv[n++] = (char *)(verbs[i].tool == 1 ? wifi : verbs[i].tool == 3 ? remotetool : doer);
		argv[n++] = (char *)verbs[i].f1;
		if (verbs[i].f2) argv[n++] = (char *)verbs[i].f2;
		for (k = 0; k < verbs[i].args; k++) { snprintf(key, sizeof key, "a%d", k + 1); if (form(body, key, arg[k], sizeof arg[k]) && arg[k][0]) last = k + 1; else arg[k][0] = 0; }
		for (k = 0; k < last; k++) argv[n++] = arg[k];		/* an empty one in the middle stays (a join without a password, with a fixed address) */
		argv[n] = NULL;
		if (verbs[i].away) {
			memcpy(away_argv, argv, sizeof argv); *away = 1;
			puts_(b, "rc=0\nstarted");
			return 1;
		}
		t = runv(argv, verbs[i].max, &st);
		{ char h[24]; snprintf(h, sizeof h, "rc=%d\n", st); puts_(b, h); }
		if (t) puts_(b, t);
		free(t);
		return 1;
	}
	return 0;
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
	char v1[80], v2[80], query[600] = "", path[512], *q, *sp; int st;
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
	} else if (is(req, "GET /api/state")) {
		api_state(&b);
		reply_as(s, "text/plain; charset=utf-8", &b);
	} else if (is(req, "GET /api/get")) {
		form(query, "what", v1, sizeof v1); form(query, "p", path, sizeof path);
		if (!api_get(&b, v1, path)) puts_(&b, "rc=2\nunknown");
		reply_as(s, "text/plain; charset=utf-8", &b);
	} else if (is(req, "POST /api/do")) {
		char *away_argv[12]; int away = 0;
		if (!api_do(&b, body, away_argv, &away)) puts_(&b, "rc=2\nunknown action");
		reply_as(s, "text/plain; charset=utf-8", &b);
		if (away) { shutdown(s, SHUT_WR); launchv(2, away_argv); }
	} else if (is(req, "POST /api/upload-fw")) {
		/* a firmware file from the browser, onto the data partition, then checked as a job */
		char name[160], *out, *pth; const char *msg = NULL;
		form(query, "mode", v2, sizeof v2);
		if (strcmp(v2, "keep") && strcmp(v2, "slot")) snprintf(v2, sizeof v2, "keep");
		st = 1;
		if (clen > FW_MAX) msg = "File larger than 700 MB.";
		else {
			pth = run(doer, "fw-upload-path", NULL, NULL, 512, &st);
			if (st != 0) { puts_(&b, "rc=1\n"); puts_(&b, pth && *pth ? pth : "No storage available for the upload."); }
			else if (upload_to_file(s, req, body, n - (body - req), clen, pth, name, sizeof name)) {
				free(run(doer, "fw-discard", NULL, NULL, 64, NULL));
				st = 1; msg = "Upload incomplete. Select the file and retry.";
			} else {
				out = run(doer, "fw-check", name, v2, 512, &st);
				if (st == 0) puts_(&b, "rc=0\n");
				else { puts_(&b, "rc=1\n"); puts_(&b, out && *out ? out : "The file could not be verified."); }
				free(out);
			}
			free(pth);
		}
		if (msg) { puts_(&b, "rc=1\n"); puts_(&b, msg); }
		reply_as(s, "text/plain; charset=utf-8", &b);
	} else if (is(req, "POST /api/upload-file")) {
		/* a small file into a folder of the data partition */
		char *data, name[160], dest[700], *out; size_t len;
		form(query, "p", path, sizeof path);
		if (clen > UPLOAD_MAX) puts_(&b, "rc=1\nFile larger than 8 MB.");
		else if (!upload(req, body, blen, &data, &len, name, sizeof name)) puts_(&b, "rc=1\nNo file selected.");
		else {
			snprintf(dest, sizeof dest, "%s%s%s", path, path[0] ? "/" : "", name);
			out = feed(doer, "put", dest, data, len, &st);
			puts_(&b, st == 0 ? "rc=0\n" : "rc=1\n");
			if (st) puts_(&b, out && *out ? out : "The file could not be written.");
			free(out);
		}
		reply_as(s, "text/plain; charset=utf-8", &b);
	} else if (is(req, "GET /dl/logs")) {
		stream(s, "nocry-board-logs.tar.gz", "application/gzip", doer, "bundle", "logs");
	} else if (is(req, "GET /dl/config")) {
		stream(s, "nocry-printer-configuration.tar.gz", "application/gzip", doer, "bundle", "config");
	} else if (is(req, "GET /dl/identity")) {
		stream(s, "nocry-identity-record.bin", "application/octet-stream", doer, "bundle", "identity");
	} else if (is(req, "GET /files/get")) {
		const char *base;
		if (!form(query, "p", path, sizeof path)) { redirect(s, "/"); free(b.p); return; }
		base = strrchr(path, '/'); base = base ? base + 1 : path;
		{ char safe[128]; size_t i; for (i = 0; base[i] && i < sizeof safe - 1; i++) safe[i] = (isalnum((unsigned char)base[i]) || strchr("._-", base[i])) ? base[i] : '_'; safe[i] = 0;
		  stream(s, safe[0] ? safe : "file", "application/octet-stream", doer, "get", path); }
	} else if (!strncmp(req, "GET ", 4)) {
		/* everything else is the application */
		char *app = slurp(webfile, 400000);
		if (app && *app) puts_(&b, app);
		else { head(&b, NULL); puts_(&b, "<div class=c><h2>Not available</h2><p>This rescue image has no web application file.</p></div>"); }
		reply_as(s, "text/html; charset=utf-8", &b);
		free(app);
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
		else if (!strcmp(argv[i], "--usb") && i + 1 < argc) usbtool = argv[++i];
		else if (!strcmp(argv[i], "--remote") && i + 1 < argc) remotetool = argv[++i];
		else if (!strcmp(argv[i], "--web") && i + 1 < argc) webfile = argv[++i];
		else if (!strcmp(argv[i], "--settings") && i + 1 < argc) settingsfile = argv[++i];
		else if (!strcmp(argv[i], "--code-file") && i + 1 < argc) codefile = argv[++i];
		else if (!strcmp(argv[i], "--state") && i + 1 < argc) statefile = argv[++i];
		else { fprintf(stderr, "usage: nocry-webd [--address A] [--port N] [--wifi PATH] [--do PATH] [--usb PATH] [--remote PATH] [--web FILE] [--settings FILE] [--code-file F] [--state F]\n"); return 2; }
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
