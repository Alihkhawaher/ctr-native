"""xa_player.py - minimal XA voice player for CTR disc images.

Usage:
  python xa_player.py <image.bin> <relative/path.XA> [channel] [out.wav]

Walks the ISO9660 filesystem of the image, finds the XA file, collects its
audio sectors (submode & 4, matching channel), decodes 4-bit XA ADPCM with the
same math as the native port, and writes a 16-bit mono WAV.
"""
import struct, sys, wave

def main():
    image = sys.argv[1] if len(sys.argv) > 1 else r'E:\Games\RomStation 1\Games\Playstation\643\Crash Team Racing2.bin'
    want = sys.argv[2] if len(sys.argv) > 2 else 'XA/ENG/EXTRA/S00.XA'
    chan = int(sys.argv[3]) if len(sys.argv) > 3 else -1
    out = sys.argv[4] if len(sys.argv) > 4 else 'voice_test.wav'

    RAW = 2352
    img = open(image, 'rb').read()

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

    # find file by path
    parts = want.replace('\\','/').split('/')
    entries = read_dir(root[0], root[1])
    lba = size = None
    for part in parts:
        found = None
        for nm, l, s, fl in entries:
            if nm.upper() == part.upper():
                found = (l, s); break
        if not found:
            print("not found:", part); return
        if part != parts[-1]:
            entries = read_dir(found[0], found[1])
        else:
            lba, size = found
    print(f"file: {want} lba={lba} size={size} sectors={size//2048}")

    # collect sectors
    POS_T = [0, 60, 115, 98, 122]
    NEG_T = [0, 0, -52, -55, -60]
    def clamp16(v):
        return max(-32768, min(32767, v))

    def decode_sector(buf, base, state):
        out_ = []
        for frame in range(18):
            frameOff = base + 8 + frame*128
            header = frameOff + 4
            for su in range(8):
                pi = (su & 3) | ((su & 4) << 1)
                param = buf[header + pi]
                shift = param & 0xf
                weight = (param >> 4) & 0xf
                if weight > 4: weight = 4
                w0 = POS_T[weight]; w1 = NEG_T[weight]
                for i in range(28):
                    byte = buf[frameOff + 16 + i*4 + (su >> 1)]
                    nib = (byte & 0xf) << 4 if (su & 1) == 0 else byte & 0xf0
                    s = ((nib if nib < 128 else nib - 256) * 0x100) >> shift
                    s += (state[0]*w0) >> 6
                    s += (state[1]*w1) >> 6
                    s = clamp16(s)
                    state[1] = state[0]; state[0] = s
                    out_.append(s)
        return out_

    # scan sectors: which channels are present?
    from collections import Counter
    chans = Counter()
    nsec = size // 2048
    audio = []
    for i in range(nsec):
        b = (lba+i)*RAW
        sub = (img[b+16], img[b+17], img[b+18], img[b+19])
        if (sub[2] & 4) and sub[0] == 1:
            chans[sub[1]] += 1
            audio.append((i, sub[1], sub[3]))
    print("audio sectors by channel:", dict(chans))
    if not audio:
        print("NO AUDIO SECTORS FOUND (image stripped?)"); return
    if chan < 0:
        chan = chans.most_common(1)[0][0]
    sel = [a for a in audio if a[1] == chan]
    print(f"decoding channel {chan}: {len(sel)} sectors")

    rate = None
    state = [0, 0]
    pcm = []
    for (i, c, coding) in sel:
        buf = img[(lba+i)*RAW+16:(lba+i)*RAW+16+2336]
        if rate is None:
            rate = 37800 if ((coding >> 2) & 3) == 0 else 18900
        pcm.extend(decode_sector(buf, 0, state))

    peak = max(abs(x) for x in pcm)
    rms = (sum(x*x for x in pcm)/len(pcm)) ** 0.5
    dur = len(pcm)/rate
    print(f"decoded {len(pcm)} samples @ {rate}Hz = {dur:.2f}s  peak={peak}  rms={rms:.0f}")

    with wave.open(out, 'wb') as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(rate)
        w.writeframes(struct.pack('<%dh' % len(pcm), *pcm))
    print("wrote", out)

if __name__ == '__main__':
    main()
