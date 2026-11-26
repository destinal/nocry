#!/usr/bin/env python3
"""Runs Creality's rtos image in an emulator and logs every hardware register access in order.
The scheduler is not emulated: thread functions are run one after another, OS waits return at once."""
import sys, struct, collections, os
from unicorn import *
from unicorn.mips_const import *

IMG = sys.argv[1]; REGDUMP = sys.argv[2]
img = open(IMG, 'rb').read()
import hashlib, json
_h = struct.unpack_from('<9I', img, 0)
CODE_END = _h[5]; HEAP_END = _h[7]; SPL_WORD = _h[8]
img = img[:CODE_END - 0x86000000]
KEY = hashlib.sha256(img).hexdigest()[:16]
PROFILES = json.load(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'profiles.json')))
if KEY not in PROFILES:
    sys.exit('no address profile for this image (%s); make one with match.py' % KEY)
A = {k: int(v, 16) for k, v in PROFILES[KEY]['functions'].items()}
print('# image %s: %s' % (KEY, PROFILES[KEY]['name']))
BASE = 0x86000000
mu = Uc(UC_ARCH_MIPS, UC_MODE_MIPS32 | UC_MODE_LITTLE_ENDIAN)
mu.ctl_set_cpu_model(UC_CPU_MIPS32_74KF)
# the emulator translates kseg0/kseg1 itself: map physical addresses
mu.mem_map(0x00000000, 0x08000000)          # RAM
_w, _r = mu.mem_write, mu.mem_read
def P(a): return a & 0x1fffffff
class M:
    pass
mu_mem_write = lambda a, b: _w(P(a), b)
mu_mem_read = lambda a, n: _r(P(a), n)
# CP0 registers this emulated CPU lacks (Config7, ErrCtl...): reads give 0, writes do nothing
img = bytearray(img); patched = 0
for o in range(0, len(img) - 3, 4):
    w = struct.unpack_from('<I', img, o)[0]
    if (w & 0xffe007f8) in (0x40000000, 0x40800000):
        rd, sel, rt = (w >> 11) & 31, w & 7, (w >> 16) & 31
        if (rd, sel) in ((16, 7), (26, 0), (16, 2), (16, 3), (16, 4), (16, 5), (28, 0), (28, 1), (29, 0)) or (rd == 15 and sel == 1 and w & 0x00800000):
            struct.pack_into('<I', img, o, (0x24000000 | rt << 16) if not w & 0x00800000 else 0); patched += 1
for a in (A['msa_on'], A['msa_off'], A['fpu_on'], A['fpu_off']):   # MSA enable/disable: cfcmsa/ctcmsa do not exist on the emulated CPU
    struct.pack_into('<II', img, a - BASE, 0x03e00008, 0)
print('# patched', patched, 'CP0 accesses')
img = bytes(img)
mu_mem_write(BASE, img)
# what the SPL provides
SNMAC = ('112233CC001234;AABBCC001234;%s;%s;;;;;\0' % (os.environ.get('MODEL', 'Nebula Pad'), os.environ.get('BOARD', 'NEBULA V1.0.0.1'))).encode()
mu_mem_write(HEAP_END - 0x100000, SNMAC)
mu_mem_write(SPL_WORD, struct.pack('<I', 0x100000))
ARGS = 0x80001000
# peripheral registers: a dictionary, seeded with CPM values of a running stock system
# (the clock code reads the PLL settings); everything else starts at 0
regs = {}
for l in open(REGDUMP):
    p = l.split()
    if len(p) == 2 and p[1].startswith('0x') and len(p[0]) == 8:
        a = int(p[0], 16)
        if 0x10000000 <= a < 0x10000100:
            regs[a | 0xa0000000] = int(p[1], 16)

if os.environ.get('GATED'):      # second run: all clock gates closed, dividers cleared, to see what the image itself sets
    regs[0xb0000020] = 0xffffffff; regs[0xb0000028] = 0xffffffff; regs[0xb0000064] = 0; regs[0xb000006c] = 0
NAMES = {0xb0000000: 'CPM', 0xb0001000: 'INTC', 0xb0002000: 'TCU', 0xb0010000: 'GPIO', 0xb0075000: 'DSI?', 0xb2000000: 'OST',
         0xb2200000: 'CCU', 0xb3050000: 'DPU', 0xb3420000: 'PDMA', 0xb34c0000: 'PWM', 0xb0030000: 'UART'}
