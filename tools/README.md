# tools/

Scripts that produce facts about the target. They are read-only with respect to the game data.

| Script | Purpose | Run from |
|---|---|---|
| `elf_census.py` | ELF32/ARM symbol, section, size and exception census for `libFast5.so` | WSL, repo root |
| `instruction_census.py` | Capstone instruction/condition/VFP census (verified, used for M1 planning) | WSL, repo root |
| `emulate_map.py` | Executes the shipped obfuscation-map loop with Unicorn; **ground truth** for `file000000.dat` | WSL, repo root |
| `deobfuscate_map.py` | Fast static model of the same loop; verified byte-identical to `emulate_map.py` | WSL, repo root |
| `parse_map.py` | Parses the decoded map into `dat-map.tsv` | WSL, repo root |
| `trace_map_loop.py` | Development diagnostic: prints register values of the real decode loop | WSL, repo root |
| `dump_pcode.sh` | Runs Ghidra headless and writes `analysis/reports/pcode.tsv`; `reuse` mode reruns the script on the saved project without re-analysis | WSL, repo root |
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
