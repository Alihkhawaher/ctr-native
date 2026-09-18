# CTR-Native — Debug Log, 2026-09-17/18

Everything from the overnight session: the ~2-minute crash (root-caused + fixed),
the silent-voices investigation (root-caused: disc image), the tooling built, and
the operational quirks learned. Companion scripts are in `docs/scripts/`.

## 1. The ~2-minute crash — FIXED

**Symptom.** Every session died 2–3.5 minutes in, reliably, with an access
violation. Reproduced on every instrumented build at the exact same instruction
(`ctr_native.exe+0x3E930` = `LevInstDef_UnPack+0x30`). Never crashed during
normal play; always died around the intro/attract sequence.

**Trigger.** The intro/attract (CS script overlay 233) requests adventure-hub
levels; each request runs `LOAD_Hub_ReadFile` → mempack swap → `LevInstDef_UnPack`.

**Root cause.** `LevInstDef_UnPack` flips PVS list entries to their "peer"
(`visInstSrc[0] = LevInstDef_Peer(visInstSrc[0])` reads record+0x2C). One hub
contains a *self-sentinel* list — `[self-offset, NULL]` — the engine's "empty
list" marker (the renderer skips such lists via `[eax] == eax`). The unpacker
didn't recognize it: it read "peer" data at +0x2C, which landed on raw vertex
data (`0xA1E0A1EB` — literal disc bytes), and wrote that junk into the sentinel.
A second quadblock referencing the same shared list then dereferenced the junk.
On PSX neither step faults (no MMU; non-trapping divide); on Windows the junk
deref is an instant AV, and a follow-on renderer walk (`MainFrame_RenderFrame`)
divided by the list's zero terminator (PSX divides by zero silently; x86 raises
#DE).

**Fix** (`game/LevInstDef.c`, CTR_NATIVE only): preserve self-sentinels in both
`LevInstDef_UnPack` and `LevInstDef_RePack`:

```c
if (visInstSrc[0] == (void *)visInstSrc)
    continue;   // empty-list sentinel; toggling it corrupts it
```

**Verification.** 5+ minutes clean, multiple hub swaps, zero exceptions;
`ctr_match_unit` still passes. The intro now plays through (screenshots in
`docs/screenshots/`).

## 2. Silent character voices — root cause: the disc image

**Symptom.** Cutscene/menu XA voices silent (music fine). User report: in the
rare runs where voices worked, they worked for the whole session; most runs,
dead from the start.

**Chain (from instrumented logs):**
```
XAPlay req: cat=1 id=80 useDisc=0 volV=255 state=0
PlayXATrack: PrepareXAStream FAILED path=XA/ENG/EXTRA/S01.XA sectors=835
```
The file is found, opened, and its manifest entry parses. Stream-prep fails
because `NativeAudio_IsXAAudioSector` finds zero matching sectors.

**Image forensics** (`docs/scripts/subhdr_check.py`, `whole_scan.py`):
- `E:\Games\CrashCTR-Win\assets\ctr-u.bin` (606 MB, identical MD5 to
  `E:\Games\RomStation 1\Games\Playstation\643\Crash Team Racing.bin`):
  **all 257,675 sectors** carry the generic data sub-header `00 00 08 00` —
  zero XA audio sectors in the entire image. The XA files are ~94% zeros.
  This is a *converted/rebuilt* image (a tool re-chunked files and dropped the
  per-sector XA sub-headers + payloads), not a raw dump.
- `Crash Team Racing2.bin` (740 MB, PAL, `Crash Team Racing-PSX-PAL.bin`):
  **a proper raw dump** — XA sectors have real sub-headers
  (`file=1, channel=0..30, submode=0x64, coding=0x04`), dense real audio.
- The PAL manifest (`XA/ENG.XNF`) is *not* table-compatible with NTSC-U
  (14 XA files / 358 tracks vs 29 / 414; EXTRA 81 vs 87) — so cross-region
  asset override would play wrong lines. Not recommended.

**"Why sometimes it worked":** only runs with a proper image (or properly
extracted XA files) could ever play voices; the current `ctr-u.bin` cannot,
and no code fix changes that — the audio bytes aren't in it.

**Fix:** obtain a proper raw NTSC-U dump (`MODE2/2352`, per README) and use it
as `assets/ctr-u.bin`. Verify with `docs/scripts/whole_scan.py` (expect
`submode=0x64` audio sectors) and `xa_player.py` (should decode real audio).

**Voice player** (`docs/scripts/xa_player.py`): walks the ISO of any image,
finds an XA file, collects audio sectors by channel, decodes 4-bit XA ADPCM
(same math as `platform/native_audio.c`), writes a WAV. Proof run on the PAL
image: `XA/ENG/EXTRA/S00.XA`, 31 channels × 36 sectors, channel 0 → 7.7 s
clip, peak 31757 (`docs/voice_test_pal.wav`).

