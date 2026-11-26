#!/usr/bin/env python3
"""Makes an address profile for another rtos image from a profiled one, by instruction pattern
(addresses in jal/j and immediates masked).   match.py <profiled image> <new image> <name>"""
import sys, struct, json, hashlib, os
def load(p):
    d = open(p, 'rb').read(); end = struct.unpack_from('<I', d, 0x14)[0] - 0x86000000
    d = d[:end]; return d, struct.unpack('<%dI' % (len(d) // 4), d[:len(d) & ~3])
def mask(w):
    op = w >> 26
    if op in (2, 3): return op << 26
    if op in (15, 9, 13, 35, 43, 32, 36, 40, 33, 37, 41, 8, 10, 11, 12, 14): return w & 0xffff0000
    return w
PF = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'profiles.json')
prof = json.load(open(PF))
da, A = load(sys.argv[1]); db, B = load(sys.argv[2])
ka, kb = hashlib.sha256(da).hexdigest()[:16], hashlib.sha256(db).hexdigest()[:16]
MA, MB = [mask(w) for w in A], [mask(w) for w in B]
out = {}
for name, v in prof[ka]['functions'].items():
    i = (int(v, 16) - 0x86000000) // 4
    for n in (24, 16, 12, 8, 6):
        hits = [j for j in range(len(MB) - n) if MB[j] == MA[i] and MB[j:j + n] == MA[i:i + n]]
        if hits: break
    if not hits:
        print('NOT FOUND', name); continue
    j = min(hits, key=lambda h: abs(h - i))        # several identical functions: the nearest
    out[name] = '%#x' % (0x86000000 + 4 * j)
    print('%-16s %s -> %s%s' % (name, v, out[name], '  (%d candidates)' % len(hits) if len(hits) > 1 else ''))
prof[kb] = {'name': sys.argv[3], 'functions': out}
json.dump(prof, open(PF, 'w'), indent=1)
print('profile', kb, 'written')
