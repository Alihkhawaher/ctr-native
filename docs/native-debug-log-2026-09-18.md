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
  gamepad to player" handlers consume them), **P PGXP** (perspective-correct 3D).
  The overlay panel lists every option with live values plus the full control
  reference (save/load state, screenshot, VRAM dump, replay, pad assignment,
  debug keys).
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
- **PGXP (perspective-correct 3D; `P` key / `pgxp` config / launcher checkbox):**
  `GTE_RotTransPers` (native_gte_core.c) publishes every transformed vertex's
  unclamped float screen x/y (from the full-precision MACs — no 16-bit
  truncation, no IR saturation) + view depth `mac3f/4096` via `Pgxp_PushVertex`
  into a 4096-slot hash cache keyed by the exact (SX2, SY2) the game copies
  into its primitives. `MakeVertexTriangle`/`MakeVertexQuad` (native_gpu.c) match
  polygon vertices back with `Pgxp_FillVertex` (2D excluded via `s_gpu.primIs2D`;
  misses store `(0,0,0)` = PSX-exact fallback). Data rides a second VBO
  (`s_glPgxpBuffer`, attribute `a_pgxp` vec3 float) uploaded with the GrVertex
  buffer in `NativeRenderer_UpdateVertexBuffer`; `TriangulateQuad` mirrors the
  vertex copies. Vertex shader (`pgxpMode`): `gl_Position = Projection *`
  `vec4(pos * w, 0.0, w)` — the position must be pre-multiplied by w, or the
  ortho translation gets divided by w as well and the scene explodes into
  streaks. `v_z` still comes from the non-PGXP `grOrtho` position so
  semi-transparency is unchanged. Pitfalls (all three measured with the
  `[CTR Debug] PGXP stats` counters): this port parses primitives at DRAW
  time, after the game's GTE work, so a per-frame cache clear in
  `NativeRenderer_BeginScene` wipes exactly the entries the lookups need
  (measured 2,052,762 pushes / 0 hits — the feature was a silent no-op);
  matching is now last-transform-wins with a generation stamp kept for
  diagnostics only (multiple DrawOTag calls per frame make strict frame
  windows unreliable; a stale match only mis-warps one vertex, a miss
  disables correction for that vertex). `OFX`/`OFY` are 16.16 fixed-point —
  use `(OFX >> 16)` (the raw value displaced every vertex by ~10M px: giant
  streak triangles). The float copy must ALSO clamp to the hardware
  saturation range `[-0x400, 0x3FF]` (Lm_G1/Lm_G2) — games rely on it, and
  unclamped off-screen vertices tear edge geometry. Do NOT use ±1px tolerance
  matching in this pool: the 320x240 coordinate space is dense, so vertices
  matched unrelated geometry (wrong depth → smeared textures); exact matching
  + a 16k-slot table gives ~96% hit rate (measured). Each `glVertexAttribPointer`
  must be captured while its own VBO is bound; the GTE hook must run AFTER
  `C2_SX2`/`C2_SY2` are stored.
  Runtime toggle like the bilinear filter (`u_pgxpModeLoc` set in
  `NativeRenderer_SetTexture`). After the fixes: 77–91% of 3D vertices matched
  (title/intro/attract render cleanly at Auto 5x, 30 fps locked); the effect
  is most visible in-motion on large angled surfaces (road/ground texture
  swim) — check at 1x internal resolution.
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
      → **DONE**: implemented as the `P` toggle (see section 4); keep an eye on
      Adventure-mode cutscenes + heavy traffic scenes for edge cases.
- [x] Debug instrumentation is **permanent** (user requirement: never remove
      debug/verbose). Control it with `--quiet` / `--verbose` CLI flags — the
      `[CTR Debug]` lines stay in the code.
- [x] Crash fix + tooling + graphics options + true-res present committed and
      pushed to the checkpoint repo (`f1f8edd1a`, `236830796`).

## 6. Subpixel geometry correction + debug tooling (2026-09-18, later session)

### Geometry correction (`G` key)
- New `pgxp_geometry` config key + `G` toggle + `PGXP geometry` overlay row +
  launcher checkbox. Default ON (matches emulator defaults; the reference port
  ships geometry correction, off-switch documented for troubled games).
- Shader (`GTE_PERSPECTIVE_CORRECTION`): when `pgxpGeoMode != 0` and the vertex
  matched (`a_pgxp.z > 0`), `grPos = clamp(a_pgxp.xy, a_position.xy - 0.5,
  a_position.xy + 0.5)` — subpixel precision limited to ±0.5 PSX px so adjacent
  corrected/uncorrected edges cannot open seams (validated approach: psxrecomp
  PR #148 "hairline seams eliminated by the 0.5px clamp"; DuckStation exposes
  the same as "PGXP Geometry Tolerance").
