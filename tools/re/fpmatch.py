import sys, struct, subprocess, collections, json
ours_elf, ours_bin, blob = sys.argv[1:4]
BASE=0x80100000
def words(b): return [struct.unpack_from('<I',b,i)[0] for i in range(0,len(b)-3,4)]
BRANCH_OPS={0x04,0x05,0x06,0x07,0x14,0x15,0x16,0x17}
def norm(w):
    op=w>>26
    if op in (2,3): return op<<26                      # j/jal: mask target
    if op==1: return w & 0xffff0000                     # regimm (bal/bgez..): mask offset (callee distance varies)
    if op in BRANCH_OPS: return w                       # local branches: keep
    if op==0: return w                                   # R-type: keep
    if op==0x1c: return w                                # SPECIAL2 (mul, ext...): keep
    if op==0x1f: return w                                # SPECIAL3 (ins/ext): keep
    return w & 0xffff0000                                # I-type: mask immediate
nm=subprocess.run(['llvm-nm','-S','--defined-only',ours_elf],capture_output=True,text=True).stdout
funcs=[]
for l in nm.splitlines():
    p=l.split()
    if len(p)==4 and p[2] in 'Tt':
        a=int(p[0],16)&0xffffffff; sz=int(p[1],16)
        if sz>=16: funcs.append((p[3],a,sz))
ob=open(ours_bin,'rb').read(); bb=open(blob,'rb').read()
bw=[norm(w) for w in words(bb)]
idx=collections.defaultdict(list)
K=4
for i in range(len(bw)-K): idx[tuple(bw[i:i+K])].append(i)
covered=bytearray(len(bw))
res={'exact':[], 'moved_prefix':[], 'missing':[]}
for name,a,sz in funcs:
    o=a-BASE; fw=[norm(w) for w in words(ob[o:o+sz])]
    if len(fw)<K: continue
    hit=None; best=(0,None)
    for c in idx.get(tuple(fw[:K]),[]):
        n=0
        while n<len(fw) and c+n<len(bw) and bw[c+n]==fw[n]: n+=1
        if n==len(fw): hit=c; break
        if n>best[0]: best=(n,c)
    if hit is not None:
        res['exact'].append((name,sz,hex(BASE+hit*4)))
        for k in range(hit,hit+len(fw)): covered[k]=1
    elif best[1] is not None:
        res['moved_prefix'].append((name,sz,hex(BASE+best[1]*4),round(best[0]/len(fw),2)))
    else:
        res['missing'].append((name,sz))
tot=sum(s for _,_,s in funcs)
ex=sum(s for _,s,_ in res['exact'])
print(f"our functions: {len(funcs)} ({tot} B)")
print(f"exact normalized match in blob: {len(res['exact'])} funcs, {ex} B ({100*ex/tot:.1f}% of our code bytes)")
print(f"prefix-only match (body differs): {len(res['moved_prefix'])}")
print(f"no match at all: {len(res['missing'])}")
print(f"blob words covered by our exact matches: {100*sum(covered)/len(bw):.1f}%")
json.dump(res,open(sys.argv[4],'w'),indent=0)
