# CTR-Native — project skill / playbook

Self-contained working guide for this repo (mirrors the Hermes `ctr-native`
skill). The long-form technical reference is `docs/native-debug-log-2026-09-18.md`
(§7 disc images & XA audio, §8 graphics stack, §9 works/experimental/blocked/
tried-and-reverted ledger).

## Paths
- Repo: `E:\Games\ctr-native` — unity build (`main.c` includes all
  `platform/native_*.c`). MSVC x86 via CMake presets.
- Deployed game: `E:\Games\CrashCTR-Win` — exe, `ctr-native-config.json`,
  launcher `ctr_config_launcher.py`, log `Crash Team Racing.log`,
  `screenshots/` (F12), `boot_dump/` (--dump-boot), `ab/` (A/B harness),
  `debug/states/quick.ctrstates` (F5/F8).
- GitHub: `origin` = private checkpoint (Alihkhawaher/ctr-native);
  `upstream` = CTR-tools/ctr-native; PR fork = Alihkhawaher/ctr-native-pr
  (PR #57 open). Push with `git push origin master` + the fork URL.

## Build & deploy
- Configure once: `cmake --preset windows-msvc-x86`; build:
  `cmake --build --preset windows-msvc-x86-release`; tests:
  `ctest --preset windows-msvc-x86-release` (expect 2/2).
- Deploy needs ALL `ctr_native.exe` instances dead (file locks):
  `powershell Stop-Process -Name ctr_native -Force`, then copy
  `build-msvc-x86/Release/ctr_native.exe` to the game dir and relaunch with
  `Start-Process`. The user plays in the background — never steal focus.

## Runtime keys
- F7 = diagnostic VRAM dump pack into `vram_dump/`: raw 1024x512 .bin (exact
  PSX VRAM, LE u16, row0=top) + decoded .bmp preview + frame .bmp, timestamped,
  taken at the moment of the press (full VRAM sync first). Capture issues at
  the spot (debug log §18).
PgUp/PgDn internal resolution 1/2/3/4/8/Auto · F3 bilinear · Tab anti-aliasing
· P PGXP master (EXPERIMENTAL) · O PGXP status view · G PGXP geometry
(experimental) · H debug freeze · F12 screenshot · F5/F8 save/load state ·
F1/F2 wireframe/texless · F7 VRAM dump · F9/F10 replay · Home aspect · End
window size · F11/Alt+Enter fullscreen · Insert FPS. F4/F6 are consumed by the
keyboard/gamepad-assign handlers — never bind options to them. The overlay
panel (top-left) lists everything with live values.

## Debug tooling
- `[CTR Debug]` instrumentation is PERMANENT (user requirement — never remove;
  use `--verbose`/`-q`/`-h` CLI flags, gated inside `Platform_Log*`).
- Crash filter: exception + registers to the log;
  `docs/scripts/disasm_crash.py` resolves `module+0xRVA` against
  `ctr-native.map` and disassembles with capstone.
- F12 = timestamped `screenshots/ctr_*.bmp` (path logged).
- `--dump-boot` = frames 0..135 to `boot_dump/` (glReadPixels right before the
  swap; BMPs come out vertically flipped — flip when viewing).
- H freeze-frame = exact-frame A/Bs (pause loop pumps events + re-presents, so
  toggles update live; frozen pairs are pixel-static).
- `tools/pgxp_ab.py` = A/B harness (cua-driver keys + game captures + diff
  reports with worst-tile crops/heat maps into `<game>/ab/`).
- Everything drives the game in background via cua-driver:
  `cua-driver call press_key '{"key":"p","pid":N,"window_id":W}'`.

## Disc images & XA audio (voices)
- Raw Mode 2 sectors: 2352 B; sub-header at +16, user data at +24.
- **Voices work with a complete NTSC-U dump** (SCUS-94426, 53,428 XA audio
  sectors): `PlayXATrack OK` for EXTRA/MUSIC tracks. The old `assets/ctr-u.bin`
  is audio-gutted (0 XA sectors) — boot only, no voices. PAL images
  (SCES-02105, 115k XA sectors) MOUNT but the NTSC-U build segfaults on PAL
  data; JP (SCPS-10118) runs but XA lookups fail (manifest ids differ).
- **Check any image with `tools/disc_probe.py`** — prints sector format,
  region/BOOT id, BIGFILE presence, and the XA-audio census (the voice test).
- **`disc_image` config key** (`game_data` section; launcher "Game data" row):
  absolute or game-folder-relative path; engine logs `Disc image (config):`.
  Config loads BEFORE `NativeAssets_Init` (asset validation must see it).
- Conversions: **`tools/unecm.py`** (ECM → raw 2352; type 2/3 records carry
  only the 2336-byte Mode2 body — writing 2352 desyncs by 16 B/sector).
  **chdman** (MAME): `extractcd -i x.chd -ob x.bin -o x.cue` — in 0.289
  `-o` is the TOC and `-ob` the data (reversed vs older docs; same-file use
  fails with "Permission denied"). CHDs are not engine-readable directly.
- XNF: `XNFf` magic, counts 0x0C/0x10, sizes 0x44, entries
  `{channelFilter, fileNumber, numSectors}` at `0x44 + numXAs*4`.
- PR #27 in the upstream repo has a useful `extract_assets.sh` (bin/cue + iso,
  extracts XA as raw 2352-byte sectors, validates).

