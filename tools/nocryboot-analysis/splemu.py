#!/usr/bin/env python3
"""Runs a first-stage loader (SPL) of an X2000E board in an emulator, from its first instruction to the
jump into the kernel, and logs every hardware register access in order.

Used to compare Creality's SPL with nocryboot's: same clocks, same memory setup, same pins, same
kernel arguments?  The eMMC is not emulated at register level: the two routines "read blocks" and
"send a command" are replaced, so reads come from a disk image file.  The rtos image on core 1 is not
run either: when the SPL starts core 1, the emulator plays its part (changes the first word of the
image and stores a panel id).

usage: splemu.py <spl image: 0x800 header + code> <disk image> --block-read ADDR [--cmd ADDR]
                 [--init ADDR] [--elf u-boot-spl]  [--lcm N]
  --elf     take the addresses from the symbols of nocryboot's own build
"""
import sys, struct, collections, os, argparse, subprocess
from unicorn import *
from unicorn.mips_const import *

ap = argparse.ArgumentParser()
ap.add_argument('spl'); ap.add_argument('disk')
ap.add_argument('--block-read'); ap.add_argument('--cmd'); ap.add_argument('--init'); ap.add_argument('--elf')
ap.add_argument('--delay', action='append', default=[], help='address of a delay routine to skip (repeatable)')
ap.add_argument('--lcm', type=int, default=7, help='panel id the rtos image reports; -1: it reports none')
ap.add_argument('--slot', choices=['a', 'b'], help='pretend the ota record selects this slot')
ap.add_argument('--blank', default='', help='comma list of partitions to read as zeros: rtos,rtos2,kernel,kernel2')
ap.add_argument('--copy-a-to-b', action='store_true', help='read slot B (rtos2, kernel2) from slot A')
ap.add_argument('--console-input', default='', help='characters waiting at the serial console, e.g. "\\x1b\\x1b\\x1b"'); ap.add_argument('--ram-mb', type=int, default=256)
ap.add_argument('--limit', type=int, default=400_000_000); ap.add_argument('-o', default='trace.txt')
args = ap.parse_args()

LOAD = 0x12401000                    # physical address of 0xb2401000, where the boot ROM puts the SPL
spl = bytearray(open(args.spl, 'rb').read()[:0x6000])
disk = open(args.disk, 'rb')
A = {}
if args.elf:
    for l in subprocess.run(['nm', args.elf], capture_output=True, text=True).stdout.splitlines():
        p = l.split()
        if len(p) == 3: A[p[2]] = int(p[0], 16)
def addr(opt, sym):
    if opt: return int(opt, 16)
    return A.get(sym)
BLOCK_READ = addr(args.block_read, 'mmc_block_read'); CMD = addr(args.cmd, None); INIT = addr(args.init, 'jzmmc_init')
DELAYS = [int(x, 16) for x in args.delay] + [A[n] for n in ('udelay', 'mdelay', '__udelay') if n in A]

mu = Uc(UC_ARCH_MIPS, UC_MODE_MIPS32 | UC_MODE_LITTLE_ENDIAN)
mu.ctl_set_cpu_model(UC_CPU_MIPS32_74KF)
def P(a): return a & 0x1fffffff
mu.mem_map(0, 0x10000000)                         # RAM
mu.mem_map(0x12400000, 0x10000)                   # the SoC's small boot RAM
mu.mem_map(0x1fc00000, 0x10000)                   # boot ROM (reads as 0)
HOOKIO = 0x1fe00000                               # our own "device": replaced routines talk to it

# CP0 registers and cache instructions the emulated CPU lacks
patched = 0
for o in range(0x800, len(spl) - 3, 4):
    w = struct.unpack_from('<I', spl, o)[0]
    if (w & 0xffe007f8) in (0x40000000, 0x40800000):
        rd, sel, rt = (w >> 11) & 31, w & 7, (w >> 16) & 31
        if (rd, sel) in ((16, 7), (26, 0), (16, 2), (16, 3), (16, 4), (16, 5), (28, 0), (28, 1), (28, 2), (28, 3), (29, 0), (12, 2), (9, 7), (11, 7)) or (rd == 15 and sel == 1 and w & 0x00800000):
            struct.pack_into('<I', spl, o, (0x24000000 | rt << 16) if not w & 0x00800000 else 0); patched += 1
    elif w >> 26 == 0x2f:                         # cache
        struct.pack_into('<I', spl, o, 0); patched += 1

def stub(a, code):
    o = P(a) - LOAD
    struct.pack_into('<%dI' % len(code), spl, o, *code)
T9 = 25
def io_stub(a, fn, nargs):
    # lui t9,0xbfe0 ; sw a0..,(t9) ; lw v0,0x40+4*fn(t9) ; jr ra ; nop
    code = [0x3c190000 | 0xbfe0] + [0xaf200000 | (4 + i) << 16 | i * 4 for i in range(nargs)] + [0x8f220000 | (0x40 + 4 * fn), 0x03e00008, 0]
    stub(a, code)
