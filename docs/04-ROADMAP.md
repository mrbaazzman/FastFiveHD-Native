# 04 — Roadmap

Each milestone has an objective, entrance/exit gates, and the evidence that must exist before it is
marked complete. Milestones are sequential; a gate that fails sends us back to strategy review, not
forward.

## M0 — Foundation and reconnaissance ✅ (current)

**Objective:** know exactly what we are porting and have a place to put the work.

Deliverables:

- [x] Toolchain verified (REA + Craft + Ghidra 12.1.4 + WSL + MinGW/CMake/Ninja)
- [x] Target identity and hash recorded (`docs/01-TARGET.md`)
- [x] Engine identified as Irrlicht-derived Glitch + Spark + Vox
- [x] Architecture and data model documented (`docs/02-ARCHITECTURE.md`)
- [x] Strategy and risk register written (`docs/03-STRATEGY.md`)
- [x] Project skeleton (`recomp/`, `runtime/`, `tools/`, `analysis/`, `build/`)
- [x] Data-tree sha256 manifest (`analysis/data-manifest.sha256`, 662 entries)
- [x] ELF census report (`analysis/reports/elf-census.md`, `instruction-census.md`)

**Exit gate:** every claim in the target dossier cites evidence; no unexplained files at the root.

## M1 — Translator proof gate

**Objective:** prove that a P-code-driven emitter produces bit-identical behaviour on a representative
function set, before scaling to 20k functions.

Steps:

1. **[x] ELF census** — ARM vs Thumb counts, function size histogram, instruction mnemonic histogram,
   VFP/NEON usage, exception usage, `__aeabi_*`/`__cxa_*`/`pthread_*` call counts.
   Report: `analysis/reports/elf-census.md`, `instruction-census.md`.
2. **[x] Image model (reference)** — `recomp/elf_image.py` maps both LOAD segments at link addresses,
   applies 62,432 relocations and reads 433 constructors; verified on the real library.
   A C implementation of the same model is part of the runtime (M3).
3. **[x] Emitter v0** — P-code → C for integer ops, flags, branches, calls, memory, returns,
   VFPv3 float ops and the ARM userops observed so far; `recomp/emit_c.py`, runtime `cpu.h`/`mem.h`.
4. **[x] Differential harness** — `recomp/diff_test.py` + `batch_diff.py` run translated C and the
   original ARM code under Unicorn on identical memory/register state and compare r0–r3 + hashes.
   Latest: 40/40 integer, 12/12 setter, 4/4 VFP — 0 mismatches; faults compared by address.
5. **Gate** — 200 functions across 8 classes (see `docs/03-STRATEGY.md` §3.5), zero mismatches.
   Must include a C++ throw unwinding across three frames and two virtual calls.

**Exit gate:** harness report in `analysis/reports/m1-differential.md`; any remaining mismatch has a
written root cause and a plan.

## M2 — Full translation and link

1. Emit all 20,211 functions; resolve the 3,548 indirect-jump sites.
   **[x] Emitted 20,325 / 20,325 non-empty functions (100 %) and compiled all 102 chunks cleanly.**
2. Exception model complete (exidx → landing pads, `__cxa_*` replacement, destructor unwinding).
3. `.init_array` 433 constructors run in order; `.fini_array` on exit.
   **[x] All 433 constructors complete in the linked translated binary.**
4. Link `fast5.exe` with stubbed runtime; it must load, run constructors, and exit cleanly.
   **[x] Linked `build/fast5_recomp_test` (20,033 non-PLT functions, no missing symbols); a
   translated function executes and all constructors run.**
