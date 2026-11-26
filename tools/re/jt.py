import sys, struct, capstone
data=open(sys.argv[1],'rb').read(); base=0x80100000
md=capstone.Cs(capstone.CS_ARCH_MIPS, capstone.CS_MODE_MIPS32|capstone.CS_MODE_LITTLE_ENDIAN); md.skipdata=True
ins=list(md.disasm(data[:0x50000], base))
for k,i in enumerate(ins):
    if i.mnemonic=='sltiu' and k+12<len(ins):
        win=ins[k:k+14]
        if any(x.mnemonic=='jr' and x.op_str not in('$ra',) for x in win):
            back=ins[max(0,k-6):k]
            bias=[x for x in back if x.mnemonic=='addiu' and '-' in x.op_str]
            lb=[x for x in ins[max(0,k-12):k] if x.mnemonic in('lbu','lhu')]
            print(hex(i.address), i.op_str, '| bias:', [b.op_str for b in bias], '| loads:', [f'{x.mnemonic} {x.op_str}' for x in lb])
