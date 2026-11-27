/*
 * nocry-rescue-copy: the copies of the rescue system that the nocryboot loader starts
 * (docs/nocryboot.md).  A copy is a uImage named "nocry-rescue..." in the last 32 MiB of a
 * root file system partition, far behind the end of the system image there; the loader
 * starts it only if the CRCs in its header are right, and this tool checks exactly that.
 *
 *   nocry-rescue-copy check FILE          is FILE such an image?            prints its bytes
 *   nocry-rescue-copy status PART         "yes <bytes>", "damaged" or "no"
 *   nocry-rescue-copy store PART FILE     write FILE into PART's room, read it back
 *   nocry-rescue-copy erase PART          blank the start of the room
 *   nocry-rescue-copy keep-put PART FILE  a small file (the touch calibration) into the last
 *   nocry-rescue-copy keep-get PART       4 KiB of the room / back out of it
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#define _FILE_OFFSET_BITS 64
#include <arpa/inet.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ROOM	(32u << 20)
#define HEAD	64
#define KEEP	4096		/* the end of the room: "nocry-keep\0", length, CRC, the bytes */
#define IMAGE	(ROOM - KEEP)	/* what an image may take */

static uint32_t table[256];
static uint32_t crc(uint32_t c, const unsigned char *p, size_t n)
{
	c = ~c;
	while (n--) c = table[(c ^ *p++) & 0xff] ^ (c >> 8);
	return ~c;
}
static uint32_t be(const unsigned char *p) { uint32_t v; memcpy(&v, p, 4); return ntohl(v); }

static int die(const char *what) { fprintf(stderr, "nocry-rescue-copy: %s\n", what); return 1; }

/* the image at `off` of `fd`: its length with the header, 0 if there is none, -1 if damaged */
static long image_at(int fd, off_t off, off_t room)
{
	static unsigned char buf[1 << 16];
	unsigned char h[HEAD], z[HEAD];
	uint32_t len, c = 0, left;
	ssize_t n;

	if (pread(fd, h, HEAD, off) != HEAD || be(h) != 0x27051956 || memcmp(h + 32, "nocry-rescue", 12))
		return 0;
	memcpy(z, h, HEAD); memset(z + 4, 0, 4);
	len = be(h + 12);
	if (crc(0, z, HEAD) != be(h + 4) || !len || (off_t)len + HEAD > room)
		return -1;
	for (left = len, off += HEAD; left; left -= n, off += n) {
		n = pread(fd, buf, left < sizeof buf ? left : sizeof buf, off);
		if (n <= 0) return -1;
		c = crc(c, buf, n);
	}
	return c == be(h + 24) ? (long)len + HEAD : -1;
}

/* where the room starts in the partition, or -1: too small, or a system image reaches into it */
static off_t room_of(int fd)
{
	unsigned char sb[48];
	off_t size = lseek(fd, 0, SEEK_END);
	uint64_t used = 0;
	int i;

	if (size < (off_t)ROOM * 2 || size % 512)
		return -1;
	if (pread(fd, sb, sizeof sb, 0) == sizeof sb && !memcmp(sb, "hsqs", 4)) {
		for (i = 7; i >= 0; i--) used = used << 8 | sb[40 + i];
		if (used > (uint64_t)(size - ROOM))
			return -1;
	}
	return size - ROOM;
}

int main(int argc, char **argv)
{
	static unsigned char a[1 << 16], b[1 << 16];
	off_t room, off;
	long len, n;
	int i, k, fd, in;
	uint32_t c;

	for (i = 0; i < 256; i++) {
		for (c = i, k = 0; k < 8; k++) c = c & 1 ? c >> 1 ^ 0xedb88320 : c >> 1;
		table[i] = c;
	}
	if (argc == 3 && !strcmp(argv[1], "check")) {
		if ((fd = open(argv[2], O_RDONLY)) < 0) return die("cannot open the file");
		len = image_at(fd, 0, IMAGE);
		if (len <= 0 || len != lseek(fd, 0, SEEK_END)) return die("that is not a nocry rescue image (or it is damaged)");
		printf("%ld\n", len);
		return 0;
	}
	if (argc < 3 || (fd = open(argv[2], strcmp(argv[1], "status") && strcmp(argv[1], "keep-get") ? O_RDWR : O_RDONLY)) < 0)
		return die("usage: nocry-rescue-copy check FILE | status|erase|keep-get PART | store|keep-put PART FILE");
	room = room_of(fd);
	if (argc == 3 && !strcmp(argv[1], "status")) {
		len = room < 0 ? 0 : image_at(fd, room, IMAGE);
		if (len > 0) printf("yes %ld\n", len); else puts(len ? "damaged" : "no");
		return 0;
	}
	if (room < 0)
		return die("no room for a copy in this partition (the system image reaches into the last 32 MiB)");
	if (argc == 3 && !strcmp(argv[1], "erase")) {
		memset(a, 0, sizeof a);
		if (pwrite(fd, a, sizeof a, room) != sizeof a || fsync(fd)) return die("could not write");
		return 0;
	}
	if (argc == 4 && !strcmp(argv[1], "keep-put")) {
		memset(a, 0, KEEP); memcpy(a, "nocry-keep", 10);
		if ((in = open(argv[3], O_RDONLY)) < 0 || (n = read(in, a + 20, KEEP - 20)) <= 0 || n == KEEP - 20)
			return die("nothing to keep, or more than fits");
		c = htonl(n); memcpy(a + 12, &c, 4); c = htonl(crc(0, a + 20, n)); memcpy(a + 16, &c, 4);
		if (pwrite(fd, a, KEEP, room + IMAGE) != KEEP || fsync(fd)) return die("could not write");
		return 0;
	}
	if (argc == 3 && !strcmp(argv[1], "keep-get")) {
		if (pread(fd, a, KEEP, room + IMAGE) != KEEP || memcmp(a, "nocry-keep", 11) ||
		    (n = be(a + 12)) <= 0 || n >= KEEP - 20 || crc(0, a + 20, n) != be(a + 16))
			return 1;
		return fwrite(a + 20, 1, n, stdout) != (size_t)n;
	}
	if (argc != 4 || strcmp(argv[1], "store"))
		return die("usage: nocry-rescue-copy check FILE | status|erase|keep-get PART | store|keep-put PART FILE");
	if ((in = open(argv[3], O_RDONLY)) < 0) return die("cannot open the image");
	len = image_at(in, 0, IMAGE);
	if (len <= 0 || len != lseek(in, 0, SEEK_END)) return die("that is not a nocry rescue image (or it is damaged)");
	/* the header last: until the whole image is there the loader sees no copy here */
	memset(a, 0, 512);
	if (pwrite(fd, a, 512, room) != 512 || fsync(fd)) return die("could not write");
	for (off = len; off > 0; ) {
		n = off % (long)sizeof a ? off % (long)sizeof a : (long)sizeof a;
		off -= n;
		if (pread(in, a, n, off) != n) return die("could not read the image");
		if (pread(fd, b, n, room + off) == n && !memcmp(a, b, n)) continue;	/* already there */
		if (pwrite(fd, a, n, room + off) != n) return die("could not write");
	}
	if (fsync(fd)) return die("could not write");
	posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
	if (image_at(fd, room, IMAGE) != len) return die("read-back checksum mismatch");
	printf("%ld\n", len);
	return 0;
}
