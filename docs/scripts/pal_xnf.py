import struct
PAL = r'E:\Games\RomStation 1\Games\Playstation\643\Crash Team Racing2.bin'
RAW = 2352
img = open(PAL, 'rb').read()

def read_sectors(lba, nbytes):
    o = []; need = nbytes; i = 0
    while need > 0:
        b = (lba + i)*RAW + 24
        t = min(2048, need)
        o.append(img[b:b+t]); need -= t; i += 1
    return b''.join(o)

def read_dir(lba, size):
    data = read_sectors(lba, size); pos = 0; out_ = []
    while pos < len(data):
        rl = data[pos]
        if rl == 0:
            pos = (pos // 2048 + 1) * 2048; continue
        rec = data[pos:pos+rl]
        l, s = struct.unpack_from('<I', rec, 2)[0], struct.unpack_from('<I', rec, 10)[0]
        fl = rec[25]; nl = rec[32]
        out_.append((rec[33:33+nl].decode('ascii','replace').split(';')[0], l, s, fl))
        pos += rl
    return out_

pvd = read_sectors(16, 2048)
rr = pvd[156:156+34]
root = (struct.unpack_from('<I', rr, 2)[0], struct.unpack_from('<I', rr, 10)[0])

def find_path(parts):
    entries = read_dir(root[0], root[1]); found = None
    for part in parts:
        found = None
        for nm, l, s, fl in entries:
            if nm.upper() == part.upper():
                found = (l, s); break
        if not found: return None
        if part != parts[-1]:
            entries = read_dir(found[0], found[1])
    return found

xnf = find_path(['XA', 'ENG.XNF'])
print("PAL XNF:", xnf)
if xnf:
    data = read_sectors(xnf[0], min(xnf[1], 4096))
    w = lambda off: struct.unpack_from('<i', data, off)[0]
    print("size:", xnf[1])
    print("magic:", hex(w(0)), "word1:", w(4), "numTypes:", w(8))
    numXasTotal = w(0x0C); numTracksTotal = w(0x10)
    print("numXasTotal:", numXasTotal, "numTracksTotal:", numTracksTotal)
    for cat in range(3):
        ns = w(0x2c + cat*4); fs = w(0x38 + cat*4)
        print(f"cat {cat}: numSongs={ns} firstSongIndex={fs}")
    xaSizeOffset = 0x44 + numXasTotal*4
    # EXTRA id 80 (cat 1)
    ns1 = w(0x2c + 4); fs1 = w(0x38 + 4)
    if ns1 > 80:
        idx = fs1 + 80
        off = xaSizeOffset + idx*4
        chf, fn, nsec = data[off], data[off+1], struct.unpack_from('<h', data, off+2)[0]
        print(f"PAL cat1 id80 -> entryIdx {idx}: channelFilter={chf} fileNumber={fn} numSectors={nsec}")
    # first few EXTRA entries
    print("PAL first 6 EXTRA entries:")
    for k in range(6):
        off = xaSizeOffset + (fs1 + k)*4
        chf, fn, nsec = data[off], data[off+1], struct.unpack_from('<h', data, off+2)[0]
        print(f"  id {k}: ch={chf} file={fn} sectors={nsec}")
