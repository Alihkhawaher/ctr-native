import struct
IMG = r'E:\Games\CrashCTR-Win\assets\ctr-u.bin'
RAW = 2352
img = open(IMG, 'rb').read()

def dump(lba, label):
    b = lba * RAW
    raw = img[b:b+RAW]
    print(f"{label} (lba {lba}):")
    print(f"  [0..16)   : {raw[0:16].hex()}")
    print(f"  [16..24)  : {raw[16:24].hex()}   file={raw[16]} ch={raw[17]} submode=0x{raw[18]:02X} coding=0x{raw[19]:02X}")
    print(f"  [24..48)  : {raw[24:48].hex()}")

# PVD sector, system area, SCUS exe, S01.XA first sectors, MUSIC dir file, TEST.STR
dump(16, "PVD")
dump(24, "SCUS_944.26")
dump(139097, "S01.XA sector 0")
dump(139098, "S01.XA sector 1")
dump(139097+500, "S01.XA sector 500")
dump(139097+943, "S01.XA last sector")
dump(160730, "XA/MUSIC dir")
dump(173635, "TEST.STR sector 0")

# S01.XA: check a few sectors across the file for subheader variance at other offsets
print("\nsubheader-region bytes [16..24) across S01.XA every 100 sectors:")
for i in range(0, 944, 100):
    b = (139097 + i) * RAW
    print(f"  s{i}: {img[b+16:b+24].hex()}")

# what does the DATA region look like (is it XA adpcm? look for typical shift/filter params)
b = 139097 * RAW
print("\nS01.XA s0 data[24:24+32]:", img[b+24:b+56].hex())
b2 = (139097 + 109) * RAW
print("S01.XA s109 data[24:24+32]:", img[b2+24:b2+56].hex())