5. Boot the real main path: `appInit` → `appResume` → `appRender` → touch → menu.
   **[~] Native Windows build (`build/fast5_win.exe`) runs `appInit`/`appResume`, advances from the
   logo to the Universal legal text, then renders the **Fast Five title** with real textures. A
   posted Space or `FAST5_AUTO_MENU=1` advances to the **main menu** (Quick Race, Career 0%,
   Multiplayer, Options, More games!, Gameloft LIVE); captured as `build/clean_title.png` and
   `build/auto_menu.png`. No guest fault at about 30–35 FPS. Remaining: prove the Favelas
   scene/PVR binding and compare body livery against a reference; deterministic Quick Race input
   and the effect-data gap remain. Car body/wheels are textured via the tracked `*_Body*.tga`
   fallback and body-row flip; the material-renderer parameter workaround is still not a root
   fix.**

**Exit gate:** no missing symbols at link; constructor trace matches an ARM constructor trace; no
crashes in a 60 s idle run.

## M3 — Platform layer

1. Window + real GLES2 context (Win32 + EGL/ANGLE), resize, focus, message pump.
   **[x] `runtime/platform_win32.c` creates a real Win32 window and an EGL/GLES2 context through the
   vendored x64 ANGLE runtime (`third_party/angle`); `tools/win32_angle_test.c` captured a real
   `build/angle_test.bmp`. The harness swaps with `eglSwapBuffers`; Enter/Space maps to touch.**
2. GLES2 bridge: all 91 GL imports accounted for; shader translation; FBO/VBO/texture paths.
   **[~] `runtime/gl_win32.c` dispatches all 91 imports; shaders/programs/uniforms/FBOs/VBOs/draws
   run on ANGLE. The shipped `#endif]` quirk is cleaned, and declaration-order attribute binding
   matches the game's PowerVR assumption. The menu now renders real text/menu textures; missing
   `mm_*PVRTC2.tga` scene names now resolve to full-header `.pvr` aliases; the remaining question
   is the visible scene/PVR binding, not file lookup. Car body TGA row order and the 3D font-atlas
   bind are fixed. `pthread_create` is now implemented.**
3. FS redirection to the immutable data tree.
   **[x] `runtime/vfs.c`: reads map to `GloftF5HP/res/`, writes to `build/userdata/`, overlay in
   `build/overlay/`; game data is never modified.**
4. pthread subset, timing, `__android_log_print` to a log file.
   **[~] `pthread_once` runs its initializer; `gettimeofday`/`time`/`localtime_r` implemented;
   mutex/cond/TLS are still no-ops; `pthread_create` is a no-op. The only observed create is the
   persistent timer routine `0x68d694`, which never runs. The menu renders without it; audio/timing
   paths still need it.**
5. Input mapping to the engine's `SEvent` structures.
   **[x] Synthetic touch verified: `notifyTouchPress` (`0x29CF94`) / `notifyTouchRelease`
   (`0x29D1A0`) advance the title screen; gates `mWaitForInit=0`, orientation=0, scale=2.0.**

**Exit gate:** a standalone test app renders a translated engine scene with a real mesh and texture.
**[~] Met for the title and menu UI with real textures; full menu scene texture parity is still
open.**

## M4 — First boot

1. `JNI_OnLoad` and the Java→native surface implemented by our `jni` module.
2. `GS_Init` → `GS_MenuMain_Load` executed; first frame presented.
   **[~] Title screen and main menu are presented with real textures after a Space key; a
   deterministic second touch and full menu texture parity are still pending.**
3. Deterministic mode: fixed timestep and input script.

**Current pre-M4 state (2026-11-06):** `GS_MenuMain_Load` executes, the title renders, and a posted
Space key reaches the main menu (`build/clean_menu.png`). No guest fault. The remaining visual issue
is missing `mm_*PVRTC2.tga` scene names / black-white car bands; see `docs/05-STATE.md` item 41.

**Exit gate:** screenshot of the title screen produced from original data; crash-free 10 minutes.

## M5 — Asset pipeline parity

