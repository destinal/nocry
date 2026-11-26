import sys, struct, pickle, capstone
path=sys.argv[1]; base=0x80100000
data=open(path,'rb').read()
strs,refs=pickle.load(open('uboot.pkl','rb'))
gp=struct.unpack_from('<I',data,8)[0]
md=capstone.Cs(capstone.CS_ARCH_MIPS, capstone.CS_MODE_MIPS32|capstone.CS_MODE_LITTLE_ENDIAN); md.skipdata=True
# reverse ref map
rev={}
for t,l in refs.items():
    for a in l: rev[a]=t
def fstart(a):
    o=a-base
    while o>0:
        w=struct.unpack_from('<I',data,o)[0]
        if w>>16==0x27bd and (w&0x8000): return base+o
        o-=4
def dump(a,n):
    o=a-base
    for i in md.disasm(data[o:o+n*4], a):
        c=''
        if i.address in rev:
            t=rev[i.address]; c=' ; -> '+hex(t)
            if t in strs: c+=' '+repr(strs[t][:60])
            else:
                for s0,s in strs.items():
                    if s0<=t<s0+len(s): c+=' '+repr(s[t-s0:][:60]); break
        print(f'{i.address:08x}: {i.mnemonic:8s} {i.op_str}{c}')
mode=sys.argv[2]; a=int(sys.argv[3],16)
if mode=='f':
    s=fstart(a); print('func start',hex(s)); dump(s,int(sys.argv[4]))
else: dump(a,int(sys.argv[4]))
