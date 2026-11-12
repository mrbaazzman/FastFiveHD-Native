# recomp/ — ARMv7 → C static recompiler

Working modules:

| File | Purpose |
|---|---|
| `elf_image.py` | ELF32 loader: segments at link addresses, 62,432 relocations, symbols, `.init_array` (verified `OK`) |
| `pcode.py` | Parser for the Ghidra P-code TSV; normalizes Ghidra addresses (`base 0x10000`) to ELF addresses |
| `emit_c.py` | P-code → C instruction state machine (integer, flags, memory, branches, calls, returns) |
| `diff_test.py` | Differential test of one function: translated C vs shipped ARM under Unicorn, randomized inputs + memory hash |
| `batch_diff.py` | Runs the differential test over many functions and summarizes |
| `coverage.py` | Streams the full P-code dump and reports emitter coverage and every unhandled op |
| `emit_all.py` | Emits every function into C chunks and compiles them (whole-binary gate) |
| `link_all.py` | Generates the global function table/chunks; merges `pcode_extra.tsv`; `--emit-only` skips the Linux compile |
| `gen_imports.py` | Maps all 300 PLT entries to import names (`runtime/imports_table.h`) |
| `ghidra_scripts/DumpPcode.java` | Headless Ghidra script that dumps raw P-code per instruction |
| `ghidra_scripts/AddAndDumpFunctions.java` | Creates caller-supplied ELF addresses as functions and dumps only them |

Supporting runtime (`runtime/`): `cpu.h` (guest register file), `mem.h` (little-endian, unaligned-safe
loads/stores). Native Windows runtime: `main_win32.c` (constructors/appInit/appRender/window harness),
`platform_win32.c` (Win32 + EGL/ANGLE window), `gl_win32.c` (all 91 GLES2 imports); build with
`python tools/build_win.py` (local MinGW, no WSL). ANGLE x64 lives in `third_party/angle/`.

Generated files live in `recomp/generated/` (gitignored) or `build/`; they are reproducible from
`analysis/reports/pcode.tsv`.

Current status:

- **Full P-code dump:** 20,627 functions (223 MB), all userop names included.
- **Emitter coverage:** 20,325 / 20,325 non-empty functions (100 %, 0 failures).
- **Whole-binary compile:** 102/102 chunks clean with `gcc -O1 -Wall -Wextra`.
- **Whole-binary link:** `build/fast5_recomp_test` links all translated functions with a global
  address→function table; translated `nativeGetTotalSounds` runs and returns 635.
- **Runtime imports:** `runtime/imports.c` + `runtime/imports_table.h` (300 PLT entries);
  **all 433 `.init_array` constructors complete** in the linked binary.
- **Native Windows window:** `python tools/build_win.py` compiles 101 chunks + `table.c` + 7 runtime
  files with MinGW and links `build/fast5_win.exe`. `runtime/platform_win32.c` creates a real Win32
  window and EGL/GLES2 context through vendored x64 ANGLE (GLES 3.0 / Direct3D 11), and
  `runtime/gl_win32.c` dispatches all **91 GL imports** (shaders, programs, uniforms, textures,
  FBOs, VBOs, draws). `build/angle_test.bmp` is the standalone window proof.
- **Boot harness:** `python3 recomp/link_all.py analysis/reports/pcode_all.tsv --boot` builds
  `build/fast5_boot_test`, which runs the constructors, calls `appInit(800,480,0)`, `appResume`,
  then `appRender` for 200 frames with a synthetic touch press/release. It advances from the logo to
  the Universal Studios legal/title screen and loads the menu scene `#menu_favelas04.max`; the old
  Linux-harness frontier was `CMaterial::getTechnique` reading a garbage material pointer. This is
  historical: the native Windows run below now renders the menu.
- **Native run:** `build/fast5_win.exe` runs the same path with real GL. Shaders/programs compile;
  the shipped `#endif]` quirk is cleaned in `gl_win32.c`. A compatibility mode
  (`FAST5_FAKE_GPU=1`, PowerVR identity strings while rendering through ANGLE) skips the slow CPU
  JPEG path and reaches the menu. The game **does** query attribute locations through the engine's
  own shader map; the bridge binds `attribute` declarations to declaration order before linking and
  deduplicates the two `#if` branches. Textures load correctly
  (`build/tex_0_256x256.bmp`). The new `runtime/thread_win32.c` layer starts/joins the Vox worker
  threads with real mutexes and a locked guest allocator; left-click is mapped to guest touch. Set
  `FAST5_GL_TRACE=1` for GL state/texture diagnostics and `FAST5_THREAD_TRACE=1` for thread lifecycle.
- **Dispatch performance:** `call_target` uses a sorted binary-search table (20,041 entries) instead
  of a 20k-case switch; `table.c` is compiled at `-O2`. This roughly halved the native frame time
  (frames 30/60/120 in ~15 s).
