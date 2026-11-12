# NOTICE — what this repository is, and what it deliberately is not

## The short version

This repository contains **the porting toolchain and the native platform layer**
for running *Fast Five HD* on desktop Windows x64.  It contains **none of the
game**.  The game is not here, and you cannot play it without supplying your
own copy of the game files.

The MIT licence in `LICENSE` covers exactly those parts: `recomp/`, `runtime/`,
`tools/`, `analysis/` and `docs/`.  It covers none of the game's content,
because none of the game's content is here.

## What is in this repository

| Path | What it is | Copyright |
|---|---|---|
| `recomp/` | The static recompiler: ARMv7 machine code → portable C. Our own code. | MIT (see `LICENSE`) |
| `runtime/` | The native Win64 platform layer the recompiled code runs on: window, GLES2 bridge, threads, files, JNI surface, presentation transform. Our own code. | MIT (see `LICENSE`) |
| `tools/` | Build and diagnostic scripts. Our own code. | MIT (see `LICENSE`) |
| `analysis/` | Census reports and small analysis helpers derived from our own tooling. | MIT (see `LICENSE`) |
| `docs/` | The engineering documentation: target dossier, architecture, strategy, roadmap, live state, evidence ledger. | MIT (see `LICENSE`) |

## What is deliberately NOT in this repository

The following are all **copyrighted by the game's publishers**
(Fast Five HD © Gameloft / Square Enix) and are therefore excluded:

- the APK (`00742-Fast-5-HD-1.0.9-PowerVR.apk`);
- the native library (`lib/armeabi-v7a/libFast5.so`) in any form, including
  decompiled, disassembled, translated or reconstructed form;
- the game data tree (`GloftF5HP/`) — models, textures, levels, audio, fonts,
  videos, save files;
- decompiler output derived from the native library (the P-code dumps);
- the generated C translation units produced from the native library
  (`build/link/`), and the object files and executables built from them;
- extracted game assets, including the derived overlay in `build/overlay/`;
- captured frames, screenshots and audio recordings of the game.

You can regenerate every excluded artefact from a copy of the game that you
own.  `SETUP.md` walks through it.  Nothing excluded here is required to be
committed in order to build: the toolchain produces it from your own copy of
the library.

## How to obtain the game legitimately

Fast Five HD is a commercial Android title.  Depending on your region it is
available from Google Play and, historically, from Gameloft's own
distribution channels.  A copy you purchase for your own device is yours to
use; that copy is the input to this toolchain.

## Trademarks

"Fast Five HD", "Gameloft", "Square Enix", "Irrlicht" and "Fast Five" are the
property of their respective owners.  This project is not affiliated with,
endorsed by, or sponsored by any of them.  The names are used only to identify
the software this toolchain was written to run.

## Third-party components

This repository links against, but does not redistribute:

- **ANGLE** (Google) — the GL ES 2.0 / Direct3D 11 backend used by the native
  Windows build.  `third_party/angle/README.md` explains how to supply it.
- **Ghidra** (NSA) — the analysis front-end used to dump P-code.  Used offline
  only; not part of the shipped binary.
- **MinGW-w64 GCC** — the host compiler.

The shipped executable contains no emulator, no ARM interpreter, no JIT and no
middleware of any kind.

## Disclaimer

This project is provided "as is", without warranty of any kind.  See the
`LICENSE` file for the full text.
