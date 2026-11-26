import sys, struct, subprocess, difflib, collections, json, re
ours_elf, ours_bin, blob, out = sys.argv[1:5]
B=0x80100000
def W(d): return [struct.unpack_from('<I',d,i)[0] for i in range(0,len(d)-3,4)]
def norm(w):
    op=w>>26
    if op in (2,3): return op<<26
    if op==1: return w&0xffff0000
    if op in (0,0x1c,0x1f): return w
    if op in (4,5,6,7,0x14,0x15,0x16,0x17): return w&0xffff0000
    return w&0xffff0000
def strs(d):
    return {B+m.start(): m.group()[:-1] for m in re.finditer(rb'[\x20-\x7e\t\n]{5,}\x00', d)}
def gp_of(d): return struct.unpack_from('<I',d,8)[0]
def strrefs(d, a, n):
    # lw rX, off(gp) -> page; addiu rY, rX, lo  => string address
    gp=gp_of(d); S=STR[id(d)]; regs={}; out=[]
    for k in range(n):
        o=a-B+4*k
        if o+4>len(d): break
        w=struct.unpack_from('<I',d,o)[0]; op=w>>26; rs=(w>>21)&31; rt=(w>>16)&31; imm=w&0xffff; s=imm-0x10000 if imm&0x8000 else imm
        if op==0x23 and rs==28:
            go=gp+s-B
            if 0<=go<len(d)-3: regs[rt]=struct.unpack_from('<I',d,go)[0]
        elif op==0x09 and rs in regs:
            v=(regs[rs]+s)&0xffffffff
            if v in S: out.append(S[v])
    return out
ob=open(ours_bin,'rb').read(); bb=open(blob,'rb').read()
STR={id(ob):strs(ob), id(bb):strs(bb)}
# our functions from ELF symbols
nm=subprocess.run(['llvm-nm','-S','--defined-only',ours_elf],capture_output=True,text=True).stdout
of=[]
for l in nm.splitlines():
    p=l.split()
    if len(p)==4 and p[2] in 'Tt' and int(p[1],16)>=16:
        of.append((p[3],int(p[0],16)&0xffffffff,int(p[1],16)))
# blob functions: split at PIC prologue (lui gp,X ; addiu gp,gp,Y ; addu gp,gp,t9) and at addiu sp,sp,-N after jr ra
bw=W(bb); starts=set()
for i in range(len(bw)-2):
    if bw[i]>>16==0x3c1c and bw[i+1]>>16==0x279c and bw[i+2]==0x0399e021: starts.add(i)
code_end=max(starts)+400
st=sorted(starts)
bf=[(B+4*s, 4*((st[k+1] if k+1<len(st) else code_end)-s)) for k,s in enumerate(st)]
def feat(d,a,sz): 
    w=W(d[a-B:a-B+sz]); return [norm(x) for x in w]
OF=[(n,a,sz,feat(ob,a,sz),strrefs(ob,a,sz//4)) for n,a,sz in of]
BF=[(a,sz,feat(bb,a,sz),strrefs(bb,a,sz//4)) for a,sz in bf]
# candidate pairing: strings first, else size+multiset
bystr=collections.defaultdict(list)
for j,(a,sz,f,s) in enumerate(BF):
    for x in s: bystr[x].append(j)
used=set(); res=[]
for n,a,sz,f,s in OF:
    cands=set()
    for x in s: cands.update(bystr.get(x,[]))
    if not cands:
        cands={j for j,(ba,bsz,bfe,bs) in enumerate(BF) if 0.7*sz<=bsz<=1.4*sz+64}
    best=(0,None)
    for j in cands:
        sm=difflib.SequenceMatcher(None,f,BF[j][2],autojunk=False)
        if sm.real_quick_ratio()<best[0] or sm.quick_ratio()<best[0]: continue
        r=sm.ratio()
        if r>best[0]: best=(r,j)
    ms=0
    if best[1] is not None:
        ca,cb=collections.Counter(f),collections.Counter(BF[best[1]][2])
        ms=2*sum((ca&cb).values())/max(1,len(f)+len(BF[best[1]][2]))
    res.append((n,hex(a),sz,round(best[0],3),hex(BF[best[1]][0]) if best[1] is not None else None, bool(s), round(ms,3)))
    if best[1] is not None and best[0]>=0.6: used.add(best[1])
json.dump({'ours':res,'blob_unmatched':[(hex(BF[j][0]),BF[j][1],[x.decode() for x in BF[j][3][:3]]) for j in range(len(BF)) if j not in used]},open(out,'w'),indent=0)
tot=sum(r[2] for r in res)
for lo,hi in ((0.98,1.01),(0.9,0.98),(0.75,0.9),(0.6,0.75),(0,0.6)):
    sel=[r for r in res if lo<=r[3]<hi]
    print(f"similarity [{lo:.2f},{hi:.2f}): {len(sel):4d} funcs  {100*sum(r[2] for r in sel)/tot:5.1f}% of our code")
for lo,hi in ((0.98,1.01),(0.9,0.98),(0.75,0.9),(0,0.75)):
    sel=[r for r in res if lo<=r[6]<hi]
    print(f"  multiset [{lo:.2f},{hi:.2f}): {len(sel):4d} funcs  {100*sum(r[2] for r in sel)/tot:5.1f}% of our code")
print('blob functions (prologue-split):',len(BF),' unmatched by any of ours (<0.6):',len(BF)-len(used))
