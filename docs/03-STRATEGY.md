# 03 — Strategy

The decision, the alternatives rejected, and the engineering consequences.

## 1. The requirement

A native Windows x64 build that reproduces the Android build 1:1, with no emulator, interpreter,
JIT or middleware at runtime, using the original data files untouched.

## 2. Why the obvious approaches fail

| Approach | Why not |
|---|---|
| Android emulator / Android-x86 / Waydroid | explicitly excluded; that is the emulator path already tried |
| Box64 / QEMU user / ARM JIT | it is a runtime translator (an emulator in all but name); forbidden by R7 |
| Hand reimplementation of gameplay | cannot be 1:1; 20k functions of C++ game logic would take years and still diverge |
| Rewriting the engine (Irrlicht fork) from public sources | engine is only half the binary; game logic and Glitch modifications are not public |
| Recompiling DEX to x86 and keeping the `.so` | the Java layer is a thin shell; all logic is in the ARM `.so` |

## 3. Chosen approach — static recompilation (ARMv7 → C → x64)

This is the technique behind the "recomp" ports the requirement refers to: translate the shipped
machine code ahead of time into portable source, then compile it natively with a small runtime that
provides the platform the code expects.

### 3.1 Pipeline

```
libFast5.so
   │  ELF loader: symbols, relocations, sections, exidx/extab, init_array
   ▼
Ghidra (analysis + P-code per instruction, ARM and Thumb)
   │  our emitter: P-code → C
   ▼
C translation units (one per original namespace/module, function-named)
   │  + ABI shims (softfp, VFP register file, guest address space)
   ▼
clang/gcc (x86-64) ──► fast5.exe  +  runtime.lib (Win32/GL/audio/FS)
```

### 3.2 Fidelity decisions (why this can be 1:1)

- We translate **the shipped instructions**, not a decompiler's C-like reconstruction. Ghidra's
  decompiler is used for human understanding and for validation, never as the code generator.
- Guest memory is preserved with its original layout: a reserved 1 GiB region holds the image at its
  link addresses. Every load/store keeps its original address, so absolute pointers in `.data`,
  vtables and RTTI stay valid without fixups.
- Register file and flags are modelled exactly (`NZCV`, 16 core registers, `s0–s31`, `d0–d31`,
  PC semantics of ARM vs Thumb).
- Floating point: VFPv3 semantics with single/double distinction preserved; softfp argument passing
  is modelled in the call boundary, so C++ ABI behaviour matches.
- C++ exceptions: `.ARM.exidx` is parsed into a PC-range → landing-pad table; our runtime unwinder
  walks translated frames and transfers control exactly as the ARM unwinder would.
- Function identity: every translated function keeps its original symbol; indirect calls resolve via
  the guest address space, so vtables and function pointers work unchanged.

### 3.3 The platform layer we must write (allowed, part of the port)

`runtime/` — a small, documented C library:

| Module | Responsibility |
|---|---|
| `mem` | reserve/commit guest address space, guards, image mapping |
| `regs` | register file, flags helpers used by generated code |
| `abi` | softfp call conventions, varargs, `__aeabi_*` helpers |
| `eh` | exidx parser, unwinder, `__cxa_throw/rethrow/begin_catch` |
| `thread` | pthread subset over Win32, TLS, once/cond/mutex |
| `time` | `gettimeofday`, monotonic clock, deterministic timer |
| `fs` | path redirection into the immutable data tree, `fopen`/`open`/`stat` subset |
| `gl` | GLES2 entry points over desktop GL 3.3/4.1, shader translation, PVRTC path |
| `win` | window, message pump, resize, focus, keyboard/mouse mapping |
| `audio` | `GLAsyncPlayer` replacement over WASAPI/XAudio2 |
| `jni` | minimal JNI surface the native code touches (`_JNIEnv` calls), Java object stubs |
| `diag` | logging (`__android_log_print`), trace capture, deterministic replay |

Generated code calls these; nothing else is "middleware".

### 3.4 Risks, with the mitigation built into the plan

| Risk | Severity | Mitigation |
|---|---|---|
| Exception unwinding correctness | high | proof gate M1.3 (throw across 3 frames); differential tests |
| ARM/Thumb decoding edge cases | medium | Ghidra P-code is the decoder; we never write an ARM decoder |
| Unresolved indirect jumps (3,548 in naive scan) | medium | resolve at runtime through the guest address map; table-driven jump targets |
| C++ ABI details (vtables, RTTI, guard variables) | medium | keep them as data; only code is translated |
| GLES2 → desktop GL equivalence | medium | mechanical shader translation; frame-diff gate |
| Audio timing | low | original mixer runs translated; only the sink is ours |
| Performance | low | direct C, no interpretation; guest memory is native pointers |
| Legal/redistribution | n/a for personal port | no game data is ever committed or redistributed |

### 3.5 Proof gates before scaling

M1 ends with a **differential harness**: the same exported functions are called on ARM (offline
oracle under qemu-user in WSL) and on the translated x64 build, with identical inputs, comparing
return values and memory. The gate is 200 functions across these classes:

1. integer/bitfield arithmetic, 2. VFP single/double math, 3. struct-by-value calls,
4. virtual dispatch through a real vtable, 5. switch/jump table, 6. function pointers,
7. C++ throw/catch across frames, 8. static constructors (`init_array`).

If the gate does not pass, the strategy is revised **before** any further translation work.

## 4. Toolchain (fixed for reproducibility)

| Purpose | Tool | Notes |
|---|---|---|
| Analysis host | WSL Ubuntu 24.04 (`/mnt/c/...`) + REA 6.8.0 + Craft Anything 0.4.0 | shares this workspace |
| Decompiler / P-code | Ghidra 12.1.4 (installed, verified) | headless, via REA |
| Host compiler | MinGW-w64 GCC 13 (Windows) and/or clang in WSL | C11/C++17, x86-64 |
| Build | CMake + Ninja | one build graph, two presets (`debug`, `release`) |
| Scripts | Python 3.12 | ELF/symbol/exidx tooling |
| Diff oracle | qemu-user (WSL, offline only) | never shipped |

## 5. What we do NOT do

- No asset conversion pipeline as a requirement.
- No hooking, no patching the original APK.
- No reliance on Android services at runtime.
- No "close enough" reimplementation of gameplay formulas.
