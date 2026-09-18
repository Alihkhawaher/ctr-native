#!/usr/bin/env python3
"""
disc_probe.py — identify PlayStation disc images for CTR Native.

For each image: sector size / mode, volume label, game ID (from SYSTEM.CNF),
and an XA-audio sector census (voices/music presence).

Handles raw 2352 (Mode1/Mode2) and plain 2048 ISO files. CHD and ECM files
must be converted first (see chdman / unecm.py).

Usage:  python disc_probe.py <image> [<image> ...]
"""

import sys

SECTOR_2352 = 2352
SECTOR_2048 = 2048


def _sync_ok(buf, off):
    return buf[off:off + 12] == b"\x00\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\x00"


def detect_sector_size(path, size):
    """Return (sector_size, data_offset) or (None, None) if undetectable."""
    candidates = []
    if size % SECTOR_2352 == 0:
        candidates.append((SECTOR_2352, None))
    if size % SECTOR_2048 == 0:
        candidates.append((SECTOR_2048, 0))

    with open(path, "rb") as f:
        for sector, forced_off in candidates:
            f.seek(16 * sector)
            raw = f.read(sector)
            if len(raw) < sector:
                continue
            if forced_off is not None:
                off = forced_off
            else:
                # Mode2 Form1: user data at +24; Mode1: at +16.
                if _sync_ok(raw, 0) and raw[15] == 2:
                    off = 24
                else:
                    off = 16
            if raw[off + 1:off + 6] == b"CD001":
                return sector, off
    return None, None


def read_sector(f, sector_size, data_off, lba):
    f.seek(lba * sector_size)
    raw = f.read(sector_size)
    if len(raw) < sector_size:
        return b""
    return raw[data_off:data_off + 2048]


def parse_pvd(f, sector_size, data_off):
    pvd = read_sector(f, sector_size, data_off, 16)
    if len(pvd) < 190 or pvd[0] != 1 or pvd[1:6] != b"CD001":
        return None
    label = pvd[40:72].decode("ascii", "replace").strip()
    root = pvd[156:190]
    return {"label": label, "root_lba": int.from_bytes(root[2:6], "little"),
            "root_size": int.from_bytes(root[10:14], "little")}


def walk_root(f, sector_size, data_off, pvd, want_name):
    """Find a file record by name in the root directory (one level deep)."""
    data = b""
    lba, remaining = pvd["root_lba"], pvd["root_size"]
    while remaining > 0:
        chunk = read_sector(f, sector_size, data_off, lba)
        if not chunk:
            break
        data += chunk
        remaining -= len(chunk)
        lba += 1

    pos = 0
    while pos < len(data):
        rec_len = data[pos]
        if rec_len == 0:
            pos = ((pos // 2048) + 1) * 2048  # advance to next sector
            continue
        rec = data[pos:pos + rec_len]
        name_len = rec[32]
        name = rec[33:33 + name_len].decode("ascii", "replace")
        if name_len == 1 and rec[33] == 0:
            name = "."
        elif name_len == 1 and rec[33] == 1:
            name = ".."
        else:
            name = name.split(";")[0]
        if name.upper() == want_name.upper():
            return {"lba": int.from_bytes(rec[2:6], "little"),
                    "size": int.from_bytes(rec[10:14], "little")}
        pos += rec_len
    return None


def read_file(f, sector_size, data_off, rec):
    data = b""
    lba, remaining = rec["lba"], rec["size"]
    while remaining > 0:
        chunk = read_sector(f, sector_size, data_off, lba)
        if not chunk:
            break
        data += chunk
        remaining -= len(chunk)
        lba += 1
    return data[:rec["size"]]


def xa_census(path, size, sector_size, data_off):
    """Count Mode2 Form2 audio sectors (XA voices/music) and Form1 sectors."""
    if sector_size != SECTOR_2352:
        return None
    audio = 0
    form2 = 0
    total = size // SECTOR_2352
    CHUNK = 2352 * 4096
    with open(path, "rb") as f:
        remaining = total
        while remaining > 0:
            n = min(remaining, CHUNK // 2352)
            buf = f.read(n * 2352)
            if len(buf) < n * 2352:
                break
            for i in range(n):
                s = buf[i * 2352:(i + 1) * 2352]
                if len(s) < 2352 or s[15] != 2:
                    continue
                submode = s[18]
                if submode & 0x20:  # Form2
                    form2 += 1
                if submode & 0x04:  # audio
                    audio += 1
            remaining -= n
    return {"total_sectors": total, "form2_sectors": form2, "audio_sectors": audio}


def probe(path):
    print(f"\n=== {path}")
    try:
        size = __import__("os").path.getsize(path)
    except OSError as exc:
        print(f"  ERROR: {exc}")
        return

    print(f"  size: {size} bytes")
    with open(path, "rb") as f:
        head = f.read(16)
    if head[:8] == b"MComprHD":
        print("  format: CHD (compressed) — convert with chdman first")
        return
    if path.lower().endswith(".ecm"):
        print("  format: ECM (compressed) — convert with tools/unecm.py first")
        return

    sector_size, data_off = detect_sector_size(path, size)
    if sector_size is None:
        print(f"  format: unknown (size % 2352 = {size % 2352}, % 2048 = {size % 2048})")
        return

    print(f"  format: raw {sector_size} bytes/sector, user data at +{data_off} "
          f"({'Mode2' if data_off == 24 else 'Mode1' if data_off == 16 else 'plain ISO'})")
    print(f"  sectors: {size // sector_size}")

    with open(path, "rb") as f:
        pvd = parse_pvd(f, sector_size, data_off)
        if pvd is None:
            print("  PVD: NOT FOUND / invalid (not a PSX data disc?)")
            return
        print(f"  label: {pvd['label']!r}")

        cnf = walk_root(f, sector_size, data_off, pvd, "SYSTEM.CNF")
        if cnf:
            text = read_file(f, sector_size, data_off, cnf).decode("ascii", "replace")
            for line in text.splitlines():
                if "BOOT" in line.upper():
                    print(f"  {line.strip()}")
        else:
            print("  SYSTEM.CNF: not found")

        bigfile = walk_root(f, sector_size, data_off, pvd, "BIGFILE.BIG")
        print(f"  BIGFILE.BIG: {'present, ' + str(bigfile['size']) + ' bytes' if bigfile else 'MISSING'}")

    census = xa_census(path, size, sector_size, data_off)
    if census:
        print(f"  XA census: {census['audio_sectors']} audio sectors, "
              f"{census['form2_sectors']} Form2 sectors of {census['total_sectors']} total")
        print(f"  XA verdict: {'HAS XA AUDIO (voices/music possible)' if census['audio_sectors'] > 1000 else 'NO/negligible XA audio'}")


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    for path in sys.argv[1:]:
        probe(path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