def rname(a):
    for b in sorted(NAMES, reverse=True):
        if a >= b and a - b < 0x10000:
            if NAMES[b] == 'GPIO':
                return 'GPIO P%s+%#04x' % ('ABCDEFGZ'[min((a - b) >> 8, 7)], (a - b) & 0xff)
            return '%s+%#06x' % (NAMES[b], a - b)
    return '%#010x' % a

log = []
trail = collections.deque(maxlen=12)
def out(s):
    log.append(s); print(s)
calls = []
def cur_fn():
    return calls[-1] if calls else '?'

# ---- pins: decode what the GPIO writes mean, follow a bit-banged 3-wire serial link ----------------
PIN_CS, PIN_SCL, PIN_SDA, PIN_SDO = 2, 0x5b, 0x5c, 0x30      # PA2, PC27, PC28, PB16 (K1 panels)
PINS = os.environ.get('PINS', '1') != '0'
pz = {}                         # pending shadow-port bits: name -> mask
pinmode = {}                    # pin -> ('out', level) | ('in',) | ('func', n)
def pname(n): return 'P%s%d' % ('ABCDEFG'[n >> 5], n & 31)
# what the panel answers when a register is read: PANEL=tjc|pj|dc|gc9503|none
PANEL = os.environ.get('PANEL', 'none')
ANSWERS = {'tjc': {0xa1: [0x77, 0x01, 0, 0, 0], 0x04: [0x54, 0x01, 0x01]},
           'pj': {0xa1: [0, 0, 0, 0, 0], 0x04: [0x43, 0x02, 0x04]},
           'dc': {0xa1: [0, 0, 0, 0, 0], 0x04: [0xdc, 0x43, 0x48]},
           'gc9503': {0xdb: [0x95], 0xdc: [0x04]}, 'none': {}}[PANEL]
spi = {'bits': [], 'words': [], 'out': [], 'last_cmd': None}
def spi_flush(why=''):
    if spi['bits']:
        spi['words'].append('?%d bits %s' % (len(spi['bits']), ''.join(map(str, spi['bits'])))); spi['bits'] = []
    if spi['words']:
        out('SPI  ' + ' '.join(spi['words']) + why); spi['words'] = []
def spi_clock_rise():
    m = pinmode.get(PIN_SDA)
    if m and m[0] == 'out':
        spi['bits'].append(m[1])
        if len(spi['bits']) == 9:
            v = int(''.join(map(str, spi['bits'])), 2); spi['bits'] = []
            if v & 0x100:
                spi['words'].append('%02x' % (v & 0xff))
            else:
                spi_flush(); spi['words'].append('cmd %02x:' % v); spi['last_cmd'] = v
                spi['out'] = [b >> i & 1 for b in ANSWERS.get(v, []) for i in range(7, -1, -1)]
def pin_event(n, mode):
    old = pinmode.get(n); pinmode[n] = mode
    if n == PIN_SCL and mode == ('out', 1) and old != ('out', 1):
        spi_clock_rise()
    elif n == PIN_CS and mode == ('out', 1) and old != ('out', 1):
        spi_flush()
    if n in (PIN_SCL, PIN_SDA, PIN_SDO, PIN_CS):
        return                                  # the link itself is reported as SPI lines
    if old != mode:
        out('PIN  %-5s %s' % (pname(n), mode[0] + (' %d' % mode[1] if len(mode) > 1 else '')))
def pin_read(n):
    """level the image reads on a pin: the panel's answer on the serial link, else 0"""
    if n in (PIN_SDO, PIN_SDA) and spi['out']:
        b = spi['out'].pop(0)
        spi.setdefault('rd', []).append(b)
        if len(spi['rd']) == 8:
            spi['words'].append('<%02x' % int(''.join(map(str, spi['rd'])), 2)); spi['rd'] = []
        return b
    return 0
