# runtime/ — native Win64 platform layer

This is part of the port, not middleware: it provides only the platform the shipped engine expects.

| Module | Responsibility |
|---|---|
| `mem` | Reserve/commit the guest address space, map the image, guard pages |
| `regs` | Register file and flag helpers called by generated code |
| `abi` | softfp calling conventions, varargs, `__aeabi_*` helpers |
| `eh` | `.ARM.exidx` parser, unwinder, `__cxa_*` implementation |
| `thread` | pthread subset over Win32: create/join/self, TLS, mutex, guest stack/CPU |
| `time` | `gettimeofday`, monotonic clock, deterministic timer |
| `fs` | Guest path → project data tree mapping (read-only), POSIX file calls |
| `gl` | GLES2 entry points over desktop GL, shader translation, PVRTC handling |
| `win` | Window, message pump, resize, focus, keyboard/mouse/gamepad mapping |
| `audio` | `GLAsyncPlayer` replacement over WASAPI/XAudio2 |
| `jni` | The JNI surface the native code actually uses, Java object stubs |
| `diag` | `__android_log_print`, trace capture, deterministic replay |
| `resolution` | The presentation transform: the guest runs at the design resolution, the window may be any size |

## Presentation model

The engine runs at the **window** resolution (1920×1080 by default), so its device, render
targets, camera and scene are native. The game's 2D layer is authored in a fixed 800×480 space;
the port converts the screen-size globals the game reads for 2D layout back into that space
after the init, so every 2D coordinate, hit rectangle and 3D pick stays in one space.
`resolution.c` holds the single affine transform between the two:

| Consumer | Where | What the transform does |
|---|---|---|
| 2D layer | `overrides.c` → `CCommonGLDriver::draw2DRectangle` | design-space rect → window |
| 3D picking | `overrides.c` → `CMenuManager::GetRayFromScreenCoordinates` | design space → engine screen |
| Pointer | `platform_win32.c` → `queue_pointer_event` | window → design space (the guest's touch space) |
| Scissor | `gl_win32.c` → `glScissor` | design space → window (design-engine mode only) |
| Viewport | `gl_win32.c` → `glViewport` | the engine's screen area → the whole framebuffer (design-engine mode only) |

`FAST5_UI_FIT=fill` (default) scales each axis so the 2D layer covers the window;
`FAST5_UI_FIT=letterbox` scales uniformly and centres, so nothing is distorted.
`FAST5_ENGINE_RES=design` runs the engine at 800×480 instead of the window size.

Contract:

- No dependency on the game data other than reading it.
- No emulator, interpreter or JIT.
- Every function here has a written reason to exist; unused shims are deleted.
