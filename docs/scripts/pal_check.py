import struct
from collections import Counter

PAL = r'E:\Games\RomStation 1\Games\Playstation\643\Crash Team Racing2.bin'
RAW = 2352
img = open(PAL, 'rb').read()
print("PAL image:", len(img), "bytes =", len(img)//RAW, "sectors")

def read_sectors(lba, nbytes):
    out = []; need = nbytes; i = 0
    while need > 0:
        b = (lba + i)*RAW + 24
        take = min(2048, need)
        out.append(img[b:b+take]); need -= take; i += 1
    return b''.join(out)

pvd = read_sectors(16, 2048)
root_rec = pvd[156:156+34]
root_lba, root_size = struct.unpack_from('<I', root_rec, 2)[0], struct.unpack_from('<I', root_rec, 10)[0]
print("root dir: lba", root_lba, "size", root_size)

def read_dir(lba, size):
    data = read_sectors(lba, size)
    pos = 0; out = []
    while pos < len(data):
        rl = data[pos]
        if rl == 0:
            pos = (pos // 2048 + 1) * 2048
            continue
        rec = data[pos:pos+rl]
        l, s = struct.unpack_from('<I', rec, 2)[0], struct.unpack_from('<I', rec, 10)[0]
        fl = rec[25]; nl = rec[32]
        out.append((rec[33:33+nl], l, s, fl))
        pos += rl
    return out

def find_path(parts):
    entries = read_dir(root_lba, root_size)
    found = None
    for part in parts:
        found = None
        for nm, l, s, fl in entries:
            name = nm.decode('ascii', 'replace').split(';')[0]
            if name.upper() == part.upper():
                found = (l, s); break
        if not found:
            return None
        if part != parts[-1]:
            entries = read_dir(found[0], found[1])
    return found

xa = find_path(['XA'])
print("XA dir:", xa)
eng = find_path(['XA', 'ENG'])
print("XA/ENG:", eng)
if eng:
    for nm, l, s, fl in read_dir(eng[0], eng[1]):
        print("  XA/ENG/", nm, l, s, "dir" if fl & 2 else "file")
extra = find_path(['XA', 'ENG', 'EXTRA'])
print("XA/ENG/EXTRA:", extra)
if extra:
    ents = read_dir(extra[0], extra[1])
    for nm, l, s, fl in ents[:6]:
        print("  ", nm, l, s)

# scan subheaders of the first few XA files found
if extra:
    for nm, l, s, fl in read_dir(extra[0], extra[1]):
        if b'.XA' in nm.upper():
            nsec = s // 2048
            c = Counter(); audio = 0; dense = 0
            for i in range(min(nsec, 500)):
                b = (l+i)*RAW
                sub = (img[b+16], img[b+17], img[b+18], img[b+19])
                c[sub] += 1
                if sub[2] & 4:
                    audio += 1
                nz = sum(1 for x in img[b+24:b+24+2324] if x != 0)
                if nz > 1000:
                    dense += 1
            print(f"\n{nm.decode()}: {nsec} sectors, scanned {min(nsec,500)}: audio-flagged={audio}, dense(>1000nz)={dense}")
            for k, v in c.most_common(6):
                print(f"   file={k[0]} ch={k[1]} submode=0x{k[2]:02X} coding=0x{k[3]:02X} -> {v}")
            break
