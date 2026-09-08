import pefile, capstone, pathlib, sys
p=pefile.PE(r'D:\SteamLibrary\steamapps\common\DCSWorld\bin\UniModelDesc.dll')
base=p.OPTIONAL_HEADER.ImageBase
syms={base+s.address:s.name.decode(errors='replace') for s in p.DIRECTORY_ENTRY_EXPORT.symbols if s.name}
imports={s.address:s.name.decode(errors='replace') for d in p.DIRECTORY_ENTRY_IMPORT for s in d.imports if s.name}
md=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_64); md.detail=True
start=int(sys.argv[1],16) if len(sys.argv)>1 else 0x54fc0
size=int(sys.argv[2],16) if len(sys.argv)>2 else 0xa00
lines=[]
for i in md.disasm(p.get_data(start,size),base+start):
    target=0
    if i.mnemonic=='call':
        o=i.operands[0]
        if o.type==capstone.x86.X86_OP_IMM:target=o.imm
        elif o.type==capstone.x86.X86_OP_MEM and o.mem.base==capstone.x86.X86_REG_RIP:target=i.address+i.size+o.mem.disp
    lines.append(f'{i.address-base:x}: {i.mnemonic:9} {i.op_str} '+syms.get(target,imports.get(target,'')))
out=pathlib.Path('validation')/f'UniModelDesc_{start:x}.asm'
out.write_text('\n'.join(lines),encoding='utf-8')
print(out)