def gpio_decode(addr, value):
    port, off = (addr - 0xb0010000) >> 8, addr & 0xff
    if port == 7:                               # shadow port
        if off == 0xf0:
            for bit in range(32):
                m = 1 << bit; ch = {k: (v[0] & m, v[1] & m) for k, v in pz.items()}
                if not any(a or b for a, b in ch.values()):
                    continue
                st = regs_pin.setdefault((value, bit), {'int': 0, 'msk': 0, 'pat1': 0, 'pat0': 0})
                for k, (setb, clrb) in ch.items():
                    if setb: st[k] = 1
                    if clrb: st[k] = 0
                n = value * 32 + bit
                pin_event(n, ('func', st['pat1'] * 2 + st['pat0']) if not st['msk'] else (('in',) if st['pat1'] else ('out', st['pat0'])))
            pz.clear()
        else:
            k = {0x10: 'int', 0x20: 'msk', 0x30: 'pat1', 0x40: 'pat0'}.get(off & 0xf0)
            if k and (off & 0xf) in (4, 8):
                cur = pz.setdefault(k, [0, 0]); cur[0 if off & 0xf == 4 else 1] |= value
        return
    if off in (0x44, 0x48) and port < 5:        # direct level change of output pins
        for bit in range(32):
            if value >> bit & 1:
                st = regs_pin.setdefault((port, bit), {'int': 0, 'msk': 1, 'pat1': 0, 'pat0': 0})
                st['pat0'] = 1 if off == 0x44 else 0
                if st['msk'] and not st['pat1']:
                    pin_event(port * 32 + bit, ('out', st['pat0']))
regs_pin = {}

def here():
    return trail[-1] if trail else 0
def mmio_write(uc, off, size, value, ud):
    addr = 0xb0000000 + off
    value &= (1 << (size * 8)) - 1
    if PINS and 0xb0010000 <= addr < 0xb0010800:
        gpio_decode(addr, value)
    else:
        out('W %-22s = %#010x   [pc %#x]' % (rname(addr), value, here()))
    if 0xb0010000 <= addr < 0xb0010800 and (addr & 0xf) in (4, 8):     # GPIO set / clear registers
        base = addr & ~0xf
        regs[base] = (regs.get(base, 0) | value) if addr & 0xf == 4 else (regs.get(base, 0) & ~value)
        return
    if addr == 0xb34c0010: regs[0xb34c0018] = regs.get(0xb34c0018, 0) | value; return     # PWMENS
    if addr == 0xb34c0014: regs[0xb34c0018] = regs.get(0xb34c0018, 0) & ~value; return    # PWMENC
    regs[addr & ~3] = value if size == 4 else (regs.get(addr & ~3, 0) & ~(((1 << size * 8) - 1) << ((addr & 3) * 8))) | value << ((addr & 3) * 8)
reads = collections.Counter()
def mmio_read(uc, off, size, ud):
    addr = 0xb0000000 + off
    v = regs.get(addr & ~3, 0)
    v = MODEL.get(addr & ~3, lambda v: v)(v)
    if PINS and 0xb0010000 <= addr < 0xb0010500 and (addr & 0xff) == 0:      # pin level register
        port = (addr - 0xb0010000) >> 8
        for n in (PIN_SDO, PIN_SDA):
            if n >> 5 == port and pinmode.get(n, ('in',))[0] != 'out':
                v = (v & ~(1 << (n & 31))) | pin_read(n) << (n & 31)
        return (v >> ((addr & 3) * 8)) & ((1 << size * 8) - 1)
    reads[addr] += 1
    if reads[addr] <= 3:
        out('R %-22s -> %#010x   [pc %#x]' % (rname(addr), v, here()))
    return (v >> ((addr & 3) * 8)) & ((1 << size * 8) - 1)
MODEL = {}
mu.mmio_map(0x10000000, 0x04000000, mmio_read, None, mmio_write, None)

def cstr(a, n=200):
    b = bytes(mu_mem_read(a, n)); return b[:b.index(0)].decode('latin1') if 0 in b else b.decode('latin1')
RET = {}
def ret(uc, v=0):
    # no register writes from hooks: the stub's code is "jr ra; addiu v0, zero, value" (patched in below)
    pass

threads = []
STOP = 0x80002000
def h_thread_create(uc):
    name = cstr(uc.reg_read(UC_MIPS_REG_A0)); fn = uc.reg_read(UC_MIPS_REG_A2); arg = uc.reg_read(UC_MIPS_REG_A3)
    out('# thread_create("%s", fn=%#x)' % (name, fn)); threads.append((name, fn, arg)); ret(uc)
def h_printf(uc):
    fmt = cstr(uc.reg_read(UC_MIPS_REG_A0))
    a = [uc.reg_read(r) for r in (UC_MIPS_REG_A1, UC_MIPS_REG_A2, UC_MIPS_REG_A3)]
    out('# printf: %r args %s' % (fmt, ' '.join('%#x' % x for x in a))); ret(uc)
def h_assert(uc):
    out('# ASSERT %s:%d %s' % (cstr(uc.reg_read(UC_MIPS_REG_A0)), uc.reg_read(UC_MIPS_REG_A1), cstr(uc.reg_read(UC_MIPS_REG_A2)))); ret(uc)
