// SPDX-License-Identifier: GPL-2.0+
/*
 * nocry-fbd: fastboot service for the nocry recovery Linux (FunctionFS).
 *
 * Standard commands (stock `fastboot` works): getvar, download, flash (raw+sparse,
 * read-back verified), erase, fetch, set_active, flashing (un)lock_critical, reboot.
 * Streaming extensions for the nocry client (no RAM-size limit):
 *   nocry-write:<name>:<off>:<size>           -> DATA, host sends <size> bytes, OKAY
 *   nocry-hash:<sha256|xxh64>:<name>:<off>:<size>:<chunk>
 *                                             -> DATA, one digest per chunk, OKAY
 * Region names are the same as in the U-Boot nocry fastboot stage2:
 *   GPT names, kernel_a/_b rootfs_a/_b rtos_a/_b, bootloader, gpt, mmc0.
 * mmc0, gpt, bootloader and sn_mac need `flashing unlock_critical` first.
 *
 * Test mode (host): nocry-fbd --stdio <disk image>; packets are framed with a
 * little-endian u32 length, data phases are raw.
 */
#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64	/* 32-bit userland: the eMMC is larger than 2 GiB */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <linux/fs.h>
#include <openssl/evp.h>
#ifndef FBD_NO_FFS
#include <linux/usb/ch9.h>
#include <linux/usb/functionfs.h>
#endif

_Static_assert(sizeof(off_t) == 8, "64-bit file offsets required");
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;

#define FBD_VERSION	"nocry-fbd 0.1"
#define MIB		(1u << 20)
#define BUFCAP		(4 * MIB)		/* queue buffer size = largest block */
#define NQ		8
#define USB_CHUNK	(256 * 1024)
#define DL_MAX		(64 * MIB)
#define FETCH_MAX	0x40000000u
#define CMD_MAX		4096

static int stdio_mode, ep_out = -1, ep_in = -1, disk = -1;
static u64 disk_blocks;
static int critical_unlocked;

/* ------------------------------------------------------------ transport -- */

static int rd_full(int fd, void *b, size_t n)
{
	size_t d = 0;
	while (d < n) {
		ssize_t r = read(fd, (char *)b + d, n - d);
		if (r <= 0) return -1;
		d += r;
	}
	return 0;
}
static int wr_full(int fd, const void *b, size_t n)
{
	size_t d = 0;
	while (d < n) {
		ssize_t r = write(fd, (const char *)b + d, n - d);
		if (r <= 0) return -1;
		d += r;
	}
	return 0;
}

/* one command packet; returns length, <0 on error */
static int pkt_read(char *buf, size_t max)
{
	if (stdio_mode) {
		u32 n;
		if (rd_full(0, &n, 4) || n > max || rd_full(0, buf, n)) return -1;
		return n;
	}
	return read(ep_out, buf, max);
}
static int pkt_write(const char *buf, size_t n)
{
	if (stdio_mode) {
		u32 l = n;
		return wr_full(1, &l, 4) || wr_full(1, buf, n) ? -1 : 0;
	}
	return write(ep_in, buf, n) == (ssize_t)n ? 0 : -1;
}
/* data phase, host -> device: exactly n bytes; buf has 512 bytes of slack */
static int data_read(u8 *buf, size_t n)
{
	size_t d = 0;
	if (stdio_mode) return rd_full(0, buf, n);
	while (d < n) {
		size_t want = n - d > USB_CHUNK ? USB_CHUNK : n - d;
		ssize_t r = read(ep_out, buf + d, (want + 511) & ~511u);
		if (r <= 0) return -1;
		d += r;
	}
	return d == n ? 0 : -1;
}
static int data_write(const u8 *buf, size_t n)
{
	size_t d = 0;
	if (stdio_mode) return wr_full(1, buf, n);
	while (d < n) {
		size_t k = n - d > USB_CHUNK ? USB_CHUNK : n - d;
		if (write(ep_in, buf + d, k) != (ssize_t)k) return -1;
		d += k;
	}
	return 0;
}

static int resp(const char *kind, const char *fmt, ...)
{
	char b[260];
	va_list ap;
	int n;
	memcpy(b, kind, 4);
	va_start(ap, fmt);
	n = vsnprintf(b + 4, 252, fmt, ap);
	va_end(ap);
	if (n > 251) n = 251;
	return pkt_write(b, 4 + n);
}
#define OKAY(...) resp("OKAY", __VA_ARGS__)
#define FAIL(...) resp("FAIL", __VA_ARGS__)
#define INFO(...) resp("INFO", __VA_ARGS__)

/* ----------------------------------------------------------------- disk -- */

