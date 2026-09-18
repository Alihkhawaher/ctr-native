#!/usr/bin/env python3
"""
version_diff.py — multi-version difference database for Crash Team Racing discs.

Compares any number of raw PSX disc images (2352-byte sectors) at three levels:
  1. Disc file tree   — every ISO9660 file, sizes + hashes, present/missing per version
  2. Game executables — PS-X EXE header + function-level disassembly diff
                        (identical / constants-only / structural / unique)
  3. BIGFILE.BIG      — entry-level diff of the game's data archive

Outputs (in <workdir>): version_diff.sqlite, CSVs, summary.md.

Usage:  python version_diff.py <workdir> <label>=<image> [<label>=<image> ...]
Example: python version_diff.py out US=ctr-u.bin PAL=pal.bin JP=jp.bin
"""

import hashlib
import os
import re
import sqlite3
import struct
import sys

try:
    from capstone import Cs, CS_ARCH_MIPS, CS_MODE_MIPS32, CS_MODE_LITTLE_ENDIAN
except ImportError:
    Cs = None

SECTOR_2352 = 2352
SECTOR_2048 = 2048

# ---------------------------------------------------------------- disc access

class DiscImage:
    def __init__(self, path):
        self.path = path
        self.size = os.path.getsize(path)
        self.fh = open(path, "rb")
        self.sector, self.off = self._detect()
        if self.sector is None:
            raise ValueError(f"{path}: not a raw 2352/2048 disc image")
        self.root_lba, self.root_size = self._pvd()
        self.files = {}  # path (upper) -> (lba, size)
        self._walk(self.root_lba, self.root_size, "")

    def _detect(self):
        for s, o in ((SECTOR_2352, 24), (SECTOR_2352, 16), (SECTOR_2048, 0)):
            if self.size % s:
                continue
            self.fh.seek(16 * s)
            raw = self.fh.read(s)
            if len(raw) == s and raw[o + 1:o + 6] == b"CD001":
                return s, o
        return None, None

    def _pvd(self):
        pvd = self.read_sector(16)
        return (int.from_bytes(pvd[158:162], "little"),
                int.from_bytes(pvd[166:170], "little"))

    def read_sector(self, lba):
        self.fh.seek(lba * self.sector)
        raw = self.fh.read(self.sector)
        if len(raw) < self.sector:
            return b""
        return raw[self.off:self.off + 2048]

    def read_bytes(self, lba, size):
        """Read a contiguous data range; bulk-strided when large."""
        if size > 2 * 1024 * 1024 and self.sector == SECTOR_2352:
            n_sectors = (size + 2047) // 2048
            self.fh.seek(lba * SECTOR_2352)
            raw = self.fh.read(n_sectors * SECTOR_2352)
            out = bytearray()
            for i in range(n_sectors):
                base = i * SECTOR_2352 + self.off
                out += raw[base:base + 2048]
            return bytes(out[:size])
        out = bytearray()
        while len(out) < size:
            out += self.read_sector(lba)
            lba += 1
        return bytes(out[:size])

    def read_file(self, path):
        rec = self.files.get(path.upper())
        return self.read_bytes(rec[0], rec[1]) if rec else None

    def _walk(self, lba, size, prefix):
        data = self.read_bytes(lba, size)
        pos = 0
        while pos < len(data):
            rec_len = data[pos]
            if rec_len == 0:
                pos = ((pos // 2048) + 1) * 2048
                continue
            rec = data[pos:pos + rec_len]
            flags = rec[25]
            name_len = rec[32]
            name = rec[33:33 + name_len].decode("ascii", "replace")
            if name_len == 1 and rec[33] in (0, 1):
                pos += rec_len
                continue
            name = name.split(";")[0]
            child_lba = int.from_bytes(rec[2:6], "little")
            child_size = int.from_bytes(rec[10:14], "little")
            full = f"{prefix}/{name}" if prefix else name
            if flags & 0x02:
                self._walk(child_lba, child_size, full)
            else:
                self.files[full.upper()] = (child_lba, child_size)
            pos += rec_len

# ---------------------------------------------------------------- exe handling

def parse_psx_exe(data):
    if data[:8] != b"PS-X EXE":
        return None
    entry = int.from_bytes(data[0x10:0x14], "little")
    gp = int.from_bytes(data[0x14:0x18], "little")
    load = int.from_bytes(data[0x18:0x1C], "little")
    size = int.from_bytes(data[0x1C:0x20], "little")
    return {"entry": entry, "gp": gp, "load": load, "size": size,
            "code": data[0x800:0x800 + size]}


NUM_RE = re.compile(r"0x[0-9a-fA-F]+|\b\d+\b")
IMM_RE = re.compile(r"\$-?0x[0-9a-fA-F]+|\$-?\d+")


def shape_operands(op_str):
    s = NUM_RE.sub("#", op_str)
    return s


def shape_operands_smart(op_str):
    """Mask only address-like immediates (>= 0x8000); keep small literals."""
    def repl(m):
        s = m.group(0)
        try:
            v = int(s, 16) if s.lower().startswith("0x") else int(s)
        except ValueError:
            return "#"
        return "#" if v >= 0x8000 else s
    return re.sub(r"0x[0-9a-fA-F]+|\b\d+\b", repl, op_str)


def disassemble(code, base):
    md = Cs(CS_ARCH_MIPS, CS_MODE_MIPS32 + CS_MODE_LITTLE_ENDIAN)
    md.skipdata = True  # PSX exes mix data into text; keep decoding past it
    insns = []
    for insn in md.disasm(code, base):
        insns.append((insn.address, insn.mnemonic, shape_operands(insn.op_str),
                      shape_operands_smart(insn.op_str), insn.size))
    return insns


def analyze_exe(exe, label):
    """Split the text into functions (entry + jal targets) and hash each."""
    code, base = exe["code"], exe["load"]
    insns = disassemble(code, base)

    starts = set()
    if base <= exe["entry"] < base + len(code):
        starts.add(exe["entry"])

    # Collect jal call targets (shape_operands masks them, so scan raw again).
    md = Cs(CS_ARCH_MIPS, CS_MODE_MIPS32 + CS_MODE_LITTLE_ENDIAN)
    md.detail = True
    md.skipdata = True
    for insn in md.disasm(code, base):
        if insn.mnemonic == "jal" and insn.operands:
            tgt = insn.operands[0].imm
            if base <= tgt < base + len(code):
                starts.add(tgt)

    ordered = sorted(starts)
    funcs = []
    for i, start in enumerate(ordered):
        end = ordered[i + 1] if i + 1 < len(ordered) else base + len(code)
        chunk = code[start - base:end - base]
        window = [(a, m, o, sm) for (a, m, o, sm, s) in insns if start <= a < end]
        strict = hashlib.sha1(chunk).hexdigest()[:16]
        smart = hashlib.sha1("|".join(f"{m} {sm}" for (a, m, o, sm) in window).encode()).hexdigest()[:16]
        loose = hashlib.sha1("|".join(f"{m} {o}" for (a, m, o, sm) in window).encode()).hexdigest()[:16]
        mnemo = hashlib.sha1("|".join(m for (a, m, o, sm) in window).encode()).hexdigest()[:16]
        funcs.append({"start": start, "size": end - start, "strict": strict, "smart": smart,
                      "loose": loose, "mnem": mnemo, "insns": len(window)})
    return funcs


def match_functions(funcs_a, funcs_b):
    """Match A→B by strict, then loose, then mnemonic-only hash. Returns rows."""
    def index(funcs, key):
        d = {}
        for f in funcs:
            d.setdefault(f[key], []).append(f)
        return d

    rows = []
    remaining_a = list(funcs_a)
    remaining_b = list(funcs_b)

    for kind, key in (("identical", "strict"), ("address-shift-only", "smart"),
                      ("constants-only", "loose"), ("structure-only", "mnem")):
        ia, ib = index(remaining_a, key), index(remaining_b, key)
        used_a, used_b = set(), set()
        for h in set(ia) & set(ib):
            for fa in ia[h]:
                for fb in ib[h]:
                    rows.append((kind, fa["start"], fa["size"], fb["start"], fb["size"]))
                    used_a.add(id(fa))
                    used_b.add(id(fb))
        remaining_a = [f for f in remaining_a if id(f) not in used_a]
        remaining_b = [f for f in remaining_b if id(f) not in used_b]
    for f in remaining_a:
        rows.append(("unique-in-base", f["start"], f["size"], None, None))
    for f in remaining_b:
        rows.append(("unique-in-target", None, None, f["start"], f["size"]))
    return rows

# ------------------------------------------------------------- BIGFILE parsing

BIG_HEADER = struct.Struct("<ii")   # cdpos (int32), numEntry (int32)
BIG_ENTRY = struct.Struct("<ii")    # offset (int32, sectors), size (int32, bytes)


def parse_bigfile(data):
    """Return (cdpos, entries); entry data lives at offset*2048 within the file."""
    if len(data) < BIG_HEADER.size:
        return None, []
    cdpos, num = BIG_HEADER.unpack_from(data, 0)
    if num <= 0 or num > 65536:
        return cdpos, []
    entries = []
    for i in range(num):
        off, size = BIG_ENTRY.unpack_from(data, BIG_HEADER.size + i * BIG_ENTRY.size)
        entries.append({"index": i, "offset": off, "size": size})
    return cdpos, entries


def big_entry_sha1(data, entry):
    base = entry["offset"] * 2048
    size = entry["size"]
    if size <= 0 or base + size > len(data):
        return ""
    return hashlib.sha1(data[base:base + size]).hexdigest()[:16]

# ---------------------------------------------------------------- main

def sha1_file_small(disc, rec, limit=2 * 1024 * 1024):
    if rec[1] > limit:
        return ""
    return hashlib.sha1(disc.read_bytes(rec[0], rec[1])).hexdigest()[:16]


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    workdir = sys.argv[1]
    pairs = []
    names_files = {}
    for arg in sys.argv[2:]:
        if arg.startswith("NAMES:"):
            label, _, path = arg[6:].partition("=")
            names_files[label] = path
            continue
        label, _, path = arg.partition("=")
        pairs.append((label, path))
    os.makedirs(workdir, exist_ok=True)

    db = sqlite3.connect(os.path.join(workdir, "version_diff.sqlite"))
    cur = db.cursor()
    cur.executescript("""
        DROP TABLE IF EXISTS discs;
        DROP TABLE IF EXISTS disc_files;
        DROP TABLE IF EXISTS exe_functions;
        DROP TABLE IF EXISTS exe_matches;
        DROP TABLE IF EXISTS bigfile_entries;
        DROP TABLE IF EXISTS bigfile_matches;
        CREATE TABLE bigfile_entries (label TEXT, idx INTEGER, offset INTEGER, size INTEGER, sha1 TEXT, name TEXT);
        CREATE TABLE bigfile_matches (base TEXT, target TEXT, kind TEXT,
                                      a_idx INTEGER, a_size INTEGER, b_idx INTEGER, b_size INTEGER);
        CREATE TABLE discs (label TEXT PRIMARY KEY, path TEXT, size INTEGER,
                            sector INTEGER, boot_id TEXT, exe_entry INTEGER,
                            exe_load INTEGER, exe_size INTEGER, func_count INTEGER);
        CREATE TABLE disc_files (label TEXT, path TEXT, size INTEGER, sha1 TEXT);
        CREATE TABLE exe_functions (label TEXT, start INTEGER, size INTEGER,
                                    strict TEXT, smart TEXT, loose TEXT, mnem TEXT, insns INTEGER);
        CREATE TABLE exe_matches (base TEXT, target TEXT, kind TEXT,
                                  a_start INTEGER, a_size INTEGER, b_start INTEGER, b_size INTEGER);
    """)

    discs = {}
    for label, path in pairs:
        print(f"[{label}] reading {path}")
        disc = DiscImage(path)
        cnf = disc.read_file("SYSTEM.CNF")
        boot_id = ""
        if cnf:
            text = cnf.decode("ascii", "replace")
            for line in text.splitlines():
                if "BOOT" in line.upper() and "cdrom:" in line:
                    boot_id = line.split("cdrom:")[1].strip().lstrip("\\/ ").split(";")[0].strip()
        exe = None
        if boot_id:
            raw = disc.read_file(boot_id)
            if raw:
                exe = parse_psx_exe(raw)
        funcs = analyze_exe(exe, label) if (exe and Cs) else []
        print(f"[{label}] boot={boot_id} files={len(disc.files)} funcs={len(funcs)}")

        cur.execute("INSERT INTO discs VALUES (?,?,?,?,?,?,?,?,?)",
                    (label, path, disc.size, disc.sector, boot_id,
                     exe["entry"] if exe else None, exe["load"] if exe else None,
                     exe["size"] if exe else None, len(funcs)))
        for fpath, rec in disc.files.items():
            cur.execute("INSERT INTO disc_files VALUES (?,?,?,?)",
                        (label, fpath, rec[1], sha1_file_small(disc, rec)))
        for f in funcs:
            cur.execute("INSERT INTO exe_functions VALUES (?,?,?,?,?,?,?,?)",
                        (label, f["start"], f["size"], f["strict"], f["smart"], f["loose"], f["mnem"], f["insns"]))
        big_entries = []
        names = []
        if label in names_files:
            with open(names_files[label], "r", encoding="utf-8", errors="replace") as nf:
                names = [ln.strip() for ln in nf if ln.strip() and not ln.strip().startswith("#")]
        if "BIGFILE.BIG" in disc.files:
            big_data = disc.read_file("BIGFILE.BIG")
            cdpos, big_entries = parse_bigfile(big_data)
            for e in big_entries:
                e["sha1"] = big_entry_sha1(big_data, e)
                e["name"] = names[e["index"]] if e["index"] < len(names) else ""
                cur.execute("INSERT INTO bigfile_entries VALUES (?,?,?,?,?,?)",
                            (label, e["index"], e["offset"], e["size"], e["sha1"], e["name"]))
            print(f"[{label}] BIGFILE entries={len(big_entries)} cdpos={cdpos} names={len(names)}")
        discs[label] = {"disc": disc, "exe": exe, "funcs": funcs, "big": big_entries}

    # pair-wise function matching (first label = base)
    labels = [p[0] for p in pairs]
    base_label = labels[0]
    for other in labels[1:]:
        rows = match_functions(discs[base_label]["funcs"], discs[other]["funcs"])
        for kind, a_s, a_sz, b_s, b_sz in rows:
            cur.execute("INSERT INTO exe_matches VALUES (?,?,?,?,?,?,?)",
                        (base_label, other, kind, a_s, a_sz, b_s, b_sz))
        kinds = {}
        for r in rows:
            kinds[r[0]] = kinds.get(r[0], 0) + 1
        print(f"[{base_label} vs {other}] " + ", ".join(f"{k}={v}" for k, v in sorted(kinds.items())))

        a_entries = discs[base_label]["big"]
        b_entries = discs[other]["big"]
        if a_entries and b_entries:
            a_by_hash, b_by_hash = {}, {}
            for e in a_entries:
                if e["sha1"]:
                    a_by_hash.setdefault(e["sha1"], []).append(e)
            for e in b_entries:
                if e["sha1"]:
                    b_by_hash.setdefault(e["sha1"], []).append(e)
            used_a, used_b = set(), set()
            for h in set(a_by_hash) & set(b_by_hash):
                for ea in a_by_hash[h]:
                    for eb in b_by_hash[h]:
                        cur.execute("INSERT INTO bigfile_matches VALUES (?,?,?,?,?,?,?)",
                                    (base_label, other, "identical", ea["index"], ea["size"], eb["index"], eb["size"]))
                        used_a.add(ea["index"])
                        used_b.add(eb["index"])
            maxn = max(len(a_entries), len(b_entries))
            for i in range(maxn):
                ea = a_entries[i] if i < len(a_entries) else None
                eb = b_entries[i] if i < len(b_entries) else None
                if (ea and ea["index"] in used_a) or (eb and eb["index"] in used_b):
                    continue
                if ea and eb:
                    same = (ea["size"] == eb["size"]) and (ea["sha1"] == eb["sha1"])
                    kind = "identical" if same else "changed-same-index"
                    cur.execute("INSERT INTO bigfile_matches VALUES (?,?,?,?,?,?,?)",
                                (base_label, other, kind, ea["index"], ea["size"], eb["index"], eb["size"]))
                elif ea:
                    cur.execute("INSERT INTO bigfile_matches VALUES (?,?,?,?,?,?,?)",
                                (base_label, other, "only-in-base", ea["index"], ea["size"], None, None))
                else:
                    cur.execute("INSERT INTO bigfile_matches VALUES (?,?,?,?,?,?,?)",
                                (base_label, other, "only-in-target", None, None, eb["index"], eb["size"]))
            bkinds = {}
            for (k,) in cur.execute("SELECT kind FROM bigfile_matches WHERE base=? AND target=?", (base_label, other)):
                bkinds[k] = bkinds.get(k, 0) + 1
            print(f"[{base_label} vs {other}] BIGFILE: " + ", ".join(f"{k}={v}" for k, v in sorted(bkinds.items())))

    db.commit()

    # CSVs
    def dump_csv(name, query):
        rows = cur.execute(query).fetchall()
        cols = [d[0] for d in cur.description]
        with open(os.path.join(workdir, name), "w", encoding="utf-8", newline="") as fh:
            fh.write(",".join(cols) + "\n")
            for r in rows:
                fh.write(",".join("" if v is None else str(v).replace(",", ";") for v in r) + "\n")
        return rows

    dump_csv("disc_files.csv", "SELECT * FROM disc_files ORDER BY path, label")
    dump_csv("exe_functions.csv", "SELECT * FROM exe_functions ORDER BY label, start")
    dump_csv("exe_matches.csv", "SELECT * FROM exe_matches ORDER BY base, target, kind, a_start")
    dump_csv("bigfile_entries.csv", "SELECT * FROM bigfile_entries ORDER BY label, idx")
    dump_csv("bigfile_matches.csv", "SELECT * FROM bigfile_matches ORDER BY base, target, kind, a_idx")
    dump_csv("discs.csv", "SELECT * FROM discs")
    dump_csv("files_missing_per_version.csv",
             f"SELECT path, GROUP_CONCAT(label) AS present_in FROM disc_files GROUP BY path "
             f"HAVING COUNT(DISTINCT label) < {len(labels)} ORDER BY path")

    # summary
    with open(os.path.join(workdir, "summary.md"), "w", encoding="utf-8") as fh:
        fh.write("# Version difference database\n\n")
        for label in labels:
            d = discs[label]
            fh.write(f"## {label}\n- image: `{d['disc'].path}`\n- boot: `{d['disc'].files and ''}`")
            fh.write(f"{cur.execute('SELECT boot_id FROM discs WHERE label=?', (label,)).fetchone()[0]}\n")
            if d["exe"]:
                fh.write(f"- exe: entry=0x{d['exe']['entry']:08x} load=0x{d['exe']['load']:08x} "
                         f"size={d['exe']['size']} funcs={len(d['funcs'])}\n")
            fh.write(f"- disc files: {len(d['disc'].files)}\n\n")
        for other in labels[1:]:
            fh.write(f"## {base_label} vs {other} (function-level)\n\n")
            rows = cur.execute("SELECT kind, COUNT(*) FROM exe_matches WHERE base=? AND target=? GROUP BY kind",
                               (base_label, other)).fetchall()
            total = sum(c for _, c in rows)
            for kind, c in rows:
                fh.write(f"- {kind}: {c} ({100 * c / max(total, 1):.1f}%)\n")
            bcounts = cur.execute("SELECT kind, COUNT(*) FROM bigfile_matches WHERE base=? AND target=? GROUP BY kind",
                                  (base_label, other)).fetchall()
            if bcounts:
                fh.write(f"\n### BIGFILE entries ({base_label} vs {other})\n\n")
                btotal = sum(c for _, c in bcounts)
                for kind, c in bcounts:
                    fh.write(f"- {kind}: {c} ({100 * c / max(btotal, 1):.1f}%)\n")
            fh.write("\n")
        fh.write("## Files missing from some versions\n\n")
        for path, present in cur.execute(f"SELECT path, GROUP_CONCAT(label) FROM disc_files GROUP BY path "
                                         f"HAVING COUNT(DISTINCT label) < {len(labels)} ORDER BY path"):
            fh.write(f"- `{path}` (present in: {present})\n")
    print(f"done -> {workdir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