## Region dispatch (NTSC-U-only build)
- The game code = the NTSC-U decompilation; foreign discs cannot run on it
  (their own PSX executables need an emulator; their data layouts differ).
- `platform/native_region.c` detects the disc's SYSTEM.CNF BOOT id and
  refuses PAL (SCES_/SLES_) / NTSC-J (SCPS_/SLPS_) discs with a clear message
  instead of a segfault; `--allow-foreign-disc` overrides (expect crashes).
- The launcher detects the picked image's region too (colored status; warns
  on Save & Play for foreign discs). Detector logic: PVD at sector 16, root
  walk for SYSTEM.CNF, parse `BOOT = cdrom:\<id>;1`.

## Version diff database (US vs PAL vs JP)
- `tools/version_diff.py <workdir> US=img PAL=img JP=img NAMES:US=...` →
  SQLite + CSVs: disc file trees, BIGFILE entries (named via CTR-tools lists
  in `version_diff/names/`: big_usa_release.txt = 608 entries, big_pal = 723;
  NON-COMMENT lines = entry order), PS-X EXE function diff (capstone — needs
  `md.skipdata=True` + `md.detail=True`; classes: identical /
  address-shift-only / structural / value-changed / unique).
- BIGFILE format: int32 cdpos + int32 numEntry, then numEntry × (offset in
  sectors, size bytes); entry data at `offset*2048`.