## 3. Tooling built this session (docs/scripts/)

- `disasm_crash.py` — capstone PE disassembly around an RVA (crash-site decode).
- `extract_hub8.py` — ISO9660 + bigfile reader for the disc image. **Note:** the
  bigfile entry table is `{offset, size}`, offsets are in sectors relative to
  `BIGFILE.BIG`'s own LBA (276). Sectors are raw 2352; user data at +24.
- `whole_scan.py` — sub-header statistics for the entire image (XA check).
- `subhdr_check.py` — raw sector layout dumps (sync/header/subheader/data).
- `xa_player.py` — the voice player (above).
- `xnf_check.py` / `pal_xnf.py` — XNF manifest parser (counts, per-track
  `{channelFilter, fileNumber, numSectors}`).
- `pal_check.py` — PAL image XA verification.

Debug workflow that worked:
1. `ctr_native.exe` logs `[CTR Debug]` lines + a crash filter that dumps the
   exception address and CPU registers (`main.c`).
2. `/MAP` linker flag → resolve `module+0xNNNNN` to a function
   (`build-msvc-x86/Release/ctr_native.map`).
3. Disassemble the exact faulting instruction with capstone (no debugger needed).
4. Instrument the suspect code path with tagged `Platform_Log*` lines, rebuild,
   reproduce, read the log. Temporary instrumentation is marked `[CTR Debug]`.

## 4. Operational notes (learned the hard way)

- **Build/deploy:** `cmake --preset windows-msvc-x86` +
  `cmake --build --preset windows-msvc-x86-release`. Deploying requires killing
  all `ctr_native.exe` instances first (the exe file locks; `cp` fails with
  "Device or resource busy"). `taskkill //F //IM` can mis-parse under MSYS —
  `powershell Stop-Process -Name ctr_native -Force` is reliable.
- **Config:** `ctr-native-config.json` next to the exe; the launcher
  (`ctr_config_launcher.py`) *loads it at startup* — but an already-open
  launcher window holds stale values and will overwrite newer settings on
  "Save & Play". Close/reopen the launcher after external config edits.
- **Log:** `Crash Team Racing.log` next to the exe, `"wb"` (truncates per
  session). When an old process is killed while a new one starts, the file can
  show mixed remnants — read by grep of specific markers, not by head/tail.
- **Console close:** closing the Windows Terminal hosting the console used to
  silently kill the game; `SetConsoleCtrlHandler` now detaches instead
  (`console closed; detached and still running`).