static int dread(u64 off, void *b, size_t n)  { return pread(disk, b, n, off) == (ssize_t)n ? 0 : -1; }
static int dwrite(u64 off, const void *b, size_t n) { return pwrite(disk, b, n, off) == (ssize_t)n ? 0 : -1; }
static void *xalign(size_t n) { void *p; return posix_memalign(&p, 4096, n) ? NULL : p; }

static u8 *vbuf, *zbuf;		/* 1 MiB each */

static int dwrite_verified(u64 off, const u8 *b, u64 n)
{
	while (n) {
		size_t k = n > MIB ? MIB : n;
		if (dwrite(off, b, k) || dread(off, vbuf, k)) return -EIO;
		if (memcmp(vbuf, b, k)) return -EILSEQ;
		off += k; b += k; n -= k;
	}
	return 0;
}
static int dfill_verified(u64 off, u64 n, u32 pattern)
{
	u32 *z = (u32 *)zbuf; int r; size_t i;
	for (i = 0; i < MIB / 4; i++) z[i] = pattern;
	while (n) {
		size_t k = n > MIB ? MIB : n;
		if ((r = dwrite_verified(off, zbuf, k))) return r;
		off += k; n -= k;
	}
	return 0;
}

/* ------------------------------------------------------------------ GPT -- */

