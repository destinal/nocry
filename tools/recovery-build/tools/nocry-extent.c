/*
 * nocry-extent: where a file's blocks are on the disk, for a loader that cannot read a file
 * system.  SPDX-License-Identifier: GPL-2.0-or-later
 *
 *   nocry-extent FILE PARTITION-START-SECTOR
 *       prints the note the nocryboot loader reads (docs/nocryboot.md):
 *           nocryboot:rescue:<sum> <sector>+<count> <sector>+<count> ...
 *       all hexadecimal: the sum of the file's 32-bit words (the last block filled up with
 *       zeros, as it is on the disk), then the runs of 512-byte sectors, counted from the
 *       start of the whole disk, that hold the file in order.
 *   nocry-extent --check DISK < note
 *       reads those runs back from the disk itself and says whether the sum is right:
 *       exit 0 if it is.
 *
 * The file must be written and synced first.  Runs are at most 0xffff sectors (the loader
 * reads one run with one command), and the note must fit one sector.
 */
#define _FILE_OFFSET_BITS 64
#include <fcntl.h>
#include <linux/fs.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

static uint32_t sum_words(const unsigned char *b, size_t n, uint32_t s)
{
	size_t i;
	for (i = 0; i + 4 <= n; i += 4) s += b[i] | b[i + 1] << 8 | b[i + 2] << 16 | (uint32_t)b[i + 3] << 24;
	return s;
}

static int check(const char *disk)
{
	char note[600], *p; size_t n = fread(note, 1, sizeof note - 1, stdin); uint32_t want, sum = 0;
	int fd = open(disk, O_RDONLY); static unsigned char buf[1 << 16];
	note[n] = 0;
	if (fd < 0 || strncmp(note, "nocryboot:rescue:", 17)) return 2;
	p = note + 17; want = strtoul(p, &p, 16);
	for (;;) {
		unsigned long long sector = strtoull(p, &p, 16), count, done;
		if (*p != '+') break;
		count = strtoull(p + 1, &p, 16);
		for (done = 0; done < count * 512; ) {
			size_t k = count * 512 - done > sizeof buf ? sizeof buf : count * 512 - done;
			if (pread(fd, buf, k, sector * 512 + done) != (ssize_t)k) return 2;
			sum = sum_words(buf, k, sum); done += k;
		}
	}
	return sum == want ? 0 : 1;
}

int main(int argc, char **argv)
{
	int fd, bs, len; unsigned long long start, run_s = 0, run_n = 0, blocks, i; struct stat st; uint32_t sum = 0;
	static unsigned char buf[1 << 16]; ssize_t k; char note[512];
	if (argc == 3 && !strcmp(argv[1], "--check")) return check(argv[2]);
	if (argc != 3) { fprintf(stderr, "usage: nocry-extent FILE PARTITION-START-SECTOR | nocry-extent --check DISK < note\n"); return 2; }
	start = strtoull(argv[2], NULL, 0);
	fd = open(argv[1], O_RDONLY);
	if (fd < 0 || fstat(fd, &st) || ioctl(fd, FIGETBSZ, &bs) || bs < 512) { perror(argv[1]); return 1; }
	while ((k = read(fd, buf, sizeof buf)) > 0) {
		if (k % 4) { memset(buf + k, 0, 4 - k % 4); k += 4 - k % 4; }	/* the zero fill of the last block adds nothing */
		sum = sum_words(buf, k, sum);
	}
	len = snprintf(note, sizeof note, "nocryboot:rescue:%x", sum);
	blocks = (st.st_size + bs - 1) / bs;
	for (i = 0; i <= blocks; i++) {
		unsigned long long phys = 0;
		if (i < blocks) {
			int b = (int)i;
			if (ioctl(fd, FIBMAP, &b) || !b) { fprintf(stderr, "%s: block %llu is not on the disk (a hole, or no permission)\n", argv[1], i); return 1; }
			phys = start + (unsigned long long)b * (bs / 512);
		}
		if (run_n && i < blocks && phys == run_s + run_n && run_n + bs / 512 <= 0xffff) { run_n += bs / 512; continue; }
		if (run_n) {
			len += snprintf(note + len, sizeof note - len, " %llx+%llx", run_s, run_n);
			if (len >= 500) { fprintf(stderr, "%s is in too many pieces on the disk for the loader's note\n", argv[1]); return 1; }
		}
		run_s = phys; run_n = bs / 512;
	}
	printf("%s\n", note);
	return 0;
}
