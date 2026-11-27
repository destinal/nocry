/*
 * nocry-image: a range of the board's storage to an image file and back, for the rescue
 * system's backups on a USB flash drive (docs/nocry-rescue.md).
 *
 *   nocry-image save DEV OFFSET SIZE FILE [--split BYTES] [--progress P]
 *       DEV[OFFSET, OFFSET+SIZE) -> FILE, and its SHA-256 -> FILE.sha256 (the format of
 *       sha256sum and of `nocry backup` on a computer).  With --split the image is written
 *       as FILE.001, FILE.002, ... of at most BYTES each (FAT32 holds no file of 4 GiB);
 *       the checksum is that of all pieces in a row.  The file is read back and compared
 *       with the checksum before it counts.
 *   nocry-image info FILE
 *       "bytes=", "pieces=", "checksum=yes|no" of FILE or FILE.001...
 *   nocry-image load FILE DEV OFFSET SIZE [--skip OFF:LEN]... [--progress P]
 *       the image -> DEV[OFFSET, ...).  First the whole image is checked against
 *       FILE.sha256 (if there is one); then only what differs is written, and all of it is
 *       read back and compared.  --skip leaves a range of the image (offsets in the image)
 *       alone on the device.
 *
 * P gets a line "<phase> <done> <total>" a few times a second.  SIGTERM stops at the next
 * megabyte: save removes what it wrote; load leaves the device partly written.
 * Exit: 0 done, 1 failed (the reason on standard error), 3 stopped.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#define _FILE_OFFSET_BITS 64
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <openssl/evp.h>

#define CHUNK (1 << 20)

static volatile sig_atomic_t stop;
static void on_term(int s) { (void)s; stop = 1; }

static const char *progress;
static void say(const char *phase, long long done, long long total)
{
	static struct timespec last; struct timespec now; char tmp[600]; FILE *f;
	if (!progress) return;
	clock_gettime(CLOCK_MONOTONIC, &now);
	if (done < total && (now.tv_sec - last.tv_sec) * 1000 + (now.tv_nsec - last.tv_nsec) / 1000000 < 300) return;
	last = now;
	snprintf(tmp, sizeof tmp, "%s.new", progress);
	if (!(f = fopen(tmp, "w"))) return;
	fprintf(f, "%s %lld %lld\n", phase, done, total); fclose(f);
	rename(tmp, progress);
}

static int fail(const char *fmt, const char *a)
{
	fprintf(stderr, "nocry-image: "); fprintf(stderr, fmt, a); fprintf(stderr, "\n");
	return 1;
}

/* ---- an image in one file or in pieces ----------------------------------------------- */
struct set { char base[900]; long long max; int n; long long size[1000]; long long total; int fd, cur; };

static void piece_name(struct set *s, int k, char *out, size_t n)
{
	if (s->max) snprintf(out, n, "%s.%03d", s->base, k + 1); else snprintf(out, n, "%s", s->base);
}
/* what is there to read */
static int set_find(struct set *s, const char *base)
{
	struct stat st; char p[1000];
	memset(s, 0, sizeof *s); s->fd = -1; s->cur = -1;
	snprintf(s->base, sizeof s->base, "%s", base);
	if (!stat(base, &st) && S_ISREG(st.st_mode)) { s->n = 1; s->size[0] = s->total = st.st_size; return 0; }
	s->max = 1;
	for (s->n = 0; s->n < 1000; s->n++) {
		piece_name(s, s->n, p, sizeof p);
		if (stat(p, &st) || !S_ISREG(st.st_mode)) break;
		s->size[s->n] = st.st_size; s->total += st.st_size;
	}
	return s->n ? 0 : -1;
}
static int set_read(struct set *s, long long off, unsigned char *buf, size_t n)
{
	while (n) {
		long long start = 0; int k; size_t m; char p[1000];
		for (k = 0; k < s->n && off >= start + s->size[k]; k++) start += s->size[k];
		if (k == s->n) return -1;
		if (k != s->cur) {
			if (s->fd >= 0) close(s->fd);
			piece_name(s, k, p, sizeof p);
			if ((s->fd = open(p, O_RDONLY)) < 0) return -1;
			s->cur = k;
		}
		m = n; if ((long long)m > start + s->size[k] - off) m = (size_t)(start + s->size[k] - off);
		if (pread(s->fd, buf, m, off - start) != (ssize_t)m) return -1;
		off += m; buf += m; n -= m;
	}
	return 0;
}
static void set_drop_cache(struct set *s) { if (s->fd >= 0) { posix_fadvise(s->fd, 0, 0, POSIX_FADV_DONTNEED); close(s->fd); s->fd = -1; s->cur = -1; } }

static void hex(const unsigned char *d, char *out) { int i; for (i = 0; i < 32; i++) sprintf(out + 2 * i, "%02x", d[i]); }

/* the checksum noted beside the image: 1 and the 64 digits, or 0 */
static int noted_sum(const char *base, char *out)
{
	char p[1000]; FILE *f; int ok;
	snprintf(p, sizeof p, "%s.sha256", base);
	if (!(f = fopen(p, "r"))) return 0;
	ok = fscanf(f, "%64[0-9a-fA-F]", out) == 1 && strlen(out) == 64;
	fclose(f);
	if (ok) { char *c; for (c = out; *c; c++) if (*c >= 'A' && *c <= 'F') *c += 32; }
	return ok;
}