- **Uniform wiring trap (cost a white screen):** a new shader uniform needs
  FOUR touch points — (1) the `uniform int pgxpGeoMode;` declaration in the
  shader source, (2) the `GTEShader` struct field, (3) `glGetUniformLocation`,
  (4) the `glUniform` set. Missing (1) = vertex shader compile failure →
  the renderer's error path leaves the entire screen WHITE (looked like a
  catastrophic regression; `Failed to compile Vertex Shader` in the log).
- Verified scope: the geometry path acts only on MATCHED vertices. The `O`
  status view is the ground truth for which content qualifies (blue). Title
  screens and FMVs are ~0% matched (olive/yellow) → a geometry A/B there
  measures ≈0.0% and means nothing. Always A/B on blue-heavy content
  (races, attract worlds).
- Push-side diagnostic (temporary, since removed): real subpixel deviations
  run up to ~1.3 px (`dx/dy` in the push log); the ±0.5 clamp passes the
  under-half part through.

### Debug tooling (new this session)
- **F12 screenshots** now also write timestamped
  `screenshots/ctr_YYYYMMDD_HHMMSS.bmp` (plus the classic `SCREENSHOT.BMP`)
  and log the path (for automation). Captures are glReadPixels of the GL
  framebuffer — pure game pixels, no desktop.
- **`H` = debug freeze** (`g_dbg_emulatorPaused`). The pause loop in
  `native_libgpu.c` (DrawOTag) now pumps host events + re-presents every
  16 ms, so debug toggles (O/P/G/F1/F2) update LIVE while frozen. Verified:
  frozen pairs are pixel-static (0.02% diff), frozen O-toggle changes 75%.
- **`tools/pgxp_ab.py`** — A/B harness: drives the game via cua-driver keys,
  collects the game's own F12 screenshots, converts to PNG, writes diff
  reports with worst-tile crops + heat maps into `<game>/ab/`. Subcommands:
  `capture`, `ab <A> <B> --toggle <key>`, `diff`, `live`.
- **Cross-build savestates crash** (0xC0000005): `debug/states/quick.ctrstates`
  validates its own checksum, not the build ID. Regenerate states (F5) after
  every rebuild; do not load states from older builds.

### Measured (final)
- Live `P` toggle: 71% of pixels change (PGXP master clearly active).
- Frozen title/FMV geometry A/B: ≈0.0% — correct (those scenes are ~0% matched).
- Hit-rate instrumentation: `[CTR Debug] PGXP stats` line unchanged and
  permanent; the miss probes now show mostly 1–4 px deltas (near class).
- **PGXP status: EXPERIMENTAL.** User-verified: it still causes tearing in
  some scenes. Therefore **off by default** (`pgxp: false`). Everything else
  in this document is considered working.

## 7. Disc images & XA audio (full technical reference)

### Image/sector formats
- PSX discs are Mode 2 raw sectors: **2352 bytes/sector** = sync (12) + header
  (4) + **sub-header (8, at +16)** + user data (2048 for Form 1 / 2324 Form 2)
  + EDC/ECC. ISO user data starts at **+24**. A 2048-byte/sector ISO dump is a
  different container and cannot feed the XA stream directly.
- The XA sub-header layout: `{file, channel, submode, coding}` twice (the
  second copy must match). **Audio sectors have `submode & 0x04`**; real-time
  streamed audio uses `submode = 0x64` with `file = 1`. Voices are selected by
  (file, channel) pairs from the game's XNF manifest.
- **Three classes of dumps, and only one works:**
  1. **Clean raw dump (required):** real XA sectors with per-clip sub-headers.
     NTSC-U = SCUS-94426. This is what the port needs for voices.
  2. **Converted/rebuilt image (current `assets/ctr-u.bin`):** every one of the
     257,675 sectors carries the same data-type sub-header; **zero XA audio
     sectors**; XA payloads zeroed. The code path is fine — this image simply
     cannot play voices, ever.
  3. **Wrong region (PAL `Crash Team Racing2.bin`, SCES-021.05):** XNF track
     tables differ (PAL: 14 files / 358 tracks vs NTSC-U: 29 files / 414).
     Substituting PAL audio plays the wrong lines (or silence). Not a fix.
- **Check any image in seconds:** `docs/scripts/whole_scan.py` (sub-header
  histogram: audio vs data sectors), `docs/scripts/serial_check.py` (boot
  serial / region), `docs/scripts/xa_player.py` (decode one region to WAV —
  on the PAL image, channel 0 produces a real voice clip, proving the decoder).

