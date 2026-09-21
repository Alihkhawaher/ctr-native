# Credits & licensing notes

## PGXP

**PGXP** (Parallel/Precision Geometry Transform Pipeline) — the name, the
concept, and the original implementation — belong to **iCatButler**, who
introduced it in their PCSX-Reloaded fork for the original PlayStation:

- https://github.com/iCatButler/pcsxr (GPL-3.0)

The PGXP support in this port (GTE side channel, address-keyed memory cache,
vertex-cache fallback, depth-matched binding, status view) is an
**independent reimplementation** written for the CTR-native render path.
**No source code was copied from any other emulator.** The projects below were
studied as architectural references and for behavioural validation, and are
credited accordingly:

| Project | License | What was referenced |
| --- | --- | --- |
| **beetle-psx-libretro** (`pgxp/`) | GPL-2.0 | Shadow-memory design; invalidation discipline ("a recycled slot must not satisfy a pre-load association"); exact-then-near matching behaviour |
| **DuckStation** (stenzek) | non-standard license (reference only — no code used) | Geometry-tolerance seam behaviour; the observation that coordinate-based matching ships off by default |
| **psxrecomp** PR #148 | (public discussion) | The "hairline seams eliminated by the 0.5px clamp" rationale behind the geometry window |

License compatibility: ctr-native is **GPL-3.0** (CTR-ModSDK lineage); the
original PGXP is GPL-3.0 as well, so the licensing is compatible regardless.
This port's own PGXP code is original work and carries the repository license.

## The wider project

- **CTR-tools / ctr-native** — the Crash Team Racing decompilation and native
  port this work builds on (GPL-3.0, CTR-ModSDK).
- **CTR-ModSDK** — the decompilation SDK the GPL-3.0 license was adopted from.
- Community research (the CTR-Tools Discord and the crash-team-racing
  reverse-engineering community) — disc-image formats, XA audio layout, and
  PAL/NTSC-J version differences used by the tooling in `tools/` and the
  notes in `docs/`.