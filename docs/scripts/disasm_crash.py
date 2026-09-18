import capstone, struct

path = r'E:\Games\ctr-native\build-msvc-x86\Release\ctr_native.exe'
data = open(path, 'rb').read()
e_lfanew = struct.unpack_from('<I', data, 0x3C)[0]
num_sections = struct.unpack_from('<H', data, e_lfanew + 6)[0]
opt_size = struct.unpack_from('<H', data, e_lfanew + 20)[0]
base = struct.unpack_from('<I', data, e_lfanew + 52)[0]
sec_off = e_lfanew + 24 + opt_size
sections = []
for i in range(num_sections):
    off = sec_off + i * 40
    name = data[off:off + 8].rstrip(b'\0').decode()
    vsize, vaddr, rawsize, rawptr = struct.unpack_from('<IIII', data, off + 8)
    sections.append((name, vaddr, vsize, rawptr, rawsize))
print("image base:", hex(base))
for s in sections:
    print("section:", s[0], hex(s[1]), "vsize", hex(s[2]))

def rva_to_off(rva):
    for name, vaddr, vsize, rawptr, rawsize in sections:
        if vaddr <= rva < vaddr + max(vsize, rawsize):
            return rawptr + (rva - vaddr)
    return None

target = 0x3E930
start = target - 0x70
off = rva_to_off(start)
code = data[off:off + 0x100]
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
for ins in md.disasm(code, base + start):
    s = ins.address - base
    marker = '   <===== FAULTING INSTRUCTION' if s <= target < (s + ins.size) else ''
    print(f"+0x{s:06X}: {ins.mnemonic:8s} {ins.op_str}{marker}")