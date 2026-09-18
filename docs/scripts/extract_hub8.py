import struct

IMG = r'E:\Games\CrashCTR-Win\assets\ctr-u.bin'
RAW = 2352; DOFF = 24
img = open(IMG, 'rb').read()

def read_sectors(lba, nbytes):
    out = []; need = nbytes; i = 0
    while need > 0:
        b = (lba + i) * RAW + DOFF   # FIXED: advance sector
        take = min(2048, need)
        out.append(img[b:b+take]); need -= take; i += 1
    return b''.join(out)

BIG_LBA = 276
h8 = read_sectors(BIG_LBA, 8)
_, numEntry = struct.unpack('<ii', h8)
print("numEntry:", numEntry)
table = read_sectors(BIG_LBA, (8 + numEntry*8 + 2047)//2048 * 2048)
entries = [struct.unpack_from('<ii', table, 8 + i*8) for i in range(numEntry)]
print("entries[518..524]:")
for i in range(518, 525): print(" ", i, entries[i])

def subfile(i):
    off, sz = entries[i]
    return read_sectors(BIG_LBA + off, sz), BIG_LBA + off, sz

lev32, l32, s32 = subfile(522)
print(f"\nLEV32: lba={l32} size={s32}")
print("raw[0..4 words]:", [hex(struct.unpack_from('<I', lev32, 4*i)[0]) for i in range(4)])
print("word0 as i32:", struct.unpack_from('<i', lev32, 0)[0])
raw = 4 + 0x54E1C
print("real 0x54E1C (64B):", lev32[raw:raw+64].hex())
print("value@0x54E1C:", hex(struct.unpack_from('<I', lev32, raw)[0]))
pat = b'\xEB\xA1\xE0\xA1'
hits = []; p = lev32.find(pat)
while p != -1: hits.append(p); p = lev32.find(pat, p+1)
print("EB A1 E0 A1 hits in LEV32 (raw offs):", [hex(x) for x in hits])

ptr32, p32, ps32 = subfile(523)
nb = struct.unpack_from('<i', ptr32, 0)[0]
cnt = nb >> 2
print(f"\nPTR32: lba={p32} size={ps32} numBytes={nb} count={cnt}")
offs = [struct.unpack_from('<i', ptr32, 4 + 4*i)[0] & ~3 for i in range(cnt)]
print("contains 0x54E1C?", 0x54E1C in offs)
for d in (4, 8, 0xC, 0x10):
    print(f"contains 0x54E1C+0x{d:X}?", (0x54E1C+d) in offs)
near = sorted(set(x for x in offs if 0x54800 <= x <= 0x56000))
print("map offs in [0x54800..0x56000]:", [hex(x) for x in near][:60], "count", len(near))
print("min:", hex(min(offs)), "max:", hex(max(offs)))

# hub31 for comparison
lev31, l31, s31 = subfile(519)
ptr31, p31, ps31 = subfile(520)
nb31 = struct.unpack_from('<i', ptr31, 0)[0]
cnt31 = nb31 >> 2
offs31 = [struct.unpack_from('<i', ptr31, 4 + 4*i)[0] & ~3 for i in range(cnt31)]
print(f"\nLEV31: lba={l31} size={s31}")
raw31 = 4 + 0x3E298
print("real 0x3E298 (32B):", lev31[raw31:raw31+32].hex())
print("value@0x3E298:", hex(struct.unpack_from('<I', lev31, raw31)[0]))
print(f"PTR31: lba={p31} numBytes={nb31} count={cnt31}")
print("contains 0x3E298?", 0x3E298 in offs31)
near31 = sorted(set(x for x in offs31 if 0x3E000 <= x <= 0x3F000))
print("map offs in [0x3E000..0x3F000]:", [hex(x) for x in near31][:40], "count", len(near31))

# also: what SHOULD the list slot contain? Look at a known-good patched list:
# hub31's list at 0x3E298: dump 64 bytes around it in the FILE
print("\nLEV31 around 0x3E298 (file):")
for o in range(0x3E298, 0x3E298 + 96, 16):
    print(f"  0x{o:X}: {lev31[4+o:4+o+16].hex()}")
print("\nLEV32 around 0x54E1C (file):")
for o in range(0x54E1C - 32, 0x54E1C + 96, 16):
    print(f"  0x{o:X}: {lev32[4+o:4+o+16].hex()}")
