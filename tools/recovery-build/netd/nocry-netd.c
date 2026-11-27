/*
 * nocry-netd: lets nocry on a PC drive the board over the network (nocry-rescue).
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * It listens on TCP, pairs with a client that knows the short code shown on the board's
 * screen, and then relays an encrypted stream to a `nocry-fbd --stdio` child, which does
 * the actual work. One client at a time.
 *
 * Pairing: SPAKE2 (RFC 9382; P-256, SHA-256, HKDF, HMAC), the code as password. A wrong code
 * is one guess; after 3 wrong ones a new code is made. Then ChaCha20-Poly1305 records:
 *   client -> board   "NOCRY-NET 1\n", pA (65 bytes)
 *   board -> client   pB (65 bytes), cB (32 bytes)
 *   client -> board   cA (32 bytes)
 *   records           u32 LE length of the ciphertext, ciphertext with tag (<= 64 KiB plain)
 * The same is implemented in host/core/src/net.rs; both are tested against the RFC's vector.
 *
 * usage: nocry-netd [--port N] [--code-file F] [--fbd PATH] [--disk DEV]
 *        nocry-netd --selftest
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <openssl/bn.h>
#include <openssl/crypto.h>
#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/obj_mac.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;

#define PORT		5560
#define HELLO		"NOCRY-NET 1\n"
#define ID_CLIENT	"nocry"
#define ID_BOARD	"nocry-rescue"
#define RECORD		(64 * 1024)
#define M_HEX "02886e2f97ace46e55ba9dd7242579f2993b64e16ef3dcab95afd497333d8fa12f"
#define N_HEX "03d8bbd6c639c62937b04d997f38c3770719c629d7014d49a24b4f98baa1292b49"

struct keys { u8 ke[16], kca[16], kcb[16], ca[32], cb[32]; };

static void hmac256(const u8 *key, size_t klen, const u8 *msg, size_t mlen, u8 out[32])
{
	unsigned int n = 32;
	HMAC(EVP_sha256(), key, klen, msg, mlen, out, &n);
}

/* HKDF-SHA256 with no salt, one block of output (<= 32 bytes) */
static void hkdf256(const u8 *ikm, size_t ikmlen, const char *info, u8 *out, size_t outlen)
{
	static const u8 zero[32];
	u8 prk[32], t[32], msg[128];
	size_t il = strlen(info);
	hmac256(zero, 32, ikm, ikmlen, prk);
	memcpy(msg, info, il);
	msg[il] = 1;
	hmac256(prk, 32, msg, il + 1, t);
	memcpy(out, t, outlen);
}

static void tt_add(u8 *tt, size_t *n, const void *p, size_t len)
{
	u64 l = len; int i;
	for (i = 0; i < 8; i++) tt[(*n)++] = l >> (8 * i);
	memcpy(tt + *n, p, len);
	*n += len;
}

static int hex2point(const EC_GROUP *g, EC_POINT *p, const char *hex, BN_CTX *ctx)
{
	u8 b[33]; int i;
	for (i = 0; i < 33; i++) { unsigned v; sscanf(hex + 2 * i, "%2x", &v); b[i] = v; }
	return EC_POINT_oct2point(g, p, b, 33, ctx);
}

/* w = SHA-256("nocry-rescue pairing code\0" || code) mod n */
static BIGNUM *code_scalar(const EC_GROUP *g, const char *code, BN_CTX *ctx)
{
	static const char label[] = "nocry-rescue pairing code";	/* with its NUL */
	u8 h[32]; SHA256_CTX c; BIGNUM *w = BN_new();
	SHA256_Init(&c);
	SHA256_Update(&c, label, sizeof(label));
	SHA256_Update(&c, code, strlen(code));
	SHA256_Final(h, &c);
	BN_bin2bn(h, 32, w);
	BN_mod(w, w, EC_GROUP_get0_order(g), ctx);
	return w;
}

/*
 * Side B of SPAKE2. In: w, our secret y, the two identities, the client's pA (65 bytes).
 * Out: pB (65 bytes) and the keys. Returns 0, or -1 for a bad point.
 */
