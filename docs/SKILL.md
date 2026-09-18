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

## Hard-won pitfalls (do not repeat)
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

## PGXP — EXPERIMENTAL, off by default
Perspective-correct textures + subpixel geometry (0.5px clamp). User-verified
tearing in some scenes; labeled experimental everywhere; `pgxp: false` is the
default. Enable per-session with P (+G). Status view (O) legend: blue=exact,
orange=near, red=none, magenta=ambiguous, yellow=discarded, cyan=stale,
green=2D.

## Defaults (final)
`fullscreen: true`, `aspect_ratio: "4:3"`, `internal_resolution_scale: 0`
(Auto), `pgxp: false`, `pgxp_geometry: true` (inert while PGXP off),
`show_fps: true`; bilinear/AA off. Engine + launcher defaults aligned.

## User preferences
- NEVER remove debug/verbose logging — permanent. Add CLI flags instead.
- Keep gameplay screenshots for the repo (demo material).
- Prefers Python tooling over GUI automation; project learnings live BOTH in
  the repo (`docs/`) and as a Hermes skill.
- The user tests live while working — capture via background grabs/F12, never
  steal focus.