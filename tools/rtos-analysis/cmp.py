import re, sys, difflib
def norm(path):
    out = []
    for l in open(path):
        l = re.sub(r' +\[pc.*', '', l.rstrip('\n'))
        if l.startswith('SPI'):
            for t in re.findall(r'cmd [0-9a-f]{2}:|<[0-9a-f]{2}|[0-9a-f]{2}\b', re.sub(r'\?\d+ bits \S+', '', l[3:])):
                out.append('spi ' + t)
        elif l.startswith('PIN'):
            if not re.search(r'PB(\d|1[0-5]|2[4-7]) +func', l): out.append(' '.join(l.split()))
        elif l.startswith('# msleep('):
            out.append(l[2:])
        elif l.startswith('W '):
            m = re.match(r'W (\S+)\s+= (0x[0-9a-f]+)', l)
            r, v = m.group(1), m.group(2)
            if r.startswith(('CPM', 'DPU', 'PWM')) and r not in ('DPU+0x0000',):
                out.append('reg %s = %s' % (r, v))
        elif 'found lcd' in l or 'default lcd' in l:
            out.append('note ' + ('found' if 'found' in l else 'default'))
    return out
a, b = norm(sys.argv[1]), norm(sys.argv[2])
sm = difflib.SequenceMatcher(None, a, b, autojunk=False)
same = sum(t[2] - t[1] for t in sm.get_opcodes() if t[0] == 'equal')
print('%s: stock %d items, ours %d, in common %d' % (sys.argv[3], len(a), len(b), same))
n = 0
for tag, i1, i2, j1, j2 in sm.get_opcodes():
    if tag != 'equal' and n < int(sys.argv[4]) :
        print('  %-7s stock[%d:%d] %s' % (tag, i1, i2, a[i1:i2][:8])); print('          ours[%d:%d] %s' % (j1, j2, b[j1:j2][:8])); n += 1