### XNF manifest (voice track table)
- `XNFf` magic; counts at 0x0C/0x10; per-XA size array at 0x44; track entries
  `{channelFilter, fileNumber, numSectors}` (4 bytes each) at
  `0x44 + numXas*4`. The port's `PrepareXAStream` walks this table per cue.

### Voices verdict
- Engine audio (music/effects) works. **Cutscene voices do not**, because the
  shipped image has no XA audio sectors. Root cause is the image, not the
  code. Needs a clean raw NTSC-U SCUS-94426 dump; until then this item stays
  blocked. The PAL dump cannot substitute (different track tables).

## 8. Graphics stack (full technical reference)

### Frame flow
1. Game logic draws through the recompiled libgpu → GTE transforms
   (`native_gte_core.c`) → primitives are parsed at draw time
   (`ParsePrimitivesLinkedList`) → split batches (`DrawAllSplits`).
2. Draws land in a **supersampled render target** (window height ÷ 240, 1–8×;
   Auto = clamp(round(h/240), 1, 8)).
3. `StoreFrameBuffer` **packs** the target down into PSX VRAM (box filter over
   the scale×scale block, texel-center taps; 2D-majority blocks stay nearest)
   — the game itself reads VRAM for its own effects, so this must stay exact.
4. Presentation: `NativeRenderer_PresentRenderTarget` shows the **full-res
   render target directly** (emulator-class "enhanced resolution"), with a
   fallback to the packed VRAM for movie frames (`NativeGpu_FrameHadDraws()`
   + `isbg` = zero draws + background env ⇒ VRAM-direct FMV frame).

### Filters & options (keys)
- **PgUp/PgDn** internal resolution 1→2→3→4→8→Auto; **F3** bilinear;
  **Tab** anti-aliasing; **Home** aspect (Auto/4:3/16:9); **End** window size;
  **F11/Alt+Enter** fullscreen; **Insert** FPS counter.
- **Scaled dithering:** the PSX 4×4 ordered dither runs at *render-target*
  pixel period when internal res > 1 (`ditherScale` uniform) so the SSAA pack
  averages it into smooth gradients; at 1× it stays bit-exact PSX dither.
