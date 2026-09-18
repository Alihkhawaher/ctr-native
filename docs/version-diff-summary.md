# NTSC-U → PAL (and NTSC-J) porting — difference database + work estimate

*Generated 2026-09-18 from `tools/version_diff.py` (workdir: `E:\Games\CrashCTR-Win\version_diff\`).*
Sources: US `SCUS_944.26` (complete dump), PAL `SCES_021.05` (Europe EDC), JP `SCPS_101.18`
(CHD-extracted). BIGFILE entry names from CTR-tools' per-version lists.

## 1. What the database contains

`version_diff.sqlite` + CSVs in the workdir:

| Table / CSV | Content |
|---|---|
| `discs` | per version: boot id, exe load/size, function count |
| `disc_files` (+ `files_missing_per_version.csv`) | every ISO9660 file per disc |
| `exe_functions` | ~1 000 disassembled functions per version with 4 hashes |
| `exe_matches` | function-level matching US→PAL / US→JP |
| `bigfile_entries` | all BIGFILE entries (named) per version |
| `bigfile_matches` | entry-level matching (identical / changed / only-in) |

Re-run: `python tools/version_diff.py <workdir> US=... PAL=... JP=... NAMES:US=...`

## 2. Executable diff (the code layer)

All three builds load at `0x80010000`; exe sizes 514 048 / 516 096 / 528 384 B.

| class | US↔PAL | USJP |
|---|---|---|
| byte-identical functions | 175 | 179 |
| address-shift-only (same code, moved refs) | 136 | 144 |
| **structurally changed** (real logic edits) | **23** | **336** |
| value-changed (immediates differ; index/address re-basing + constants) | ~683 | ~337 |
| unique to base / target | 62 / 64 | 85 / 102 |
| totals | 986 / 989 | 986 / 1006 |

Notable: **every one of the 23 structurally-changed PAL functions has the same
size as its US counterpart** — small in-place edits (an opcode/register/constant
forcing a different encoding), not rewrites. JP diverges an order of magnitude
more (336 structural), consistent with its separate font/text/audio overhaul.

## 3. Data layer (BIGFILE: 608 US, 723 PAL, 608 JP entries)

- **449/608 US entries are byte-identical in PAL** (content-hash matched, incl.
  moved indices) — the base game data is shared.
- **169 changed at the same index**: 131 same-size (content tweaks in levels,
  models, `shared.vrm`, overlays incl. `230_Threads_MainMenu.bin`), 21 bigger,
  17 smaller. Standouts: `title01_pal.tim` / `title01_jpn.tim` (region title
  screens), cutscene size shuffles (`last_oxide_intro`, `pass_tiny`).
- **29 PAL-only entries** — all localisation: per-language cutscene variants
  (`data_french/german/italian/spanish/dutch .vrm/.lev/.ptr` for
  `last_oxide_intro` / `oxide01` / `oxide02`) plus new credits dances
  (`crashdance`, `tinydance`, `ngindance`, `polardance`).
- JP: only 161 identical / 449 changed — the most divergent data set.

## 4. Disc-level & audio

- PAL ships **six XA voice sets** (`ENG`, `FRN`, `GRM`, `ITL`, `SPN`, `DCH`),
  each with its own manifest (`XA/*.XNF`); US/JP ship one (`ENG` / `JPN`).
- The community XA list notes for PAL: *"reorders music, cuts 2 tropy lines,
  cuts unused UI lines, cuts unused 'finished Nth' lines, cuts voiced roo lines"*.
- Demo payloads differ per version (SPYRO2 demo in US/PAL, different demos on JP).
- The port's XA system is manifest-driven, so per-language audio selection is a
  data + small code problem, not a rewrite.

## 5. Why this is tractable

- The port's **asset layer already resolves files by name** (the PAL disc
  mounts, XA parses/streams — the crash came from game logic reading data that
  shifted, not from the loader).
- **Region dispatch already exists** (`native_region.c`): the piece that would
  select a PAL build variant is live; it currently refuses foreign discs.
- The genuinely different GAME CODE is small: 23 structural edits + a bounded
  set of unique/ value-diff functions to triage — not a second decompilation.

## 6. Work estimate to actually run PAL

| stage | work | estimate |
|---|---|---|
| 0. Pre-work (done) | diff DB + names, region dispatch, probe/unecm/chdman tooling, launcher | **done** |
| 1. Triage tooling | classify the ~683 value-diffs (address/index re-basing vs real constants) and map binary diffs to decompiled C functions (the decomp is non-matching — needs address/pattern mapping, partially automatable with this DB) | 1–2 weeks |
| 2. Code port | apply the 23 structural edits + unique functions + semantic constants as region conditionals in `game/` + `include/` (guarded per region), rebuild as a PAL variant | 2–4 weeks |
| 3. Language system | 6-language menus/strings, per-language cutscene + XA manifest switching, title screens, save-data language field | 3–5 weeks (largest) |
| 4. Timing | PAL 50 Hz pacing (or force-NTSC timing as a stopgap), frame-rate-independent logic checks | 1–2 weeks |
| 5. Integration & QA | boots/menus/races/cutscenes/saves per region, regression vs US build | 1–2 weeks |
| **total** | focused solo work, roughly 60 % automatable/mechanical | **≈ 8–14 weeks (2–3.5 months)** |

Risks: the decompiled-source ↔ binary mapping (stage 1) is the main tooling
risk; the language system (stage 3) is the main product risk; JP would add
several weeks on top (336 structural functions, separate font/text path).

## 7. Phased shortcut (if partial PAL is acceptable)

1. **Boot + menus + Time Trial on PAL data** — stages 1+2 partial → ~2–4 weeks.
2. **English-only full play** — + stage 4 → ~1–1.5 months.
3. **Full 6-language parity** — + stage 3 + QA → the full 2–3.5 months.