/* the SHA-256 of the whole image as it is on the drive now */
static int set_sum(struct set *s, const char *phase, char *out)
{
	static unsigned char buf[CHUNK]; unsigned char d[32]; unsigned int dl; long long off;
	EVP_MD_CTX *c = EVP_MD_CTX_new();
	EVP_DigestInit_ex(c, EVP_sha256(), NULL);
	for (off = 0; off < s->total; ) {
		size_t n = s->total - off > CHUNK ? CHUNK : (size_t)(s->total - off);
		if (stop) { EVP_MD_CTX_free(c); return 3; }
		if (set_read(s, off, buf, n)) { EVP_MD_CTX_free(c); return 1; }
		EVP_DigestUpdate(c, buf, n);
		off += n; say(phase, off, s->total);
	}
	EVP_DigestFinal_ex(c, d, &dl); EVP_MD_CTX_free(c);
	hex(d, out);
	return 0;
}

static int full_pread(int fd, unsigned char *b, size_t n, long long off)
{
	while (n) { ssize_t k = pread(fd, b, n, off); if (k <= 0) return -1; b += k; n -= k; off += k; }
	return 0;
}
static int full_pwrite(int fd, const unsigned char *b, size_t n, long long off)
{
	while (n) { ssize_t k = pwrite(fd, b, n, off); if (k <= 0) return -1; b += k; n -= k; off += k; }
	return 0;
}

/* ---- save ------------------------------------------------------------------------------ */
static void save_undo(struct set *s, int pieces)
{
	char p[1000]; int k;
	for (k = 0; k < pieces; k++) { piece_name(s, k, p, sizeof p); unlink(p); }
	if (pieces) { snprintf(p, sizeof p, "%s.sha256", s->base); unlink(p); }	/* never that of an image that was there before */
}
static int save(const char *dev, long long off, long long size, const char *file, long long split)
{
	static unsigned char buf[CHUNK]; static struct set s; unsigned char d[32]; unsigned int dl;
	char p[1000], want[65], got[65]; const char *name; long long done, in_piece = 0;
	int in = open(dev, O_RDONLY), out = -1, pieces = 0, rc; FILE *f;
	EVP_MD_CTX *c;

	if (in < 0) return fail("cannot open %s", dev);
	memset(&s, 0, sizeof s); s.fd = -1; s.cur = -1;
	snprintf(s.base, sizeof s.base, "%s", file);
	s.max = split && size > split ? split : 0;
	c = EVP_MD_CTX_new(); EVP_DigestInit_ex(c, EVP_sha256(), NULL);
	for (done = 0; done < size; ) {
		size_t n = size - done > CHUNK ? CHUNK : (size_t)(size - done);
		if (stop) { if (out >= 0) close(out); save_undo(&s, pieces); return 3; }
		if (out < 0 || (s.max && in_piece + (long long)n > s.max)) {
			if (out >= 0 && (fsync(out) | close(out))) { save_undo(&s, pieces); return fail("could not write to the drive (%s)", strerror(errno)); }
			piece_name(&s, pieces, p, sizeof p);
			if ((out = open(p, O_WRONLY | O_CREAT | O_EXCL, 0644)) < 0) { save_undo(&s, pieces); return fail("cannot create the file on the drive (%s)", strerror(errno)); }
			pieces++; in_piece = 0;
		}
		if (full_pread(in, buf, n, off + done)) { close(out); save_undo(&s, pieces); return fail("could not read the board's storage (%s)", strerror(errno)); }
		if (full_pwrite(out, buf, n, in_piece)) { int e = errno; close(out); save_undo(&s, pieces); return fail("could not write to the drive (%s)", strerror(e)); }
		EVP_DigestUpdate(c, buf, n);
		done += n; in_piece += n;
		/* not all of it in the page cache at once, and the progress stays true */
		if (!(done & ((64 << 20) - 1))) { fdatasync(out); posix_fadvise(out, 0, 0, POSIX_FADV_DONTNEED); }
		say("copying", done, size);
	}
	EVP_DigestFinal_ex(c, d, &dl); EVP_MD_CTX_free(c); hex(d, want);
	if (fsync(out) | close(out)) { save_undo(&s, pieces); return fail("could not write to the drive (%s)", strerror(errno)); }
	/* what the drive gives back, not what the memory still holds */
	if (set_find(&s, file) || s.total != size) { save_undo(&s, pieces); return fail("the file on the drive is not complete%s", ""); }
	{ int k; for (k = 0; k < s.n; k++) { piece_name(&s, k, p, sizeof p); if ((out = open(p, O_RDONLY)) >= 0) { posix_fadvise(out, 0, 0, POSIX_FADV_DONTNEED); close(out); } } }
	rc = set_sum(&s, "verifying", got); set_drop_cache(&s);
	if (rc == 3) { save_undo(&s, pieces); return 3; }
	if (rc || strcmp(want, got)) { save_undo(&s, pieces); return fail("the file read back from the drive does not match what was written%s", ""); }
	snprintf(p, sizeof p, "%s.sha256", file);
	name = strrchr(file, '/'); name = name ? name + 1 : file;
	if (!(f = fopen(p, "w")) || fprintf(f, "%s  %s\n", want, name) < 0 || fflush(f) || fsync(fileno(f)) || fclose(f)) { save_undo(&s, pieces); return fail("could not write the checksum file%s", ""); }
	sync();
	printf("bytes=%lld\npieces=%d\nsha256=%s\n", size, pieces, want);
	return 0;
}