- **AA (Tab):** edge-directed, color-space (unpacks packed VRAM bytes before
  blending), gated by a luminance-contrast mask; **3D-only** via a second R8
  attachment (COLOR_ATTACHMENT1) written by every draw (`aaMask`, `drawIs2D`
  per split: 1.0 for SPRT/TILE, 0.0 for POLY). Flat areas/images stay sharp.
  Pitfalls: filtering raw packed bytes garbles colors; the mask sampler must
  NOT use texture unit 1 (the game's palette LUT lives there — unit 2).
- **Menus/cutscenes auto-crisp:** `g_aaForceSharp` set each frame from
  `GAME_TRACKER->gameMode1 & GAME_MODE_MENU_OR_CUTSCENE_MASK` — AA off and
  pack forced nearest for menu/cutscene frames (they are "images"); races keep
  SSAA/AA. Toggling AA on the legal screen = 0.0% pixel diff (2D excluded).
- **Overlay panel:** top-left, all options with live values + full key
  reference; shown at boot and on any option change.
- **F12 screenshot:** `screenshots/ctr_YYYYMMDD_HHMMSS.bmp` + classic
  `SCREENSHOT.BMP`; path logged. glReadPixels of the GL framebuffer.
- **H freeze-frame (debug):** pauses the sim in `DrawOTag`; the pause loop
  pumps host events and re-presents every 16 ms so O/P/G/F1/F2 update live —
  exact-frame A/B testing (frozen pairs are pixel-static, 0.02%).

### PGXP (EXPERIMENTAL — off by default)
- Architecture: GTE side channel publishes unclamped float screen x/y + view
  depth (`mac3f/4096`) per transformed vertex; a 64k-entry hash cache keyed by
  exact (SX2, SY2) stores them; at draw time `Pgxp_FillVertex` matches back
  (exact → near-match: nearest live vertex within 4px, ties refused) and the
  data rides a second VBO (`a_pgxp`, vec4: px, py, w, status). Triangle/quad
  correction is **all-or-nothing**; misses fall back to PSX-exact affine.
- Shader: `gl_Position = Projection * vec4(grPos * w, 0, w)` (pre-multiplied
  w); texture correction (perspective UVs) always on when PGXP is on;
  geometry correction (`G`) additionally sets
  `grPos = clamp(a_pgxp.xy, a_position.xy ± 0.5)` — the 0.5px clamp keeps
  seams from opening (psxrecomp PR#148 / DuckStation "geometry tolerance").
- **Status view (O):** blue=exact, orange=near, red=none, magenta=ambiguous,
  cyan=stale, yellow=discarded, green=2D. This is the ground truth for which
  content the correction affects: title screens and FMVs are ~0% matched
  (olive/yellow), races/attract worlds are largely blue.
- **Known problem:** tearing in some scenes (user-verified). Experimental —
  off by default; enable per-session with `P` (and `G` for geometry) to
  experiment.

## 9. Status ledger — works / experimental / blocked / tried-and-reverted

### Working (verified)
- Crash fix (self-sentinel PVS lists) — verified by play.
- Boot/legal screen (white legal text) — **fixed**: the per-frame VRAM pack is
  now gated on `NativeGpu_FrameHadDraws()`; zero-draw frames (boot/legal,
  FMV `LoadImage` frames) keep the game's own VRAM. Before the gate, the pack
  of a never-seeded (black) render target overwrote the displayed region and
  blacked those screens out. Diagnosed with `--dump-boot` (see tooling).
- Full graphics stack: SSAA 1–8× + Auto, true-resolution presentation, FMV
  fallback, scaled dithering, edge-directed 3D-only AA, menu/cutscene
  auto-crisp, bilinear, aspect, fullscreen, window size, FPS counter.
- Overlay panel, config persistence (game + launcher), CTR icon (exe + window
  + launcher), launcher UI (incl. Auto resolution), CLI verbosity flags.
- Debug tooling: F12 timestamped screenshots, H freeze-frame, O/P/G toggles,
  `[CTR Debug]` instrumentation (permanent), A/B harness `tools/pgxp_ab.py`,
  `--dump-boot` (capture frames 0–135 to `boot_dump/`, glReadPixels right
  before the swap — catches the first second external tools cannot),
  crash filter with register dump + map-based symbol resolution.
- 30 fps locked in normal play.

### Experimental (not working well)
- **PGXP** (P / `pgxp`): perspective-correct textures work in most scenes
  (~96% of 3D vertices match) but **tearing still occurs in some scenes** —
  user-verified. **Default OFF.**
- PGXP geometry (G / `pgxp_geometry`): subpixel positions, 0.5px-clamped.
  Subtle by design; acts only on matched (blue) content. Part of the same
  experimental feature — default is inert because the PGXP master is off.

### Blocked
- Cutscene voices: need a clean raw NTSC-U SCUS-94426 dump (current image has
  zero XA sectors; PAL cannot substitute). See §7.
- FMV fallback visual verification: same blocker (no movie frame reachable
  with a real cutscene on the stripped image — logic is in place and the
  attract's VRAM-direct frames render correctly).

### Tried and reverted (so nobody repeats them)
- Per-frame PGXP cache clear → silently zero hits (draws parse after the GTE
  work) → generation-stamped cache instead.
- Raw `C2_OFX/OFY` in the float path → ±10M-pixel offsets → `(OFX >> 16)`.
- ±1px tolerance matching → texture smearing (dense 320×240 neighborhood,
  whole-frame pool) → exact match + near-match (4px, nearest, tie-refused).
- Mixed corrected/affine vertices in one triangle → texture tearing →
  all-or-nothing per triangle/quad.
- `contested` flag as raw hash-collision detector → ~5% of all lookups
  refused → full-screen blue/yellow flicker → true ambiguity only (same
  pixel + same frame + different depth).
- Unclamped geometry correction → hairline seams between corrected and affine
  neighbors → 0.5px clamp (validated in psxrecomp #148).
- Message/position-changing PGXP (the "real" PGXP geometry) in a decompiled-C
  port without guest memory to shadow → abandoned in favor of the matched
  float side-channel (position matching has a ~93% ambiguity ceiling; this
  port measures better but the class is inherent — see psxrecomp notes).
- F5/F8 savestate spam during A/B → per-load jitter (baseline ≥8-diff ~10%
  per load) → freeze-frame instead.
- A/B comparisons on title screens / FMVs → ~0.0% by definition (0% matched
  content) → always A/B on blue-heavy scenes.
- New shader uniform with only 3 of 4 wiring points → vertex shader compile
  failure → entire screen white → always add: declaration, struct field,
  glGetUniformLocation, glUniform set.

### Defaults (final)
- `fullscreen: true`, `aspect_ratio: "4:3"`, `internal_resolution_scale: 0`
  (Auto), `pgxp: false` (experimental), `pgxp_geometry: true` (inert while
  PGXP is off), `show_fps: true`, bilinear/AA off. Launcher and engine share
  these defaults.
