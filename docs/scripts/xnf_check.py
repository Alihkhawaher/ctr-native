import struct

IMG = r'E:\Games\CrashCTR-Win\assets\ctr-u.bin'
RAW = 2352; DOFF = 24
img = open(IMG, 'rb').read()

def read_sectors(lba, nbytes):
    out = []; need = nbytes; i = 0
    while need > 0:
        b = (lba + i)*RAW + DOFF
        take = min(2048, need)
        out.append(img[b:b+take]); need -= take; i += 1
    return b''.join(out)

# --- parse XNF (XA/ENG.XNF at lba 138438, size 1840) ---
xnf = read_sectors(138438, 1840)
print("XNF size:", len(xnf))
w = lambda off: struct.unpack_from('<i', xnf, off)[0]
print("magic:", hex(w(0)), "word1:", w(4), "numTypes:", w(8))
numXasTotal = w(0x0C); numTracksTotal = w(0x10)
print("numXasTotal:", numXasTotal, "numTracksTotal:", numTracksTotal)
XA_HEADER_SIZE = 0x44
xaSizeOffset = XA_HEADER_SIZE + numXasTotal * 4
print("xaSizeOffset:", hex(xaSizeOffset))
for cat in range(3):
    ns = w(0x2c + cat*4); fs = w(0x38 + cat*4)
    print(f"cat {cat}: numSongs={ns} firstSongIndex={fs}")
# EXTRA = cat 1 (CDSYS_XA_TYPE_EXTRA?); the log said cat=1 id=80
for cat, name in ((1, 'cat1/EXTRA'), (2, 'cat2/GAME'), (0, 'cat0/MUSIC')):
    ns = w(0x2c + cat*4); fs = w(0x38 + cat*4)
    if ns > 80:
        idx = fs + 80
        off = xaSizeOffset + idx * 4
        chf, fn, nsec = xnf[off], xnf[off+1], struct.unpack_from('<h', xnf, off+2)[0]
        print(f"{name}: xaID 80 -> entryIdx {idx}: channelFilter={chf} fileNumber={fn} numSectors={nsec}")

# --- scan S01.XA (lba 139097, size 1933312 = 944 sectors) ---
lba, size = 139097, 1933312
nsec = size // 2048
print(f"\nS01.XA: {nsec} sectors")
from collections import Counter
combos = Counter()
audio_first = None
audio_list = []
for i in range(nsec):
    b = (lba + i) * RAW
    f, c, sm, cod = img[b+16], img[b+17], img[b+18], img[b+19]
    combos[(f, c, sm, cod)] += 1
    if sm & 0x04:
        if audio_first is None:
            audio_first = i
        if len(audio_list) < 10:
            audio_list.append((i, f, c, sm, cod))
print("distinct (file,channel,submode,coding) -> count:")
for k, v in combos.most_common(20):
    print(f"   {k} -> {v}")
print("first audio sector index:", audio_first)
print("first audio sectors:", audio_list)