- **Background input:** `cua-driver call press_key '{"key":"pagedown","pid":N,"window_id":W}'`
  delivers to the game without focus. "effect: unverifiable" is normal; keys
  do land (confirmed via the game's own log).
- **Runtime keys:** PgUp/PgDn internal resolution (1/2/4/8x), F3 bilinear,
  Home aspect, End window size, F11 / Alt+Enter fullscreen, Insert FPS counter,
  **Tab anti-aliasing** (smooth/color-space-filtered presentation; NOTE: F4 and
  F6 never reach the option switch — the port's pre-existing "assign keyboard/
  gamepad to player" handlers consume them). The overlay panel lists every
  option with live values plus the full control reference (save/load state,
  screenshot, VRAM dump, replay, pad assignment, debug keys).
- **Anti-aliasing internals:** `g_cfg_antialiasing` sets the `smoothPresent`
  uniform; the present shader does an **edge-directed** blend: a luminance-
  contrast mask (smoothstep 0.06..0.22 over ±1-texel deltas) gates a manual
  bilinear that runs **in color space** (unpack packed VRAM bytes first, then
  blend). Flat areas, text and images stay pixel-sharp; only edges get
  smoothed. **2D exclusion:** the main render target carries a second R8
  attachment (COLOR_ATTACHMENT1, `aaMaskTexture`) that every PSX draw writes
  via a second fragment output (`aaMask`, 1.0 for SPRT/TILE 2D primitives via
  `drawIs2D`, 0.0 for POLY lines/polys). The present shader multiplies the
  edge mask by `(1 - mask)` so non-3D elements are never smoothed. NOTE:
  `glDisablei(GL_BLEND, 1)` keeps the mask attachment out of blend math.
  Never filter the raw packed bytes with GL_LINEAR (mixes bitfields → garbled
  colors), never blend unconditionally (whole screen out of focus), and
  **never bind the mask sampler to texture unit 1** — unit 1 is the game's
  color LUT; clobbering it makes every textured draw sample the mask (black
  screen). The mask uses unit 2.
- **Auto internal resolution:** `internal_resolution_scale: 0` in the config
  (launcher: "Auto (screen)") = match the window/display at 240 lines:
  `scale = clamp(round(windowHeight / 240), 1, 8)`, re-resolved on window
  resize and fullscreen changes (`NativeRenderer_ResolveAutoResolution`).
  PageUp/PageDown cycle 1→2→4→8→Auto. Overlay shows "Auto (Nx)".
- **2D stays crisp at any internal resolution:** the pack shader also samples
  the same 2D mask; when an output pixel's block is majority-2D it keeps the
  nearest sample instead of the supersample average. So SPRT/TILE content is
  pixel-crisp even at Auto/8x while 3D keeps the smooth SSAA. (The Tab AA is
  gated the same way in the present shader.)
- **Menus/cutscenes auto-excluded (`g_aaForceSharp`):** each frame
  `StoreFrameBuffer` reads the game's own mode:
  `GAME_TRACKER->gameMode1 & GAME_MODE_MENU_OR_CUTSCENE_MASK` (MAIN_MENU |
  GAME_CUTSCENE). When set, the present AA is disabled AND the pack forces the
  nearest sample for every pixel — menus, intro, title, cutscenes are pixel-
  crisp at any internal resolution; races keep the smoothed 3D look. The user's
  model: menus = "images" (crisp), racing = 3D (smoothed).
- **True-resolution presentation (emulator-class "enhanced resolution"):** the
  frame is displayed directly from the supersampled render target
  (`ctr_present_fb_shader` via `NativeRenderer_PresentRenderTarget`), not from
  the packed VRAM — so 3D both renders and displays at the full internal
  resolution (Auto 5x = 1600x1200 here). `StoreFrameBuffer` still packs into
  the VRAM every frame so the game's own VRAM reads/effects stay correct. The
  AA and the 2D mask apply unchanged (mask + target share orientation,
  `flipY = 0`). **Fallback for VRAM-direct frames (movies / decoded video):**
  `s_frameHadDraws` is set on any `AddSplit` and reset in
  `NativeRenderer_BeginScene`; the present draws the target only when
  `NativeGpu_FrameHadDraws() || !activeDrawEnv.isbg`, otherwise it presents the
  packed VRAM region (the original path) so zero-draw background-env frames
  (FMV playback) can never show a stale target.
- **GitHub checkpoint:** `https://github.com/Alihkhawaher/ctr-native` (private).
  `origin` = that repo; `upstream` = `CTR-tools/ctr-native` (kept for syncing).
  Commits: `f1f8edd1a` (crash fix, tooling, graphics options, icon) and
  `236830796` (true-resolution presentation). Launcher is tracked at
  `tools/ctr_config_launcher.py`.
- **The "sandy" look (solved):** the fine grain over gradients was the PSX's
  4x4 ordered dither carried at *PSX-pixel* period (`v_ditherCoord = a_position.xy`
  in `native_renderer.c`). Because one PSX pixel = one dither cell, supersampling
  could never average it away — the dots survived the upscale as "sand". Fix:
  a `ditherScale` uniform scales the dither grid to the *render-target* pixel
  ("scaled dithering", like modern PSX emulators) whenever internal resolution
  > 1, so the SSAA pack averages it into smooth gradients. At 1x behaviour is
  bit-identical to before (true PSX dither). The F4 anti-aliasing option adds
  optional linear smoothing on top for the presentation blit.
- **CLI flags:** `--verbose` (default: show `[CTR Debug]` lines) /
  `-q`, `--quiet` (silence them) / `-h`, `--help`. The debug channel is gated
  inside `Platform_Log*` by the `[CTR Debug]` prefix and is **permanent by user
  requirement — never strip it; add flags instead**.

## 5. Open items

- [ ] Voices: replace `assets/ctr-u.bin` with a proper raw NTSC-U dump
      (a PAL image cannot be substituted — XNF track tables differ).
- [ ] FMV fallback: verify visually on a story cutscene (Adventure mode). Logic
      is in place (`s_frameHadDraws` + `isbg` gate) but no movie frame has been
      captured yet on the new present path.
- [ ] Optional next-level "modern 3D" (researched, NOT implemented): PGXP-style
      perspective-correct texture mapping + subpixel vertex precision — the
      Beetle PSX / DuckStation approach. The port carries PSX fixed-point
      screen-space coords; would need W carried from the GTE into the shaders.
- [x] Debug instrumentation is **permanent** (user requirement: never remove
      debug/verbose). Control it with `--quiet` / `--verbose` CLI flags — the
      `[CTR Debug]` lines stay in the code.
- [x] Crash fix + tooling + graphics options + true-res present committed and
      pushed to the checkpoint repo (`f1f8edd1a`, `236830796`).