static int spake2_b(const BIGNUM *w, const BIGNUM *y, const char *ida, const char *idb,
		    const u8 *pa, u8 *pb, struct keys *k)
{
	BN_CTX *ctx = BN_CTX_new();
	EC_GROUP *g = EC_GROUP_new_by_curve_name(NID_X9_62_prime256v1);
	EC_POINT *M = EC_POINT_new(g), *N = EC_POINT_new(g), *PA = EC_POINT_new(g), *T = EC_POINT_new(g), *K = EC_POINT_new(g);
	u8 kb[65], wb[32], h[32], kc[32], tt[512]; size_t n = 0; int ret = -1;

	if (!hex2point(g, M, M_HEX, ctx) || !hex2point(g, N, N_HEX, ctx)) goto out;
	if (!EC_POINT_oct2point(g, PA, pa, 65, ctx) || EC_POINT_is_at_infinity(g, PA) || EC_POINT_is_on_curve(g, PA, ctx) != 1) goto out;
	/* pB = y*G + w*N */
	if (!EC_POINT_mul(g, T, y, N, w, ctx)) goto out;
	if (EC_POINT_point2oct(g, T, POINT_CONVERSION_UNCOMPRESSED, pb, 65, ctx) != 65) goto out;
	/* K = y * (pA - w*M) */
	if (!EC_POINT_mul(g, T, NULL, M, w, ctx) || !EC_POINT_invert(g, T, ctx) || !EC_POINT_add(g, T, PA, T, ctx)) goto out;
	if (!EC_POINT_mul(g, K, NULL, T, y, ctx) || EC_POINT_is_at_infinity(g, K)) goto out;
	if (EC_POINT_point2oct(g, K, POINT_CONVERSION_UNCOMPRESSED, kb, 65, ctx) != 65) goto out;
	BN_bn2binpad(w, wb, 32);

	tt_add(tt, &n, ida, strlen(ida));
	tt_add(tt, &n, idb, strlen(idb));
	tt_add(tt, &n, pa, 65);
	tt_add(tt, &n, pb, 65);
	tt_add(tt, &n, kb, 65);
	tt_add(tt, &n, wb, 32);
	SHA256(tt, n, h);
	memcpy(k->ke, h, 16);
	hkdf256(h + 16, 16, "ConfirmationKeys", kc, 32);
	memcpy(k->kca, kc, 16);
	memcpy(k->kcb, kc + 16, 16);
	hmac256(k->kca, 16, tt, n, k->ca);
	hmac256(k->kcb, 16, tt, n, k->cb);
	ret = 0;
out:
	OPENSSL_cleanse(kb, sizeof(kb)); OPENSSL_cleanse(tt, sizeof(tt));
	EC_POINT_free(M); EC_POINT_free(N); EC_POINT_free(PA); EC_POINT_free(T); EC_POINT_free(K);
	EC_GROUP_free(g); BN_CTX_free(ctx);
	return ret;
}

/* ------------------------------------------------------------- self-test -- */

static void tohex(const u8 *b, size_t n, char *out) { size_t i; for (i = 0; i < n; i++) sprintf(out + 2 * i, "%02x", b[i]); }

static int selftest(void)
{
	/* RFC 9382 appendix B, first vector (A = "server", B = "client") */
	BN_CTX *ctx = BN_CTX_new();
	EC_GROUP *g = EC_GROUP_new_by_curve_name(NID_X9_62_prime256v1);
	EC_POINT *M = EC_POINT_new(g), *PA = EC_POINT_new(g);
	BIGNUM *w = NULL, *x = NULL, *y = NULL;
	u8 pa[65], pb[65]; struct keys k; char hex[140]; int bad = 0;
	BN_hex2bn(&w, "2ee57912099d31560b3a44b1184b9b4866e904c49d12ac5042c97dca461b1a5f");
	BN_hex2bn(&x, "43dd0fd7215bdcb482879fca3220c6a968e66d70b1356cac18bb26c84a78d729");
	BN_hex2bn(&y, "dcb60106f276b02606d8ef0a328c02e4b629f84f89786af5befb0bc75b6e66be");
	hex2point(g, M, M_HEX, ctx);
	EC_POINT_mul(g, PA, x, M, w, ctx);		/* pA = x*G + w*M */
	EC_POINT_point2oct(g, PA, POINT_CONVERSION_UNCOMPRESSED, pa, 65, ctx);
	if (spake2_b(w, y, "server", "client", pa, pb, &k)) { puts("FAIL spake2_b"); return 1; }
#define CHECK(name, buf, len, want) do { tohex(buf, len, hex); if (strncmp(hex, want, strlen(want))) { printf("FAIL %s: %s\n", name, hex); bad = 1; } else printf("ok   %s\n", name); } while (0)
	CHECK("pA", pa, 65, "04a56fa807caaa53a4d28dbb9853b9815c61a411118a6fe516a8798434751470");
	CHECK("pB", pb, 65, "0406557e482bd03097ad0cbaa5df82115460d951e3451962f1eaf4367a420676");
	CHECK("Ke", k.ke, 16, "0e0672dc86f8e45565d338b0540abe69");
	CHECK("KcA", k.kca, 16, "00c12546835755c86d8c0db7851ae86f");
	CHECK("KcB", k.kcb, 16, "a9fa3406c3b781b93d804485430ca27a");
	CHECK("cA", k.ca, 32, "58ad4aa88e0b60d5061eb6b5dd93e80d9c4f00d127c65b3b35b1b5281fee38f0");
	CHECK("cB", k.cb, 32, "d3e2e547f1ae04f2dbdbf0fc4b79f8ecff2dff314b5d32fe9fcef2fb26dc459b");
	return bad;
}