def h_delay(name):
    def f(uc):
        spi_flush(); out('# %s(%d)' % (name, uc.reg_read(UC_MIPS_REG_A0))); ret(uc)
    return f
def h_stop(uc):
    uc.emu_stop()
def h_sched(uc):
    out('# scheduler start'); uc.emu_stop()
def h_const(v, name):
    def f(uc):
        ret(uc, v)
    return f
STUBS = {
    A['thread_create']: h_thread_create, A['printf']: h_printf, A['assert']: h_assert,
    A['msleep']: h_delay('msleep'), A['sleep2']: h_delay('sleep_e5c0'), A['udelay']: h_delay('udelay'), A['ndelay']: h_delay('ndelay'),
    A['sched_start']: h_sched, STOP: h_stop,
    A['cpu_id']: h_const(1, 'cpu_id'),
}
import os
if os.path.exists('stubs_extra.py'):
    g = dict(globals()); exec(open('stubs_extra.py').read(), g); STUBS.update(g['EXTRA']); EXTRA_RET = g.get('EXTRA_RET', {})

RETVAL = {A['thread_create']: 1}; RETVAL.update(EXTRA_RET)
# stub code: "addiu v0, zero, value; jr ra; nop" (a jr as the very first, hooked instruction misbehaves in this emulator)
for a_ in STUBS:
    if 0x86000000 <= a_ < CODE_END:
        mu_mem_write(a_, struct.pack('<III', 0x24020000 | RETVAL.get(a_, 0), 0x03e00008, 0))
mu_mem_write(A['cpu_id'], struct.pack('<III', 0x24020001, 0x03e00008, 0))   # cpu id = 1
STUBS.pop(A['cpu_id'])
hot = collections.Counter(); count = [0]
lastpc = [0, 0]

def code(uc, addr, size, ud):
    count[0] += 1
    lastpc[0], lastpc[1] = lastpc[1], addr
    trail.append(addr)
    if not (0x86000000 <= addr < CODE_END or addr == STOP):
        out('# left the image: pc %#x; before that: %s' % (addr, ' '.join('%x' % x for x in list(trail)[-6:])))
        out('# blocks: ' + ' '.join('%x' % x for x in blocks) + '  sp %#x' % uc.reg_read(UC_MIPS_REG_SP))
        uc.emu_stop(); return
    f = STUBS.get(addr)
    if f:
        f(uc)
    elif count[0] > LIMIT - 20000:
        hot[addr] += 1
mu.hook_add(UC_HOOK_CODE, code)
def intr(uc, no, ud):
    out('# trail ' + ' '.join('%x' % a for a in trail))
    sp_ = uc.reg_read(UC_MIPS_REG_SP); st = struct.unpack('<64I', bytes(mu_mem_read(sp_, 256)))
    out('# ra %#x; return addresses on the stack: %s' % (uc.reg_read(UC_MIPS_REG_RA), ' '.join('%x' % w for w in st if 0x86000000 <= w < CODE_END)))
    out('# exception %d after pc %#x / %#x, insn %s' % (no, lastpc[0], lastpc[1], bytes(mu_mem_read(lastpc[0], 8)).hex())); uc.emu_stop()
mu.hook_add(UC_HOOK_INTR, intr)
blocks = collections.deque(maxlen=14)
mu.hook_add(UC_HOOK_BLOCK, lambda uc, a, sz, ud: blocks.append(a))
def bad(uc, access, addr, size, value, ud):
    out('# bad memory access %#x (size %d) at pc %#x ra %#x' % (addr, size, uc.reg_read(UC_MIPS_REG_PC), uc.reg_read(UC_MIPS_REG_RA))); return False
mu.hook_add(UC_HOOK_MEM_UNMAPPED, bad)

LIMIT = int(os.environ.get('LIMIT', 60_000_000))
GPR = [getattr(__import__('unicorn.mips_const', fromlist=['x']), 'UC_MIPS_REG_%d' % i) for i in range(32)] + [UC_MIPS_REG_HI, UC_MIPS_REG_LO]
yield_req = [False]
irq_req = []
irq_done = set()
started = [0]
sleeps = collections.Counter()
def h_sleep(name):
    def f(uc):
        ra = uc.reg_read(UC_MIPS_REG_RA); sleeps[ra] += 1
        if uc.reg_read(UC_MIPS_REG_A0) > 2 or not PINS:      # short ones are bit timing on the serial link
            spi_flush(); out('# %s(%d)' % (name, uc.reg_read(UC_MIPS_REG_A0)))
        if started[0] < len(threads):       # let threads created meanwhile run, like the scheduler would
            yield_req[0] = True; uc.emu_stop()
    return f