static u32 crc32(const u8 *p, size_t n)
{
	u32 c = ~0u; int k;
	while (n--) { c ^= *p++; for (k = 0; k < 8; k++) c = (c >> 1) ^ (0xedb88320 & -(c & 1)); }
	return ~c;
}
static u32 le32(const u8 *p) { return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24; }
static u64 le64(const u8 *p) { return le32(p) | (u64)le32(p + 4) << 32; }
static void put32(u8 *p, u32 v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static void put64(u8 *p, u64 v) { put32(p, v); put32(p + 4, v >> 32); }

/* header at img+512: sig 0, hdr_size 12, hdr_crc 16, my_lba 24, alt_lba 32, first 40,
 * last 48, entries_lba 72, n_entries 80, entry_size 84, entries_crc 88 */
static u32 gpt_hcrc(u8 *h)
{
	u8 t[512]; u32 sz = le32(h + 12);
	memcpy(t, h, sz); memset(t + 16, 0, 4);
	return crc32(t, sz);
}
static const char *gpt_check(u8 *img, u64 len, u64 disk_last)
{
	u8 *h = img + 512;
	u32 hs, es, ne; u64 eb;
	if (len < 1024 || memcmp(h, "EFI PART", 8)) return "no GPT header at LBA 1";
	hs = le32(h + 12); es = le32(h + 84); ne = le32(h + 80); eb = (u64)es * ne;
	if (hs < 92 || hs > 512 || gpt_hcrc(h) != le32(h + 16)) return "GPT header CRC mismatch";
	if (es < 128 || !ne || ne > 512 || le64(h + 72) != 2 || 1024 + eb > len || 1024 + eb > 34 * 512)
		return "unsupported GPT entry layout";
	if (crc32(img + 1024, eb) != le32(h + 88)) return "GPT entries CRC mismatch";
	if (le64(h + 40) < 34 || le64(h + 48) > disk_last - 33 || le64(h + 40) > le64(h + 48))
		return "GPT usable range does not fit this disk";
	return NULL;
}

struct part { char name[37]; u64 lba, nblk; };
static struct part parts[128];
static int nparts;

static int gpt_load(void)
{
	static u8 *img;
	u8 *h; u32 i, es, ne;
	if (!img) img = xalign(34 * 512);
	nparts = 0;
	if (dread(0, img, 34 * 512) || gpt_check(img, 34 * 512, disk_blocks - 1)) return -1;
	h = img + 512; es = le32(h + 84); ne = le32(h + 80);
	for (i = 0; i < ne && nparts < 128; i++) {
		u8 *e = img + 1024 + i * es; static const u8 z[16];
		u64 first = le64(e + 32), last = le64(e + 40); int k;
		if (!memcmp(e, z, 16) || last < first) continue;
		for (k = 0; k < 36; k++) {
			u16 ch = e[56 + 2 * k] | e[57 + 2 * k] << 8;
			if (!ch) break;
			parts[nparts].name[k] = ch < 0x80 ? ch : '?';
		}
		parts[nparts].name[k] = 0;
		parts[nparts].lba = first; parts[nparts].nblk = last - first + 1;
		nparts++;
	}
	return 0;
}

struct region { char name[40]; u64 lba, nblk; int critical, is_gpt; };

static int region_find(const char *name, struct region *r, char *why)
{
	char base[40]; size_t n = strlen(name); int i;
	memset(r, 0, sizeof(*r));
	snprintf(r->name, sizeof(r->name), "%s", name);
	snprintf(base, sizeof(base), "%s", name);
	if (!strcmp(name, "mmc0")) { r->nblk = disk_blocks; r->critical = 1; return 0; }
	if (!strcmp(name, "gpt")) { r->nblk = 34; r->critical = r->is_gpt = 1; return 0; }
	if (gpt_load()) { strcpy(why, "no valid GPT on the eMMC (flash 'gpt' first, or use mmc0)"); return -1; }
	if (!strcmp(name, "bootloader")) {
		u64 first = disk_blocks;
		for (i = 0; i < nparts; i++) if (parts[i].lba < first) first = parts[i].lba;
		r->lba = 34; r->nblk = first - 34; r->critical = 1;
		return 0;
	}
	if (n > 2 && n < 38 && name[n - 2] == '_' && (name[n - 1] == 'a' || name[n - 1] == 'b')) {
		base[n - 2] = 0;
		if (!strcmp(base, "kernel") || !strcmp(base, "rootfs") || !strcmp(base, "rtos")) {
			if (name[n - 1] == 'b') strcat(base, "2");
		} else snprintf(base, sizeof(base), "%s", name);
	}
	for (i = 0; i < nparts; i++)
		if (!strcmp(parts[i].name, base)) {
			r->lba = parts[i].lba; r->nblk = parts[i].nblk;
			r->critical = !strcmp(base, "sn_mac");
			return 0;
		}
	snprintf(why, 80, "no partition '%.40s'", name);
	return -1;
}
static int need_critical(struct region *r)
{
	if (r->critical && !critical_unlocked) {
		FAIL("'%s' is critical: run 'fastboot flashing unlock_critical' first", r->name);
		return 1;
	}
	return 0;
}

static char snmac_area[4096 + 1], *snf[8];
static int snmac_load(void)
{
	struct region r; char why[80], *p; int i; static u8 *b;
	if (!b) b = xalign(4096);
	memset(snf, 0, sizeof(snf));
	if (region_find("sn_mac", &r, why) || dread(r.lba * 512, b, 4096)) return -1;
	memcpy(snmac_area, b, 4096); snmac_area[4096] = 0;
	for (p = snmac_area, i = 0; i < 8; i++) {
		char *s = strchr(p, ';');
		if (!s) return -1;
		*s = 0; snf[i] = p; p = s + 1;
	}
	for (i = 0; i < 8; i++) for (p = snf[i]; *p; p++) if (*p < 0x20 || *p > 0x7e) return -1;
	return 0;
}
static int current_slot(void)
{
	struct region r; char why[80]; static u8 *b;
	if (!b) b = xalign(512);
	if (region_find("ota", &r, why) || dread(r.lba * 512, b, 512)) return -1;
	return !memcmp(b, "ota:kernel2", 11);
}

/* ------------------------------------------------- block queue (2 threads) */

static u8 *qbuf[NQ];
static size_t qlen[NQ];
static unsigned long qprod, qcons;
static int qdone, qerr;
static pthread_mutex_t qm = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t qc = PTHREAD_COND_INITIALIZER;

static void q_reset(void) { qprod = qcons = 0; qdone = qerr = 0; }
static u8 *q_space(void)		/* producer: next free buffer, NULL if aborted */
{
	u8 *b;
	pthread_mutex_lock(&qm);
	while (qprod - qcons >= NQ && !qerr) pthread_cond_wait(&qc, &qm);
	b = qerr ? NULL : qbuf[qprod % NQ];
	pthread_mutex_unlock(&qm);
	return b;
}
static void q_commit(size_t len)
{
	pthread_mutex_lock(&qm); qlen[qprod % NQ] = len; qprod++;
	pthread_cond_broadcast(&qc); pthread_mutex_unlock(&qm);
}
static void q_finish(int err)
{
	pthread_mutex_lock(&qm); if (err) qerr = 1; qdone = 1;
	pthread_cond_broadcast(&qc); pthread_mutex_unlock(&qm);
}
static void q_abort(void)
{
	pthread_mutex_lock(&qm); qerr = 1; pthread_cond_broadcast(&qc); pthread_mutex_unlock(&qm);
}
static u8 *q_get(size_t *len)		/* consumer: NULL when done or error */
{
	u8 *b = NULL;
	pthread_mutex_lock(&qm);
	while (qcons == qprod && !qdone && !qerr) pthread_cond_wait(&qc, &qm);
	if (!qerr && qcons != qprod) { b = qbuf[qcons % NQ]; *len = qlen[qcons % NQ]; }
	pthread_mutex_unlock(&qm);
	return b;
}
static void q_release(void)
{
	pthread_mutex_lock(&qm); qcons++; pthread_cond_broadcast(&qc); pthread_mutex_unlock(&qm);
}

struct span { u64 off, size; size_t blk; };

static void *disk_reader(void *arg)	/* producer */
{
	struct span *s = arg; u64 off = s->off, rem = s->size; int err = 0;
	while (rem) {
		size_t n = rem > s->blk ? s->blk : rem;
		u8 *b = q_space();
		if (!b) break;
		if (dread(off, b, (n + 511) & ~511u)) { err = 1; break; }
		q_commit(n); off += n; rem -= n;
	}
	q_finish(err);
	return NULL;
}
static void *disk_writer(void *arg)	/* consumer */
{
	struct span *s = arg; u64 off = s->off; size_t n; u8 *b;
	while ((b = q_get(&n))) {
		if (dwrite(off, b, n)) { q_abort(); break; }
		off += n; q_release();
	}
	return NULL;
}

/* ----------------------------------------------------------------- xxh64 -- */

#define P1 11400714785074694791ULL
#define P2 14029467366897019727ULL
#define P3 1609587929392839161ULL
#define P4 9650029242287828579ULL
#define P5 2870177450012600261ULL
#define ROTL(x, r) (((x) << (r)) | ((x) >> (64 - (r))))
static inline u64 xr(u64 acc, u64 in) { acc += in * P2; acc = ROTL(acc, 31); return acc * P1; }
static u64 xxh64(const u8 *p, size_t len)
{
	const u8 *end = p + len; u64 h;
	if (len >= 32) {
		u64 v1 = P1 + P2, v2 = P2, v3 = 0, v4 = 0 - P1;
		do { v1 = xr(v1, le64(p)); v2 = xr(v2, le64(p + 8)); v3 = xr(v3, le64(p + 16)); v4 = xr(v4, le64(p + 24)); p += 32; } while (p + 32 <= end);
		h = ROTL(v1, 1) + ROTL(v2, 7) + ROTL(v3, 12) + ROTL(v4, 18);
		h = (h ^ xr(0, v1)) * P1 + P4; h = (h ^ xr(0, v2)) * P1 + P4;
		h = (h ^ xr(0, v3)) * P1 + P4; h = (h ^ xr(0, v4)) * P1 + P4;
	} else h = P5;
	h += len;
	while (p + 8 <= end) { h ^= xr(0, le64(p)); h = ROTL(h, 27) * P1 + P4; p += 8; }
	if (p + 4 <= end) { h ^= (u64)le32(p) * P1; h = ROTL(h, 23) * P2 + P3; p += 4; }
	while (p < end) { h ^= *p++ * P5; h = ROTL(h, 11) * P1; }
	h ^= h >> 33; h *= P2; h ^= h >> 29; h *= P3; h ^= h >> 32;
	return h;
}

/* ------------------------------------------------------------- commands -- */

static u8 *dl;
static u32 dl_size;

static void getvar_one(const char *v, char *out)
{
	struct region r; char why[80];
	out[0] = 0;
	if (!strcmp(v, "version")) strcpy(out, "0.4");
	else if (!strcmp(v, "version-bootloader")) strcpy(out, FBD_VERSION);
	else if (!strcmp(v, "product")) strcpy(out, (!snmac_load() && *snf[2]) ? snf[2] : "nocry-x2000");
	else if (!strcmp(v, "serialno")) snprintf(out, 64, "%s", (!snmac_load() && *snf[0]) ? snf[0] : "nocry");
	else if (!strcmp(v, "max-download-size")) sprintf(out, "0x%08x", DL_MAX);
	else if (!strcmp(v, "max-fetch-size")) sprintf(out, "0x%08x", FETCH_MAX);
	else if (!strcmp(v, "is-userspace")) strcpy(out, "yes");
	else if (!strcmp(v, "secure")) strcpy(out, "no");
	else if (!strcmp(v, "unlocked")) strcpy(out, "yes");
	else if (!strcmp(v, "critical-unlocked")) strcpy(out, critical_unlocked ? "yes" : "no");
	else if (!strcmp(v, "nocry-ext")) strcpy(out, "write,hash:sha256,hash:xxh64");
	else if (!strcmp(v, "slot-count")) strcpy(out, "2");
	else if (!strcmp(v, "current-slot")) { int s = current_slot(); if (s >= 0) strcpy(out, s ? "b" : "a"); }
	else if (!strncmp(v, "has-slot:", 9))
		strcpy(out, (!strcmp(v + 9, "kernel") || !strcmp(v + 9, "rootfs") || !strcmp(v + 9, "rtos")) ? "yes" : "no");
	else if (!strncmp(v, "is-logical:", 11)) strcpy(out, "no");
	else if (!strncmp(v, "partition-size:", 15)) { if (!region_find(v + 15, &r, why)) sprintf(out, "0x%016llx", (unsigned long long)r.nblk * 512); }
	else if (!strncmp(v, "partition-type:", 15)) { if (!region_find(v + 15, &r, why)) strcpy(out, "raw"); }
	else if (!strcmp(v, "emmc-size")) sprintf(out, "0x%016llx", (unsigned long long)disk_blocks * 512);
}
static void cmd_getvar(const char *v)
{
	static const char *const all[] = { "version", "version-bootloader", "product", "serialno",
		"max-download-size", "max-fetch-size", "is-userspace", "secure", "unlocked",
		"critical-unlocked", "nocry-ext", "slot-count", "current-slot", "emmc-size", NULL };
	char out[256]; int i;
	if (strcmp(v, "all")) {
		getvar_one(v, out);
		if (out[0]) OKAY("%s", out); else FAIL("unknown variable '%.60s'", v);
		return;
	}
	for (i = 0; all[i]; i++) { getvar_one(all[i], out); if (out[0]) INFO("%s: %s", all[i], out); }
	if (!gpt_load())
		for (i = 0; i < nparts; i++)
			INFO("partition-size:%s: 0x%016llx", parts[i].name, (unsigned long long)parts[i].nblk * 512);
	OKAY("");
}

static void cmd_download(const char *arg)
{
	u32 n = strtoul(arg, NULL, 16);
	dl_size = 0;
	if (!dl) dl = xalign(DL_MAX + 4096);
	if (!n || n > DL_MAX || !dl) { FAIL("data too large (max 0x%08x)", DL_MAX); return; }
	if (resp("DATA", "%08x", n)) return;
	if (data_read(dl, n)) return;
	dl_size = n;
	OKAY("");
}

#define SPARSE_MAGIC 0xed26ff3a
static int flash_sparse(struct region *r)
{
	u8 *p = dl, *end = dl + dl_size;
	u16 fhs = p[8] | p[9] << 8, chs = p[10] | p[11] << 8;
	u32 bs = le32(p + 12), total = le32(p + 16), nch = le32(p + 20), i;
	u64 off = r->lba * 512, lim = (r->lba + r->nblk) * 512;
	int ret;
	if ((p[4] | p[5] << 8) != 1 || fhs < 28 || chs < 12 || !bs || bs % 512) { FAIL("unsupported sparse image"); return -1; }
	if ((u64)total * bs > r->nblk * 512) { FAIL("sparse image larger than '%s'", r->name); return -1; }
	p += fhs;
	for (i = 0; i < nch; i++) {
		u16 type; u32 csz, tsz; u64 nb;
		if (p + chs > end) { FAIL("truncated sparse image"); return -1; }
		type = p[0] | p[1] << 8; csz = le32(p + 4); tsz = le32(p + 8); nb = (u64)csz * bs;
		if (tsz < chs || p + tsz > end || off + nb > lim) { FAIL("bad sparse chunk %u", i); return -1; }
		switch (type) {
		case 0xcac1:
			if (tsz - chs != nb) { FAIL("bad raw chunk %u", i); return -1; }
			if ((uintptr_t)(p + chs) & 511) {	/* O_DIRECT needs aligned memory */
				u64 d = 0; ret = 0;
				while (d < nb && !ret) { size_t k = nb - d > MIB ? MIB : nb - d; memcpy(zbuf, p + chs + d, k); ret = dwrite(off + d, zbuf, k) || dread(off + d, vbuf, k) || memcmp(vbuf, zbuf, k) ? -EIO : 0; d += k; }
			} else ret = dwrite_verified(off, p + chs, nb);
			break;
		case 0xcac2: ret = dfill_verified(off, nb, le32(p + chs)); break;
		case 0xcac3: case 0xcac4: ret = 0; break;
		default: FAIL("unknown sparse chunk type 0x%04x", type); return -1;
		}
		if (ret) { FAIL("write/verify failed (%d)", ret); return -1; }
		if (type != 0xcac4) off += nb;
		p += tsz;
	}
	return 0;
}

static int flash_gpt(void)
{
	u64 last = disk_blocks - 1; u8 *h = dl + 512, *bk, *bh; u32 esec; int r;
	const char *bad = gpt_check(dl, dl_size, last);
	if (bad) { FAIL("%s", bad); return -EINVAL; }
	esec = (le32(h + 80) * le32(h + 84) + 511) / 512;
	put64(h + 32, last); put32(h + 16, gpt_hcrc(h));
	if (dl[510] == 0x55 && dl[511] == 0xaa && dl[446 + 4] == 0xee)
		put32(dl + 446 + 12, last > 0xffffffffULL ? 0xffffffff : (u32)last);
	if ((r = dwrite_verified(0, dl, 34 * 512))) return r;
	bk = zbuf; memset(bk, 0, (esec + 1) * 512);
	memcpy(bk, dl + 1024, esec * 512);
	bh = bk + esec * 512; memcpy(bh, h, le32(h + 12));
	put64(bh + 24, last); put64(bh + 32, 1); put64(bh + 72, last - esec); put32(bh + 16, gpt_hcrc(bh));
	r = dwrite_verified((last - esec) * 512, bk, (esec + 1) * 512);
	ioctl(disk, BLKRRPART);
	return r;
}

static void cmd_flash(const char *name)
{
	struct region r; char why[80]; int ret;
	if (region_find(name, &r, why)) { FAIL("%s", why); return; }
	if (need_critical(&r)) return;
	if (!dl_size) { FAIL("no data downloaded"); return; }
	if (r.is_gpt) { if ((ret = flash_gpt()) == -EINVAL) return; }
	else if (dl_size >= 28 && le32(dl) == SPARSE_MAGIC) { if (flash_sparse(&r)) return; ret = 0; }
	else {
		u64 n = ((u64)dl_size + 511) & ~511ULL;
		if (n > r.nblk * 512) { FAIL("image (%u bytes) larger than '%s' (%llu bytes)", dl_size, name, (unsigned long long)r.nblk * 512); return; }
		memset(dl + dl_size, 0, n - dl_size);
		ret = dwrite_verified(r.lba * 512, dl, n);
	}
	fdatasync(disk);
	if (ret) FAIL("write/verify failed (%d)", ret); else OKAY("");
}

static void cmd_erase(const char *name)
{
	struct region r; char why[80]; u64 off, end, range[2], next = 0; static const u8 *zero;
	if (region_find(name, &r, why)) { FAIL("%s", why); return; }
	if (need_critical(&r)) return;
	off = r.lba * 512; end = off + r.nblk * 512;
	range[0] = off; range[1] = end - off;
	if (ioctl(disk, BLKDISCARD, range)) INFO("discard not available; writing zeros");
	/* guarantee zeros: verify, and overwrite whatever does not read back as zero */
	memset(zbuf, 0, MIB); zero = zbuf;
	for (; off < end; off += MIB) {
		size_t k = end - off > MIB ? MIB : end - off;
		if (dread(off, vbuf, k)) { FAIL("read error"); return; }
		if (memcmp(vbuf, zero, k) && dwrite(off, zero, k)) { FAIL("write error"); return; }
		if (off - r.lba * 512 >= next + 1024ULL * MIB) { next = off - r.lba * 512; INFO("erase %s: %llu/%llu MiB verified", name, (unsigned long long)(next >> 20), (unsigned long long)(r.nblk >> 11)); }
	}
	fdatasync(disk);
	OKAY("");
}

/* parses name[:off[:size]] (hex); returns 0 and byte span inside the region */
static int parse_span(char *arg, struct region *r, u64 *off, u64 *size, int size_required)
{
	char why[80], *o = strchr(arg, ':'), *s = NULL; u64 bytes;
	*off = 0;
	if (o) { *o++ = 0; s = strchr(o, ':'); if (s) *s++ = 0; *off = strtoull(o, NULL, 16); }
	if (region_find(arg, r, why)) { FAIL("%s", why); return -1; }
	bytes = r->nblk * 512;
	if (*off > bytes || *off % 512) { FAIL("offset must be 512-aligned and inside '%s'", arg); return -1; }
	if (size_required && !s) { FAIL("size missing"); return -1; }
	*size = s ? strtoull(s, NULL, 16) : bytes - *off;
	if (*size > bytes - *off) { FAIL("range exceeds '%s'", arg); return -1; }
	return 0;
}

static void cmd_fetch(char *arg)
{
	struct region r; u64 off, size; struct span sp; pthread_t th; u8 *b; size_t n; int bad = 0;
	if (parse_span(arg, &r, &off, &size, 0)) return;
	if (!size || size > FETCH_MAX) { FAIL("fetch size must be 1..0x%08x bytes", FETCH_MAX); return; }
	if (resp("DATA", "%08x", (u32)size)) return;
	sp.off = r.lba * 512 + off; sp.size = size; sp.blk = MIB;
	q_reset(); pthread_create(&th, NULL, disk_reader, &sp);
	while ((b = q_get(&n))) {
		if (data_write(b, n)) { bad = 1; q_abort(); break; }
		q_release();
	}
	pthread_join(th, NULL);
	if (bad) return;
	if (qerr) FAIL("read error"); else OKAY("");
}

static void cmd_nocry_write(char *arg)
{
	struct region r; u64 off, size, rem; struct span sp; pthread_t th; int bad = 0;
	if (parse_span(arg, &r, &off, &size, 1)) return;
	if (need_critical(&r)) return;
	if (!size || size % 512 || size > 0xffffffffULL) { FAIL("size must be a non-zero multiple of 512 below 4 GiB"); return; }
	if (resp("DATA", "%08x", (u32)size)) return;
	sp.off = r.lba * 512 + off;
	q_reset(); pthread_create(&th, NULL, disk_writer, &sp);
	for (rem = size; rem; ) {
		size_t n = rem > MIB ? MIB : rem; u8 *b = q_space();
		if (!b) {		/* writer failed: drain the host's data */
			static u8 *sink; if (!sink) sink = xalign(MIB + 4096);
			if (data_read(sink, n)) { bad = 1; break; }
			rem -= n; continue;
		}
		if (data_read(b, n)) { bad = 1; q_abort(); break; }
		q_commit(n); rem -= n;
	}
	q_finish(0);
	pthread_join(th, NULL);
	if (bad) return;
	if (qerr) { FAIL("write error"); return; }
	fdatasync(disk);
	OKAY("");
}

static void cmd_nocry_hash(char *arg)
{
	struct region r; u64 off, size, chunk, nch; struct span sp; pthread_t th; u8 *b; size_t n;
	char *algo = arg, *rest = strchr(arg, ':'), *c; int sha, dlen, bad = 0;
	if (!rest) { FAIL("usage: nocry-hash:<sha256|xxh64>:<name>:<off>:<size>:<chunk>"); return; }
	*rest++ = 0;
	sha = !strcmp(algo, "sha256");
	if (!sha && strcmp(algo, "xxh64")) { FAIL("unknown hash '%.20s'", algo); return; }
	c = strrchr(rest, ':');
	if (!c) { FAIL("chunk missing"); return; }
	*c++ = 0; chunk = strtoull(c, NULL, 16);
	if (parse_span(rest, &r, &off, &size, 1)) return;
	if (!size || !chunk || chunk % 512 || chunk > BUFCAP) { FAIL("chunk must be a multiple of 512 up to 0x%x", BUFCAP); return; }
	dlen = sha ? 32 : 8; nch = (size + chunk - 1) / chunk;
	if (nch * dlen > 0xffffffffULL) { FAIL("too many chunks"); return; }
	if (resp("DATA", "%08x", (u32)(nch * dlen))) return;
	sp.off = r.lba * 512 + off; sp.size = size; sp.blk = chunk;
	q_reset(); pthread_create(&th, NULL, disk_reader, &sp);
	{
		static u8 out[4096]; size_t fill = 0;
		while ((b = q_get(&n))) {
			if (sha) { unsigned l; EVP_Digest(b, n, out + fill, &l, EVP_sha256(), NULL); }
			else put64(out + fill, xxh64(b, n));
			fill += dlen;
			q_release();
			if (fill + 32 > sizeof(out)) { if (data_write(out, fill)) { bad = 1; q_abort(); break; } fill = 0; }
		}
		if (!bad && !qerr && fill && data_write(out, fill)) bad = 1;
	}
	pthread_join(th, NULL);
	if (bad) return;
	if (qerr) FAIL("read error"); else OKAY("");
}

static void cmd_set_active(const char *slot)
{
	struct region r; char why[80];
	if (strcmp(slot, "a") && strcmp(slot, "b")) { FAIL("slot must be a or b"); return; }
	if (region_find("ota", &r, why)) { FAIL("%s", why); return; }
	memset(zbuf, 0, 512);
	strcpy((char *)zbuf, slot[0] == 'b' ? "ota:kernel2\n" : "ota:kernel\n");
	if (dwrite_verified(r.lba * 512, zbuf, 512)) FAIL("write failed"); else { fdatasync(disk); OKAY(""); }
}

static void dispatch(char *c)
{
	if (!strncmp(c, "getvar:", 7)) cmd_getvar(c + 7);
	else if (!strncmp(c, "download:", 9)) cmd_download(c + 9);
	else if (!strncmp(c, "flash:", 6)) cmd_flash(c + 6);
	else if (!strncmp(c, "erase:", 6)) cmd_erase(c + 6);
	else if (!strncmp(c, "fetch:", 6)) cmd_fetch(c + 6);
	else if (!strncmp(c, "nocry-write:", 12)) cmd_nocry_write(c + 12);
	else if (!strncmp(c, "nocry-hash:", 11)) cmd_nocry_hash(c + 11);
	else if (!strncmp(c, "set_active:", 11)) cmd_set_active(c + 11);
	else if (!strcmp(c, "flashing unlock_critical")) { critical_unlocked = 1; INFO("critical regions (mmc0, gpt, bootloader, sn_mac) writable until reboot"); OKAY(""); }
	else if (!strcmp(c, "flashing lock_critical")) { critical_unlocked = 0; OKAY(""); }
	else if (!strcmp(c, "reboot") || !strcmp(c, "continue")) {
		sync(); INFO("rebooting; the board boots from eMMC unless BOOT is held"); OKAY("");
		if (!stdio_mode) { usleep(200000); reboot(RB_AUTOBOOT); }
	} else FAIL("unknown command");
}

#ifndef FBD_NO_FFS
static int ffs_init(const char *dir)
{
	struct {
		struct usb_functionfs_descs_head_v2 head;
		__le32 fs_count, hs_count;
		struct { struct usb_interface_descriptor intf; struct usb_endpoint_descriptor_no_audio out, in; } __attribute__((packed)) fs, hs;
	} __attribute__((packed)) d;
	struct { struct usb_functionfs_strings_head head; __le16 lang; char s[9]; } __attribute__((packed)) s;
	char p[256]; int ep0, i;

	memset(&d, 0, sizeof(d));
	d.head.magic = FUNCTIONFS_DESCRIPTORS_MAGIC_V2;
	d.head.length = sizeof(d);
	d.head.flags = FUNCTIONFS_HAS_FS_DESC | FUNCTIONFS_HAS_HS_DESC;
	d.fs_count = d.hs_count = 3;
	for (i = 0; i < 2; i++) {
		typeof(d.fs) *x = i ? &d.hs : &d.fs;
		x->intf.bLength = sizeof(x->intf); x->intf.bDescriptorType = USB_DT_INTERFACE;
		x->intf.bNumEndpoints = 2; x->intf.bInterfaceClass = 0xff;
		x->intf.bInterfaceSubClass = 0x42; x->intf.bInterfaceProtocol = 0x03; x->intf.iInterface = 1;
		x->out.bLength = x->in.bLength = sizeof(x->out);
		x->out.bDescriptorType = x->in.bDescriptorType = USB_DT_ENDPOINT;
		x->out.bEndpointAddress = 1 | USB_DIR_OUT; x->in.bEndpointAddress = 2 | USB_DIR_IN;
		x->out.bmAttributes = x->in.bmAttributes = USB_ENDPOINT_XFER_BULK;
		x->out.wMaxPacketSize = x->in.wMaxPacketSize = i ? 512 : 64;
	}
	memset(&s, 0, sizeof(s));
	s.head.magic = FUNCTIONFS_STRINGS_MAGIC; s.head.length = sizeof(s);
	s.head.str_count = 1; s.head.lang_count = 1; s.lang = 0x0409; strcpy(s.s, "fastboot");

	snprintf(p, sizeof(p), "%s/ep0", dir);
	if ((ep0 = open(p, O_RDWR)) < 0) { perror(p); return -1; }
	if (write(ep0, &d, sizeof(d)) < 0) { perror("ffs descriptors"); return -1; }
	if (write(ep0, &s, sizeof(s)) < 0) { perror("ffs strings"); return -1; }
	snprintf(p, sizeof(p), "%s/ep1", dir); ep_out = open(p, O_RDWR);
	snprintf(p, sizeof(p), "%s/ep2", dir); ep_in = open(p, O_RDWR);
	if (ep_out < 0 || ep_in < 0) { perror("ffs endpoints"); return -1; }
	return 0;		/* ep0 stays open for the life of the function */
}
#endif

int main(int argc, char **argv)
{
	const char *dev = "/dev/mmcblk0", *ffs = "/dev/usb-ffs/fb";
	static char cmd[CMD_MAX + 1];
	struct stat st; u64 bytes = 0; int i, n;

	if (argc >= 3 && !strcmp(argv[1], "--stdio")) { stdio_mode = 1; dev = argv[2]; }
	else { if (argc >= 2) dev = argv[1]; if (argc >= 3) ffs = argv[2]; }
	disk = open(dev, O_RDWR | O_DIRECT);
	if (disk < 0) disk = open(dev, O_RDWR);
	if (disk < 0 || fstat(disk, &st)) { perror(dev); return 1; }
	if (S_ISBLK(st.st_mode)) ioctl(disk, BLKGETSIZE64, &bytes); else bytes = st.st_size;
	disk_blocks = bytes / 512;
	vbuf = xalign(MIB); zbuf = xalign(MIB);
	for (i = 0; i < NQ; i++) qbuf[i] = xalign(BUFCAP + 4096);
	if (!disk_blocks || !vbuf || !zbuf || !qbuf[NQ - 1]) { fprintf(stderr, "nocry-fbd: init failed\n"); return 1; }
#ifndef FBD_NO_FFS
	if (!stdio_mode && ffs_init(ffs)) return 1;
#else
	(void)ffs;
	if (!stdio_mode) return 1;
#endif
	fprintf(stderr, "nocry-fbd: %s, %llu bytes, ready\n", dev, (unsigned long long)bytes);
	for (;;) {
		n = pkt_read(cmd, CMD_MAX);
		if (n <= 0) { if (stdio_mode) return 0; usleep(100000); continue; }
		cmd[n] = 0;
		dispatch(cmd);
	}
}
