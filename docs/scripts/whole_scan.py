import struct
from collections import Counter
IMG = r'E:\Games\CrashCTR-Win\assets\ctr-u.bin'
RAW = 2352
img = open(IMG, 'rb').read()
total = len(img) // RAW
print("scanning", total, "sectors for subheader stats...")

c = Counter()
audio_lbas = []
textish = []
for i in range(total):
    b = i * RAW
    sub = (img[b+16], img[b+17], img[b+18], img[b+19])
    c[sub] += 1
    if sub[2] & 4:
        if len(audio_lbas) < 30:
            audio_lbas.append((i, sub))
    # sample texty sectors in the XA file area
    if 139097 <= i < 140041 and len(textish) < 6:
        chunk = img[b+24:b+24+64]
        if sum(1 for x in chunk if 32 <= x < 127) > 40:
            textish.append(i)

print("top subheader combos across whole image:")
for k, v in c.most_common(15):
    print(f"   file={k[0]} ch={k[1]} submode=0x{k[2]:02X} coding=0x{k[3]:02X} -> {v}")
print("\naudio-flagged (submode&4) sector count sample:", audio_lbas[:10])

print("\ntexty sectors in S01.XA:", textish)
for i in textish[:3]:
    b = i * RAW
    print(f"\n--- sector {i} text [24:300):")
    print(img[b+24:b+300].decode('ascii', 'replace'))

# also: full nonzero density of the entire S01.XA file
nz = 0; tot = 0
for i in range(139097, 140041):
    b = i * RAW + 24
    nz += sum(1 for x in img[b:b+2324] if x != 0)
    tot += 2324
print(f"\nS01.XA total non-zero: {nz}/{tot} ({100.0*nz/tot:.2f}%)")

# compare: a known-real data file (BIGFILE.BIG area) density for reference
b = 276 * RAW + 24
nz2 = sum(1 for x in img[b:b+2324] if x != 0)
print("BIGFILE.BIG sector 0 density:", nz2, "/2324")
