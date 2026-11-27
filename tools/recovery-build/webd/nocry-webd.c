/*
 * nocry-webd: the set-up page of nocry-rescue's own Wi-Fi network (docs/nocry-rescue.md).
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * A phone or laptop that joins "nocry-rescue" opens http://192.168.77.1 (the access point
 * resolves every name to the board, so most devices show the page by themselves) and tells the
 * board which Wi-Fi network to join. That is all this server does:
 *
 *   GET  anything   the page: what the board is doing, the networks in range, a form
 *   POST /join      name, password and the pairing code from the board's screen
 *
 * The pairing code is asked for because the access point's password is public: without it,
 * anyone in radio range could move the board to a network of theirs. A wrong code costs two
 * seconds, and requests are served one at a time.
 *
 * No scripts, no files, no state: one request per connection, plain HTML without JavaScript.
 * The work is done by /usr/sbin/nocry-wifi, started with its arguments as they are (no shell).
 *
 * usage: nocry-webd [--address A] [--port N] [--wifi PATH] [--code-file F] [--state F]
 */
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

static const char *wifi = "/usr/sbin/nocry-wifi", *codefile = "/run/nocry-pair-code", *statefile = "/run/nocry-wifi.state";

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

/* output of `nocry-wifi <verb>`, at most max bytes */
static char *run(const char *verb, size_t max)
{
	int fd[2]; pid_t pid; char *out = calloc(1, max + 1); size_t n = 0; ssize_t k;
	if (!out || pipe(fd)) return out;
	pid = fork();
	if (pid == 0) {
		dup2(fd[1], 1); close(fd[0]); close(fd[1]);
		execl(wifi, wifi, verb, (char *)NULL);
		_exit(127);
	}
	close(fd[1]);
	while (pid > 0 && n < max && (k = read(fd[0], out + n, max - n)) > 0) n += k;
	close(fd[0]);
	if (pid > 0) waitpid(pid, NULL, 0);
	out[n] = 0;
	return out;
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

static const char *HEAD =
	"<!doctype html><html lang=en><meta charset=utf-8>"
	"<meta name=viewport content=\"width=device-width,initial-scale=1\">"
	"<title>nocry-rescue</title><style>"
	"body{font:16px/1.4 system-ui,sans-serif;margin:0 auto;max-width:30em;padding:1em;color:#111;background:#fff}"
	"h1{font-size:1.3em;margin:.2em 0}p{margin:.6em 0}"
	"label{display:block;margin:.7em 0 .2em;font-weight:600}"
	"input[type=text],input[type=password]{width:100%;box-sizing:border-box;font:inherit;padding:.5em;border:1px solid #888;border-radius:4px}"
	".n{display:flex;align-items:center;gap:.6em;padding:.5em .3em;border-bottom:1px solid #ddd;font-weight:400;margin:0}"
	".n small{margin-left:auto;color:#555;white-space:nowrap}"
	"button{font:inherit;font-weight:600;padding:.7em 1.2em;margin-top:1em;border:0;border-radius:4px;background:#1463c2;color:#fff;width:100%}"
	".e{background:#fde8e8;border:1px solid #c33;padding:.6em;border-radius:4px}.s{background:#eef3fa;padding:.6em;border-radius:4px}"
	"@media(prefers-color-scheme:dark){body{background:#111;color:#eee}.s{background:#1c2733}.e{background:#3a1616}.n{border-color:#333}.n small{color:#aaa}}"
	"</style><h1>nocry-rescue</h1>";

static void page(struct buf *b, const char *error, const char *keep_ssid)
{
	char *st = slurp(statefile, 1024), *scan = run("scan", 16384), *line, *save;
	char mode[32], ssid[128], src[160]; int any = 0;
	field(st, "mode", mode, sizeof mode); field(st, "ssid", ssid, sizeof ssid); field(st, "source", src, sizeof src);
	puts_(b, HEAD);
	if (error) { puts_(b, "<p class=e>"); esc(b, error); puts_(b, "</p>"); }
	else if (!strncmp(src, "failed:", 7)) {
		puts_(b, "<p class=e>The board could not join <b>"); esc(b, src + 7);
		puts_(b, "</b>: wrong password, or the network is out of range. Nothing was changed; try again.</p>");
	}
	puts_(b, "<p class=s>This printer board is running its rescue system on its own Wi-Fi network. "
		 "Choose the Wi-Fi network it should join instead, so a computer on that network can reach it. "
		 "Only 2.4&nbsp;GHz networks are in reach of this board.</p>"
		 "<form method=post action=/join><label>Network</label>");
	for (line = strtok_r(scan, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		char *t1 = strchr(line, '\t'), *t2 = t1 ? strchr(t1 + 1, '\t') : NULL; int sig, bars;
		if (!t2 || !t2[1]) continue;
		sig = atoi(line); bars = sig >= -60 ? 3 : sig >= -72 ? 2 : 1;
		puts_(b, "<label class=n><input type=radio name=ssid value=\""); esc(b, t2 + 1); puts_(b, "\"");
		if ((keep_ssid && !strcmp(keep_ssid, t2 + 1)) || (!keep_ssid && !any)) puts_(b, " checked");
		puts_(b, "> "); esc(b, t2 + 1);
		puts_(b, "<small>"); puts_(b, !strncmp(t1 + 1, "open", 4) ? "open, " : "");
		puts_(b, bars == 3 ? "strong" : bars == 2 ? "medium" : "weak"); puts_(b, "</small></label>");
		any = 1;
	}
	if (!any) puts_(b, "<p>No networks were found. Type the name below.</p>");
	puts_(b, "<label for=o>Or another name (hidden network)</label><input id=o type=text name=other maxlength=32 autocapitalize=none autocorrect=off>"
		 "<label for=p>Wi-Fi password</label><input id=p type=password name=pass maxlength=63 autocomplete=off>"
		 "<label for=c>Pairing code (the six digits on the board's screen)</label>"
		 "<input id=c type=text name=code inputmode=numeric maxlength=7 autocomplete=off>"
		 "<button>Join this network</button></form>"
		 "<p><small>The board then leaves this network. If it cannot join, this network comes back within a minute.</small></p>");
	free(st); free(scan);
}

static void joining(struct buf *b, const char *ssid)
{
	puts_(b, HEAD);
	puts_(b, "<p class=s>The board is joining <b>"); esc(b, ssid);
	puts_(b, "</b>.</p><p>This network (nocry-rescue) now disappears and your device will go back to its usual one.</p>"
		 "<p><b>If it works,</b> the board's screen shows the new network and its address there. "
		 "Put the computer with nocry on the same network; nocry finds the board by itself.</p>"
		 "<p><b>If it does not,</b> nocry-rescue comes back within a minute: join it again and check the password.</p>");
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

static void reply(int s, const char *status, struct buf *b)
{
	char h[256]; size_t off = 0; ssize_t k;
	int n = snprintf(h, sizeof h, "HTTP/1.1 %s\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: %zu\r\n"
			 "Cache-Control: no-store\r\nConnection: close\r\n\r\n", status, b->n);
	if (write(s, h, n) != n) return;
	while (off < b->n && (k = write(s, b->p + off, b->n - off)) > 0) off += k;
}

static void serve(int s)
{
	char req[4096]; size_t n = 0, clen = 0; ssize_t k; char *body, *cl; struct buf b = { 0 };
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
	body += 4;
	if (!strncmp(req, "POST /join", 10)) {
		char ssid[80], other[80], pass[80], code[32]; size_t pl;
		form(body, "ssid", ssid, sizeof ssid); form(body, "other", other, sizeof other);
		form(body, "pass", pass, sizeof pass); form(body, "code", code, sizeof code);
		if (other[0]) snprintf(ssid, sizeof ssid, "%s", other);
		pl = strlen(pass);
		if (!ssid[0] || strlen(ssid) > 32) page(&b, "Choose a network, or type its name.", NULL);
		else if (pl && (pl < 8 || pl > 63)) page(&b, "A Wi-Fi password has 8 to 63 characters. Leave it empty only for an open network.", ssid);
		else if (strchr(pass, '"')) page(&b, "A password with a double quote (\") cannot be entered here. Use nocry on a computer instead.", ssid);
		else if (!code_ok(code)) { sleep(2); page(&b, "That is not the pairing code on the board's screen.", ssid); }
		else {
			pid_t pid;
			joining(&b, ssid);
			reply(s, "200 OK", &b);
			shutdown(s, SHUT_WR);
			pid = fork();
			if (pid == 0) {		/* on its own, so it outlives this server: the access point goes down with us */
				setsid();
				if (fork() == 0) {
					int fd; for (fd = 3; fd < 64; fd++) close(fd);
					sleep(2);	/* let the answer reach the phone first */
					if (pass[0]) execl(wifi, wifi, "join", ssid, pass, (char *)NULL);
					else execl(wifi, wifi, "join", ssid, (char *)NULL);
				}
				_exit(0);
			}
			if (pid > 0) waitpid(pid, NULL, 0);
			free(b.p);
			return;
		}
		reply(s, "200 OK", &b);
	} else {
		/* every other address gets the page: that is what makes phones offer it after joining */
		page(&b, NULL, NULL);
		reply(s, "200 OK", &b);
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
		else if (!strcmp(argv[i], "--code-file") && i + 1 < argc) codefile = argv[++i];
		else if (!strcmp(argv[i], "--state") && i + 1 < argc) statefile = argv[++i];
		else { fprintf(stderr, "usage: nocry-webd [--address A] [--port N] [--wifi PATH] [--code-file F] [--state F]\n"); return 2; }
	}
	signal(SIGPIPE, SIG_IGN);
	ls = socket(AF_INET, SOCK_STREAM, 0);
	setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	memset(&sa, 0, sizeof sa);
	sa.sin_family = AF_INET; sa.sin_port = htons(port);
	if (!inet_aton(addr, &sa.sin_addr)) { fprintf(stderr, "nocry-webd: bad address %s\n", addr); return 2; }
	if (ls < 0 || bind(ls, (struct sockaddr *)&sa, sizeof sa) || listen(ls, 8)) { fprintf(stderr, "nocry-webd: %s:%d: %s\n", addr, port, strerror(errno)); return 1; }
	fprintf(stderr, "nocry-webd: set-up page at http://%s:%d\n", addr, port);
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