if BLOCK_READ: io_stub(BLOCK_READ, 0, 3)
if CMD: io_stub(CMD, 1, 2)
if INIT: stub(INIT, [0x24020000, 0x03e00008, 0])
for d in DELAYS: stub(d, [0x24020000, 0x03e00008, 0])
mu.mem_write(LOAD, bytes(spl))

log = []
def out(s):
    log.append(s)
blocks = collections.deque(maxlen=16)
def here(): return blocks[-1] if blocks else 0

NAMES = {0xb0000000: 'CPM', 0xb0001000: 'INTC', 0xb0002000: 'TCU', 0xb0010000: 'GPIO', 0xb2000000: 'OST', 0xb2100000: 'OST2',
         0xb2200000: 'CCU', 0xb3010000: 'DDRPHY', 0xb3011000: 'DDRPHY', 0xb3012000: 'DDR', 0xb34f0000: 'DDRC', 0xb3450000: 'MSC0',
         0xb3540000: 'EFUSE', 0xb0030000: 'UART0', 0xb0031000: 'UART1', 0xb0032000: 'UART2', 0xb0033000: 'UART3', 0xb0034000: 'UART4'}
def rname(a):
    for b in sorted(NAMES, reverse=True):
        if a >= b and a - b < 0x1000 or (a >= b and NAMES[b] in ('CPM',) and a - b < 0x1000):
            if NAMES[b] == 'GPIO':
                return 'GPIO P%s+%#04x' % ('ABCDEFGZ'[min((a - b) >> 8, 7)], (a - b) & 0xff)
            return '%s+%#06x' % (NAMES[b], a - b)
    return '%#010x' % a

regs = {}
uart = bytearray()
ticks = [0]
reads = collections.Counter()
lastreads = collections.deque(maxlen=8)
core1 = {'started': False}
RTOS = {}
PARTS = {'rtos': (0x1800, 0x2000), 'rtos2': (0x3800, 0x2000), 'kernel': (0x5800, 0x4000), 'kernel2': (0x9800, 0x4000)}
conin = bytearray(args.console_input.encode().decode('unicode_escape').encode('latin1'))
def rtos_header():
    # which slot the SPL will use, and where its image goes
    disk.seek(0x100000); slot_b = disk.read(11) == b'ota:kernel2'
    if args.slot: slot_b = args.slot == 'b'
    disk.seek(0x700000 if slot_b else 0x300000); h = struct.unpack('<9I', disk.read(36))
    RTOS.update(slot='B' if slot_b else 'A', start=h[4], size_word=h[8])
rtos_header()

def mmio_write(uc, off, size, value, ud):
    a = 0xb0000000 + off
    value &= (1 << (size * 8)) - 1
    if 0xb0030000 <= a < 0xb0035000 and (a & 0xfff) == 0 and not regs.get((a & ~0xfff) + 0xc, 0) & 0x80:
        if value == 10:
            out('# uart%d: %s' % ((a >> 12) & 7, uart.decode('latin1').strip('\r'))); uart.clear()
        else:
            uart.append(value)
        return
    out('W %-22s = %#010x   [%#x]' % (rname(a), value, here()))
    if a == 0xb0002004 and value & 1 and regs.get(0xb00000cc, 0) >> 8 == 0x425753:
        out('# watchdog reset requested with "software boot select" = %d (2 = USB boot mode)' % (regs[0xb00000cc] & 0xff))
        stop['door'] = True; uc.emu_stop(); return
    if 0xb0010000 <= a < 0xb0010800 and (a & 0xf) in (4, 8):
        base = a & ~0xf
        regs[base] = (regs.get(base, 0) | value) if a & 0xf == 4 else (regs.get(base, 0) & ~value)
        return
    if a == 0xb2200040 and value == 0 and not core1['started']:     # CCU reset release: core 1 runs the rtos image
        core1['started'] = True
        w = struct.unpack('<I', bytes(uc.mem_read(P(RTOS['start']), 4)))[0]
        uc.mem_write(P(RTOS['start']), struct.pack('<I', w ^ 0xffffffff))
        if args.lcm >= 0: uc.mem_write(P(RTOS['size_word']), struct.pack('<I', args.lcm))
        out('# core 1 started (entry %#x); emulator: image "done", panel id %d' % (regs.get(0xb2200f00, 0), args.lcm))
    regs[a & ~3] = value if size == 4 else (regs.get(a & ~3, 0) & ~(((1 << size * 8) - 1) << ((a & 3) * 8))) | value << ((a & 3) * 8)

MODEL = {}
def mmio_read(uc, off, size, ud):
    a = 0xb0000000 + off
    if 0xb0030000 <= a < 0xb0035000 and (a & 0xfff) == 0 and conin:
        return conin.pop(0)
    v = regs.get(a & ~3, 0)
    f = MODEL.get(a & ~3)
    if f: v = f(v)
    reads[a] += 1; lastreads.append(a)
    if reads[a] <= 2:
        out('R %-22s -> %#010x   [%#x]' % (rname(a), v, here()))
    return (v >> ((a & 3) * 8)) & ((1 << size * 8) - 1)
