# CTR-Native — Distortion ("blob") Investigation — State Handoff

> **RESOLVED 2026-10-01** — root cause: split-screen DecalMP impostor tiles (stale/partial in the native VRAM path). Fixed by drawing rivals as real 3D (`disable_mp_impostors`, default true). Full write-up: `docs/native-debug-log-2026-09-18.md` §22. Several hypotheses below (FIFO reuse, LOD) were disproved.

**Date:** 2026-09-26 (late night session) · **Prepared for:** continuing this work in a fresh chat
**Original session history searchable via:** `session_search(session_id='20260917_233444_df506f')`

---

## 1. Where everything lives

| Thing | Location |
|---|---|
| Engine repo | `e:/Games/ctr-native` (branch `master`) |
| Our fork (origin) | `https://github.com/Alihkhawaher/ctr-native` |
| Upstream remote | `upstream = https://github.com/CTR-tools/ctr-native` (the project moved to the **CTR-tools** org; old `aalhendi/ctr-native` URLs redirect) |
| Play folder (user's game) | `E:/Games/CrashCTR-Win` |
| Sandbox (instrumented test copy) | `$LOCALAPPDATA/Temp/ctr_retest` |
| Scratch helpers | `C:/Users/Ali/AppData/Local/hermes/cache/scratch/` (`drive.py`, `vbdump_*.py`, `repair_sandbox_config.py`, `set_sandbox_disc.py`, `grid_blob.py`, `mark_blob.py`, `disasm_rva.py`, …) |
| Analysis artifacts | `$LOCALAPPDATA/Temp/rel/` (`pre_dump.png`, `vbdump_f0/f1/f3.png`, `blob_marked_scr.png`, `blob_marked_wf.png`, `cortex_pair.png`, `f7_vram.png`, `wire.png`, `healoff_check.png`, …) |
| Debug log / docs | `docs/native-debug-log-2026-09-18.md`, `docs/SKILL.md` (repo) |
| Agent skill | `ctr-native` (Hermes skill) |

### Build & deploy matrix (as of handoff)

| Build | md5 | Notes |
|---|---|---|
| `build-msvc-x86/Release/ctr_native.exe` (latest) | `ffd1db4c9c852e8b5d61057f97212934` | Instrumented + self-heal **OFF** (default). Deployed to sandbox. |
| Sandbox `ctr_retest/ctr_native.exe` | `ffd1db4c9c852e8b5d61057f97212934` | Same build (matches above). Sandbox process currently **not running**. |
| Play folder `CrashCTR-Win/ctr_native.exe` | `f92e39de3677a3cb56f175e85281bb69` | **Clean build — contains NO probes, NO self-heal** (verified: no `instprim probe`/`chainbreak`/`reuse-contam` strings). mtime 2026-09-26 00:59. |
| Committed HEAD | `906250b4d` "fix: battle-mode crash (raw page deref) + restore gamepad rumble" | Only this + earlier commits are on GitHub. **All 2026-09-26 instrumentation is UNCOMMITTED working-tree change.** |

`git status --short` (uncommitted): `game/RenderBucket/RenderBucket_QueueExecute.c`, `include/platform/native_config.h`, `main.c`, `platform/native_config.c`, `platform/native_gpu.c`, `platform/native_platform.c`, `platform/native_renderer.c`.

**Build command:** `cmake --build --preset windows-msvc-x86-release` (MSVC, 32-bit, x86). Kill any running `ctr_native.exe` before overwriting the exe.

---

## 2. The open issue: rival-kart distortion ("the blob")

**Symptom:** In 2P split-screen races (and 1P races per user), the **rival (non-camera-followed) karts render as mangled blobs** — the kart's *upper/driver parts get replaced by unrelated flat pieces* (user's screenshot: a big tan "plate" top view; a red flame-like mass bottom view), while wheels/chassis stay mostly right. **The own kart is pristine in its own viewport.** Same kart renders clean in its own viewport and mangled in the other.

### Evidence chain (established, in order)

