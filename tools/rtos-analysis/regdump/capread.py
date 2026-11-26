import sys, struct
d = open(sys.argv[1], 'rb').read()
o = 0x6e80000
assert d[o:o+8] == b'NCRDUMP1', d[o:o+8]
n = struct.unpack_from('<I', d, o + 8)[0]
done = struct.unpack_from('<I', d, o + 12 + 8 * n)[0] == 0xffffffff
print('#', n, 'registers', 'complete' if done else 'INCOMPLETE')
for i in range(n):
    a, v = struct.unpack_from('<II', d, o + 12 + 8 * i)
    print('%08x %08x' % (a, v))
q = o + 12 + 8 * n + 4
if d[q:q+4] == b'NCRS':
    s = struct.unpack_from('<64I', d, q + 4)
    print('# DC_ST samples:', ' '.join('%x' % x for x in s))