- PAL verdict: 175 identical functions, 23 structural (same size as US!),
  ~683 value-diffs; data = 449/608 identical, 169 changed, +29 localisation
  entries; six XA voice sets. Port estimate ≈ 8–14 weeks —
  `docs/version-diff-summary.md`. Full artifacts:
  `E:\Games\CrashCTR-Win\version_diff\`.

## Hard-won pitfalls (do not repeat)
- PGXP side-channel units: `w` must be SZ3-scale (`mac3f / 4096`) — `C2_H` is
  compared against SZ3, so floor it at `H/2`. Dividing by 4096 a second time
  silently disables the clamp (it never fires, no error anywhere).
- Rect/tile/sprite builders bypass `Pgxp_FillVertex` — their PGXP slots hold
  stale 3D data unless `MakeVertexRect` marks them 2D (status 5). Symptom:
  HUD sprites warp with PGXP on; O view shows blue/orange where green is due.
- This port draws from order tables built over multiple frames: PGXP cache
  entries legitimately live >1 frame. The freshness window is 16 epochs for a
  measured reason (4 → 1.7M stale refusals, 70% hit rate).
- PSX-harmless NULL writes: decompiled code that writes to `(NULL + offset)`
  was fine on PSX (low RAM is valid there) but is a hard crash on native.
  Audit loops that span ALL driver slots (e.g. `CLOCK_DRIVER_COUNT` = 8) —
  `numPlyrCurrGame`-bounded loops are safe. Fixed example:
  `game/Vehicle/VehPickupItem.c` clock weapon (`drivers[i]->clockFlash` wrote
  before the null check → crash with <8 drivers + a clock pickup).
- The native exe is x86-32 — for crash addresses, disassemble with
  `CS_ARCH_X86/CS_MODE_32`; MIPS disassembly applies only to the PSX disc
  images (tools/version_diff.py). Crash sentinel prints `module+offset`
  (RVA); map via the PE section table.
- Gamepad slots: `s_controllerToSlotMapping` must be WRITTEN on open (and
  cleared on close); SDL sends `GAMEPAD_ADDED` for pads already connected at
  startup during `SDL_Init` AND whenever `gamecontrollerdb.txt` mappings load
  (confirmed in SDL3 docs/examples), and without a device-already-open-in-any-
  slot guard one physical pad lands in TWO slots — a single controller then
  drives both players. Diagnose via the `gamepad in pad slot N` /
  `duplicate gamepad add ignored` log lines.
- Gamepad config (launcher "Gamepad" group, `input` section of the JSON):
  `pad_mode` (1 = **4 pads always on, even if disconnected** — default: the
  multitap bus is presented from startup AND empty slots report as connected
  idle pads, so the game always sees 4 controllers and pad drops/reconnects
  are invisible to it; 0 = auto, latch reality at boot; 2 = 2 pads),
  `keyboard_slot` (-2 = **"Pads only"** default — keyboard drives no player;
  -1 = Auto, moves aside for pads; 0-3 = fixed player, e.g. player 4 =
  keyboard; F4 assigns at runtime), `gamepad_deadzone` (percent 0-50, default
  5 — the old fixed 1.5% left Xbox stick drift active; also centers
  AxisToByte), `gamepad_analog` (new pads start analog), `gamepad_rumble`.
  The bus layout is FIXED for the session (per-frame multitap switching broke
  the game's boot-time pad detection when a flaky pad dropped). Pads attach
  into slots as they connect and STICKY-return to the same slot after a
  battery/cable drop (matched by `SDL_GetGamepadPathForID`, unique per
  physical device incl. Bluetooth address; stale handles are replaced).
  Log lines: `gamepad connected/reconnected to pad slot N`, `duplicate gamepad
  add ignored`, `pad slot N disconnected (device remembered for
  auto-reconnect)`.
- Launcher gotcha: tk grid rows must not collide — the status note and the
  button row both sat at row=3 and the note (drawn later) hid Save / Save &
  Play / Quit. When adding a group, renumber EVERYTHING below it.
- New shader uniform = FOUR wiring points: shader declaration, GTEShader
  struct, glGetUniformLocation, glUniform set. Missing declaration = vertex
  shader compile failure = entire screen WHITE.
- VRAM pack (`StoreFrameBuffer`) must only run when the frame drew
  (`if (NativeGpu_FrameHadDraws())`) — packing a never-seeded target over
  zero-draw frames (boot legal text, FMV LoadImage) blacks them out.
- PGXP matching: exact (x,y) + near-match ≤4px nearest, tie-refused; ±1px
  tolerance smears (reverted). All-or-nothing per triangle (mixed = tearing).
  `contested` = true ambiguity (same pixel+frame+different depth), never raw
  hash collision (flicker). Epoch-stamped cache, freshness window 16.
- A/B verify ONLY on matched (blue) content — title screens/FMVs are ~0%
  matched and a geometry A/B there measures nothing.
- F8 savestates have no build id — loading across builds crashes (0xC0000005);
  regenerate with F5 after rebuilds.
- Launcher caches old config while open — close/reopen after external edits.
- Log truncates per session and interleaves at kill — grep markers, not tails.
- XA camera/voice blockers are IMAGE problems, not code (see §7).

## PGXP — two modes, both working; run one at a time
Perspective-correct textures + subpixel geometry (window [-1.0, +2.5]px after the
2026-09-19 review fixes; was ±0.5). Both modes work; **never enable both simultaneously**
(that combination tears - user-verified); `pgxp: false` is the default in config, and
`pgxp_geometry` defaults off too. **Tearing rule (debug log §19):** P alone = perspective-correct
*textures* only (vertices stay on the integer grid - nothing moves,
cannot tear). G adds subpixel *vertex* positions, which seam wherever a
neighbouring triangle is not bound and the internal resolution magnifies it. Check the
geometry toggle FIRST on any tearing report. Enable per-session with P
(+G). Status view (O) legend: blue=exact, orange=near, red=none,
magenta=ambiguous, yellow=discarded, cyan=stale, green=2D — with PGXP on, **HUD
elements must be green** (blue/orange = the stale-slot 2D regression).

- **Near-match DISABLED** (`PGXP_NEAR_MATCH_ENABLE = 0`): on the main menu it
  snapped UI vertices onto unrelated live 3D vertices -> yellow shards + ghost
  panel (see debug log §15). Exact-only + the **screen-aligned quad guard**
  (`MakeVertexQuad` marks `x0==x3 && x1==x2 && y0==y1 && y2==y3` quads 2D -
  setXYWH-style UI never matches): hits 98.2%, true no-match ~0, contested 1.8%.
- **Memory cache IMPLEMENTED (address-keyed provenance, debug log §16):** the
  game's `CTR_GteStoreSXY*` helpers bind each `posScreen` field address to the
  full-precision transform (freshest push within a tight window); prim writers
  bind via exact address chain (`DrawLevelOvr1P_PackProjectedSxy` sticky ->
  `CtrGpu_WritePackedXY`) or tight window (RenderBucket writers). The draw
  resolves by address first: positive = exact; **negative = affine AND the
  coordinate fallback is blocked** (proven non-transform never false-matches);
  no entry = fallback. Measured: 6.0M proven-exact vertices, ~57% of all
  resolved, menu/cinematic phases now address-bound; 0 crashes. Knobs:
  `PGXP_BIND_WINDOW` (64), `PGXP_SHADOW_SIZE`/`PGXP_TRANSFORM_SIZE` (32768).
- **External review channel ("Fable")**: `python
  C:/Developments/Tools/AI-MediaLens/openrouter_media.py <media> -p "<short
  prompt>" -m anthropic/claude-fable-5.1 --max-tokens 4000` - SHORT prompts
  only (expensive model); `--max-tokens` required (65k default reserves full
  credit -> HTTP 402 on low balances).
