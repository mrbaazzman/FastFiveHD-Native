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
| `resolution` | The presentation transform: the guest's 2D canvas is 800×480, the window may be any size |


## Presentation model

The engine runs at the **window** resolution (1920×1080 by default), so its device, render
targets, camera and scene are native.

The game's 2D layer is **not** resolution-independent: it is authored on a fixed 800×480
canvas. (`Application::Init` computes a logical size of `screen/scale`, and the HUD anchors
itself to `SCR_WIDTH`/`SCR_HEIGHT`, but the 2D *screens* — the Universal logo, the loading
widgets — are positioned for the reference canvas and do not re-centre. `FAST5_UI_2D=native`
lets the engine lay the 2D layer out for the window instead; the logo then lands in the corner,
which is what the shipped game does on a large screen, and is why it is not the default.)

So the port keeps the guest's 800×480 canvas exactly as the reference device has it — same
coordinates, same hit rectangles, same 3D picking — and converts it to the window with **one**
affine transform in `resolution.c`:

| Consumer | Where | What the transform does |
|---|---|---|
| 2D layer | `overrides.c` → `CCommonGLDriver::draw2DRectangle` | design-space rect → window |
| 2D clip | same hook | scaled for the duration of the draw, then restored (persistent engine state) |
| 3D picking | `overrides.c` → `CMenuManager::GetRayFromScreenCoordinates` | design space → window |
| Pointer | `platform_win32.c` → `queue_pointer_event` | window → design space (the guest's touch space) |
| Scissor | `gl_win32.c` → `glScissor` | design space → window (design-engine mode only) |
| Viewport | `gl_win32.c` → `glViewport` | the engine's screen area → the whole framebuffer (design-engine mode only) |

Because every consumer uses the same transform, they cannot drift apart: a button, its hit
rectangle and the 3D pick under it always agree.

### How the 2D layer is fitted

The canvas is 5:3 and the window usually is not (16:9 at 1080p), so something has to give.
`FAST5_UI_FIT` picks what:

| Mode | What it does | Result at 1920×1080 | Cost |
|---|---|---|---|
| `fill` (default) | each axis scaled separately | 2.4× / 2.25× → the canvas covers the window, the HUD reaches the screen edges | the artwork and the text are 6.7 % wider than the reference |
| `crop` | one uniform scale, big enough to cover the window | 2.4× → 1920×1152, 36 px cut off top and bottom | the top of the GARAGE title and the bottom of the stat bars are clipped |
| `letterbox` | one uniform scale, small enough to fit | 2.25× → 1800×1080, centred | the HUD is inset 60 px (3 %) from the screen edges |

The canvas is 5:3 and the screen is 16:9, so no single affine map can both fill the window and
keep the aspect ratio — one of the three has to give.

`FAST5_ENGINE_RES=design` runs the engine at 800×480 instead of the window size (the render
targets are then design sized, so the road reflection is upscaled).

### The engine's own resolution independence — and why it is not used by default

`FAST5_UI_2D=native` lets the engine lay the 2D layer out for the window instead: the guest is
told the real screen size, the artwork is then drawn 1:1 (crisp at 1080p) and the HUD follows the
screen edges.  It is **not** the default, and the reason is measured:

* The game's 2D screens are composed for an 800×480 canvas.  The Universal logo is at
  `(143, 124)` in the guest's screen space at *every* window size, so it is centred in an
  800-wide screen and in the top-left corner of a 1920-wide one.  The "ARE YOU SURE?" dialog
  of the car select lands in the corner for the same reason.
* The artwork is 800×480 pixels (the game ships a 400×240 set plus one 2× "highres" set), so
  filling a 1920×1080 window means magnifying it 2.25×.  There is no larger art to draw from.

So the two are mutually exclusive with this game's assets: **crisp 1:1 artwork** means the
canvas keeps its size, and **a correctly composed screen** means the canvas fills the window.
The default (`design`) keeps the composition and magnifies; `native` keeps the pixels and
mis-composes the screens.  The 3D scenes are composed for the canvas as well — that is why the
logo is where it is — so the host cannot re-centre it with a 2D transform.

One genuine bug did come out of the experiment: the host forced the window's aspect ratio onto
**every** camera, including the off-screen ones.  The engine computes each camera's aspect from
its own render target, so the override is only needed when the engine itself runs at the design
resolution (`FAST5_ENGINE_RES=design`); it is now applied only there.

The port checks the transform at start-up: the canvas must land inside the framebuffer and the
frame → design → frame round trip must be the identity (`resolution: invariant …` in the log).

Contract:

- No dependency on the game data other than reading it.
- No emulator, interpreter or JIT.
- Every function here has a written reason to exist; unused shims are deleted.
