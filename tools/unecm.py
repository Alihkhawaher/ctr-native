#!/usr/bin/env python3
"""
unecm.py — decode ECM (Error Code Modeler) disc images to raw 2352-byte BIN.

Python port of Neill Corlett's unecm.c (GPL) decode loop, adjusted for CTR
Native: output sectors are ALWAYS the full 2352 bytes (sync + address + mode +
payload), with ECC/EDC areas zero-filled. The engine never reads ECC/EDC, and
the source images for this project are No-EDC dumps anyway; for byte-perfect
images use the original unecm plus a re-ECC pass.

Usage:  python unecm.py <file.ecm> [output.bin]
"""

import os
import sys

SECTOR = 2352


def msf_bcd(lba):
    """Address field for a sector (BCD minutes:seconds:frames, +150 offset)."""
    pos = lba + 150
    m, rem = divmod(pos, 75 * 60)
    s, f = divmod(rem, 75)
    return bytes((((m // 10) << 4) | (m % 10),
                  ((s // 10) << 4) | (s % 10),
                  ((f // 10) << 4) | (f % 10)))


def decode(inp, out):
    magic = inp.read(4)
    if magic != b"ECM\x00":
        print("ERROR: not an ECM file (bad magic)", file=sys.stderr)
        return 1

    sectors_written = 0
    literal_total = 0
    type_counts = {1: 0, 2: 0, 3: 0}

    while True:
        c = inp.read(1)
        if not c:
            print("ERROR: unexpected EOF", file=sys.stderr)
            return 1
        c = c[0]
        rec_type = c & 3
        num = (c >> 2) & 0x1F
        bits = 5
        while c & 0x80:
            c = inp.read(1)
            if not c:
                print("ERROR: unexpected EOF", file=sys.stderr)
                return 1
            c = c[0]
            num |= (c & 0x7F) << bits
            bits += 7
        if num == 0xFFFFFFFF:
            break
        num += 1

        if rec_type == 0:
            remaining = num
            literal_total += num
            while remaining:
                b = min(remaining, SECTOR)
                chunk = inp.read(b)
                if len(chunk) != b:
                    print("ERROR: unexpected EOF (literal)", file=sys.stderr)
                    return 1
                out.write(chunk)
                remaining -= b
            continue

        for _ in range(num):
            sector = bytearray(SECTOR)
            sector[1:11] = b"\xff" * 10  # sync 00 FF*10 00
            if rec_type == 1:
                sector[0x0F] = 0x01
                addr = inp.read(3)
                if len(addr) != 3:
                    print("ERROR: unexpected EOF (type1)", file=sys.stderr)
                    return 1
                sector[0x0C:0x0F] = addr
                data = inp.read(0x800)
                if len(data) != 0x800:
                    print("ERROR: unexpected EOF (type1 data)", file=sys.stderr)
                    return 1
                sector[0x10:0x10 + 0x800] = data
            elif rec_type == 2:
                data = inp.read(0x804)
                if len(data) != 0x804:
                    print("ERROR: unexpected EOF (type2)", file=sys.stderr)
                    return 1
                sector[0x14:0x14 + 0x804] = data
                sector[0x10:0x14] = sector[0x14:0x18]  # subheader dup
                # ECM type 2/3 records carry only the 2336-byte Mode2 body;
                # sync/address/mode arrive as separate literal (type 0) bytes.
                out.write(sector[0x10:0x10 + 2336])
            else:  # rec_type == 3
                data = inp.read(0x918)
                if len(data) != 0x918:
                    print("ERROR: unexpected EOF (type3)", file=sys.stderr)
                    return 1
                sector[0x14:0x14 + 0x918] = data
                sector[0x10:0x14] = sector[0x14:0x18]  # subheader dup
                out.write(sector[0x10:0x10 + 2336])
            if rec_type in (2, 3):
                sectors_written += 1
                type_counts[rec_type] += 1
                continue
            out.write(sector)
            sectors_written += 1
            type_counts[rec_type] += 1

    print(f"decoded {sectors_written} sectors "
          f"(mode1: {type_counts[1]}, mode2f1: {type_counts[2]}, mode2f2: {type_counts[3]}, "
          f"literal bytes: {literal_total}) -> {sectors_written * SECTOR} bytes")
    return 0


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    src = sys.argv[1]
    dst = sys.argv[2] if len(sys.argv) > 2 else src[:-4] if src.lower().endswith(".ecm") else src + ".bin"
    print(f"decoding {src} -> {dst}")
    with open(src, "rb") as inp, open(dst, "wb") as out:
        rc = decode(inp, out)
    if rc == 0:
        size = os.path.getsize(dst)
        print(f"output: {dst} ({size} bytes, {size % SECTOR} bytes past a sector boundary)")
    return rc


if __name__ == "__main__":
    sys.exit(main())