def h_wait_frame(uc):
    # wait (with timeout) on the display controller's completion: bit 17 of DPU+0x2010 is "frame done"
    out('# wait_event(%#x, timeout %d ms)' % (uc.reg_read(UC_MIPS_REG_A0), uc.reg_read(UC_MIPS_REG_A1)))
    if not armed[0]:
        armed[0] = True
        irq_req.append(('display frame-done', A['lcd_irq'], {0xb3052010: 0x20000})); yield_req[0] = True; uc.emu_stop()
    else:
        armed[0] = False
armed = [False]
STUBS[A['wait_event']] = h_wait_frame
STUBS[A['wake_up']] = lambda uc: out('# wake_up(%#x)' % uc.reg_read(UC_MIPS_REG_A0))
for a_, v_ in ((A['wait_event'], 1), (A['wake_up'], 0)):
    mu_mem_write(a_, struct.pack('<III', 0x24020000 | v_, 0x03e00008, 0))
for a_ in (A['msleep'], A['sleep2']):
    STUBS[a_] = h_sleep('msleep' if a_ == A['msleep'] else 'sleep_e5c0')

def run(name, pc, a0=0, sp=None, fresh=True):
    if fresh:
        out('\n##### %s (%#x)' % (name, pc))
        mu.reg_write(UC_MIPS_REG_RA, STOP); mu.reg_write(UC_MIPS_REG_A0, a0)
        if sp:
            mu.reg_write(UC_MIPS_REG_SP, sp)
    count[0] = 0; hot.clear()
    while True:
        yield_req[0] = False
        try:
            mu.emu_start(pc, 0xffffffff, count=LIMIT)
        except UcError as e:
            out('# emulator error %s at pc %#x' % (e, mu.reg_read(UC_MIPS_REG_PC)))
        if not yield_req[0]:
            break
        saved = [mu.reg_read(r) for r in GPR]; pc = mu.reg_read(UC_MIPS_REG_PC)
        if irq_req:
            # the thread waits for a hardware interrupt: deliver it by running the handler
            what, handler, setreg = irq_req.pop(0)
            out('# (%s waits for the %s interrupt; running its handler)' % (name, what))
            regs.update(setreg)
            irq_done.add(pc)
            run('interrupt handler ' + what, handler, 0, sp=0x87e00000)
            for k in setreg: regs[k] = 0
        else:
            # the thread went to sleep: run the threads created so far, then come back
            out('# (%s sleeps; running other threads)' % name)
            run_pending()
        out('\n##### back in %s' % name)
        for r, v in zip(GPR, saved):
            mu.reg_write(r, v)
    if count[0] >= LIMIT:
        out('# instruction limit; hottest: ' + ', '.join('%#x' % a for a, _ in hot.most_common(6)) + ' ra %#x' % mu.reg_read(UC_MIPS_REG_RA))

stacks = [0x87f00000]
def run_pending():
    while started[0] < len(threads) and started[0] < 16:
        n, fn, arg = threads[started[0]]; started[0] += 1
        stacks[0] -= 0x40000
        if n == 'console thread':
            out('\n##### (console thread skipped: it only serves the serial console)'); continue
        run('thread ' + n, fn, arg, sp=stacks[0])

mu_mem_write(STOP, b'\0' * 16)
run('entry', BASE, ARGS)
run_pending()
def words(a, n):
    return struct.unpack('<%dI' % n, bytes(mu_mem_read(a, 4 * n)))
out('\n##### memory the display controller was pointed at')
frm = regs.get(0xb3050000, 0)
if frm:
    w = words(frm | 0x80000000, 16); out('frame descriptor @%#x: %s' % (frm, ' '.join('%08x' % x for x in w)))
    for i, a in enumerate(w[5:9]):
        if a:
            out('  layer %d descriptor @%#x: %s' % (i, a, ' '.join('%08x' % x for x in words(a | 0x80000000, 12))))
out('final register values (non-zero, not CPM seed): ')
for a in sorted(regs):
    if regs[a] and not (0xb0000000 <= a < 0xb0000100) and not (0xb0030000 <= a < 0xb0040000):
        out('  %-22s %#010x' % (rname(a), regs[a]))
open('trace.txt', 'w').write('\n'.join(log) + '\n')
