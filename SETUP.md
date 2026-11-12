# SETUP — building the port from scratch

This walks through a clean checkout to a running game. It assumes you own a copy of *Fast Five
HD*; the game files are **not** in this repository and cannot be redistributed with it — see
[`NOTICE.md`](NOTICE.md).

Total time: a few minutes of unpacking, a few minutes for the P-code dump, and a few minutes for
the compile.

---

## 0. Prerequisites

| Tool | Why | Notes |
|---|---|---|
| Python 3.9+ | the toolchain | no third-party Python packages are needed |
| MinGW-w64 GCC (x86-64) | the host compiler | `gcc` on PATH; the build script finds it |
| Ghidra 12+ | the P-code dump | used **offline only**, never shipped |
| Git | obviously | — |
| A copy of the game | the input | see step 1 |

The native build also needs a **GLES2 backend**.  The port uses ANGLE (Google's GL ES 2.0 →
Direct3D 11 translation layer) and loads `libEGL.dll` / `libGLESv2.dll` at start-up.  Any
GLES2-capable `libEGL.dll` works; ANGLE is the one this project was developed against
(`third_party/angle/README.md` explains where to get it).  Without one, the port will start and
then fail to create a context.

---

## 1. Supply the game

You need two things.

### 1a. The APK — for the native library

The game's logic is entirely in `lib/armeabi-v7a/libFast5.so` (8 357 420 bytes,
SHA-256 `1c8d16f572c3b504edb2084b1a7887bfe8c7659a5d67f0690e813c1e483ccb12`).  An APK is a ZIP:

```bash
mkdir -p analysis/apk
cd analysis/apk
unzip -o "path/to/00742-Fast-5-HD-1.0.9-PowerVR.apk" 'lib/*' 'res/*'
cd ../..
```

Verify the library:

```bash
sha256sum analysis/apk/lib/armeabi-v7a/libFast5.so
# 1c8d16f572c3b504edb2084b1a7887bfe8c7659a5d67f0690e813c1e483ccb12
```

### 1b. The data tree — for the assets

The 3D models, textures, levels, audio and music live in a separate ~775 MB data pack that the
game downloads on first run.  On a device it lands in:

```
/sdcard/gameloft/games/GloftF5HP/res/
```

Copy that directory next to the repository, so the layout is:

```
<checkout>/
  GloftF5HP/
    InsTime
    res/
      file000000.dat … file000651.dat   (652 payloads, 775 MB)
      LOGO_Gameloft_480x800.mp4
      oconf.bar
      profile1.sav
      tracklog.dat
```

The port reads that tree **in place** and never writes to it.  The URL the game itself uses for
the pack is recorded in the APK's `res/raw/data.txt`.

> `file000000.dat` is the **obfuscation map**: it maps the payload names back to their original
> asset names.  The port uses the game's own decoder, so it does not need to be decoded by hand.

---

## 2. Build the guest image

The port does not read the `.so` at run time — it reads a **relocated guest image**, prepared
offline: the ELF's two load segments are mapped at their link addresses and all 62 432
relocations are applied, so the game's absolute pointers, vtables and RTTI stay valid without
any fixups.

```bash
python tools/patch_guest.py
```

This writes `build/guest.bin` (about 9 MB).  It also points the three libc **data** symbols the
library imports (`_ctype_`, `_tolower_tab_`, `_toupper_tab_`) at the tables the runtime provides
— the shipped library imports them as `R_ARM_GLOB_DAT`, which a host libc cannot satisfy.

---

## 3. Dump the P-code

The recompiler does not read machine code directly — it reads Ghidra's P-code, which is the
decoder's own output.  That way we never have to write an ARM decoder.

```bash
bash tools/dump_pcode.sh
```

This runs headless Ghidra over the library and writes `analysis/reports/pcode.tsv`
(20 627 functions, ~223 MB).  Edit `GHIDRA=` at the top of `dump_pcode.sh` if Ghidra is not at
`/opt/tools/ghidra`.

If you already have a Ghidra project, `bash tools/dump_pcode.sh 128 reuse` re-runs the script on
it without re-analysing.

---

## 4. Emit the C translation units

```bash
python recomp/link_all.py analysis/reports/pcode.tsv --emit-only
```

This writes `build/link/chunk_000.c … chunk_101.c` and `build/link/table.c` — the 20 325
translated functions and the global address → function table that makes direct and indirect calls
resolve across the whole binary.

Nothing in `build/` is committed; it is all reproducible from the two steps above.

---

## 5. Build the native Windows binary

```bash
python tools/build_win.py
```

The script compiles the chunks and the platform layer with MinGW and links `build/fast5_win.exe`.
It is incremental — only changed files are recompiled.  Use `--force` for a full rebuild (this
takes a few minutes; if the link is interrupted, delete the zero-byte objects in `build/win/`
and relink).

---

## 6. Play

```bash
./play.bat            # normal play
./play.bat menu       # auto-advance the title into the main menu
./play.bat race       # auto-drive all the way into a race
```

The window opens at 1920×1080 (borderless if that fills the screen).  The mouse is the game's
touch; the arrow keys steer; Enter/Space taps the screen centre.

If the window is black, check the log for `platform: could not load libEGL.dll` (step 0) or for
`GUEST FAULT` (something went wrong in the guest itself — the address and the context are in the
log).

---

## 7. Optional: the GLES2 backend

`third_party/angle/README.md` describes where the ANGLE binaries come from.  They are looked up
in this order:

1. `third_party/angle/` (next to the executable's working directory),
2. `.` (the working directory),
3. a system location.

---

## 8. Optional: verifying the translation

The differential test runs a translated function and the original ARM code under Unicorn on
identical register and memory state, and compares the results.  It is the gate that keeps the
translator honest:

```bash
python recomp/diff_test.py analysis/reports/pcode.tsv 0x29b014
python recomp/batch_diff.py analysis/reports/pcode.tsv --count 40
```

It needs Unicorn and a copy of the library, so it is a development tool rather than part of the
build.

---

## 9. Troubleshooting

| Symptom | Cause |
|---|---|
| `platform: could not load libEGL.dll` | no GLES2 backend — see step 0 / step 6 |
| `platform: ANGLE loaded from …` then a black window | the data tree is missing or misplaced — see step 1b |
| `vfs: … -> MISS` for `file000123.dat` | the data tree is incomplete |
| `GUEST FAULT` in the log | a guest-side error; the address identifies the function — see `docs/01-TARGET.md` for the address map |
| The build fails with `file truncated` | an interrupted link: delete the zero-byte objects in `build/win/` and relink |

---

## Where to look next

* [`docs/05-STATE.md`](docs/05-STATE.md) — the current state of the work and what is still open.
* [`docs/04-ROADMAP.md`](docs/04-ROADMAP.md) — the milestones and their acceptance gates.
* [`runtime/README.md`](runtime/README.md) — the platform layer, module by module.
* [`recomp/README.md`](recomp/README.md) — the recompiler, module by module.