1. **Textures are fine** — F7 VRAM dump: no corrupted/torn pages.
2. **Not AA, not any runtime toggle** — AA A/B: no effect on blob (`pixels>8=120228 max=255`); user confirmed all toggles tried.
3. **Geometry-level** — F1 wireframe on the blob shows stretched/degenerate triangles.
4. **Not disc data** — full NTSC-U image (`ctr-usa-from-chd.bin`, XA audio playing) reproduces identically.
5. **Split-primitive path ruled out** — `RenderBucket_DrawSplitPrimitiveAtRange` probe fired **0 times** during real races; that path is dormant for CTR; probe parked in code as diagnostic.
6. **Emitters clean** — inst-prim probes (`DispatchDrawInstPrim` / `…AtRange`, GTE FIFO reads): **7.6M prims, 0 wild vertices**, census **100% "N" (normal writer)** — karts go through the normal instance path (confirmed in `include/namespace_Instance.h` notes: kart funcPtr[1] = `0x8006AD88`).
7. **Blob IS in the assembled GL frame vertex buffer** — vbdump (V-key one-shot, 3 flushes) renders show the scrambled cluster at the rival's position; `NativeRenderer_DrawTriangles` draws the uploaded buffer verbatim ⇒ corruption happens between the emitters and the buffer content — vertex/packet grouping, not transform range.
8. **Chain probe (new today):** the GTE FIFO is a chained vertex stream (each prim's `SXY0` feeds the next prim's reuse — see `RenderBucket_LoadPrimRTPS` → `MTC2(sxy0, 13)` "bit30 continuation"). Measured **~69K FIFO chain breaks / 2 min** total; kart instances break ~190+ times; deltas small (2–34 px).
9. **Reuse-contamination probe (new today):** at `RenderBucket_LoadPrimRTPS` reuse time, the FIFO's `SXY0` frequently ≠ what that instance's last dispatch left (`reuse-contam` log, 96+ events/2 min, deltas 4–34 px, drifting) ⇒ **interleaved draws do clobber chained reuse state**.
10. **SELF-HEAL EXPERIMENT — FAILED, REVERTED.** Substituting the instance's own pinned SXY0/SZ1 on contamination (`reuse_fifo_self_heal`) **caused visible 3D tearing** and **did NOT fix the blob** ⇒ conclusion: (a) the substitution semantics are incomplete (false positives in special/mirrored flows), and (b) reuse-contamination as modeled is **not** the sole/full root cause. Flag kept (`reuse_fifo_self_heal`, **default 0/OFF** — config field `reuseFifoSelfHeal`), contamination logging kept (read-only) as diagnostics.

### Ruled out / parked

- Disc data, VRAM textures, AA, all user-facing toggles, split-prim path, emitter transform correctness (in-range), GL draw path.
- PGXP texture/geometry modes: not the cause (blob exists with both off / mixed).

---

## 3. Online research findings (2026-09-26)

- **Upstream = `CTR-tools/ctr-native`.** Our fork is **11 commits behind**; those 11 are decomp-matching + small vehicle fixes (incl. their own battle-crash fix `59bcb19ed fix(230): preserve native game tracker access when starting battle`, `c7bc76896 fix(vehicle): skip empty driver slots when using the clock`, `bf2ed389c fix(vehicle): handle native weapon allocation failures`). **No distortion fix upstream.** Render-relevant diff since our base = only 4 lines (field renames in `MainFrame_RenderFrame.c` / `MainFreeze.c`).
- Upstream releases stop at **beta-7.1 (2026-07-07)**.
- **Issue #56 "Missing quad at a certain distance from camera"** (closed 2026-09-10; "possibly off-by-one bug in some LoD switching code") → fix = **`b37cd1b7e` "Fixed sporadic clipped primitives" — ALREADY IN OUR TREE**. The fix originated in **`Rinnegatamante/Crash-Team-Racing-High-Octane`** (another native CTR engine) — commit `5e426ece…` — a *terminal-face emission / preserved-slot* fix in the level's dynamic/LOD path.
  - **High-Octane = a promising source of same-class fixes for kart/instance/split paths. Commit scan for their kart/instance/render fixes was STARTED BUT INTERRUPTED — not completed.**
- **`thecodingbob/ctr-native`** (user has this tab open; fork of the project with config options) has a **`disable_split_screen_lod`** option: "Use high-detail character models in 3–4P split-screen" ⇒ **split-screen uses LOW-detail models by design.** Their implementation location = not yet found.
- **THE ACTIVE LEAD — LOD hypothesis:** the blob affects exactly the **rivals** (low-detail LOD models in split / distant karts in 1P) while own kart (high-detail) is clean ⇒ suspect: the port's selection/rendering of **low-LOD kart models** is broken (same bug class as the #56 terminal-face/LOD off-by-one). **Experiment not yet run.**

---

## 4. Instrumentation currently in the working tree (all `[CTR Debug]`-tagged, permanent per house rule; read-only except the OFF-by-default self-heal)

- `game/RenderBucket/RenderBucket_QueueExecute.c`
  - Split-spike probe (parked, 0 hits historically).
  - Inst-prim probes in `RenderBucket_DispatchDrawInstPrim` (norm) + `…AtRange`: `calls/wild/census` logging, GTE FIFO sanity checks.
  - Chain-continuity probe **v2**: per-instance table, `driverID` tag (from `ctx->inst->compressedNormalAndDriverIndex` high byte − 1) → `chainbreak:` log lines.
  - Reuse-contamination bridge: `g_rbChainInst/Sx0/Sz1` tables + `reuse-contam:` log + self-heal gated by `g_cfg_reuseFifoSelfHeal` (**default 0**).