/* ---- load ------------------------------------------------------------------------------ */
static long long skip_off[8], skip_len[8]; static int skips;
/* how much of [off, off+n) may be written, from its start (0: it starts in a skipped range, then *jump is where that ends) */
static size_t writable(long long off, size_t n, long long *jump)
{
	int i;
	for (i = 0; i < skips; i++) {
		if (off >= skip_off[i] && off < skip_off[i] + skip_len[i]) { *jump = skip_off[i] + skip_len[i]; return 0; }
		if (skip_off[i] > off && skip_off[i] < off + (long long)n) n = (size_t)(skip_off[i] - off);
	}
	return n;
}
static int load(const char *file, const char *dev, long long off, long long size)
{
	static unsigned char a[CHUNK], b[CHUNK]; static struct set s; char want[65], got[65];
	long long done, jump, written = 0; int out, rc, pass;

	if (set_find(&s, file)) return fail("no such image: %s", file);
	if (s.total != size) return fail("the image is not the size of what it is to be written to%s", "");
	if (noted_sum(file, want)) {
		rc = set_sum(&s, "verifying", got);
		if (rc == 3) return 3;
		if (rc) return fail("the image cannot be read from the drive%s", "");
		if (strcmp(want, got)) return fail("the image is damaged: it does not match its checksum file%s", "");
	}
	if ((out = open(dev, O_RDWR)) < 0) return fail("cannot open %s", dev);
	/* pass 0 writes what differs; pass 1 reads everything back (not from memory) and compares */
	for (pass = 0; pass < 2; pass++) {
		if (pass) { fsync(out); posix_fadvise(out, 0, 0, POSIX_FADV_DONTNEED); }
		for (done = 0; done < size; ) {
			size_t n = size - done > CHUNK ? CHUNK : (size_t)(size - done);
			if (stop) { fsync(out); return 3; }
			n = writable(done, n, &jump);
			if (!n) { done = jump > size ? size : jump; continue; }
			if (set_read(&s, done, a, n)) return fail("the image cannot be read from the drive%s", "");
			if (full_pread(out, b, n, off + done)) return fail("could not read the board's storage (%s)", strerror(errno));
			if (memcmp(a, b, n)) {
				if (pass) return fail("read-back mismatch: the board's storage did not take the image%s", "");
				if (full_pwrite(out, a, n, off + done)) return fail("could not write the board's storage (%s)", strerror(errno));
				written += n;
			}
			done += n;
			say(pass ? "verifying the board" : "writing", done, size);
		}
	}
	if (fsync(out) | close(out)) return fail("could not write the board's storage (%s)", strerror(errno));
	printf("bytes=%lld\nwritten=%lld\n", size, written);
	return 0;
}

int main(int argc, char **argv)
{
	static struct set s; struct sigaction sa; long long split = 0; char sum[65]; int i;

	memset(&sa, 0, sizeof sa); sa.sa_handler = on_term; sigaction(SIGTERM, &sa, NULL); sigaction(SIGINT, &sa, NULL);
	if (argc == 3 && !strcmp(argv[1], "info")) {
		if (set_find(&s, argv[2])) return fail("no such image: %s", argv[2]);
		printf("bytes=%lld\npieces=%d\nchecksum=%s\n", s.total, s.n, noted_sum(argv[2], sum) ? "yes" : "no");
		return 0;
	}
	if (argc >= 6 && (!strcmp(argv[1], "save") || !strcmp(argv[1], "load"))) {
		for (i = 6; i + 1 < argc; i += 2) {
			if (!strcmp(argv[i], "--progress")) progress = argv[i + 1];
			else if (!strcmp(argv[i], "--split")) split = atoll(argv[i + 1]);
			else if (!strcmp(argv[i], "--skip") && skips < 8 && sscanf(argv[i + 1], "%lld:%lld", &skip_off[skips], &skip_len[skips]) == 2) skips++;
			else break;
		}
		if (i == argc) {
			if (!strcmp(argv[1], "save")) return save(argv[2], atoll(argv[3]), atoll(argv[4]), argv[5], split & ~(long long)(CHUNK - 1));
			return load(argv[2], argv[3], atoll(argv[4]), atoll(argv[5]));
		}
	}
	fprintf(stderr, "usage: nocry-image save DEV OFFSET SIZE FILE [--split BYTES] [--progress P]\n"
			"       nocry-image info FILE\n"
			"       nocry-image load FILE DEV OFFSET SIZE [--skip OFF:LEN]... [--progress P]\n");
	return 2;
}