- **Current native state (2026-11-07):** the fake-GPU path loads `#menu_favelas04.max`,
  `All_merge_low_.max` and menu sounds, then renders the **Fast Five title** and—after Space or
  `FAST5_AUTO_MENU=1`—the **main menu** (`build/auto_menu.png`); no guest fault or host GL error at
  about 30–35 FPS on ANGLE/D3D11. The key rendering fix was the bionic ctype table:
  `_tolower_tab_` is 16-bit, so the previous 8-bit table made `guessShaderVertexAttribute` return
  255 and collapsed all vertex attributes onto location 0. `guest_data.c` now writes 16-bit ASCII
  tables and `gl_win32.c` deduplicates attribute declarations. The two `GL_INVALID_OPERATION`
  reports were non-program `glGetProgramInfoLog` objects 112/115; the bridge now ignores them.
  Texture work: `overrides.c` rewrites the 14 Favelas `.PVRTC2` names to full-header `.pvr` aliases
  in `build/overlay`; `gl_win32.c` converts BGRA/BGR uploads to RGBA/RGB. The menu car body was
  binding the font atlas on 3D FBO passes; the bridge now prefers the tracked
  `Highline_H3_1972_Body` texture (fallback: last `*_Body*.tga`) and flips body TGA rows
  (descriptor `0x08`), fixing the red Highline body/wheels (`build/final_body.png`). The Favelas
  scene/background binding and reference-livery comparison remain; `service_smoke`/`service_dust`
  remain missing effect data. Full handoff: `docs/05-STATE.md`.
- **Runtime modules:** `runtime/vfs.c` (host-backed read-only data tree, writable user dir, derived
  overlay), `runtime/guest_data.c` (bionic ctype/tolower/toupper tables),
  `runtime/java_bridge.c` (version/device/language/sleep/getIMEI/license no-ops). `tools/patch_guest.py`
  patches the libc GOT slots; `tools/add_missing_functions.sh` recovers functions only reachable
  through tables (**8 recovered**, including `0x553208` in the PVRTC path and the vtable method
  `0x552e8c`).
- **Known limitation:** PLT stubs that Ghidra merged into function bodies are excluded from the body
  and routed to `call_import`; unknown imports are logged once and return 0. Missing string imports
  were a real bug: `strncmp` returned 0, which made the PVR parser take the `"BTEXpvr"` branch and
  reject every texture. `strncmp`/`strcasecmp`/`strncasecmp`/`strchr`/`strrchr`/`strstr` are now
  implemented.

Current scope / limits:

- Integer ops, flags, branches, direct calls, returns; memory loads/stores are bounds-checked.
- VFP/float ops: arithmetic, compares, NaN, abs/neg/sqrt, int↔float and float↔float conversions,
  trunc/ceil/floor/round. The game is VFPv3-D32 with no NEON.
- ARM userops are named in the dump; `setISAMode` is a no-op for this ARM-only target,
  `VectorSignedToFloat`/`VectorUnsignedToFloat`/`VectorFloatToSigned`/`VectorFloatToUnsigned` are
  translated. Unknown userops emit an explicit `/* ... not yet translated */`.
- Calls go through `call_target` / `call_indirect` hooks; the global function table is generated by
  `link_all.py`, so direct and indirect calls resolve across the whole binary.
- Differential tests compare r0–r3 and a memory hash; guest faults are compared by address.
  With `--float-scratch`, any NaN is treated as equal because x86 SSE and ARM VFP pick different
  NaN signs/payloads (documented host difference, not a translation defect).
- VFP rounding modes from FPSCR are not modelled yet; current conversions use round-to-nearest.

## Generated-code policy

- `build/link/` is disposable generated output. Regenerating it never touches `runtime/`,
  `analysis/reports/pcode_extra.tsv`, or the shipped data. Never hand-edit generated chunks; a
  regeneration will correctly overwrite them.
- **Hidden functions:** recover them with `tools/add_missing_functions.sh` into
  `analysis/reports/pcode_extra.tsv`. `link_all.py` merges that file automatically. No edit to the
  generated C is needed, so future recoveries survive every regeneration.
- **Host behavior:** window/GL/import/VFS/diagnostics live in `runtime/`. They are compiled and
  linked outside the generated tree.
- **Host overrides:** if a translated guest function must be replaced, handled, or accelerated on
  the host, implement it in `runtime/overrides.c` via `call_override(c, mem, target)` and return 1.
  `call_target` checks that hook before the generated switch. Returning 0 falls through to the
  translated body.
- **Diagnostics:** keep them in runtime code behind `FAST5_GL_TRACE=1` (or similar), never in
  generated output.
- **Regeneration:** `python recomp/link_all.py analysis/reports/pcode_all.tsv --emit-only` rewrites
  only changed files under `build/link/` (unchanged chunks keep their mtimes) and removes stale
  generated chunks; `python tools/build_win.py` then rebuilds only stale objects and relinks when
  needed. Use `python tools/build_win.py --force` for a full recompile.

Rules:

- The emitter is the only place that turns machine code into source. Ghidra's decompiler is an aid,
  never the generator.
- Every translation must earn trust through `diff_test.py`; no function is "obviously fine".
- Generated files carry the source address; nothing here reads or writes the game data.