1. `.dat` readers validated against the shipped files (all 653).
2. PVRTC textures decode/upload correctly (pixel-compare a known texture).
3. Audio banks decode and play; movie playback path decided (native MP4 decode or OS codec).
4. Save/profile format round-trips `profile1.sav` byte-for-byte.

**Exit gate:** asset audit report: every file opened, every format exercised, zero silent fallbacks
(silent fallbacks are banned — a missing asset must behave like the original).

## M6 — Game flow
1. Menus, car selection, race start, race loop, debrief, save.
   **[x] Full cycle confirmed by the user on 2026-11-08:** boot → menu → race → debrief → back to
   menu → quit → relaunch, with the player's own `profile1.sav` preserved and cash displayed.
2. Input semantics (accelerometer/gyro → keyboard/gamepad mapping documented).
   **[x] Keyboard scheme built on the game's *touch* path** (the accelerometer path cannot work: the
   same values drive the camera lean). Steering, throttle, brake, nitro, camera cycle and look-back
   are wired; all input is gated on the player car (`car+0x400 == 0`). Rewind (R) is deferred by the
   user — it needs its own hook rather than the button-mask path.
3. Multiplayer code paths present but offline-safe.
   **[~] Present and dormant:** `serverConfig.sav` is absent so `GLXPlayerSereverConfig` reports "not
   found" and the online layer stays inert.

**Exit gate:** complete race finished, result written, state reloaded correctly.
**[x] Met** — see `docs/05-STATE.md` → "Game cycle — what it took". Audio, progress and cash all
survive a restart with 0 guest faults.

## M6.5 — Presentation (Full HD) ✅ 2026-11-11, corrected 2026-11-12

The port presents the game in a window of any size. The engine runs at the window size (native
device, render targets, camera aspect, scene); the 2D layout keeps the authored 800×480 space
so the game logic, the hit rectangles and the 3D picking stay 1:1 with the reference device.

1. **[x]** Borderless window at the requested size (1920×1080 by default); the client area and
   the EGL surface are exactly the requested size.
2. **[x]** 2D layer (menus, HUD, text) scaled from the design space to the window; the
   transform is shared with the pointer and the 3D picking.
3. **[x]** 3D scene rendered at the window resolution with the window's aspect ratio.
4. **[x]** Render targets at the window resolution (2304×1296 instead of 960×576).
5. **[x]** One uniform scale for the 2D layer (the default), so the artwork and the bitmap font
   keep the reference proportions at any window size; `FAST5_UI_FIT=fill` stretches each axis
   instead; `FAST5_ENGINE_RES=design` keeps the engine at 800×480.
6. **[x]** A start-up invariant check: the 2D canvas must land inside the framebuffer and the
   frame → design → frame round trip must be the identity.
7. **[ ]** Live window resize (the EGL surface is created once at start-up).

## M7 — 1:1 validation

1. Reference recordings from an Android device or emulator (offline only).
2. Frame-diff at fixed timestep; audio waveform compare; input replay.
3. Performance target: stable 60 fps at the original render resolution.

**Exit gate:** fidelity report with quantified differences and their causes.

## M8 — Release quality

1. Reproducible build from a clean checkout (`build.ps1` + CMake presets).
   **[~]** `tools/build_win.py` is incremental and reproducible; the CMake/Ninja preset path was
   dropped in favour of the incremental MinGW build that the project actually uses.
2. User-facing README: how to run, where data must live, legal note.
   **[x]** `public/README.md`, `public/SETUP.md` and `public/NOTICE.md` — including the
   statement that no game data, no native library and no captures are redistributed.
3. Full documentation index, evidence export, and handoff for future sessions.
   **[x]** `public/README.md` carries the documentation index; the evidence ledger is
   `docs/06-EVIDENCE.md`.

**Exit gate:** fresh clone + documented steps → running game, no undocumented manual steps.
**[~]** A fresh clone builds and runs once the user supplies their own copy of the game; the
remaining step is a verified clean-room reproduction of the whole pipeline.