/* ------------------------------------------------------------------ link -- */

static int rd_full(int fd, void *b, size_t n)
{
	size_t d = 0;
	while (d < n) {
		ssize_t r = read(fd, (char *)b + d, n - d);
		if (r <= 0) { if (r < 0 && errno == EINTR) continue; return -1; }
		d += r;
	}
	return 0;
}
static int wr_full(int fd, const void *b, size_t n)
{
	size_t d = 0;
	while (d < n) {
		ssize_t r = write(fd, (const char *)b + d, n - d);
		if (r <= 0) { if (r < 0 && errno == EINTR) continue; return -1; }
		d += r;
	}
	return 0;
}

static void nonce(u64 n, u8 out[12])
{
	int i;
	memset(out, 0, 4);
	for (i = 0; i < 8; i++) out[4 + i] = n >> (8 * i);
}

/* one record; out must hold inlen + 16 (seal) or inlen (open). Returns output length or -1. */
static int aead(int seal, const u8 key[32], u64 n, const u8 *in, int inlen, u8 *out)
{
	EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new();
	u8 iv[12]; int len, total = -1;
	nonce(n, iv);
	if (seal) {
		if (EVP_EncryptInit_ex(c, EVP_chacha20_poly1305(), NULL, key, iv) == 1 &&
		    EVP_EncryptUpdate(c, out, &len, in, inlen) == 1) {
			total = len;
			if (EVP_EncryptFinal_ex(c, out + total, &len) == 1 &&
			    EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_AEAD_GET_TAG, 16, out + total + len) == 1)
				total += len + 16;
			else total = -1;
		}
	} else if (inlen >= 16) {
		if (EVP_DecryptInit_ex(c, EVP_chacha20_poly1305(), NULL, key, iv) == 1 &&
		    EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_AEAD_SET_TAG, 16, (void *)(in + inlen - 16)) == 1 &&
		    EVP_DecryptUpdate(c, out, &len, in, inlen - 16) == 1) {
			total = len;
			if (EVP_DecryptFinal_ex(c, out + total, &len) == 1) total += len; else total = -1;
		}
	}
	EVP_CIPHER_CTX_free(c);
	return total;
}

/* relay between the paired client and a nocry-fbd child until either side ends */
static void serve(int s, const struct keys *k, const char *fbd, const char *disk)
{
	static u8 plain[RECORD], ct[RECORD + 16];
	u8 kin[32], kout[32]; u64 nin = 0, nout = 0;
	int to_child[2], from_child[2]; pid_t pid;

	hkdf256(k->ke, 16, "nocry-net c2b", kin, 32);
	hkdf256(k->ke, 16, "nocry-net b2c", kout, 32);
	if (pipe(to_child) || pipe(from_child)) return;
	pid = fork();
	if (pid == 0) {
		dup2(to_child[0], 0); dup2(from_child[1], 1);
		close(to_child[1]); close(from_child[0]); close(s);
		execl(fbd, fbd, "--stdio", disk, (char *)NULL);
		_exit(127);
	}
	close(to_child[0]); close(from_child[1]);
	for (;;) {
		struct pollfd p[2] = { { s, POLLIN, 0 }, { from_child[0], POLLIN, 0 } };
		if (poll(p, 2, -1) < 0) { if (errno == EINTR) continue; break; }
		if (p[0].revents) {		/* client -> child */
			u8 l[4]; u32 n; int m;
			if (rd_full(s, l, 4)) break;
			n = l[0] | l[1] << 8 | l[2] << 16 | (u32)l[3] << 24;
			if (n < 16 || n > RECORD + 16 || rd_full(s, ct, n)) break;
			m = aead(0, kin, nin++, ct, n, plain);
			if (m < 0) { fprintf(stderr, "nocry-netd: a record failed its check; closing\n"); break; }
			if (wr_full(to_child[1], plain, m)) break;
		}
		if (p[1].revents) {		/* child -> client */
			u8 l[4]; int m; ssize_t r = read(from_child[0], plain, RECORD);
			if (r <= 0) break;
			m = aead(1, kout, nout++, plain, r, ct);
			if (m < 0) break;
			l[0] = m; l[1] = m >> 8; l[2] = m >> 16; l[3] = m >> 24;
			if (wr_full(s, l, 4) || wr_full(s, ct, m)) break;
		}
	}
	close(to_child[1]); close(from_child[0]);
	kill(pid, SIGTERM);
	waitpid(pid, NULL, 0);
	OPENSSL_cleanse(kin, 32); OPENSSL_cleanse(kout, 32);
}

