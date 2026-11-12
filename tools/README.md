# tools/

Scripts that produce facts about the target. They are read-only with respect to the game data.

| Script | Purpose | Run from |
|---|---|---|
| `builder_gui.py` | The end-user front end: a single window that asks for the APK and the data folder and runs the whole build. Bundled into one executable by `build_builder.py`. | Windows, repo root |
| `build_builder.py` | Packages the front end into `dist/FastFivePortBuilder.exe` with PyInstaller, so the end user needs no Python. | Windows, repo root |
| `installer.py` | Installs the built port into a folder of the user's choosing: the executable, the game data (copied) and a launcher. |
| `prereq.py` | Finds the tools the build needs - Ghidra, a JDK and a C compiler - and, when one is missing, downloads the official package, checks its SHA-256 and unpacks it. So the end user installs nothing. |
| `hostinfo.py` | The CPU count and the free memory, without dependencies. The build sizes its parallelism from it, so a small machine builds slowly instead of swapping. |
| `pyrun.py` | Runs a Python script from a normal interpreter *or* from inside the packaged executable (where `sys.executable` is the executable itself, not a Python). |
| `bootstrap.py` | **The end-user build.** Takes the APK and the game data and produces `build/fast5_win.exe`: checks the toolchain, extracts and verifies the native library, links the data into the tree, builds the guest image, dumps the P-code, translates every function and compiles the binary. | Windows, repo root |
| `dump_pcode.py` | Analyses the library in Ghidra and dumps its P-code (and recovers the functions only the data tables reach). Cross-platform; finds Ghidra on `PATH`, in `GHIDRA_INSTALL_DIR`, in the usual locations or inside WSL. | Windows/Linux, repo root |
| `elf_census.py` | ELF32/ARM symbol, section, size and exception census for `libFast5.so` | WSL, repo root |
| `instruction_census.py` | Capstone instruction/condition/VFP census (verified, used for M1 planning) | WSL, repo root |
| `emulate_map.py` | Executes the shipped obfuscation-map loop with Unicorn; **ground truth** for `file000000.dat` | WSL, repo root |
| `deobfuscate_map.py` | Fast static model of the same loop; verified byte-identical to `emulate_map.py` | WSL, repo root |
| `parse_map.py` | Parses the decoded map into `dat-map.tsv` | WSL, repo root |
| `trace_map_loop.py` | Development diagnostic: prints register values of the real decode loop | WSL, repo root |
| `dump_pcode.sh` | The original bash version of `dump_pcode.py` (kept for reference; the Python one is cross-platform and also recovers the table-only functions) | WSL, repo root |
| `capture_window.ps1` | Captures a named Win32 window with a screen copy; **caution:** if the window is not foreground it captures whatever is behind it. Use a `PrintWindow`/topmost fallback for game evidence | Windows, repo root |
| `capture_printwindow.ps1` | Captures the game window through `PrintWindow` (falls back to a foreground screen copy). Run with `powershell -NoProfile -ExecutionPolicy Bypass -File tools/capture_printwindow.ps1 -Out build/menu.png` | Windows, repo root |
| `extract_archive_tgas.py` | Extracts Favelas texture records from `file000145.dat` into `build/overlay/Track/Favelas/`: raw `.tga` names plus complete-header `.pvr` aliases. Read-only on the shipped data. | Windows/Linux, repo root |
| `extract_car_textures.py` | Extracts `carTextures.zip` (`file000591.dat`) TGA/PVR basenames into `build/overlay/` with zlib method-8 decompression. Read-only on the shipped data. | Windows/Linux, repo root |
| `run_test.py` | Headless run of the port: sets the window size, runs for N seconds, stops the process and converts the engine captures to PNG | Windows, repo root |
| `check_orientation.py` | Reports whether a captured frame is vertically flipped (the logo is in the top-left corner) | Windows, repo root |
| `capture_window_run.ps1` | Runs the game with a visible window and grabs the real window content with `PrintWindow` — the ground truth for "what the player sees" | Windows, repo root |
| `click_test.ps1` | Clicks at a window position and shows how the port maps it into the guest's design space | Windows, repo root |
| `run_detached.ps1` | Starts the port detached (it keeps running after the script returns) so a long session — e.g. the race load at 1080p — can be captured | Windows, repo root |
| `compare_frames.py` | Compares a frame captured at two resolutions (scales the small one up and reports the tiles that differ) | Windows, repo root |

The two overlay extractors are derived-data builders: they never write into `GloftF5HP/` and the
overlay is served by `runtime/vfs.c` only after the immutable tree misses. The Favelas records are
PVR images (`0x34` header, `PVR!` at offset 44, format `0x19`), not TGA; the `.tga` copies are for
inspection/fallback, while the full-header `.pvr` aliases are the loader-selected path.

Verification rule: a decoder or format model is only trusted after it matches the shipped code's
execution (or a documented evidence record). `deobfuscate_map.py` earns that status; future format
tools must earn it the same way.

Conventions:

- Always pass absolute or repo-relative paths; output goes to `analysis/reports/`.
- No third-party Python modules; Python 3.12 in WSL is the reference interpreter.
- A script must never write into `GloftF5HP/` or the APK.
