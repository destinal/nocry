import subprocess, struct, sys, hashlib, os
fbd, dev, ref = sys.argv[1:4]
p = subprocess.Popen([fbd, "--stdio", dev], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
def rd(n):
    b = b""
    while len(b) < n:
        c = p.stdout.read(n - len(b)); assert c, "eof"; b += c
    return b
def pkt():
    n = struct.unpack("<I", rd(4))[0]; return rd(n).decode()
def send(c):
    c = c.encode(); p.stdin.write(struct.pack("<I", len(c)) + c); p.stdin.flush()
def result(infos=None):
    while True:
        r = pkt()
        if r.startswith("INFO"):
            if infos is not None: infos.append(r[4:])
            continue
        return r
def cmd(c, infos=None):
    send(c); return result(infos)
def fetch(spec):
    r = cmd("fetch:" + spec); assert r.startswith("DATA"), r
    d = rd(int(r[4:], 16)); assert result() == "OKAY"; return d
def download(data):
    r = cmd("download:%08x" % len(data)); assert r.startswith("DATA"), r
    p.stdin.write(data); p.stdin.flush(); assert result() == "OKAY"
def nwrite(spec, data):
    r = cmd("nocry-write:%s:%x" % (spec, len(data)))
    if not r.startswith("DATA"): return r
    p.stdin.write(data); p.stdin.flush(); return result()
def nhash(algo, name, off, size, chunk):
    r = cmd("nocry-hash:%s:%s:%x:%x:%x" % (algo, name, off, size, chunk)); assert r.startswith("DATA"), r
    d = rd(int(r[4:], 16)); assert result() == "OKAY"; return d
R = open(ref, "rb")
def refb(off, n): R.seek(off); return R.read(n)
ok = fail = 0
def check(c, what):
    global ok, fail
    print(("PASS  " if c else "FAIL  ") + what)
    if c: ok += 1
    else: fail += 1
MIB = 1 << 20
inf = []; check(cmd("getvar:all", inf) == "OKAY" and any("partition-size:userdata" in i for i in inf) and any("product: Nebula Pad" in i for i in inf), "getvar all (%d lines)" % len(inf))
check(cmd("getvar:current-slot") == "OKAYa", "current-slot a")
check(fetch("gpt") == refb(0, 34 * 512), "fetch gpt")
check(fetch("kernel") == refb(0xb00000, 0x800000), "fetch kernel")
check(fetch("rootfs_a:100000:300000") == refb(0x1b00000 + 0x100000, 0x300000), "fetch rootfs_a with offset+size")
check(fetch("rootfs:0:1201") == refb(0x1b00000, 0x1201), "fetch with odd size")
h = nhash("sha256", "kernel", 0, 0x800000, MIB)
check(h == b"".join(hashlib.sha256(refb(0xb00000 + i * MIB, MIB)).digest() for i in range(8)), "sha256 per-MiB hashes of kernel")
h = nhash("sha256", "rootfs", 0, 5 * MIB + 512, 4 * MIB)
check(h == hashlib.sha256(refb(0x1b00000, 4 * MIB)).digest() + hashlib.sha256(refb(0x1b00000 + 4 * MIB, MIB + 512)).digest(), "sha256 with short last chunk")
try:
    import xxhash
    h = nhash("xxh64", "rootfs", 0, 9 * MIB + 1024, 2 * MIB)
    exp = b"".join(struct.pack("<Q", xxhash.xxh64(refb(0x1b00000 + i * 2 * MIB, min(2 * MIB, 9 * MIB + 1024 - i * 2 * MIB))).intdigest()) for i in range(5))
    check(h == exp, "xxh64 matches reference implementation")
    for n in (0x200, 0x400, 0x600):
        check(nhash("xxh64", "ota", 0, n, n) == struct.pack("<Q", xxhash.xxh64(refb(0x100000, n)).intdigest()), "xxh64 small %d" % n)
except ImportError:
    print("SKIP  xxh64 reference (python xxhash missing)")
# guards
check("critical" in nwrite("sn_mac:0", b"\0" * 512), "nocry-write sn_mac refused while locked")
download(b"\0" * 4096); check("critical" in cmd("flash:mmc0"), "flash mmc0 refused while locked")
download(os.urandom(9 * MIB)); check("larger" in cmd("flash:kernel"), "oversize flash refused")
check("exceeds" in cmd("nocry-write:kernel:700000:200000"), "nocry-write past partition end refused")
check(fetch("kernel") == refb(0xb00000, 0x800000) and fetch("sn_mac") == refb(0x200000, 0x100000), "nothing changed by refused writes")
# writes into rootfs2
rnd = os.urandom(20 * MIB + 512 * 3)
check(nwrite("rootfs_b:200000", rnd) == "OKAY", "nocry-write 20 MiB+ at offset 2 MiB")
got = fetch("rootfs_b:0:%x" % (24 * MIB))
exp = bytearray(refb(0x20f00000, 24 * MIB)); exp[2 * MIB:2 * MIB + len(rnd)] = rnd
check(got == bytes(exp), "content correct, neighbours untouched")
r2 = os.urandom(3 * MIB + 100); download(r2); check(cmd("flash:rootfs_b") == "OKAY", "flash raw (non-512 size)")
check(fetch("rootfs_b:0:%x" % (4 * MIB)) == r2 + b"\0" * 412 + bytes(exp[3 * MIB + 512:4 * MIB]), "raw flash content + zero padding")
# sparse: raw, fill, skip
def chunk(t, blocks, payload=b""): return struct.pack("<HHII", t, 0, blocks, 12 + len(payload)) + payload
raw = os.urandom(8 * 4096)
sp = struct.pack("<IHHHHIIII", 0xed26ff3a, 1, 0, 28, 12, 4096, 8 + 16 + 4 + 8, 4, 0) + chunk(0xcac1, 8, raw) + chunk(0xcac2, 16, b"ABCD") + chunk(0xcac3, 4) + chunk(0xcac1, 8, raw[::-1])
before = fetch("rootfs_b:0:%x" % (36 * 4096))
download(sp); check(cmd("flash:rootfs_b") == "OKAY", "flash sparse")
check(fetch("rootfs_b:0:%x" % (36 * 4096)) == raw + b"ABCD" * (16 * 1024) + before[24 * 4096:28 * 4096] + raw[::-1], "sparse content (raw, fill, skip)")
check(cmd("erase:rootfs_b") == "OKAY" and fetch("rootfs_b:0:%x" % (32 * MIB)).count(0) == 32 * MIB, "erase -> zeros")
check(cmd("set_active:b") == "OKAY" and cmd("getvar:current-slot") == "OKAYb" and fetch("ota:0:200")[:12] == b"ota:kernel2\n", "set_active b")
# critical path: gpt flash rewrites backup GPT
check(cmd("flashing unlock_critical") == "OKAY", "unlock_critical")
download(refb(0, 34 * 512)); check(cmd("flash:gpt") == "OKAY", "flash gpt")
g = fetch("gpt"); size = os.path.getsize(dev); last = size // 512 - 1
check(struct.unpack_from("<Q", g, 512 + 32)[0] == last, "primary GPT alt LBA = this disk's last LBA")
bk = fetch("mmc0:%x:200" % (last * 512))
check(bk[:8] == b"EFI PART" and struct.unpack_from("<Q", bk, 24)[0] == last and struct.unpack_from("<Q", bk, 32)[0] == 1, "backup GPT header written at the end")
p.stdin.close(); p.wait()
print("== %d passed, %d failed" % (ok, fail)); sys.exit(1 if fail else 0)