- `platform/native_gpu.c` — vb spike probe + franken-triangle probe (mismatched page/CLUT across a triangle) before `NativeRenderer_UpdateVertexBuffer`; `extern int g_dbg_dumpFrame`; `ParsePrimitive` polydump (packet addr + region + raw words); vbdump (3 flushes, auto-clear).
- `platform/native_platform.c` — `int g_dbg_dumpFrame = 0;` + dump key case (F10 / `v` trigger).
- `platform/native_config.c/.h`, `platform/native_renderer.c`, `main.c` — `reuse_fifo_self_heal` config field + parse + write; `g_cfg_reuseFifoSelfHeal` global (default 0).

**Useful log greps (sandbox log = `$LOCALAPPDATA/Temp/ctr_retest/Crash Team Racing.log`):**
`grep -a "instprim probe"`, `grep -a "chainbreak"`, `grep -a "reuse-contam"`, `grep -a "vb probe"`, `grep -a "PGXP stats"`.

---

## 5. Operational playbook (how to drive/test)

- **Window activation is mandatory:** call cua-driver `bring_to_front` (pid+window_id) before ANY key batch — SDL drops input while not foreground. `drive.py` (scratch) does `front` automatically; usage: `uv run -q python drive.py press <key>|shot|info|front`; key map: cross=`C`, start=`return`, arrows=d-pad.
- **Keyboard slot:** press `F4` until log shows `[CTR Native] Keyboard assigned to player 1` (it drifts to P2 after nudges).
- **Attract/demo:** `return` nudges; 2P demo race appears ~1–2 min in; a second `return` can pause — press again to resume.
- **In-game keys:** `PgUp/PgDn` internal res · `Tab` AA · `P` PGXP textures · `G` PGXP geometry · `O` PGXP status overlay (**turn OFF — red overlay**) · `H` freeze · `F1` wireframe · `F7` VRAM dump · `F12` screenshot (→ `screenshots/*.bmp`) · `F10`/`v` one-shot frame dump (polydump + vbdump) · `F3` bilinear · `F5/F8` save/load state · `F9` replay · `F11`/`Alt+Enter` fullscreen · `F4/F6` input assign · `R` rumble test.
- **Sandbox config:** `$LOCALAPPDATA/Temp/ctr_retest/ctr-native-config.json` — window 1280×800, `internal_resolution_scale` 4, `antialiasing` false, `pgxp` true, `pgxp_geometry` false, disc = `ctr-usa-from-chd.bin` (full NTSC-U), keyboard player 1.
- **User's play config:** `E:/Games/CrashCTR-Win/ctr-native-config.json` (preserve it; don't xform).

---

## 6. Constraints & standing rules (do not violate)

- **NEVER remove debug/verbose logging** — add flags instead. All new probes follow the `[CTR Debug]` + `#ifdef`-gate convention.
- **"do no release now"** — do NOT cut a new release; `beta-9` stays untouched. `ctr_config.exe` stays local to the play folder + repo, not shipped.
- The user plays on his **second monitor**; don't steal focus except when actively driving the game (then activate, per above).
- Keep any reviewer-AI (Fable via AI-MediaLens) prompts SHORT (costly).
- Don't touch `D:\Design` (read-only), outputs only under `C:\Developments\Arch` for the villa project (unrelated but standing rule).
- The user checks GitHub himself — keep pushes verifiable (hashes/IDs), and explain in-place changes.

---

## 7. Immediate next steps (planned, not yet run)

1. **Finish the High-Octane scan** (interrupted): `gh api "repos/Rinnegatamante/Crash-Team-Racing-High-Octane/commits?per_page=100"` — look for kart/instance/split/LOD/vertex fixes; cherry-pick candidates into our tree (they fixed #56's class for the level already).
2. **List `game/231/` fully** and identify the kart/driver render-bucket file; read how the driver "upper" parts emit vs wheels (target for the "upper half replaced" symptom). *(listing was interrupted — was mid-command.)*
3. **Find the split-screen LOD model switch** in our tree (the low-detail kart selection) + find `thecodingbob`'s `disable_split_screen_lod` implementation for reference.
4. **RUN THE DECISIVE LOD EXPERIMENT:** temporarily force high-detail models in split (or force low-detail everywhere in 1P) → **does the blob vanish/move?** If yes → the port's low-LOD kart path = the bug; then compare low-model prim stream/stride/parse vs high.
5. Also re-check "normal race mode" repro per user ("the distortion also happens in normal race mode") — capture a 1P race blob frame + check whether the mangled rival is a distant LOD'd kart.
6. Only after a REAL fix: commit probes + fix (or probes first), update `docs/native-debug-log-2026-09-18.md` + `docs/SKILL.md` + skill, keep debug (flag-gated).

## 8. Known-adjacent statuses (not to redo)

- Battle-mode crash = fixed (`MM_Battle.c:576`, GAME_TRACKER guard) + **user-verified fixed**.
- Rumble = fixed (per-poll `motorSubmit` forward, `R` test key) + **user-verified working**.
- Native Win32 config launcher `ctr_config.exe` = built, deployed, committed (`b271c884f`).
- Mystery Caves crash = root-caused (proposed guard not yet landed) — awaiting user "go".
- PGXP dual-mode = both work; run one at a time (never both — tearing); labelled experimental, default OFF.
