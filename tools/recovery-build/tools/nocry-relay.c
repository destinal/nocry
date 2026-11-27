/*
 * nocry-relay: accepts TCP connections on a port of every interface and passes each on to the
 * same or another port of this machine.  The rescue system puts it in front of adbd, which
 * listens on 127.0.0.1 only, while the owner has adb over the network turned on (nocry-remote).
 *
 *   nocry-relay LISTEN-PORT [TO-PORT]
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>

static void pump(int a, int b)
{
	struct pollfd p[2] = { { a, POLLIN, 0 }, { b, POLLIN, 0 } }; char buf[16384];
	for (;;) {
		int i;
		if (poll(p, 2, -1) < 0) return;
		for (i = 0; i < 2; i++) {
			if (p[i].revents & (POLLIN | POLLHUP | POLLERR)) {
				ssize_t n = read(p[i].fd, buf, sizeof buf), o = 0, k;
				if (n <= 0) return;
				while (o < n) { k = write(p[!i].fd, buf + o, n - o); if (k <= 0) return; o += k; }
			}
		}
	}
}

int main(int argc, char **argv)
{
	int from = argc > 1 ? atoi(argv[1]) : 0, to = argc > 2 ? atoi(argv[2]) : from, ls, one = 1;
	struct sockaddr_in6 sa; struct sockaddr_in in;

	if (from <= 0 || to <= 0) { fprintf(stderr, "usage: nocry-relay LISTEN-PORT [TO-PORT]\n"); return 2; }
	signal(SIGCHLD, SIG_IGN); signal(SIGPIPE, SIG_IGN);
	/* not on 127.0.0.1 itself: that is where the service being relayed to listens */
	memset(&sa, 0, sizeof sa); sa.sin6_family = AF_INET6; sa.sin6_port = htons(from); sa.sin6_addr = in6addr_any;
	ls = socket(AF_INET6, SOCK_STREAM, 0);
	setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	if (ls < 0 || bind(ls, (struct sockaddr *)&sa, sizeof sa) || listen(ls, 8)) { perror("nocry-relay: listen"); return 1; }
	for (;;) {
		struct sockaddr_in6 peer; socklen_t pl = sizeof peer; int c = accept(ls, (struct sockaddr *)&peer, &pl), s;
		if (c < 0) continue;
		if (fork() == 0) {
			close(ls);
			memset(&in, 0, sizeof in); in.sin_family = AF_INET; in.sin_port = htons(to); in.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
			s = socket(AF_INET, SOCK_STREAM, 0);
			if (s < 0 || connect(s, (struct sockaddr *)&in, sizeof in)) _exit(1);
			setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one); setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
			pump(c, s);
			_exit(0);
		}
		close(c);
	}
}