static void new_code(char *code, const char *file)
{
	u8 r[4]; u32 v; FILE *f;
	RAND_bytes(r, 4);
	v = (r[0] | r[1] << 8 | r[2] << 16 | (u32)r[3] << 24) % 1000000;
	sprintf(code, "%06u", v);
	f = fopen(file, "w");
	if (f) { fprintf(f, "%s\n", code); fclose(f); }
	fprintf(stderr, "nocry-netd: pairing code %s\n", code);
}

int main(int argc, char **argv)
{
	const char *codefile = "/run/nocry-pair-code", *fbd = "/usr/sbin/nocry-fbd", *disk = "/dev/mmcblk0";
	int port = PORT, i, ls, one = 1, wrong = 0;
	struct sockaddr_in6 sa; char code[8];

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--selftest")) return selftest();
		else if (!strcmp(argv[i], "--port") && i + 1 < argc) port = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--code-file") && i + 1 < argc) codefile = argv[++i];
		else if (!strcmp(argv[i], "--fbd") && i + 1 < argc) fbd = argv[++i];
		else if (!strcmp(argv[i], "--disk") && i + 1 < argc) disk = argv[++i];
		else { fprintf(stderr, "usage: nocry-netd [--port N] [--code-file F] [--fbd PATH] [--disk DEV] | --selftest\n"); return 2; }
	}
	signal(SIGPIPE, SIG_IGN);
	ls = socket(AF_INET6, SOCK_STREAM, 0);		/* dual stack: IPv4 clients too */
	setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	memset(&sa, 0, sizeof(sa));
	sa.sin6_family = AF_INET6; sa.sin6_port = htons(port);
	if (ls < 0 || bind(ls, (struct sockaddr *)&sa, sizeof(sa)) || listen(ls, 1)) { perror("nocry-netd: listen"); return 1; }
	new_code(code, codefile);
	fprintf(stderr, "nocry-netd: listening on port %d\n", port);

	for (;;) {
		struct timeval tv = { 10, 0 };
		u8 hello[sizeof(HELLO) - 1], pa[65], pb[65], ca[32], yb[32]; struct keys k;
		BN_CTX *ctx; EC_GROUP *g; BIGNUM *w, *y; int s, ok;

		s = accept(ls, NULL, NULL);
		if (s < 0) continue;
		setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
		setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
		if (rd_full(s, hello, sizeof(hello)) || memcmp(hello, HELLO, sizeof(hello)) || rd_full(s, pa, 65)) { close(s); continue; }

		ctx = BN_CTX_new(); g = EC_GROUP_new_by_curve_name(NID_X9_62_prime256v1);
		w = code_scalar(g, code, ctx);
		y = BN_new(); RAND_bytes(yb, 32); BN_bin2bn(yb, 32, y); BN_mod(y, y, EC_GROUP_get0_order(g), ctx);
		ok = !BN_is_zero(y) && !spake2_b(w, y, ID_CLIENT, ID_BOARD, pa, pb, &k);
		BN_clear_free(w); BN_clear_free(y); EC_GROUP_free(g); BN_CTX_free(ctx);
		OPENSSL_cleanse(yb, 32);
		/* the client proves itself first only in its last message: send ours, then check its */
		if (ok && !wr_full(s, pb, 65) && !wr_full(s, k.cb, 32) && !rd_full(s, ca, 32) && !CRYPTO_memcmp(ca, k.ca, 32)) {
			wrong = 0;
			tv.tv_sec = 0;		/* no timeout while working: erasing can take minutes */
			setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
			fprintf(stderr, "nocry-netd: a computer paired\n");
			serve(s, &k, fbd, disk);
			fprintf(stderr, "nocry-netd: connection closed\n");
		} else {
			fprintf(stderr, "nocry-netd: pairing failed\n");
			sleep(1);
			if (++wrong >= 3) { wrong = 0; new_code(code, codefile); }
		}
		OPENSSL_cleanse(&k, sizeof(k));
		close(s);
	}
}
