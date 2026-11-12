# SETUP — building the port from your own copy of the game

## The short version

Download **`FastFivePortBuilder.exe`** (from the project's releases), run it, point it at your
APK and your game data folder, and press **Build the port**.  It needs no Python: everything it
uses is inside the executable.

If you would rather work from a checkout, the same pipeline is one command:

```bash
python tools/bootstrap.py --apk "00742-Fast-5-HD-1.0.9-PowerVR.apk" \
                          --data  "D:/FastFiveHD/GloftF5HP"
```

Either way you end up with a native `fast5_win.exe`.

The game is **not** in this repository and is never downloaded by the build: you supply your own
copy, and the build reads it in place.  See [`NOTICE.md`](NOTICE.md) for the legal position.

---

## 1. The builder

### Three ways to run it

| | |
|---|---|
| **Double-click it** | a window: pick the APK, the game data and where to install, press **Install the game**.  A progress bar shows the steps; the details are below it. |
| **From a terminal** | `FastFivePortBuilder.exe --apk <the APK> --data <the data> --install <the folder>` |
| **Silently** | `FastFivePortBuilder.exe /S --apk <the APK> --data <the data> --install <the folder>` - no window, no output, and the exit code says whether it worked.  The log is in `%LOCALAPPDATA%\FastFivePortBuilder\silent.log`.  This is the mode for unattended deployments. |

`FastFivePortBuilder.exe` is the whole Python side of the build in one file: the bootstrap
pipeline, the Ghidra dumper, the recompiler and the native build script.  It unpacks itself
into

```
%LOCALAPPDATA%/FastFivePortBuilder/
    app/       the toolchain
    build/     the native binary, the captures, the writable game data
    analysis/  the extracted library, the P-code, the Ghidra project
```

and works there, so nothing is written outside that folder (except the game data, which is
only ever read).

Two things it cannot bundle, because they are not ours and they are large:

* **Ghidra** — the analysis of the library.
* **a C compiler** — MinGW-w64 GCC.

The builder finds both and tells you exactly what to install when they are missing.

To build the builder yourself (it needs PyInstaller):

```bash
python -m pip install pyinstaller
python tools/build_builder.py       # -> dist/FastFivePortBuilder.exe
```

## 2. What you need

**The end user needs two things: the APK and the game data.**  Everything else — the analysis
tool, the Java runtime, the compiler — the builder fetches, installs and uses on the first run.

| | |
|---|---|
| **The APK** | `Fast Five HD 1.0.9`, the PowerVR build (`lib/armeabi-v7a/libFast5.so`). The build verifies its SHA-256, because the recompilation works on the shipped instructions at their shipped addresses — a different build is a different port. |
| **The game data** | the ~775 MB data tree the game downloads on its first run. On a device it is at `/sdcard/gameloft/games/GloftF5HP/`. Copy it off the device, or take it from the expansion the game downloads (`res/raw/data.txt` in the APK records the URL). |
| **An internet connection** | only for the first run, and only for what the machine does not already have. |

### What the builder installs for you

If they are missing, the builder downloads the official package, checks its SHA-256 and unpacks
it into `%LOCALAPPDATA%\FastFivePortBuilder\tools\`:

| Tool | Why | Size |
|---|---|---|
| **Ghidra 12.1.4** | the analysis of the library (the recompiler reads Ghidra's P-code, so the project never contains an ARM decoder) | 543 MB |
| **Eclipse Temurin JDK 21** | Ghidra runs on it | 196 MB |
| **MinGW-w64 GCC 16.2** | the compiler for the native build | 263 MB |

Anything you already have is used instead and never downloaded: a Ghidra on `PATH`, in
`GHIDRA_INSTALL_DIR` or inside WSL; a JDK 21+ (the version is checked - Ghidra 12 will not run
on 17); a `gcc` on `PATH`.  `FAST5_PREREQ_DIR` chooses where the fetched tools live, and
`--no-download` turns the fetching off (the builder then only reports what is missing).

A **GLES2 backend** is also needed at run time (the port talks to a real EGL/GLES2
implementation).  **ANGLE** — Google's GL ES 2.0 over Direct3D 11 — is the one this project was
developed against, and every current Chromium-based browser ships it: the port looks in
`third_party/angle/` and in the usual browser install directories.  See
`third_party/angle/README.md`.

**Time and disk:** the first build analyses the library in Ghidra and then compiles ~20 000
translated functions.  Measured on a 12-core Windows machine:

| Step | Time |
|---|---|
| Ghidra: import, analyse, dump the P-code | **4 min 05 s** |
| Ghidra: recover the table-only functions | 18 s |
| translate 20 041 functions to C | 1 min 11 s |
| compile 112 objects and link the binary | 3 min 18 s |

So a first build is about **9 minutes** (plus the tool downloads), and it needs roughly
**1.5 GiB** of disk for the build itself plus about **1.2 GiB** for the fetched tools (the
Ghidra project, the P-code dump, the generated C, the object files and the binary).  Later
builds are incremental and take seconds.

**Memory:** the build needs about **4 GiB** of RAM at its peak, and it sizes itself to the
machine:

* compiling the generated translation units is the hungry part - measured, twelve parallel
  jobs peak at 5.0 GiB, so a job costs about half a gigabyte.  The build therefore runs
  `min(CPUs, free memory, 8)` jobs and says so in its log; on a small machine it slows down
  instead of swapping;
* Ghidra's own launcher already caps the JVM heap (`MAXMEM` in `support/launch.sh`, 2 GB by
  default), so the analysis needs about a gigabyte and the WSL virtual machine about another.

`--jobs N` overrides the job count.

## 3. The build

Graphically: run `FastFivePortBuilder.exe`, choose the APK, the data folder and **where to
install the game**, then press **Build the port**.  The window shows the whole pipeline as it
runs, and a **Play** button appears when the game is installed.

The result is a folder you can keep and play from:

```
<install>/
    fast5_win.exe      the native port
    play.bat           a launcher (starts the game in that folder)
    GloftF5HP/res/...  the game data
    build/userdata/    the game's writable data
```

The data is copied, never moved or modified; the port reads the shipped files in place and
writes only into `build/userdata`.  From a command prompt:

```bash
FastFivePortBuilder.exe --apk <the APK> --data <the data> --install <the folder>
```

From a checkout:

```bash
python tools/bootstrap.py --apk <the APK> --data <the GloftF5HP directory>
```

It does seven things, and you can run them one at a time if you prefer:

| Step | What it does | Tool |
|---|---|---|
| 1 | checks the toolchain and the free disk space | — |
| 2 | extracts `libFast5.so` from the APK and verifies its SHA-256 | — |
| 3 | links the game data into the tree (a link, not a 775 MB copy) | — |
| 4 | builds the relocated guest image the port runs | `tools/patch_guest.py` |
| 5 | analyses the library in Ghidra and dumps its P-code | `tools/dump_pcode.py` |
| 6 | translates every function to C | `recomp/link_all.py` |
| 7 | compiles and links `build/fast5_win.exe` | `tools/build_win.py` |

Useful flags:

| Flag | Effect |
|---|---|
| `--skip-pcode` | reuse the existing P-code dump (skips Ghidra entirely) |
| `--force` | redo everything, including the analysis |
| `--jobs N` | compile with N workers |
| `--ghidra <dir>` | where Ghidra is, if it is not on `PATH` |
| `--no-run` | build only, do not smoke-test the result |

After the build the script boots the game for 20 seconds as a smoke test and writes
`build/bootstrap.log`.

## 4. Playing

```bash
play.bat            # normal play
play.bat menu       # auto-advance the title into the main menu
play.bat race       # auto-drive all the way into a race
```

The window opens at 1920×1080 (borderless if that fills the screen).  The mouse is the game's
touch; the arrow keys steer; Enter/Space taps the screen centre.

| Variable | Default | Effect |
|---|---|---|
| `FAST5_WIDTH` / `FAST5_HEIGHT` | `1920` / `1080` | the presentation size of the window |
| `FAST5_UI_FIT` | `fill` | `crop` fits the 2D canvas undistorted and covers the window; `letterbox` keeps the proportions and centres it |
| `FAST5_ENGINE_RES` | `native` | `design` runs the engine at 800×480 instead of the window size |
| `FAST5_UI_2D` | `design` | `native` lets the engine lay the 2D layer out for the window (crisp, but the game's own screens land in the corner) |
| `FAST5_WINDOWED` | off | force a framed window instead of borderless |
| `FAST5_FAKE_GPU` | off | report the PowerVR identity the game expects |
| `FAST5_AUTO_MENU` / `FAST5_AUTO_RACE` | off | drive the title into the menu / into a race |
| `FAST5_GL_TRACE` | off | log every GL call |

The port reads the game data **in place** and never writes to it.  Its own writable data goes to
`build/userdata/` and its derived assets to `build/overlay/`.

## 5. Doing it by hand

The same steps, individually:

```bash
# 1. the native library
python -c "import zipfile; zipfile.ZipFile('game.apk').extract('lib/armeabi-v7a/libFast5.so','analysis/apk')"

# 2. the game data next to the repository
#    (a directory junction on Windows, a symlink elsewhere)
cmd /c mklink /J GloftF5HP D:/FastFiveHD/GloftF5HP

# 3. the relocated guest image
python tools/patch_guest.py

# 4. the P-code (the slow step)
python tools/dump_pcode.py

# 5. the C translation units
python recomp/link_all.py analysis/reports/pcode_all.tsv --emit-only

# 6. the native binary
python tools/build_win.py
```

`tools/dump_pcode.py` finds Ghidra on `PATH`, in `GHIDRA_INSTALL_DIR`, in the usual install
locations, or inside WSL.  Pass `--ghidra <dir>` if it is somewhere else.

## 6. Troubleshooting

| Symptom | Cause |
|---|---|
| `Ghidra was not found` | install it, set `GHIDRA_INSTALL_DIR`, or pass `--ghidra <dir>` |
| `this is not the library the port was built for` | a different APK build; the port is tied to the shipped instructions |
| `that file is not a ZIP archive` | the APK was renamed or is not the APK |
| `has no 'res' directory` | the data path is not the `GloftF5HP` directory (it must contain `res/`) |
| `could not link; copying instead` | the filesystem cannot hold a link; the copy needs 775 MB |
| `LINK FAILED … Permission denied` | the executable is still running; close it and build again |
| `no compiler found` | MinGW-w64 GCC is not on `PATH` |
| The game shows a black screen | the data link is missing — check `build/bootstrap.log` |

## 7. Where to look next

* [`docs/05-STATE.md`](docs/05-STATE.md) — the current state of the work and what is still open.
* [`docs/04-ROADMAP.md`](docs/04-ROADMAP.md) — the milestones and their acceptance gates.
* [`runtime/README.md`](runtime/README.md) — the platform layer, module by module.
* [`recomp/README.md`](recomp/README.md) — the recompiler, module by module.
* [`tools/README.md`](tools/README.md) — the scripts, and what each one proves.
