import sys, struct, re, pickle
path, base = sys.argv[1], int(sys.argv[2],16)
data = open(path,'rb').read()
gp = struct.unpack_from('<I', data, 8)[0]
def rd(a):
    o=a-base
    return struct.unpack_from('<I',data,o)[0] if 0<=o<=len(data)-4 else None
strs = {base+m.start(): m.group()[:-1].decode() for m in re.finditer(rb'[\x20-\x7e\t\n]{4,}\x00', data)}
refs = {}; regs = {}
for off in range(0, len(data)-3, 4):
    w = struct.unpack_from('<I', data, off)[0]; a=base+off
    op=w>>26; rs=(w>>21)&31; rt=(w>>16)&31; imm=w&0xffff; simm=imm-0x10000 if imm&0x8000 else imm
    if op==0x0f: regs[rt]=(imm<<16,a); continue
    if op==0x23 and rs==28:   # lw rt, off(gp)
        v=rd((gp+simm)&0xffffffff)
        if v is not None:
            regs[rt]=(v,a); refs.setdefault(v,[]).append(a)
        continue
    if op in (0x09,0x0d,0x23,0x21,0x25,0x24,0x20,0x2b,0x29,0x28) and rs in regs and rs!=28 and a-regs[rs][1]<0x80:
        v=(regs[rs][0]|imm) if op==0x0d else (regs[rs][0]+simm)&0xffffffff
        refs.setdefault(v,[]).append(a)
        if op in (0x09,0x0d): regs[rt]=(v,a)
pickle.dump((strs,refs), open(sys.argv[3],'wb'))
print('gp',hex(gp),len(refs))