def counter(v):
    ticks[0] += 2400; return ticks[0] & 0xffffffff
mu.mmio_map(0x10000000, 0x02400000, mmio_read, None, mmio_write, None)
mu.mmio_map(0x12410000, 0x01bf0000, lambda uc, off, size, ud: mmio_read(uc, off + 0x02410000, size, ud), None,
            lambda uc, off, size, value, ud: mmio_write(uc, off + 0x02410000, size, value, ud), None)

# ---- replaced eMMC routines ----
hargs = [0, 0, 0]
def hook_w(uc, off, size, value, ud):
    if off < 12: hargs[off // 4] = value
def hook_r(uc, off, size, ud):
    if off == 0x40:                 # block_read(start sector, count, destination)
        start, cnt, dst = hargs
        disk.seek(start * 512); data = disk.read(cnt * 512)
        for name, (first, n) in PARTS.items():
            if first <= start < first + n:
                if args.copy_a_to_b and name.endswith('2'):
                    disk.seek((start - first + PARTS[name[:-1]][0]) * 512); data = disk.read(cnt * 512)
                if name in args.blank.split(','):
                    data = bytes(len(data))
        if start == 0x800 and args.slot:
            data = (b'ota:kernel2\n' if args.slot == 'b' else b'ota:kernel\n').ljust(len(data), b'\0')
        if len(data) >= 36 and data[8:12] == b'RTOS':          # the image the SPL is about to start on core 1
            h = struct.unpack('<9I', data[:36]); RTOS.update(start=h[4], size_word=h[8])
        uc.mem_write(P(dst), data)
        out('# eMMC read: sector %#x, %d blocks -> %#x' % (start, cnt, dst))
        return cnt
    if off == 0x44:                 # send command (index, argument): answer like an eMMC does
        out('# eMMC command %d, argument %#x' % (hargs[0], hargs[1]))
        return 0x80000440 if hargs[0] in (8, 55) else 0x80000400     # no answer to the SD-only commands; else all ones
    return 0
mu.mmio_map(HOOKIO, 0x1000, hook_r, None, hook_w, None)
mu.mem_write(0x400, b'\xff' * 32)

stop = {}
def code(uc, a, size, ud):
    # only called outside the SPL: the jump to the kernel (or a wild jump)
    stop['pc'] = a; uc.emu_stop()
mu.hook_add(UC_HOOK_CODE, code, begin=0x80000000, end=0x8fffffff)
mu.hook_add(UC_HOOK_BLOCK, lambda uc, a, sz, ud: blocks.append(a))
def bad(uc, access, a, size, value, ud):
    out('# bad memory access %#x (size %d) at pc %#x ra %#x' % (a, size, uc.reg_read(UC_MIPS_REG_PC), uc.reg_read(UC_MIPS_REG_RA))); return False
mu.hook_add(UC_HOOK_MEM_UNMAPPED, bad)
def intr(uc, no, ud):
    out('# exception %d near %s' % (no, ' '.join('%x' % b for b in blocks))); uc.emu_stop()
mu.hook_add(UC_HOOK_INTR, intr)

exec(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'model.py')).read())

mu.reg_write(UC_MIPS_REG_SP, 0xb2401000)       # the boot ROM's stack sits below the SPL
try:
    mu.emu_start(0xb2401800, 0xffffffff, count=args.limit)
except UcError as e:
    out('# emulator error %s at pc %#x' % (e, mu.reg_read(UC_MIPS_REG_PC)))
def cstr(a, n=400):
    b = bytes(mu.mem_read(P(a), n)); return b[:b.index(0)].decode('latin1') if 0 in b else b.decode('latin1')
if 'door' in stop:
    out('\n##### restart into USB boot mode')
elif 'pc' in stop:
    a0, a1, a2 = (mu.reg_read(r) for r in (UC_MIPS_REG_A0, UC_MIPS_REG_A1, UC_MIPS_REG_A2))
    out('\n##### jump to %#x  a0=%#x a1=%#x a2=%#x' % (stop['pc'], a0, a1, a2))
    if 0x80000000 <= a1 < 0x90000000:
        argv = struct.unpack('<%dI' % a0, bytes(mu.mem_read(P(a1), 4 * a0)))
        for i, p in enumerate(argv):
            out('  argv[%d] = %s' % (i, repr(cstr(p)) if p else 'NULL'))
    out('  first words there: ' + bytes(mu.mem_read(P(stop['pc']), 16)).hex())
else:
    out('# stopped without reaching the kernel. pc %#x ra %#x; last blocks %s' % (mu.reg_read(UC_MIPS_REG_PC), mu.reg_read(UC_MIPS_REG_RA), ' '.join('%x' % b for b in blocks)))
    out('# last register reads: ' + ' '.join(rname(a) for a in lastreads))
if uart: out('# uart (unfinished line): ' + uart.decode('latin1'))
open(args.o, 'w').write('\n'.join(log) + '\n')
print('\n'.join(log[-25:]))
