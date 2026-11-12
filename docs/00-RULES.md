# 00 — Operating Rules

These rules are binding for every session, including after context compaction. They are the first
thing to re-read when resuming.

## R1 — No guesses

Every technical claim about the target must be backed by an EVIDENCE record from REA, Craft Anything,
or direct byte-level inspection, and must be cited by its `ev_…` id in the relevant document.

- "I think / probably / likely" is not allowed as a finding. It may only appear in **Open questions**.
- Unresolved items go into `docs/06-EVIDENCE.md` → *Unknowns*, with the probe that would settle them.
- If two observations conflict, record the contradiction; do not silently pick one.

## R2 — Game data is immutable

`00742-Fast-5-HD-1.0.9-PowerVR.apk` and everything under `GloftF5HP/` are **read-only, forever**.

- Never write, move, rename, delete, repack, recompress or "fix" them.
- All analysis works on copies under `analysis/`; all runtime reads them in place.
- Any new derived file lives under `analysis/` or `build/`, never beside the originals.
- A sha256 manifest of the data tree is kept (see `docs/01-TARGET.md`) so drift is detectable.

## R3 — Workspace confinement

Work only inside this project directory — the checkout root:

```
<checkout>/
```

Tools that run in the Linux analysis host see the same tree under `/mnt/c/<checkout>` (or the
equivalent mount).  No other folders are read or written.

## R4 — Clean, maintainable structure

- One purpose per directory; no loose files at the root beyond the immutable inputs and README.
- Generated artifacts are disposable; source and docs are the deliverable.
- Code follows the conventions documented in `docs/03-STRATEGY.md` §Toolchain.
- No dead code, no commented-out experiments, no duplicate copies of the same analysis.
- **Nothing derived from the shipped game is ever committed.**  The APK, the native library, the
  game data, the decompiler dumps, the generated translation units and the frame captures are
  excluded by `.gitignore` and stay on the machine that owns them.  The published repository
  (`public/`) contains the toolchain, the platform layer, the tools and the documentation only.

## R5 — Continuity

Work continues across compaction. Therefore:

- `docs/05-STATE.md` is updated **at the end of every work block**: what was done, what is next,
  what is blocked, and the exact command/tool to resume with.
- Craft Anything's investigation state (`core_brief_write`, `core_plan_update`, findings) mirrors the
  docs so the next session can recover the reasoning, not just the conclusions.
- Evidence bundles are exported to `analysis/evidence/` before context-heavy phases end.

## R6 — 1:1 means 1:1

The port must reproduce the Android build's behaviour, not approximate it:

- Same data bytes, same loaders, same scene graph, same simulation timing, same input mapping,
  same save format, same audio.
- No asset conversion pipeline as a *requirement*; conversions may exist only as debug aids.
- Visual differences caused by the host GPU are acceptable only where the original GLES2 contract is
  preserved (see `docs/03-STRATEGY.md` §Rendering).
- "It boots" is not done. Milestone gates in `docs/04-ROADMAP.md` define done.

## R7 — No emulator, no middleware

The shipped executable may not contain or require: QEMU, Box64/Box86, Android-x86, an ARM interpreter,
a JIT, WINE, or a third-party engine/runtime. The platform layer we write is part of the port and is
allowed to call OS APIs (Win32, OpenGL, WASAPI/XAudio2, threads, files).

An emulator may be used **only** as an offline test oracle during development, never shipped and never
in the boot path.

## R8 — Evidence discipline for tooling

- Use REA for shipped-binary truth (decompilation, semantics, ABI, native behaviour).
- Use Craft Anything for archive/inventory/format facts and one-page overviews.
- Prefer one authoritative observation over three corroborating guesses.
- Never re-run an identical expensive analysis; cite the retained evidence id instead.
