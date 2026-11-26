import sys,pickle
strs,refs=pickle.load(open(sys.argv[1],'rb'))
for pat in sys.argv[2:]:
    for a,s in strs.items():
        if pat in s:
            print(hex(a), repr(s[:70]), [hex(x) for x in refs.get(a,[])])