Review fixes landed (verified against code, results measured — see debug log §14):
- Side channel: `OFX/OFY` full 16.16 (no `>>16` truncation), `w` in SZ3 units,
  depth floored at `H/2` for on/behind-eye vertices (pushed, not dropped) —
  sampled self-check vs the register chain: 0 drift.
- Rect/tile/sprite paths mark their PGXP slots 2D (status 5) in
  `MakeVertexRect` — they bypass `Pgxp_FillVertex`, so stale 3D data used to
  warp the HUD. Lines are fine (they go through `MakeVertexQuad`).
- Contested = same-epoch + same-pixel + different-depth only (sticky clause
  removed — it over-refused).
- Cache is 4-way set associative (16384×4): true no-match 4.5% → 1.1%.
- `noperspective` on `v_color`/`v_ditherCoord` (PSX shading/dither are
  screen-linear).
- Freshness window stays 16 (~8 frames): measured — this port draws from order
  tables built over multiple frames; tightening to 4 restored 1.7M stale
  refusals at 70% hit rate. Do not "optimize" it without a hit-age histogram.

## Defaults (final)
`fullscreen: true`, `aspect_ratio: "4:3"`, `internal_resolution_scale: 0`
(Auto), `pgxp: false`, `pgxp_geometry: false` (inert while PGXP off; see §19 -
subpixel geometry is opt-in because partial bindings seam at high internal
res; texture-only PGXP cannot tear), `show_fps: true`; bilinear/AA off.
Engine + launcher defaults aligned.

## User preferences
- NEVER remove debug/verbose logging — permanent. Add CLI flags instead.
- Keep gameplay screenshots for the repo (demo material).
- Prefers Python tooling over GUI automation; project learnings live BOTH in
  the repo (`docs/`) and as a Hermes skill.
- The user tests live while working — capture via background grabs/F12, never
  steal focus.

## Pitfalls

- **Fullscreen toggle vs window size (debug log 20):** the persisted window size is the separate s_windowedWidth/Height preference, updated only while windowed; never store a fullscreen size as the window preference.
