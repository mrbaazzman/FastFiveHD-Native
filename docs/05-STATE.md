# 05 — Live State

**Read this first when resuming.** Update it at the end of every work block (rule R5).

> Paths such as `build/final_check.png` refer to captures in the **local** `build/` directory.
> They are not committed: `build/` is disposable, and the images show the game, which is not
> part of the published repository.  `tools/run_test.py` regenerates the captures on a machine
> that has the game.

## The published repository

`public/` is the tree that gets published.  It is a **clean subset**: the recompiler
(`recomp/`), the native platform layer (`runtime/`), the tools, the analysis helpers and the
documentation — and nothing that is derived from the shipped game.

| Published | Kept local only (never committed) |
|---|---|
| `recomp/`, `runtime/`, `tools/`, `docs/`, `analysis/` | the APK, `libFast5.so`, `GloftF5HP/` |
| `LICENSE` (MIT), `NOTICE.md`, `README.md`, `SETUP.md` | `build/link/` — the 329 MB of generated C |
| `docs/diagrams/architecture.svg` | the P-code dumps, the Ghidra project, the frame captures |

`tools/make_public_repo.py` assembles it from this working copy.  It was verified end to end:
the published tree was built, linked and run against the game, and the resulting frames are
**pixel-identical** to this working tree (mean absolute difference 0.0000 over frames 30/120/240).

## 2026-11-11 — Full HD: the game presents correctly at 1920×1080 (current)

### The rule

The guest engine runs at the **window** resolution, so its device, render targets, camera and
scene are native 1080p. The game's 2D layer, however, is authored in a fixed 800×480 space, so
the host keeps that space intact: the screen-size globals the engine reads for 2D positioning
are converted back into it, and the host scales the 2D draws, the pointer and the 3D picks
from there to the window. One space for the game, one transform for the host (rule R6).

### What the shipped game actually does (measured)

| Question | Answer | Evidence |
|---|---|---|
| Where is the 2D layer laid out? | A fixed 800×480 pixel space | `draw2D` traces at 800×480 and at 1920×1080 are identical (`dest=298,321 36x51`, `dest=162,325 52x41`, …) |
| How is the 2D projection built? | From the render target's size → 2D coordinates are render-target pixels | `CCommonGLDriverBase::set2DProjection` (`ev_7b3a8a3c`) |
| Where does the HUD anchor itself? | To `SCR_WIDTH`/`SCR_HEIGHT` as ratios | `HudManager::InitButtons` (`ev_a9c3112a`) |
| Where does the legal text anchor itself? | To the application's *logical* size (`Application+0x250/0x254`) | `GS_UniversalLogo::StateRender` (`ev_f529d110`) |
| What aspect does the camera use? | The render target's, 5:3 (measured 1.6667) | `Camera::Camera_FixFov` (`ev_be7c5d2b`), `CCameraSceneNode::setAspectRatio` (`ev_68a1b22f`) |
| How does the touch reach the game? | Screen pixels; the engine divides by the app scale and the 2D menu multiplies it back | `CMenuScreen::ScreenCoordsToProjectedScreen` (`ev_c7138c8`) |

### The host's presentation transform (`runtime/resolution.c`)

One affine transform, shared by every consumer, so they cannot drift apart:

| Consumer | Where | What it does |
|---|---|---|
| 2D destination rects | `overrides.c` → `CCommonGLDriver::draw2DRectangle` | design space → window (or → the render target's own size for an off-screen pass) |
| 2D clip rect | same hook | scaled for the duration of the draw, then restored (it is persistent engine state) |
| 3D viewport | `gl_win32.c` → `glViewport` | the engine's screen area → the whole framebuffer |
| Camera aspect | `overrides.c` → `setAspectRatio` | the window's aspect, so the 3D is not stretched |
| Pointer | `platform_win32.c` | window → design space (the guest's touch space) |
| 3D picking | `overrides.c` → `GetRayFromScreenCoordinates` | design space → window (the ray works on the engine's screen) |

`FAST5_UI_FIT=fill` (default) scales each axis so the 2D layer covers the window;
`FAST5_UI_FIT=letterbox` scales uniformly and centres, so nothing is distorted.
`FAST5_ENGINE_RES=native` runs the engine at the window size instead (window-sized render
targets, sharper road reflections) — but the menu's off-screen 2D screens then land wrong.

### The bugs this work uncovered

1. **The capture tool was lying.** `write_bmp` wrote the `glReadPixels` rows in reverse; GL
   returns them bottom-up and a BMP stores them bottom-up, so *every* `build/game_frame_*.bmp`
   was vertically mirrored. The window capture was the only trustworthy one.
2. **The soft-float ABI passes a float in a core register.** `CCameraSceneNode::setAspectRatio`
   stores `c->r1` into `this+0x140`; the first version of the override wrote `vfp[0]` and
   changed nothing. Proof: `res: camera aspect 1.6667 -> 1.7778`.
3. **The clip rect is persistent engine state, not a per-draw temporary.** Rewriting it in
   place shrank it a little on every draw until the 2D layer disappeared
   (`0,0 800x480 → 25,0 750x480 → 48,0 703x480 → …`).
4. **The clip has to be scaled with the destination.** The engine clips the destination
   against its own screen clip, which lives in the design space; a scaled destination outside
   that clip was culled away. The clip is now scaled for the duration of the draw and restored
   afterwards — this is what brought back the garage's Performance tab.
5. **`nativeExit` (ELF `0x29a7e4`) faulted the port** — it dereferences the uninitialised
   JNIEnv. The port answers it now, like the other Java callbacks.
6. **`Application::Init` keeps `this` in the first argument register** (the generated code
   moves it around), and the engine destroys and re-creates the application during the boot, so
   the live instance has to be remembered.
7. **`Application::s_pInstance` is the only authoritative pointer to the live application**
   (ELF `0x7f8758`, `_ZN11Application11s_pInstanceE`). The port had been reading the instance
   out of the hook's argument register, and its fallback address `0x8B85A0` turned out to be
   `mbAppRunning` — a *flag*, not a pointer. Reading a flag as a pointer produced the
   `logical=0x0` in the diagnostics that looked like a broken conversion. The conversion now
   reads the engine's own singleton, and the diagnostic reads 400×240 at every frame, as it
   should. The rendering is pixel-identical before and after the change (mean absolute
   difference 0.0000 over frames 30/120/240), so the earlier behaviour was accidentally right.

### Verification

* `build/final_check.png` — one 1920×1080 run: the Universal logo **with** its licence text,
  the garage with every panel (Performance/Appearance, Fever A4, the arrows, BACK, the stat
  bars, OK), the menu, and the Rio race with the HUD.
* `build/hd_120.png`, `build/hd_1200.png`, `build/hd_2400.png`, `build/hd_4800.png` — the same
  frames individually.
* `build/sd_1200.png` — the garage at 800×480 for comparison: identical layout, native size.
* `tools/click_test.ps1` — a click at window (1500, 375) reaches the guest as design
  (625, 166); the 3D pick is converted back to window pixels.
* No guest fault, no host exception in any run.

### Still open

* The window size cannot be changed while running (the EGL surface is created once).
* The road reflection and the post-effect buffers are window sized in native mode; in
  `FAST5_ENGINE_RES=design` they stay design sized and are upscaled to the window.

## 2026-11-09 — native texture path: cars, road, live reflections

The remaining graphical defects were host compensations, not translation errors in the texture
code. They are removed and the engine's own paths are the default.

**1. Car body — shipped high-memory NOPVR path.** `DeviceConfig::SetupGameForDevice` hardcodes
`s_DeviceType = 0xB` (I9000) and `s_GPUType = 0` (`ev_f34ba6f9`). `MenuScene::LoadCars`
(`ev_5f0aa64c`) and `TrackScene::LoadCars` (`ev_5c0455df`) build `SRaceCarTunning`; byte +1
(car `+0x17d1`) is `m_bIsHighMemDevice == 0`, i.e. **0** on the reference profile. `RaceCar::RaceCar`
copies the 13 tuning bytes at `+0x17d0` (`ev_7079f431`), and `RaceCar::InitCar` (`ev_9689a96a`)
passes +0x17d1 into `InitBodyTexture` (`ev_f944967d`), selecting `*_Body.NOPVR.tga` /
`*_mask.NOPVR.tga`. Forcing `+0x17d1 = 1` switched every car to the low-memory PVR branch, which is
what produced the green/black menu car. The override is deleted.

**2. Sampler binding — engine group 2 owns it.** `commitCurrentMaterialImpl` commits only direct
parameters (`ev_1fe0e1bc`); `drawImpl` (`ev_0a885402`) calls `commitPassParameters`
(`ev_ab4a4c66`), which calls `commitCurrentMaterialIndirectParameters` (`ev_a82c309d`) for the
texture group. The old host `gl_bridge_force_sampler` bound textures directly, bypassing the
engine's per-unit texture cache. After the engine dropped a texture and reset its GL name to 0,
`CTexture::bindImpl` (`ev_1c85e8a1`) skipped regeneration because its cache still said the texture
was current; the surface then sampled the placeholder/black. All host sampler/default/mask code is
deleted. Verified: the Cranky body keeps GL name 95 stable across frames and no `forceSampler`
unresolved message can occur.

**3. Live reflections — native pass every frame.** `PostEffects::PreDraw` (`ev_656c12c9`) and
`PostDraw` (`ev_38024d2f`) return early in `GS_Race` while the active effect equals the last-rendered
one; the gate is `FUN_003a3978` (`ev_1ee4b084`). Every car batch samples `GetPrevRT`
(`ev_f58ab4e3`) as `ReflSampler`, so the frozen RTTs are the roof/hood artifact. The gate is now
bypassed by default (`FAST5_LIVE_REFLECTION=0` restores the shipped skip); the engine's own
PreDraw/PostDraw/GetPrevRT run every frame. Verified in `build/native_default.log`: over 1501 frames,
`preDraw=1501`, `postDraw=1500`, `getPrevRT=4507`, no fault; six RTT dumps at frames
1500–3900 have six different SHA-256 values and show city content.

**4. Parameter interning — engine lookup is correct.** `CMaterialRenderer::getParameterID`
(`ev_de5c4e41`) compares interned `SData*` pointers; the heap search is `SData::get`
(`ev_6770c48a`) / `FUN_0060b1ec` (`ev_57f72847`). A probe over a full menu load measured **0
pointer conflicts in >40k lookups**: the shared-string heap is intact. The strcmp fallback at ELF
`0x52157c` and its probe are deleted.

**5. Body row flip and mask default removed.** `CImageLoaderTGA::loadImage` (`ev_8f1dfa48`) does not
inspect the TGA descriptor; it preserves the file's row order. Body TGAs use descriptor 0x08
(bottom-left), which is the row order GL expects, so the host flip was a double flip. Removed. The
host red 1×1 `MaskSampler` default is also removed; the native-default menu has zero
bright-green placeholder pixels.

**6. Other removed host workarounds.** Driver-type forcing (`COpenGLES2Driver::getDriverType`
returns 8, `ev_9ac1dea0`; forcing 1 was wrong), `CGenericBaker` batch-cache invalidation,
`CGlobalMaterialParameterManager` ID normalization and grab guard, and material/pass shader
normalization are deleted. The native-only menu and native-default race runs had no guest fault, no
GL error and correct car textures.

**Remaining data-variant adaptation (not a rendering patch).** The engine suffix table is `.TGA`,
`.NOPVR.tga`, `.PVRTC4.tga`, `.PVRTC2.tga` (`ev_7535bb6c`, `ev_0ac6792d`, `ev_9ec6a571`); the
installed data tree stores the Favelas/menu assets as `Track/Favelas/*.LINEAR.tga` and `mm_*` names.
`runtime/overrides.c` resolves those requested PVRTC names to the shipped `.LINEAR` path at the
CFileSystem entry, but only after probing that the target exists in `build/overlay/` or
`GloftF5HP/res/`; unrelated names (e.g. `DESERT_*`) are left untouched so the engine's own suffix
fallbacks run. This is a data-variant resolver, not a sampler override; moving it into `vfs.c` is
optional cleanup. `sam_dark`, `sam_difuse` and `civilian_coste` remain unresolved in race logs
(character atlas references); they are not records in the sampled texture archives, so they need a
`CResFileManager`/atlas trace, not a sampler workaround. Verified in `build/alias_safe.log`: menu
load with `FAST5_TEXTURE_TRACE=1`, zero missing textures, all rewrites to existing Favelas assets,
no guest fault and no GL error.

**Effects audit (2026-11-09).** The APK has **no** `MotionBlur` symbol or string. `PostEffects`
creates `RadialBlur` (index 8) and `RadialBlurMap` (index 9); the latter also loads `blur_map.tga`.
Only `GP_RaceNormal::UpdatePostEffects` activates effect 9, and only when
`DeviceConfig::s_GPUType == 1`; `SetupGameForDevice` sets it to 0 on this I9000 build, so normal
driving does not use radial blur. `GP_RaceCrash::ExecuteGPInit` activates effect 14 (Desaturate) and
`GP_RaceCrashAI::ExecuteGPInit` effect 15 (DesaturateTakeDown); both are shipped crash effects.
Slow-motion (`Game::SlowMotion_*`) changes the time scale, not pixels. The port adds and forces no
effect; the only gate change is the live-reflection pass above. Evidence: `ev_85730c06`,
`ev_b421aad9`, `ev_d2d3493b`, `ev_3bfeab4e`, `ev_4d213710`, `ev_e107f235`, `ev_4970ec58`,
`ev_d692a3d8`, `ev_db6844c7`, `ev_afd89e0a`, `ev_003df835`, `ev_ba0cb49c`, `ev_c397a3c4`.

**Character texture references (2026-11-09).** `Objects/start_chick_rio.bdae` names
`girl_color.tga`, `afro_female.tga`, `sam_dark.tga`, `sam_difuse.tga` and `civilian_coste.tga`.
`objectsTextures.zip` (`file000247.dat`) contains `Data/#temp/ObjectsTga/afro_female.tga` (first
record, offset 0x34) and `girl_color.tga`, and both load in the race. No `.dat` contains a record
for `sam_dark`, `sam_difuse` or `civilian_coste`, and the BDAE has no embedded image data (no
PVR/JPEG/PNG/DDS magic). The engine therefore tries every suffix and logs the miss exactly as the
shipped code does; these are absent alternate/unused material maps, not missing visible textures.
Adding them would be non-1:1. Evidence: direct byte scans of all 653 `.dat` files and the nested
BDAE; the visible female materials resolve to the shipped `afro_female`/`girl_color` textures.

**Native 800x480 (2026-11-10).** The window is a DPI-aware, standard 800x480 client window;
`FAST5_WIDTH`/`FAST5_HEIGHT` can override the size and it clamps to the monitor. `main_win32.c`
passes the platform size to `appInit`; the engine renders at its original resolution. All Full HD,
2D/menu upscaling, hit-rectangle remapping and static-image 2x experiments were reverted. Test runs
set `FAST5_TEST_MINIMIZED=1` so the window stays hidden while the client/EGL surface remains 800x480.
`build/game_frame_1320_native.png` is the native main menu. Normal `play.bat` opens the 800x480
window.

**Sections below are historical** and describe the workarounds that have now been removed.

## Audio — RESOLVED (2026-11-08): the game has sound

Three independent defects each produced total silence. All three are now fixed and verified in both
the menu and a race, with 0 guest faults.

**1. The mixer thread never started.** `vox::DriverAndroid::_InitAT` (ELF `0x68963c`) performs
*everything* — including `pthread_create(UpdateThreadedAT)` — inside `if (s_javaVM != NULL)`. The port
has no JavaVM, so nothing ran at all. `_InitAT` is now replaced: it opens a Win32 **waveOut** device
(44100 Hz, stereo, 16-bit, 4 × 1024-frame buffers) and starts a host pump thread that calls the
shipped mixer on a private guest stack and queues each block to waveOut. No JNI is involved.

**2. No source was ever attached to the driver.** `_InitAT`'s replacement set the driver fields
`+0x54`, `+0x58`, `+0x5c`, `+0x60` but not **`+0x08`**, the "started" byte. The shipped
`UpdateThreadedAT` (`ev_8b52dd8b…`) sets `driver+8 = 1` only after the audio track is constructed, and
`DriverCallbackInterface::FillBuffer` (`ev_9b9f77dd…`) is literally:

```c
Mutex::Lock(this + 4);
if (this[8] != 0) _FillBuffer(this, out, frames);
Mutex::Unlock(this + 4);
```

So with `+8` clear the mixer was called but did nothing at all. Evidence of the failure: the driver's
source list at `driver+0x10` stayed **EMPTY** for the whole run while the engine happily created 480
emitters and played 24 of them. Setting `driver+8 = 1` made the list non-empty immediately.

**3. The driver sample rate was never set.** The shipped `_InitAT` calls
`DriverCallbackSourceInterface::SetDriverSampleRate(44100)` *before* the JavaVM check, so on Android it
always ran; the replacement skipped it. Every source derives its resample step from that rate —
`step = doppler * (pos * pitch >> 14) >> 14` (`ev_436dcbfe…`) — so with a zero rate the sources walked
their sample data with a nonsense stride. Symptom: the mixer emitted a smooth **positive DC ramp**
(mean ≈ rms, **zero crossings = 0 per second**) instead of audio, which is silent and inaudible. After
calling `SetDriverSampleRate(44100)` the same dump shows real AC audio (mean ≈ 0, 5k–25k zero
crossings/s).

**Correction to an earlier conclusion in this file.** `SoundManager::Update` (ELF `0x35eef8`) is *not*
missing: its only callers are `GS_Race::StateUpdate` (`ev_d00e6d61…`) and `GS_Tutorial::StateUpdate`
(`ev_61762700…`), so it correctly runs in a race (measured 2437 calls) and not in a menu — it only
moves the 3D listener and does not tick the engine. `vox::VoxEngine::Update` has no callers at all
(`ev_732eece6…`); the engine is driven by the two `VoxThread`s that `VoxEngine::Initialize` creates
(`ev_f66b5372…`, `UpdateEmittersThreaded` / `UpdateSourcesThreaded`), and both were measured running
(~5000 iterations per session). Calling `SoundManager::Update` from the host — the approach attempted
in the previous session — was therefore never the fix and both attempts were reverted.

**Also needed:** call `DriverCallbackInterface::FillBuffer` (ELF `0x68d008`), not the inner
`_FillBuffer` (`0x68d04c`), so the driver mutex is taken.

**Verified:** menu and race both produce continuous audio; `waveOutWrite` returns 0 with 0 errors,
`prepErr=0`, device is the Microsoft Sound Mapper; heap at f3480 is identical before and after the fix
(live ≈ 38 MiB), so no audio leak; no `audio:` lines in the log during normal play.

**Diagnostics** (all off by default, no cost in normal play): `FAST5_AUDIO_TRACE=1` logs per-block
peak/loud-block ratio/source-list state; `FAST5_AUDIO_DUMP=1` writes `build/audio_dump.wav` with
exactly the bytes handed to waveOut; `FAST5_AUDIO_TESTTONE=1` replaces the mix with a 440 Hz sine — the
sink self-test that proved the waveOut path was audible while the mix was still broken.

Signatures:

| Symbol | Ghidra | ELF |
|---|---|---|
| `vox::DriverAndroid::_InitAT` | `0x69963c` | `0x68963c` |
| `vox::DriverCallbackInterface::FillBuffer` | `0x69d008` | `0x68d008` |
| `vox::DriverCallbackInterface::_FillBuffer` | `0x69d04c` | `0x68d04c` |
| `vox::DriverCallbackSourceInterface::SetDriverSampleRate` | `0x69ca80` | `0x68ca80` |
| `vox::DriverCallbackSourceInterface::FillBuffer` | `0x69ae5c` | `0x68ae5c` |
| `vox::DriverAndroid::UpdateThreadedAT` | `0x699d28` | inside `fn_698ccc` |
| `vox::VoxEngine::Initialize` | `0x66e17c` | `0x65e17c` |
| `SoundManager::Update` | `0x36eef8` | `0x35eef8` |
| `SoundManager::Play3DInternal` | `0x36d268` | `0x35d268` |

Source list layout: `_FillBuffer` walks a circular list at `driver+0x10` (the sentinel is
`driver+0x10` itself), mixes into a global int32 buffer of `frames*8` bytes, then clamps and
downconverts to int16 stereo (`frames*4` bytes out).




## Save / progress — RESOLVED (2026-11-08): progress and cash now persist

**Symptom:** race progress reset to 0 after quitting and relaunching, and the cash counter never
showed a value.

**Root cause — one bug, both symptoms.** `runtime/vfs.c` wrote saves to `build/userdata/<base>` but
never *read* from there. The read order was `GloftF5HP/res/…` → `GloftF5HP/…` → `build/overlay/…` →
raw path, so `build/userdata/profile1.sav` was invisible and every launch loaded the pristine
shipped profile instead. The game was saving correctly the whole time; the save was then ignored.

**Fix:** reads now consult `build/userdata/<base>` first, so saved data shadows shipped data. The
write side already used the basename, so the two now agree.

**Verified** (`FAST5_DEBUG_VFS=1`): `vfs: …/res/profile1.sav -> build/userdata/profile1.sav` and the
same for `tracklog.dat`. Load path is `Loading profile from path: profile1.sav` → `PROFILE LOADED`.

**Money label was blank — a second, independent bug: `strncat` was not implemented.**
`PlayerProfile::AwardCash` (Ghidra `0x3be460`, `ev_ec3e54df…`) updates `this+0x5c` (cash) and
`this+0x118` (lifetime). `profile1.sav` is a struct dump — the player name `"John Doe"` sits at file
offset `0x10` — and the cash word at `0x5c` reads **0 in the shipped copy** versus **14132 in the
saved copy**.

With correct cash the counter still rendered blank, so the text path was probed directly. A hook on
`FormatMoney` (`0x34c7a4`, `ev_9418201f…`) logged:

```
fmtMoney: val=14132 mode=2 -> '$,'      before
fmtMoney: val=14132 mode=2 -> '$14,132' after
```

`FormatNumber` (`0x35d508`, `ev_7fd5fccabe…`) builds the digits with `sprintf("%d")`, appends them
with **`strncat`**, and writes the group separators with `memcpy`. `strncat` is a real import (12
call sites) that was falling through to the generic no-op, so every digit was dropped and only the
separators survived — `"$,"`, i.e. visually blank. `runtime/imports.c` now implements it
(append at most `n` bytes from `src`, stop at its NUL, return `dest`). The earlier note in this file
claiming the string path was sound was wrong.

**Import audit.** A silent no-op in this table has now caused two real bugs (`atoi` crashed the
cinematic thread, `strncat` blanked every formatted number), so `runtime/imports.c` records the name
of every unimplemented import once when `FAST5_IMPORT_TRACE=1`. A full menu session reports only:
`basename`, `btowc`, `wctob`, `wctype`, `gethostbyname`, `sigaction`, `uname`, `vsnprintf`,
`vsprintf`, `wcslen`. The `vsnprintf`/`vsprintf` pair is the one worth watching — any path that
formats through a `va_list` will silently produce nothing — and is recorded as an open item.



## Graphics — intermittent car-roof/road texture — reflection overrides removed (2026-11-08)

**Symptom (user):** occasionally the car's roof/hood shows the wrong texture for a few moments, then
looks normal again. Car textures are otherwise correct; it happens while moving and in the intro.

**Root cause — the race reflection buffer was frozen.** The car body shader samples `ReflSampler`
unconditionally. Every car material's `ReflSampler` is set once per graphics update to
`PostEffects::GetPrevRT()` (`RaceCar::UpdateShaderParams`, ev_81a24a3b…/ev_0534f8f3…), i.e. the
previous frame's post-effect render target (`RTT_1`/`RTT_2`, created at 1.2× screen size — 960×576
for the 800×480 window — ev_598c79b3…).

Those buffers are only refreshed by `PostEffects::PreDraw` / `PostDraw` (ev_656c12c9… / ev_38024d2f…).
Both begin with a gate (`FUN_003a3978`, ev_1ee4b084…) that returns 1 only when the active state's
name is exactly `"GS_Race"`; inside a race they then return immediately while the active effect id
equals the last-rendered one (`current == default`). `BaseState::StateCreate` resets `default` to
`0x12` (ev_341f365c…), the first race frame renders once and stores `default = current = 0`
(Null/pass-through effect), and **every later race frame skips the whole off-screen pass**. The RTTs
therefore keep the race-entry frame, and every car panel with a strong fresnel term (roof, bonnet,
boot) samples that frozen image — a wrong, warped texture that shifts as the camera and surface
normals move, until a nitro/crash effect activates (`ActivateEffect`, ev_1c66d649…), refreshes the
buffer once, and it looks right again for a moment. Measured before the fix:

```
postfx: … current=0 default=0
fbo_tick: frame=1800 prev_binds=0 ids=
```

i.e. no render-target binding at all in race frames. `GetPrevRT` is correct and returns the *other*
buffer (verified: no FBO is ever sampled while it is bound), so this is not a sampler or feedback
defect.

**Earlier hypotheses disproved.** The unresolved-`RTT_1` stale-binding path and the body-atlas
UV/packing mismatch were both wrong: the body map and mask are correct (the menu/showroom car and the
raw RTT render both show the correct red body), and the artifact changes with the *reflection*
content, not with the diffuse UV.

**Reflection overrides removed (2026-11-08).** Host-side variants were tried against the raw
screen-space reflection: the authored `Car_Body_Reflection_Fixed` map (hid the artefact but was not a
live city reflection), mipmapped/attenuated copies of the RTT (white sheen or black surfaces on the
user's run), a black texel for every `ReflSampler` (turned parts of the car and road black), and
shader-side retunes of the reflection curve/blur (pale car or wrong-looking reflection). All
reflection-specific overrides were removed at the user's request: the engine's own `ReflSampler`
path (raw previous-frame `GetPrevRT()` buffer) is restored, and the race-state gate override at ELF
`0x393978` is removed so `PostEffects::PreDraw/PostDraw` use the shipped "refresh on effect change"
behaviour. There is no reflection context, filtered copy, black texel, static substitution, shader
patch or env switch left in the runtime.

Consequence: the white/purple collage class can reappear, because it is inherent to sampling the raw
screen buffer on those materials. The per-car batch-material fix below is unrelated and stays.

**Fix 3 — per-car batch materials.** `CGenericBaker::getBatchMaterial` keeps one cached material in
the baker and reuses it for every car, so each newly-baked car overwrote the previous car's
`MaskSampler`/`Color1`/`Color2` and body textures (material `0x08cfbe10` carried Cranky and Comet
textures in the same race). The override now forces a cache miss for car-body source materials
(detected by a `MaskSampler` definition), so every car body gets its own batch material. Verified:
0 materials carrying two cars' textures in a race. This is also the documented "next step" for the
shared menu-car batch material bug below.

The batch-material fix announces itself once per session with
`batchFix: car-body batch material cache invalidated …`.

**Diagnostics (off by default):** `FAST5_RTT_DUMP=1` dumps `RTT_1`/`RTT_2` and their FBO contents at
capture frames (`build/rtt_*.bmp`, `build/fbo*.bmp`) and reports per-frame framebuffer binds. The
pre-existing fixes below remain in place and were independently verified:

- `force_sampler` resolves render targets through the texture object's own GL name at `+0x40`
  (`CCommonGLDriverBase::CTextureBase::getGLName`, ELF `0x62a8e0`), with the 8192-entry pointer map
  and name cache as fallback; a race peaks at `texIdPeak = 101`.
- Sampler units advance per sampler slot so no two samplers share one; `loc < 0` is a no-op;
  `gl_bridge_bind_default_sampler()` points the uniform at its own unit.
- `Color1`/`Color2` zero plus a `(255,0,0)` mask default is a legitimate plain-body appearance; the
  dark-green glass is the shipped `HIGHLINE_H3_1972_WINDOWS.TGA` asset.





| Field | Value |
|---|---|
| Milestone | **M6 — game cycle COMPLETE** (2026-11-08, confirmed by the user playing a real session) |
| Phase | The full loop works end to end: boot → menu → race → debrief → back to menu → quit → relaunch, with the player's own save file preserved and cash displayed |
| Last updated | 2026-11-08 |
| Next milestone | lap/finish polish and M5 asset parity |

### Game cycle — what it took

Five work blocks, each measured rather than guessed:

1. **Workspace cleanup** — removed debug dumps; kept `build/overlay`, `build/link`, `build/guest.bin`.
2. **Loading speed** — import time across one menu load 41.1 s → 0.7 s. Sub-millisecond `usleep` was
   the dominant cost (115,614 of 120,217 sleeps); it now yields instead of sleeping. `nanosleep` was
   implemented; the per-import 300-entry linear scan became O(1); `call_override` early-outs on a page
   bitmap; chunks compile at `-O2`. Net 600 → 1560 frames in the same 75 s budget.
3. **Keyboard controls** — built on the game's *touch* scheme, not the accelerometer (the tilt path
   moves the camera with the steering, so it cannot work here). Steering uses the shipped
   `UpdateSteeringOnscreenButtons` bit set, so engine steering rate and physics are unchanged. All
   input is guarded on `car+0x400 == 0` (player only) — writing that byte is what made every car obey
   the keyboard. Nitro is edge-triggered and consumes `player+0x16d4`; the engine's own timer ends the
   boost. Camera and look-back drive `GP_RaceNormal+0x1c`/`+0x1d`, which is what the touch HUD sets.
4. **Audio** — three separate defects, each sufficient for total silence: `_InitAT` did all its setup
   inside `if (s_javaVM != NULL)`; the driver's `+0x08` "started" byte was never set, so the mixer was
   called and did nothing; and `SetDriverSampleRate(44100)` was skipped, making every source resample
   with a nonsense stride and emit a DC ramp.
5. **Save persistence + money** — the VFS saved to `build/userdata/` but never read from there, so
   every launch reloaded the pristine profile; and `strncat` was an unimplemented import, which
   dropped all digits from `FormatNumber` and rendered money as `"$,"`.

### Outstanding, in priority order

| Item | Status |
|---|---|
| `vsnprintf` / `vsprintf` unimplemented | open — any `va_list` formatting silently yields "". Probe recorded in `06-EVIDENCE.md`. Use `FAST5_IMPORT_TRACE=1` to re-audit |
| Rewind (R) | deferred by the user; needs its own hook (`HudItem` click → `RewindManager::AllowStartRewind` → `Game::SetCurrentMenu(0x38)`), not the button-mask path |
| Loading perf | deferred by the user; the remaining ~14 s frame is genuine single-threaded guest CPU in `CColladaDatabase::constructScene` / `fn_5cdb7c` — a codegen problem, not I/O |
| Lap/finish flow, M5 asset parity | next milestone work |

Every diagnostic added during this block is off by default and costs nothing in normal play:
`FAST5_AUDIO_TRACE`, `FAST5_AUDIO_DUMP`, `FAST5_AUDIO_TESTTONE`, `FAST5_TEXT_TRACE`,
`FAST5_IMPORT_TRACE`, `FAST5_DEBUG_VFS`, `FAST5_PROFILE`, `FAST5_TEX_DUMP`, `FAST5_STEER_TRACE`,
`FAST5_PAD_TRACE`, `FAST5_AUTO_STEER`, `FAST5_AUTO_MENU`, `FAST5_AUTO_RACE`.

## Loading stalls — RESOLVED (2026-11-07, measured)

Loading hung for seconds before the first touch, on race load, and when returning to the menu.
Measured with an import profiler (`FAST5_PROFILE=1`, dumps on any frame >250 ms) plus a
`Cpu::pc` sampler. Three independent causes, all confirmed by measurement rather than inspection:

1. **Sub-millisecond `usleep` was the dominant cost.** 115,614 of 120,217 sleeps in one menu load
   asked for *less than 1 ms*. Windows cannot sleep shorter than the timer tick, so
   `Sleep(1)` cost a full millisecond each — over a minute of pure stall. Those calls are the
   engine polling for ready work, so they now `SwitchToThread()` instead of sleeping.
   `timeBeginPeriod(1)` was also added at startup (needs `-lwinmm`) so genuine 4–33 ms sleeps stop
   rounding up to 15.6 ms.
   Import time across one menu load fell from **41.1 s to 0.7 s**.
2. **`nanosleep` was unimplemented** and fell through to the no-op default, returning success without
   sleeping. It is now implemented with the same sub-ms rule. (`select` and `poll` are still no-ops;
   the game was not observed calling them during loading.)
3. **Diagnostic texture dumps ran during normal play.** `glTexImage2D` wrote multi-megabyte BMPs to
   `build/` on the first upload of every window/lightmap/glow/reflection texture — the source of the
   `build_*.bmp` clutter in the repo root. Now gated behind `FAST5_TEX_DUMP=1`.

Also fixed while measuring:
- **PLT lookup was a 300-entry linear scan per import call**, followed by a ~300-way `streq` chain.
  The generated table is densely packed at stride 12 from `0x297d4c`, so the slot is now arithmetic
  (`find_import`). `memcmp` (45M calls/load), `memcpy`, `memset`, `memmove`, `strlen`,
  `__aeabi_idiv` and `__aeabi_uidiv` also bypass the name dispatch via a PLT fast path.
- **`call_override` was a ~60-way if-chain on every indirect call.** It now rejects in O(1) through
  a page bitmap of hooked addresses.
- **Generated chunks compile at `-O2`** (were `-O1`); `tools/build_win.py` uses `-O2` throughout.
- The host frame limiter was a flat `Sleep(16)`; it now sleeps only the remainder of a 16 ms budget,
  so slow frames run back-to-back.

Result over a fixed 75 s budget, same auto-menu route: **600 → 1560 frames**, total stall
**66.6 s → 21.2 s**. Every individual stall shrank (e.g. 18.7 s → 14.0 s, 2.3 s → 1.5 s,
0.98 s → 0.63 s). Zero faults; heap unchanged (`bump=170.8 MiB`, 65 blocks, largest 29.8 MiB).

**What is left** is genuine guest CPU, not overhead: the single ~14 s frame at f199 is
`CColladaDatabase::constructScene` building `menu_favelas04.max`, dominated by `fn_5cdb7c`
(profiled via host-RIP sampling resolved against `nm`). There are no guest worker threads doing
this work — the only guest threads are the two `vox::funcUpdate` audio threads
(`0x68d694`, confirmed running) — so the scene build is inherently single-threaded and
interpreter-bound. Remaining headroom is codegen quality, not I/O.

## 2026-11-07 texture work — RESOLVED (three engine-path fixes, no fallbacks)

1. **Group-2 sampler commit** — `commitCurrentMaterialImpl` commits only binding group 1; sampler
   bindings live in group 2 and were only committed on the `commitPassParameters` path. Fixed
   host-side in `runtime/gl_win32.c` (`gl_bridge_force_sampler`) by walking the group-2 sampler
   bindings of the current shader after every Impl commit and binding the material's texture values.
2. **Scene texture assignment** — `glitch::collada::createMaterial` (Ghidra 0x59c0b8) resolves every
   scene parameter name through `CMaterialRenderer::getParameterID(const char*, u16)` (0x53157c),
   which interns via the shared-string heap and silently skipped every value on a miss. Override at
   ELF `0x52157c` resolves by exact `strcmp` against the renderer's def names, so the engine's own
   type check + `setParameter<ITexture>` run and every scene material receives its defined texture
   (138 applications: `LightMap→market_lightmap`, `DiffuseMap→market_04/05/06/07/03/01/02/08/
   service_01…`).
3. **`lrand48` (the menu-wide root)** — `CGenericBaker::getBatchMaterial` (Ghidra 0x648060) names
   every batch material with 20 chars from `lrand48()`. The import was missing (always 0), so every
   batch registered as `"00000000000000000000"`, collapsing the name-keyed renderer registry onto
   one placeholder material for the whole menu. `runtime/imports.c` now implements `lrand48`,
   `srand48`, `drand48`, `seed48` (POSIX 48-bit LCG). Unique names → per-batch materials →
   `initParameters` copies the source material's values → every surface gets its own texture
   (`MENU_LOOPABLE_LIGHTINGMAP + MM_FAVELAS_STREET / MM_SIDE_WALK / MM_STONE / MM_GRASS /
   MM_CEMENT_00 / MM_STUFF / MM_BUILDINGS_00/01 / MM_FAVELAS_REFLECTION / MM_STREET_C_00`, skybox
   `SKY_BOX_SKY`). Verified: 0 zero-name registrations, 0 guest faults, 0 GL errors
   (`build/rand1_menu.png`).

All previous heuristic fallback paths (rank/address replay, texture-name substitution tables for the
scene) are removed. `FAST5_TEXTURE_TRACE=1` provides the evidence streams used above
(`setTexParam:`, `sceneSetTex:`, `sceneTex:`, `beginMatRender`/`beginMatRenderZERO`, `forceSampler:`).

## Green car window — RESOLVED (2026-11-07, applied from the FF5WB solution)

**Symptom:** the background menu car's glass/roof rendered as a flat bright green
(`build/rand1_menu.png`).

**Root cause (measured in FF5WB, reproduced and fixed here):** the engine's
`glitch::texture-bind-failed-2d` placeholder is a 1x1 texel of `(55,215,55,255)` — exactly the
green seen on screen. The car's `MaskSampler` was NULL, so the engine bound that placeholder over
the glass. Two defects kept the mask from being filled:

1. **Wrong pass strides in the host sampler snapshot.** The shipped
   `commitCurrentMaterialImpl` (ELF `0x634e84`) computes
   `node = *(A + tech*12 + 8); pass = node + passIndex*0x30`. Our override used
   `tech*4` / `passIndex*8`, which is only correct for technique 0 / pass 0, so the car paint
   pass (tech 1) committed garbage and never bound the mask.
2. **Missing PowerVR body flag.** `RaceCar::InitBodyTexture` (ELF `0x2d36c8`) and
   `RaceCar::SetCarColor` (ELF `0x2d3280`) gate the `SetPVRBodyMatParams` calls on the per-car byte
   `car+0x17d1`; the port's device-profile path leaves it 0, so `MaskSampler`/`Color1`/`Color2` were
   never set.

**Fixes in this workspace (`runtime/overrides.c`):**
- corrected the technique/pass strides (`tech*12`, `pass*0x30`) in the group-2 sampler snapshot and
  in the `cmi:` diagnostic;
- set `car+0x17d1 = 1` at `InitBodyTexture` and `SetCarColor` (default on; `FAST5_PVR_BODY=0`
  restores the old path for A/B comparison).

**Verified here:** 0 guest faults; `initBodyTexture` = 3 (all menu cars), `setPVRBody` = 9
(3 per car) in the instrumented run; bright-green pixel count in `build/fixwb_2.png` = **0**
(was `(58,219,60)`); the background car now renders with its glass and paint correctly.

**Note:** the earlier hypotheses in this file (mask/green placeholders attributed to shared batch
materials) were superseded — the window draw sampled the engine's green bind-failed placeholder,
not a body/interior texture. The `FAST5_SKIP_REN`/`FAST5_SKIP_MAT` diagnostics and the neutral
`MaskSampler` default remain available but are inert now that the mask is set.

## First playable race — RESOLVED (2026-11-07): missing string→number imports

**What happens now:** from the title, `FAST5_AUTO_MENU=1` + `FAST5_AUTO_RACE=1` navigates
Touch → Quick Race → track select (Day Rio) → NORMAL RACE → car select (Highline) → Skip Tutorial
and the **Rio race loads and runs**: `Loading track rio.track`, the Rio scene graphs build, the
loading draw calls complete, and the race renders with the red Highline player car, opponents,
HUD (position `10/10`, speed, minimap, nitro) and the cinematic intro. Captures:
`build/game_frame_2100.bmp` (countdown, `GO`), `2400`/`2700` (city driving), `3600` (nitro/HUD).
No guest fault through frame 3960 (`build/race14.err`, `build/race_clean1.err`).

**Root cause of the pre-fix crash** (`guest fault at 0x464c45ff`, the ELF magic read as a pointer):
- `CCinematicObject::SceneObjItemInit` (ELF `0x4a5d08`) parses each `*_cinematic.cff` XML and reads
  the `object` attribute into `CCinematicThread+0x24` through the XML reader's `getAttributeValue`.
- That read ends in `atoi`/`strtol`, which were **not implemented** in `runtime/imports.c`, so every
  integer attribute came back as 0. The cinematic thread searched all 30 script objects for
  target ID 0, never matched, and left `+0x28/+0x2c` at `-1`.
- `CCinematicCommand::Object_SetEnabled` (ELF `0x4d11ac`) then called `ScriptManager::GetObj(-1)`
  → NULL, and dereferenced `*(NULL + 0x80)` — an unmapped read that resolved to the ELF header.
- Evidence: `build/race13.err` shows `cineDump` listing every script object's IDs (none is 0),
  `cineThread ... target=0x00000000`, then `getObj -> 0x00000000` and the fault.

**Fix (`runtime/imports.c`):** implemented `atoi`, `strtol` (with guest `endptr`), `strtod`
(guest `endptr`, softfp double return) and `strtok`. After the fix the same trace shows real
targets — `target=0x147 → objIdx=24`, `0x145 → 19`, `0x185 → 11` — and the cinematic commands
run (`Object_SetEnabled` on valid script objects) instead of faulting.

**Deterministic path (`runtime/main_win32.c`, `FAST5_AUTO_RACE=1`):** touch at frame 480
(Quick Race), 720 (track), 960 (race type), 1200 (car OK), 1560 (skip tutorial); captures extended
through frames 1560/1800/2100/2400/2700/3000/3300/3600/4200.

## Back to menu from a race — RESOLVED (2026-11-07): two separate faults

Reported by the player: racing, then leaving to the menu crashed. The session log
(`build/fast5.log`) recorded the exact sequence:

```
[f1184] touch press 712,438
STATE closeToMenu stack=0x080b0bb0 lr=0x002c18d8
STATE pop  stack=0x080b0bb0 all=1 end=0x080b0e80
state create  this=0x08000050 vt=0x007ac3d0
state unload3D this=0x08000050 vt=0x007ac3d0 lr=0x00431800
[android] NVThread: Error - could not find JVM!
*** GUEST FAULT at 0x464c4783 (read) ***
```

**Fault 1 — `nativeGC` through the uninitialized JNI env.** Symbolized from the log's backtrace:
`appRender → Application::Update → StateStack::DoStateChange → PushState →
GS_MenuMain_Load::StateCreate → … → BaseState::Unload3D → Game::FreeScene → TrackScene dtor →
fn_2ec6e8 → fn_2ebe34 → fn_2a89a4 → fault`. The innermost translated frame is **`nativeGC`**
(ELF `0x29a0e4`): `NVThreadGetCurrentJNIEnv()` returns the uninitialized env, so the JNIEnv vtable
read lands on the ELF header (`0x464c457f`) and the call faults at `0x464c47xx`. This is the known
U11 gap, reached because `TrackScene` teardown triggers a `System.gc()`.
*Fix:* the whole JNI-dependent export set is now answered on the host — `nativeGC` (→ 1),
`AndroidOS_IsFinishLoadBackground` (→ 1), `nativeSendAppToBackground` / `nativeOpenGLLive` /
`nativeOpenIGP` / `nativeNotifyTrophy` / `AndroidOS_LoadSoundGroup` /
`nativeResumeSoundEngine` / `nativesetfxMusicRes` (no-ops), `nativeIsNotCallSendApp` (→ 0),
`nativeGetHostName` (→ valid empty guest string, never NULL).

**Fault 2 — guest heap fragmentation (the real cause of "crashes after several races").**
After fault 1 was gone the same path stopped at `guest fault at 0x1fb64c10 (write)` preceded by
`import: guest heap exhausted (5592408 bytes)`: a 5.6 MB texture allocation failed, `GlitchAlloc`
returned NULL, and `CImageLoaderPVR::loadTextureData` wrote through it.

Instrumentation (`guest_heap_stats`, logged every 120 frames) proved it was **not a leak**:

| | before the fix | after |
|---|---|---|
| live memory peak | 43.8 MiB | 40.9 MiB |
| arena high-water | **1014 MiB** (then died) | **192 MiB** (stable) |
| free blocks | **747,101** | ~4,000 (16k transient) |
| largest free block | 4.7 MiB at death | up to 29.8 MiB |

765 MiB was free but split into ~750k fragments, so no contiguous block could satisfy the request.
The `runtime/imports.c` allocator kept a flat free list and **never merged neighbouring blocks**;
every race → menu cycle added fragmentation, which is why it crashed after more races but not the
first.

*Fix:* replaced it with a **boundary-tagged allocator with coalescing** (size/prev_size header,
two flag bits for USED/PREV_USED, 32 power-of-two free bins, merge with both physical neighbours on
free, split-off on allocate, in-place grow into a free successor or the top block). `realloc` now
also resizes in place when it can. `gmalloc` reports an exhausted heap once and returns 0 instead
of faulting. The arena stays at **512 MiB** (`FAST5_ARENA_MB` overrides); no extra headroom needed.
One bug was found and fixed during bring-up: bin links are two pointers (16 bytes on x64), so a
free-capable block needs a 16-byte minimum payload — 8 wrote over the next block's header.

**Verified:** `FAST5_AUTO_MENU=1 FAST5_AUTO_RACE=1 FAST5_AUTO_BACK=3` ran **4 complete
race → pause → menu → race cycles with 0 guest faults, 0 host exceptions and no heap-exhaustion
report.** `build/fast5.log` carries the per-120-frame heap line so regressions are measurable.

The return path itself (independent of the allocator) also logs `STATE closeToMenu`, pops all
states, recreates `GS_MenuMain_Load`, and renders the menu again (`build/game_frame_3000.bmp`).

## Input: keyboard and tilt (2026-11-07)

The shipped game has **no keyboard driving** — `notifyKeyPressed` (ELF `0x2ad338`) only handles
KEYCODE_BACK (4), KEYCODE_MENU (0x52) and 0x50; everything else is ignored. Driving input on the
reference build is the **accelerometer** plus touch.

- `runtime/platform_win32.c` now translates Win32 keys to Android key codes (DPAD L/R/U/D, ENTER,
  ESC/BACK, A/D/W/S, SPACE) and queues real press/release events (`platform_take_game_key`,
  `platform_held_key`).
- `runtime/main_win32.c` dispatches those through the game's own exports
  `nativeSetOnKeyDown`/`nativeSetOnKeyUp` (ELF `0x299d28` / `0x299cf4` → `notifyKeyPressed` /
  `notifyKeyReleased`), and feeds tilt each frame through `nativeAccelerometer`
  (ELF `0x299e28`) from the arrow keys: left/right steer, up/down pitch, `z = 9.81`.
  `Application::ReadAcceleration` → `Game::OnAcceleration` consumes the same accelerometer object
  the game uses on the device, so no gameplay code is bypassed.
- Mouse click → touch at the matching 800×480 position remains the way to drive the menus.

### Keyboard steering investigation (2026-11-07, partial)

**Fixed — held keys never reached the engine.** `platform_held_key()` scanned the key *event queue*,
but `platform_take_game_key()` drains that same queue earlier in the frame, so the scan always found
it empty and reported "released". Arrow-key tilt could therefore never activate, which is why tilt
appeared dead. `platform_win32.c` now keeps a level-triggered `g_key_down[]` bitmap indexed by
Android key code, updated in `queue_key()` and independent of queue consumption.
`platform_inject_key()` was added so scripted runs exercise the same path.

**Accelerometer interface, measured rather than guessed:**

| Item | Finding |
|---|---|
| Globals | `mAccelerometerZ = ELF 0x8B85A4`, `X = 0x8B85AC`, `Y = 0x8B85B0` — **not** laid out in X/Y/Z order |
| Writer | the JNI export `nativeAccelerometer` (`0x299e28`) stores through GOT slots this port never populates, so the harness writes the globals directly |
| Sign | `appRender` flips each axis sign before `Application::ReadAcceleration`; the harness must send `z = -9.81` to give the engine `+9.81` |
| Conversion | `Accelerometer::moved` → `_calcAngles` uses `atan2f`; angles land at object `+0x38/+0x40/+0x48` |
| Deadzone | `CarControl::UpdateSteeringAccelerometer` centres on **π/2** and ignores `\|angle\| <= 0.0436 rad (2.5°)` |
| Measured | `x = ±1.0` produces angle **0.1016 rad (5.8°)** — comfortably clears the deadzone |
| Mode selector | `CarControl::UpdateSteering` dispatches on `car+0x400`: 1 = accelerometer, 5 = gyro, 0 = wheel/buttons. The race car ships with **1**; see the resolved section below for why that mattered |

### RESOLVED — keyboard now steers through the touch/onscreen-button scheme

The accelerometer approach was **abandoned**: it drives the camera lean as well as the steering, so
every turn moved the whole view. The game already ships a touch steering scheme, and the port now
routes the keyboard into that instead.

**Two selectors control it, and both must agree** (`CarControl`, per car):

| Field | Meaning | Value to use |
|---|---|---|
| `car+0x400` | physics control mode read by `CarControl::UpdateSteering` | **0** = wheel/buttons, 1 = accelerometer, 5 = gyro |
| `carControl+0x90` | scheme read by `CarControl::CalculateWheelAngle` | **0** = onscreen buttons, 1 = accelerometer, 2 = onscreen wheel |

`UpdateSteering` returns early when `car+0x400 == 1` and never reaches `CalculateWheelAngle`, so mode 1
must be cleared **in `UpdateCarControl`**, before it calls `SetManualInputFlags` — clearing it inside
`CalculateWheelAngle` is circular, because that function is only reached once the mode is already 0/5.

**Implementation** (`runtime/overrides.c`):

- `0x34f208` `CarControl::UpdateCarControl` — forces `car+0x400 = 0` and `carControl+0x90 = 0`, then
  runs the original.
- `0x35078c` `CarControl::SetManualInputFlags` — after the original rebuilds the input bitmask at
  `CarControl+0xc`, the keyboard ORs in **bit 4 (left)** and **bit 8 (right)** for
  LEFT/A and RIGHT/D. Those are exactly the bits the touch onscreen buttons set via
  `IsGamePadDown(this, 4/8)`.
- `0x350f54` `CarControl::CalculateWheelAngle` — still hooked for tracing.

**Verified, both directions** (wheel angle, centred at π/2 = 1.5708):

| Input | flags | wheel |
|---|---|---|
| none | `0x1` | 1.5708 (centred) |
| LEFT held | `0x5` | **1.6127** |
| RIGHT held | `0x9` | **1.5289** |

Mirrored about centre, driven by the shipped `UpdateSteeringOnscreenButtons` — so the engine's own
steering rate, clamping and physics are all intact, and no accelerometer value is written, which
means the camera stays put.

Diagnostics: `FAST5_STEER_TRACE=1` logs `setFlags`/`wheelResp`;
`FAST5_AUTO_STEER=<start>:<frames>:<L|R|U|D|N|C|Z|K>` scripts a held key for headless tests
(N = SPACE/nitro, C = camera, Z = look back, K = CTRL/brake).

### CarControl input bitmask (measured)

`CarControl+0xc` is a per-frame bitmask rebuilt by `SetManualInputFlags` and consumed by
`UpdateCarControl`. Bits, confirmed from the consumers:

| Bit | Meaning | Key |
|---|---|---|
| `0x01` | throttle | **UP / W** |
| `0x02` | brake | **CTRL** |
| `0x04` | steer left | **LEFT / A** |
| `0x08` | steer right | **RIGHT / D** |

The shipped `SetManualInputFlags` sets `0x01` **unconditionally** — that is the auto-throttle, and why
the car drove itself. The hook clears `0x01`/`0x02`/`0x04`/`0x08` and re-sets them from the keyboard,
so acceleration is now manual.

**Verified:** with UP held the mask reports `0x1`; with nothing held it is `0x0`; LEFT gives `0x5`,
RIGHT `0x9` (steering bit + throttle), and the steering wheel angle ramps smoothly
`1.5708 → 1.6305 → 1.6888 → 1.7485 → 1.8094` while LEFT is held.

### Bug fixed: keyboard drove every car on the track

An earlier revision cleared `car+0x400` on **every** `CarControl`, intending to move the player off
the accelerometer scheme. But `UpdateCarControl` runs for the AI too, and `car+0x400` is what makes an
AI car read its own button mask (`carControl+0xb4`) instead of the global gamepad. Forcing it to 0
switched every CPU car onto the global gamepad — so the keyboard steered and accelerated the entire
field.

Measured mode split over one race, which is the discriminator now used:

| Car | `car+0x400` | `carControl+0x90` |
|---|---|---|
| player | **0** | 0 |
| CPU (×N) | 1 | 1 |

The mode is now **never written**. The scheme at `carControl+0x90` and the input bits are only
touched when `car+0x400 == 0`, i.e. the player car. Confirmed: only one `CarControl`
(`0x09db7388`) receives keyboard input, the wheel responds, and the CPU cars are untouched.

### Nitro, camera and look-back (RESOLVED — nitro verified in game)

`GP_RaceNormal::HandleNitro` is the per-frame nitro driver, and the shipped touch handler
`GP_RaceNormal::ExecuteTouch` shows what starting a boost actually involves:

```
player = Game::GetPlayer(-1);
if (player->charges (player+0x16d4) < 1)  -> refuse, play "empty" sound
else { player->charges--;  gp+0x17 = 1 (boost active);  player+0x16d8 = 1 }
```

So nitro is **one tap, not a hold**, and the tap **consumes a charge**. The engine's own timer
(accumulated in `gp+0x18`, ended at 5999) stops the boost by itself — nothing on the host should
clear `gp+0x17`.

Wrong revisions and why they failed:
- Setting `gp+0x17` from the *held* key state cut the boost off the moment SPACE was released.
- Setting it on the rising edge but **skipping the charge decrement** gave an endless boost.

Correct implementation (in `runtime/overrides.c`, hooking `0x454858`): on the rising edge of SPACE,
read the player via `Game::GetPlayer(-1)`, and only if `charges >= 1` decrement `player+0x16d4`, set
`gp+0x17 = 1` and `player+0x16d8 = 1`. Verified in game: `chargesLeft=1 → 0 → refused`.

Other controls from `GP_RaceNormal::ExecuteGamePad` (`GamePadManager::IsGamePadButtonActive`):

| Action query | Meaning | Key | Note |
|---|---|---|---|
| action 2, mask `0x200` | nitro | SPACE | implemented via the HandleNitro hook above |
| mask `0x800` | **extra brake** | — | see below |

**Camera and look-back are NOT gamepad actions.** They are driven by two bytes on `GP_RaceNormal`
that the touch HUD sets in `ExecuteTouch`, and `GP_RaceNormal::UpdateCameraMode` consumes:

| Field | Meaning |
|---|---|
| `gp+0x1c` | one-shot "cycle camera" request, cleared by `UpdateCameraMode` once accepted (1000 ms cooldown) |
| `gp+0x1d` | held "look back" flag, copied to `camera+0x47` every frame |

Both are driven from the keyboard in the `0x4538a8` (`UpdateCameraMode`) hook: **C** sets `gp+0x1c`
on its rising edge, **Z** mirrors the held state into `gp+0x1d`. The engine's own camera table,
mode filtering and sounds still apply. Verified: a C tap logs `camCycle ... consumed=1`, i.e. the
engine took the request and advanced the camera.

**Do not bind `0x800` to look-back.** It sets `CarControl` bit `0x400`, which acts as an extra brake
input. There is **no handbrake** in this game — drift is simply brake + turn — so holding `0x800`
while steering made the car drift. That was a wrong-guess regression and the binding was removed.

Controls that reach the game via the touch HUD rather than the gamepad cannot be driven through
`IsGamePadButtonActive` at all; hook the `GP_RaceNormal` state byte they set instead. That was the
mistake that made the C key silently do nothing at first.

**Still to find:** rewind (R). In `ExecuteTouch` it goes through a `HudItem` click that calls
`RewindManager::AllowStartRewind` then `Game::SetCurrentMenu(0x38)`, so it triggers a menu/state
transition rather than flipping a per-frame byte.





## Wheel rotation — RESOLVED (2026-11-07): float import truncation + missing double libm

**Symptom:** the menu cars' wheels rendered but never appeared to rotate.

**Root cause (measured):**
- `runtime/imports.c` declared `static uint32_t f32_of(uint32_t bits)` although it returns a
  `float`. Every float argument to a guest import was therefore truncated to the integer part
  before the call (`0.01 → 0`, `0.5 → 0`). The engine's static `sinf`/`cosf` wrappers call
  `__aeabi_f2d` → double `sin`/`cos` → `__aeabi_d2f`, so the angle passed to `sin` was 0.
- The double-precision libm imports were **not implemented at all** in `runtime/imports.c`
  (only the `*f` forms existed). `.rel.plt` imports `sin, cos, tan, asin, acos, atan, atan2,
  sqrt, pow, exp, log, log10, floor, ceil, fmod, frexp, ldexp, modf`; the unknown-import path
  returned 0.
- Consequently `glitch::core::quaternion::fromAngleAxis` produced an all-zero quaternion and
  `ISceneNode::setRotation` wrote `q=(0,0,0,0)` to all eight wheel nodes every frame.

**Fix (`runtime/imports.c`):**
- `f32_of()` now returns `float`.
- All 18 double-precision libm imports above are implemented with the guest softfp ABI
  (first double in `r0:r1`, second in `r2:r3`, result in `r0:r1`; `frexp`/`modf` write through
  their guest output pointer).

**Verified (`FAST5_WHEEL_TRACE=1`, clean run afterwards):**
- `BaseCar::UpdateCar` runs every `MenuScene::Update` for all three menu cars. Cars 1/2 carry
  `rate=(20.0,20.0)`, `Game::s_pInstance+0x1b3=0`, LOD 3; car 0 has rate 0.
- Wheel angle fields advance `0 → 0.02 → 1.62 → 3.22 → …` (car 1/2, wrapped by `fmodf` at ±2π).
- `fromAngleAxis` now returns `q=(-0.0100,0,0,0.9999)` at angle 0.02 and
  `q=(-0.7243,0,0,0.6895)` at angle 1.62.
- The same wheel node `0x13190d40` receives changing quaternions over 280 sampled frames
  (`build/wheel10.err`); no guest fault, no GL error, menu captures at frames 240/480/720.
- `build/clean_math1.err`: quiet default boot, no trace output, no guest fault, frame 720.

**Corrected earlier conclusions:**
- `RaceCar::UpdateFakeWheelRotation` (ELF `0x2d1c88`) is still uncalled/dead in this build, but it
  is **not needed**: the accumulation is `BaseCar::UpdateCar` (ELF `0x483acc`,
  `car+0x48/+0x4c += rate * 0.001 * delta`) and the consumer is
  `RaceCar::UpdateMeshes` (ELF `0x2d2810`) → `quaternion::fromAngleAxis` (ELF `0x2b8080`) →
  `ISceneNode::setRotation` (ELF `0x4dc320`).
- The earlier wheel instrumentation read `MenuScene+0x40..0x4c` (scene fields), not the car
  fields; that is why its readings were always zero. The corrected probe reads each car at
  `scene+0x60..0x68`.

## Known open bugs (next work)

1. **Second menu car can show a bright green patch (shared batch material) — RESOLVED (2026-11-08).**
   Root found 2026-11-07: the two menu cars' part batches shared material instances created by
   `CGenericBaker::getBatchMaterial`; `RaceCar::SetPVRBodyMatParams` (ELF `0x2dd140`) writes each
   car's `MaskSampler`/`Color1`/`Color2` into that shared material at init, so the last car inited
   left its paint values for both. Evidence with `FAST5_TEXTURE_TRACE=1` (`rend=` in the sampler
   log):
   - `mat=0x0dec17d0 rend='RaceCarMaterial-fx'` → Cranky M3 body (correct named renderer).
   - `mat=0x1366d238 rend='<random>'` → batch renderer that commits both cars' body textures
     (`Cranky_M3_2011_Body.NOPVR.tga` then `Comet_Rice_Rocket_2011_Body.NOPVR.tga`).
   - `mat=0x1366ce50 rend='<random>'` → shared batch material carrying the Comet interior, the
     driver (`CHARACTERS_MERGE_C`) and both cars' wheel textures.
   Fix: the baker's cached batch material is invalidated for car-body source materials (those with a
   `MaskSampler` definition), so each car gets its own material; the car-texture sharing log now
   reports 0 materials carrying two cars' textures. Re-verify the menu with
   `FAST5_TEXTURE_TRACE=1` in the normal session. `winMatDump`/`maskVal` diagnostics remain
   env-gated for this item. The separate parked-prop `RaceCarMaterial-fx` green-window defect is
   resolved (see above).

## Current reproduction

Player-facing launcher (logging always on):

```
play.bat            normal play, log to build\fast5.log
play.bat menu       auto-advance the title into the main menu
play.bat race       auto-drive through track/car selection into the Rio race
```

`play.bat` at the repo root forwards to `tools\play.bat`. It sets `FAST5_FAKE_GPU=1`, points
`FAST5_LOG` at `build\fast5.log`, runs the game, then prints the exit code and every
`STATE`/`state` line from the log so the session can be read back without opening it.

`build\fast5.log` is the session record: a timestamped header, every game/host message,
`[fN]` frame ticks, `[fN] touch press/release` and `[fN] key ...` input events, state-stack
transitions, and — on a crash — `*** GUEST FAULT ***` / `*** HOST EXCEPTION ***` blocks with
registers, a 16-word stack window and a symbolized-offset backtrace. It is line-buffered, so
killing the process still leaves a usable file.

Manual equivalent:

```powershell
python tools/build_win.py
$env:FAST5_FAKE_GPU='1'
.\build\fast5_win.exe
```

State-stack instrumentation (always on, `fast5_log_note`):

| Hook | ELF | What it records |
|---|---|---|
| `BaseState::StateCreate` | `0x420e0c` | `state create  this=… vt=…` |
| `BaseState::StateDestroy` | `0x420ea8` | `state destroy this=… vt=…` |
| `BaseState::OnMenuReset` | `0x420f64` | `state menuReset this=… vt=… lr=…` |
| `BaseState::Unload3D` | `0x42182c` | `state unload3D this=… vt=… lr=…` |
| `StateStack::PushState` | `0x2b19ac` | `STATE push stack=… state=… vt=… lr=…` |
| `StateStack::PopState` | `0x2b1a34` | `STATE pop  stack=… all=… end=… ret=… lr=…` |
| `StateStack::PopAllStates` | `0x2b1af8` | `STATE pop_all stack=… lr=…` |
| `StateStack::CloseToMenu` | `0x2b18b8` | `STATE closeToMenu stack=… lr=…` |

The `vt=` value identifies which `GS_*` state object is being created/pushed, so a
back-to-menu attempt shows the exact pop/push sequence and the caller (`lr=`) that drove it.

Expected on this build: logo → Universal legal text → **Fast Five title screen** with real textures.
Post `WM_KEYDOWN`/`WM_KEYUP` for Space (or press Space when focused) after the title appears; the
game then loads and displays the **main menu** (`Quick Race`, `Career 0%`, `Multiplayer`, `Options`,
`More games!`, Gameloft LIVE). Observed run: title at `game_frame_120.bmp`,
`build/clean_title.png`; menu at `build/clean_menu.png`; no guest fault; about 30–35 FPS on
ANGLE/D3D11. The same path without `FAST5_FAKE_GPU` is much slower in guest JPEG work.
`FAST5_GL_TRACE=1` enables GL diagnostics; `FAST5_IMPORT_TRACE=1` and `FAST5_WATCHDOG=1` re-enable
the host debug streams. `FAST5_THREAD_TRACE=1` logs the guest Vox thread lifecycle.
`FAST5_WHEEL_TRACE=1` logs the menu car wheel angles (`car+0x40..0x4c`), the quaternions produced by
`quaternion::fromAngleAxis`, and the quaternion applied to one sampled wheel node.
Left-click in the game window is mapped to a touch at the same normalized 800×480 position;
Space/Enter remain fallback touch controls.

## Files changed in the current bring-up

- `runtime/imports.c`: guest `sprintf`/`snprintf` with ARM softfp varargs; `GBLOCK_MAGIC` /
  `GFREE_MAGIC` allocator validation; first-use import logs now opt-in via `FAST5_IMPORT_TRACE`;
  **`f32_of()` returns `float`** (was `uint32_t`, truncating every float import argument);
  **all double-precision libm imports implemented** (`sin/cos/tan/asin/acos/atan/atan2/sqrt/pow/
  exp/log/log10/floor/ceil/fmod/frexp/ldexp/modf`, softfp `r0:r1`/`r2:r3`).
- `runtime/main_win32.c`: guest arena 512 MiB; watchdog is opt-in via `FAST5_WATCHDOG`.
- `runtime/guest_data.c`: bionic-compatible **16-bit** `_tolower_tab_`/`_toupper_tab_` (ASCII entries);
  `_toupper_tab_` moved clear of the 16-bit tolower table.
- `runtime/gl_win32.c`: declaration-order attribute binding; invalid `glGetProgramInfoLog`
  objects ignored; BGRA/BGR texture uploads converted to RGBA/RGB for ANGLE; per-texture
  `*_Body*.tga` row-order flip; the old global GL-texture-id-8 body substitution was removed
  (superseded by authoritative texture resolution and per-car batch materials); per-draw
  texture-unit and texture-ID diagnostics behind `FAST5_GL_TRACE`; no PVRTC extension advertised.
- `runtime/thread_win32.c` / `.h`: guest pthread create/join/self, real Win32-backed mutexes, per-thread
  guest Cpu/stack, TLS thread ids, and a thread-safe guest allocator lock.
- `runtime/platform_win32.c` / `platform.h`: left-button mouse event queue with client-to-800×480
  touch coordinate scaling; `runtime/main_win32.c` dispatches down/up to the guest touch functions.
- `runtime/overrides.c`: `FAST5_WHEEL_TRACE` probes for `MenuScene::UpdateCars`,
  `BaseCar::UpdateCar`, `quaternion::fromAngleAxis` and `ISceneNode::setRotation`; JNI capability
  answer; material-ID normalization / grab guard; texture-name
  rewriting at `0x619238` / `0x619170` / `0x61a260`; `0x504764` opt-in openFile tracing.
- `runtime/main_win32.c`: guest arena 512 MiB; watchdog opt-in; `FAST5_AUTO_MENU` second touch and
  `FAST5_AUTO_RACE` tap; press/release coordinates are paired.
- `tools/extract_archive_tgas.py`: extracts the shipped Favelas records into the derived overlay,
  writing raw `.tga` names and complete-header `.pvr` aliases without rewriting archives.
- `tools/extract_car_textures.py`: extracts `carTextures.zip` TGA/PVR basenames into the overlay.
- `tools/capture_printwindow.ps1`: reproducible `PrintWindow` capture helper.
- `docs/05-STATE.md`, `docs/06-EVIDENCE.md`, `docs/04-ROADMAP.md`, `README.md`,
  `recomp/README.md`, `tools/README.md`.

## Rendering backend note

The current backend is **ANGLE → D3D11** on the AMD GPU, proven by the live `D3D11` overlay and the
rendered title/menu. The game uses **GLES2**, not desktop OpenGL; ANGLE is the compatibility layer.
Desktop GL/WGL would require a separate GLES2→desktop-GL shim and would not by itself fix the
remaining texture binding. No `glCompressedTexImage2D`/PVRTC upload was observed in the menu trace;
the Favelas assets are full-header `.pvr` images selected through `CImageLoaderPVR`, and the open
issue is that loader/material binding, not the GL backend. If an A/B backend is wanted, add
**ANGLE's OpenGL backend** (`EGL_PLATFORM_ANGLE_TYPE_OPENGL`) before a WGL rewrite.

## Done

1. Verified both toolkits are live:
   - Craft Anything 0.4.0: healthy, 38 tools, Ghidra 12.1.4, adb, JADX, Apktool, GDB, Java, objdump.
   - REA 6.8.0: 229 tools, Ghidra 12.1.4 + Hopper + JADX Android provider all available.
2. Mapped the target and confirmed the engine is an **Irrlicht fork ("glitch")** with Spark + Vox,
   fully symbol-named (23,377 defined / 19,023 function symbols).
3. Recorded ABI: ARMv7-A, VFPv3, **softfp**, GCC 4.4.3, non-PIE, 62,432 relocations, 433 constructors,
   161 KB of exception index.
4. Confirmed the data pack is the **PowerVR** variant and identified the container shapes.
5. Extracted a read-only working copy of the APK to `analysis/apk/` (originals untouched).
6. Wrote `README.md` and `docs/00…04`, plus the two census tools.
7. **Census done** (`analysis/reports/elf-census.md`, `instruction-census.md`):
   - 19,023/19,023 functions are ARM mode; **no Thumb**.
   - mean function 189 B / 45 instructions; median 72 B / 18.
   - 93,908 VFP ops; 0 atomics; 0 SVC; 973 indirect calls; 1,502 `__cxa_*` sites.
8. Data manifest written: `analysis/data-manifest.sha256` (662 entries).
9. Confirmed `qemu-arm` exists in WSL → differential oracle available offline.

## In progress

- Ghidra session on `libFast5.so` (about to open; cold import expected 5–20 min).
- `recomp/` scaffold: ELF image loader is the next file to write.

## Confirmed since last update (M1 boot trace)

1. Java boot chain traced end-to-end (see `docs/02-ARCHITECTURE.md` §2).
2. Native entry points identified: `appInit` `0x29c830`, `appRender` `0x29cb14`,
   `JNI_OnLoad` `0x29c604`, 24 `Java_*` exports.
3. Data root confirmed both in Java (`/sdcard/gameloft/games/GloftF5HP/res/…`) and as a string
   inside the `.so` (`0x7022f0`).
4. `GameRenderer.nativeInit(w,h,lang)` at the end of `onSurfaceCreated` is normally unreachable;
   `appInit` is called from the JNI wrapper instead.
5. `nativeResize` is log-only (no engine effect observed).
6. Frame pacing is capped at 20 fps by `Thread.sleep(50 - elapsed)`; the loop is gated by
   `MyVideoView.isVideoCompleted()`.
7. Full native boot decompiled: `appInit` → `Application::CreateInstance` → `Application::Init` →
   `DeviceConfig::SetupGameForDevice` → `Game::Init` (order documented in 02-ARCHITECTURE §2).
8. **Device profile is hardcoded**: `s_DeviceType = 11` with log `PLATFORM_I9000`; LOD/clip values
   come from device-indexed tables. No adaptive device detection for the profile.
9. Initial RNG seeds are forced to 0 in `CreateInstance` (deterministic baseline).
10. `JNI_OnLoad` validates `vm->GetEnv(JNI_VERSION_1_4)` and registers
    `ALicenseCheck_setJavaVM`; `mWaitForInit = 10` skips the first 10 frames.
11. **Ghidra address convention**: Ghidra image base is `0x10000`; Ghidra address = ELF + `0x10000`.
12. `ALicenseCheck::Init` is reached from `ALicenseCheck_InitLicenseVM`; `GLGame.nativeInit` calls
    `ALicenseCheck_InitLicense`, which is the live startup path.
13. Original build paths leaked in `.rodata`: `D:/HD_Prj/_F5/.../GameSpecific/jni/{app_Android.cpp,
    jni_GameRenderer.c}`, plus Vox `stb_vorbis.cpp`, `mpc_demux.c`, `mpc_decoder.c`.
14. **Data pack decoded**: `file000000.dat` is the obfuscation map (25,582 B, key `0x4F`, marker
    `c|-|3k`, 651 entries). The other `.dat` files are renamed original assets. Verified by executing
    the shipped decode loop under Unicorn and matching a static model byte-for-byte.
    Artifacts: `analysis/reports/dat-map.tsv`, `dat-map.raw`; tools `tools/emulate_map.py`,
    `tools/deobfuscate_map.py`, `tools/parse_map.py`.
15. **VFS order confirmed**: mount `res/`, chdir `res/`, load map, mount `Glitch/ Effects/ SWF/
    strings/ Scene/ Audio/`, then mount eleven ZIP archives (`Car.zip` … `strings.zip`).
16. **License path confirmed**: `GLGame.nativeInit` → `ALicenseCheck_InitLicense`; config comes from
    `res/raw/igli.bin` (262,144 B) via `GLGame.db()`; `ValidateLicense` is called from menu, race-load
    and MP-lobby states.
17. **Device ID mapping confirmed**: `GetDeviceID()` returns 9000 for `samsung` + `i9000`; the engine
    hardcodes `s_DeviceType = 11` (PLATFORM_I9000). `CheckHighMemDevice()` returns true for type 11.
18. **Frame timing confirmed** (`Application::Update`): 1 ms step while in `GS_Race_Load`/`GS_IGP`/
    `GS_MenuMain_Load`, otherwise 33 ms, clamped to 80 ms; `Game::GameUpdate(dt)` then
    `Game::GameRender()`.
19. **First executed-instruction result**: the map decoder is a real, verified translation of
    shipped ARM code; this is the first M1 proof that translation-by-execution is achievable.
20. **ELF image loader implemented** (`recomp/elf_image.py`) and verified on the real library:
    2 LOAD segments mapped at link addresses (image end `0x8b85bc`), 23,688 symbols parsed,
    **62,432 relocations applied** (61,976 `RELATIVE`, 149 `ABS32`, 7 `GLOB_DAT`, 300 `JUMP_SLOT`),
    456 unresolved import slots over 306 names, **433 `init_array` constructors**. The most
    referenced import is `pthread_create` (139 slots).
21. **P-code extraction working**: `recomp/ghidra_scripts/DumpPcode.java` + `tools/dump_pcode.sh`
    produce `analysis/reports/pcode.tsv` (35.7 MB) = **14,185 functions ≤128 bytes**, 151,473
    instructions, 780,747 P-code ops. Raw P-code includes register names, flags, VFP and userops.
22. **Emitter v0** (`recomp/emit_c.py`) + runtime `runtime/cpu.h`, `runtime/mem.h`: instruction
    state machine, integer/flags/memory/branches/calls/returns, address normalization.
23. **Differential harness** (`recomp/diff_test.py`, `recomp/batch_diff.py`): translated C vs the
    shipped ARM code under Unicorn, randomized inputs, memory hash. Result so far:
    **22 functions pass, 0 mismatches, 2 input-domain faults** (functions that expect a valid
    pointer graph). Three real bugs found and fixed by the harness: decimal constant parsing,
    Ghidra→ELF address normalization (base `0x10000`), and `STORE` width taken from the value
    operand rather than the space-ID operand.
24. **VFP confirmed as VFPv3-D32, no NEON**: the binary uses `d16–d20` (above the D16 limit),
    `readelf` reports `Tag_FP_arch: VFPv3` with no Advanced SIMD tag, and the census has no NEON
    mnemonics. Dump also shows `s0–s18`/`d7–d20` in small functions.
25. **Userop names now in the P-code dump** (Ghidra `Language.getUserDefinedOpName`): the only
    userops in the ≤128-byte set are `setISAMode` (16,059), `VectorSignedToFloat` (94) and
    `VectorUnsignedToFloat` (14). Re-dump without re-analysis: `bash tools/dump_pcode.sh 128 reuse`.
26. **Float translation added**: `FLOAT_ADD/SUB/MULT/DIV/SQRT/ABS/NEG`, `FLOAT_LESS/EQUAL/LESSEQUAL`,
    `FLOAT_NAN`, `INT2FLOAT`, `FLOAT2FLOAT`, `TRUNC/CEIL/FLOOR/ROUND`, plus the three userops.
    VFP differential batch: **4/4 pass**.
27. **Fault-aware harness**: bounds-checked guest memory in `runtime/mem.h` + Unicorn
    `UC_HOOK_MEM_UNMAPPED`; both sides now report and compare the same fault address. VFP needs
    CPACR `0x00F00000` + FPEXC `0x40000000` in Unicorn (set in `diff_test.py`).
28. **Wide differential sweep: 40/40 pass** (integer/single-load functions), plus 12/12 setters and
    4/4 VFP functions. Total so far: **56 functions, 0 register/memory mismatches**.
29. **Known host difference**: x86 SSE and ARM VFP choose different NaN signs/payloads
    (`0xFFC00000` vs `0x7FC00000`). The float harness treats any NaN as equal and documents this;
    gameplay only sees it if NaN bits are inspected. Not a translation defect.
30. **Full P-code dump**: `analysis/reports/pcode.tsv` is now 223 MB / **20,627 functions** (all
    sizes), re-dumped with userop names via `bash tools/dump_pcode.sh 0 reuse`.
31. **Emitter coverage is 100 %**: `recomp/coverage.py` reports **20,325 / 20,325** non-empty
    functions emitted, 0 failures after adding `LZCOUNT`. Userops across the whole binary:
    `setISAMode` 42,320, `VectorSignedToFloat` 2,251, `VectorUnsignedToFloat` 573.
32. **Whole-binary compile gate passed**: `recomp/emit_all.py` emitted all 20,325 functions into 102
    chunks and compiled them with `gcc -O1 -Wall -Wextra`: **102/102 clean, 0 errors, 0 warnings**
    (8 parallel jobs, ~3.5 minutes). Report: `analysis/reports/emit-all.md`.
33. **Whole-binary link gate passed**: `recomp/link_all.py` generated a 20,325-entry
    address→function table (`call_target`/`call_indirect`), compiled 104 translation units and
    linked **`build/fast5_recomp_test`** with no missing symbols. Running the translated
    `nativeGetTotalSounds` (`0x29b014`) returns **r0 = 635** and returns through `lr`
    (`pc = 0xdeadbee0`) — the first full translate→compile→link→execute cycle. Note: function
    addresses in the dump are normalized (Ghidra `0x2ab014` → ELF `0x29b014`).
34. **Import layer + constructors**: `recomp/gen_imports.py` maps all **300 PLT entries** to their
    import names (`runtime/imports_table.h`). `runtime/imports.c` implements the softfp helpers,
    libc memory/string subset, guest heap, math, pthread no-ops, `__cxa_atexit`, logging and GL
    no-ops; unknown imports are logged once and return 0. `call_target` routes PLT addresses to
    `call_import`. A bug where Ghidra merged a PLT stub into a function body (tail branch became a
    wild indirect call) was fixed by excluding PLT ranges from emitted bodies and tail-calling the
    import instead.
    Result: **all 433 `.init_array` constructors complete** in `build/fast5_recomp_test`, and the
    test function still returns 635. 20,033 non-PLT functions are in the link.
35. **Boot harness reaches the title-screen render** (`build/fast5_boot_test`, `--boot`):
    - 256 MiB guest address space; heap starts at `0x08000000`, stack top `0x008F0000`; image still
      `0x8B85BC`. `runtime/vfs.c` maps `/sdcard/.../GloftF5HP/res/<x>` to `GloftF5HP/res/<x>`,
      writes to `build/userdata/`, and reads a derived `build/overlay/`; stdio + fd calls implemented.
    - `runtime/guest_data.c` provides bionic-style `_ctype_`, `_tolower_tab_`, `_toupper_tab_` at
      `0x9A1000/0x9A1200/0x9A1280`; `tools/patch_guest.py` patches their GOT slots.
    - `pthread_once` now runs its initializer; `sscanf`, `gettimeofday`, `time`, `localtime_r` are
      implemented; GLES2 is stubbed (strings, limits, shader/program objects); `runtime/java_bridge.c`
      serves version `1.0.9`, device `9000`, English, and a no-op sleep.
    - `appInit(800,480,0)` returns `0x09390CD8` (Application*). The harness stores it at `0x8B85A0`
      (the value `appRender` reads). `appResume` sets the resume state; `appRender` runs 40 frames.
    - Boot log: `PLATFORM_I9000`, `m_bIsHighMemDevice TRUE`, `profile1.sav` loaded, then
      `Android:: resuming game`, `glFlush`, and
      `Texture not found Sprites/universal_logo.tga !!!`; `StateRender` (`0x4460F0`) then faults at
      `0x446194` on a NULL object.
    - Data check: the entire shipped tree contains **only** `Sprites/universal_logo_highres.bsprite`
      and `.tga`, inside `file000548.dat` (a `QM` container whose embedded ZIP starts at offset 38).
      There is no plain `universal_logo` anywhere. `GetSpriteFilename` (`0x3CDA84`) appends
      `_highres` when `[obj+592] != 512`; runtime value is `0x190` (400) and the computed pointer is
      `0x710C70` (`_highres`, verified), so the plain request comes from a different path.
    - The temporary `build/overlay/Sprites/universal_logo.tga` workaround was removed. After the
      `strncmp` fix (item 36) the archive path loads `Sprites/universal_logo_highres.tga` directly;
      the boot still reaches the menu scene with no overlay present.
36. **Title/legal screen + working touch + menu-scene load** (clean 200-frame run):
    - **`strncmp` was missing** from `runtime/imports.c`; the default returned 0, so the PVR header
      parser (`FUN_00560814`) falsely matched `"BTEXpvr"`, read the header from offset 8 and rejected
      every texture. Implementing real `strncmp` (plus `strcasecmp`, `strncasecmp`, `strchr`,
      `strrchr`, `strstr`) makes `Sprites/universal_logo_highres.tga` load.
    - **Recovered function `0x553208`** (Ghidra `FUN_00563208`, 456 bytes, real prologue) from the
      runtime call target. Without it, the PVRTC decode path called an unmapped function.
      `pcode_extra.tsv` now holds 7 recovered functions; `pcode_all.tsv` = extra + main dump.
    - **Touch works**: `notifyTouchPress(400,240,0)` at frame 60 and release at frame 70 are accepted
      (`mWaitForInit=0`, `Game::s_pInstance+0x114=0`, `Application` scale=2.0). The game advances
      from the logo to the **Universal Studios legal/title screen** and renders its text.
    - **License path replaced**: `ALicenseCheck_ValidateLicense` (`0x6be364`) and `ValidateServer`
      (`0x6be258`) are host no-ops; `getIMEI` (`0x6bd0f0`) returns `"000000000000000"`. The Java
      telephony/network license service is not part of the PC runtime.
    - **New frontier**: the menu scene loads
      (`CColladaDatabase::constructScene url=#menu_favelas04.max`, plus
      `Scene/MenuGarage/menu_loopableSection1.bdae`) and `CSceneManager::compileInternal` /
      `sortBatches` / `CMaterial::getTechnique` then faults at `0xe590a17c` reading a garbage
      material pointer. This is after the title screen, in menu-scene material/shader setup.
37. **Native Windows window + real GLES2 (ANGLE)**:
    - `runtime/platform_win32.c` creates a real Win32 window and an EGL/GLES2 context through the
      vendored x64 ANGLE runtime in `third_party/angle/` (ANGLE 2.1.28027, GLES 3.0, Direct3D 11 on
      the AMD GPU). `tools/win32_angle_test.c` is the standalone proof and captured
      `build/angle_test.bmp` (800×480 blue frame).
    - `runtime/gl_win32.c` maps all **91 GL imports** to real ANGLE entry points. Guest pointers
      become host pointers into guest memory; softfp float arguments are converted; VBO offsets are
      distinguished from client pointers; FBO, texture, shader, program, uniform and draw calls run.
      `glGetString(GL_EXTENSIONS)` appends `GL_IMG_texture_compression_pvrtc`, and PVRTC uploads use
      a deterministic placeholder until the real decoder is wired.
    - `runtime/main_win32.c` is the native harness: 433 constructors, `appInit(800,480,0)`,
      `appResume`, then the frame loop with real `eglSwapBuffers`. Enter/Space maps to a synthetic
      touch press; a watchdog reports the guest PC if a frame takes longer than 3 s.
    - Native build is local (MinGW GCC 16.2.0, no WSL):
      `python tools/build_win.py` → `build/fast5_win.exe`. `recomp/link_all.py` gained
      `--emit-only` and now merges `analysis/reports/pcode_extra.tsv`, so recovered functions do not
      have to be folded into the main dump by hand.
    - **Shader template quirk:** the shipped shaders contain a stray `]` after some preprocessor
      directives (observed as `#endif]` in `build/shader_80_65.txt`). ANGLE rejects it. The bridge
      strips `]` only on lines whose first non-space character is `#`, then concatenates the source
      parts and passes one string to `glShaderSource`. This took a 25 s run from **54 shader
      compile failures** to **0 compile / 0 link failures**.
    - **Recovered vtable method `0x552e8c`:** a virtual call read the ELF pointer `0x552e8c`
      (Ghidra `FUN_00562e8c`), which Ghidra had not defined. `tools/add_missing_functions.sh
      0x552e8c ...` created it (272 bytes) and regenerated `pcode_extra.tsv` (8 recovered
      functions). Before the fix the native run faulted at `unmapped target 0x00552e8c` after
      rendering the Universal Studios legal text; after the fix it continues.
    - **Current native frontier:** the first post-resume `appRender` is still loading and decoding
      textures through the game's statically linked **libjpeg** in the interpreter
      (watchdog PC `0x005dd388` / `0x005dd3b0`, inside `FUN_005ed314` → `FUN_005ed250`/`005ed0cc`,
      the libjpeg memory manager). It is correct but slow; the run advances through texture levels
      (`TEXTURE LEVEL = 11`, then `9`) without faulting. This is a performance issue, not a
      translation error.
38. **Native render bring-up findings (window opens, frames still black)**:
    - **Compatibility GPU identity:** `FAST5_FAKE_GPU=1` reports the PowerVR strings
      (`Imagination Technologies`, `PowerVR SGX 540`, `OpenGL ES 2.0`) while all rendering still goes
      through real ANGLE. With that identity the game skips the slow CPU JPEG path and reaches the
      menu render/draw calls (`CSceneManager::renderLists`, `finalize=33`); with real ANGLE strings
      the game spends the first post-resume frame decoding JPEGs on the main thread.
    - **Attribute binding:** the game never calls `glGetAttribLocation` or `glBindAttribLocation`; it
      relies on PowerVR assigning locations in declaration order. `runtime/gl_win32.c` now parses
      each vertex shader's `attribute` declarations and binds `Vertex`, `TexCoord0`, `Color0`, ...
      to 0, 1, 2, ... before linking.
    - **Textures are valid:** `build/tex_0_256x256.bmp` (dumped from `glTexImage2D`) is a colourful
      256×256 image, so asset reads and texture uploads work.
    - **Frames are black:** captures at frames 30/60/120 are black. The `prog 30` menu draws have
      12/996 indices but the client vertex array (`stride=36`, pointer `0x8031f30`) is all zero at
      draw time. The window shows black and Windows marks it “Not Responding” during the long frame.
    - **Threads are still stubbed:** `pthread_create` remains a no-op. The only thread observed is a
      persistent timer routine at `0x68d694`; a synchronous-thread experiment was reverted.
    - **New frontier:** after `menu_staticSection1.bdae` and `DefaultEffects.bdae`, the fast path
      reaches `IMaterialTechniqueMapsReader::processMapGroup` (`0x5313d4` ELF) and writes
      `0x5f383065` through a bad pointer `0x5f38304d`. REA pseudocode: `processMapGroup`
      (`ev_37c8...`), `load` (`ev_1a40...`), `setError` (`ev_b846...`). The scene cannot finish
      building geometry until this is resolved.
    - Diagnostic GL state/texture dumps are gated by `FAST5_GL_TRACE=1`; `FAST5_NO_CULL=1` remains a
      targeted diagnostic. Default runs are quiet apart from real shader/program errors.
39. **Dispatch performance + material-frontier diagnosis**:
    - `call_target` no longer uses the 20k-case C switch (which GCC compiled into a very large
      comparison tree). `link_all.py` now emits a sorted `TargetEntry` table and `call_target` does
      a binary search; `table.c` is compiled at `-O2`. Frame 30/60/120 now complete in ~15 s instead
      of ~30 s for two frames, and a one-file runtime rebuild is ~1.9 s.
    - `tools/build_win.py` is incremental (object/source/header mtimes, dirty-only parallel compile,
      link only if needed; `--force` for full). `link_all.py --emit-only` preserves unchanged chunks
      and reports changed-file counts.
    - The previous “JPEG loader” theory was wrong for the current stall: an override on
      `CImageLoaderJPG::loadImage` (`0x54fd10`) was never hit. `CTexture::updateData` (`0x640d1c`,
      Ghidra) only logs `TEXTURE LEVEL` and uploads already-decoded data.
    - **Material fault now isolated:** `CMaterialRenderer::allocate` (`0x521328`) calls the
      `CMaterialRenderer` constructor (`0x520a20`). Entry logging shows correct argument order
      (`r0=this`, `r1=driver`, `r2=material index`, `r3=material name`, stack0=technique list), but
      the technique/pass data contains a field that points at the string table entry
      `sh_us.bsprite` (`0x703bb8`). That string is then read as a pointer (`0x755f6873`, the ASCII
      bytes `"sh_u"`) and dereferenced at `+0x18`, producing
      `guest fault at 0x755f688b (read)`.
    - Forcing an empty technique list moved the fault to `0xa8787715`; the list is not safe to stub.
      A trial change to the `stmdb sp!` writeback (`sp=mult_addr` instead of `+4`) made constructors
      abort in `_Unwind_Resume_or_Rethrow`; it was reverted. The fault is therefore a data/field
      mapping problem in the technique/parameter list, not a simple stack shift.
    - The guest allocator now validates block magic and sizes on free/realloc (`GBLOCK_MAGIC` for
      live blocks, `GFREE_MAGIC` for freed blocks), and reused blocks are reused through a free list.
      This removed allocator-poisoning risk, but did not change the material fault.
    - **Material 43 boundary:** the last successful constructor is material 43
      (`ProfileCOMMON_Material__117849376...menu_staticSection1`). Its allocate entry has 7 technique
      nodes, each with one pass, and all pass `f7` shader pointers and all 22 parameter pointers are
      valid guest-heap addresses. The fault is inside the constructor while following a shader/pass
      field at `+0x18`; the observed value is still a texture-string pointer (`sh_us.bsprite`), so
      the remaining mismatch is in the `IShader`/render-pass field layout consumed by
      `CMaterialRenderer`, not in the list heads or parameter array.
    - Temporary list/shader probes were removed; the override hook remains. Later diagnostic
      wrappers are listed in item 40.
40. **Native boot loop reached; menu load no longer faults (historical; superseded by item 41)**:
    - **Root fix — guest `sprintf` was missing.** `runtime/imports.c` had no `sprintf` import handler, so
      `sprintf("menu_screen_dummy_slot%d_0%d_node", slot, ...)` returned without writing and left the
      name empty. `CMainMenuObjects::ReloadDummies` therefore compared every lookup against `""`,
      matched the scene root, and looped forever. A guest-ABI `sprintf`/`snprintf` formatter was added
      (ARM softfp varargs: `r2/r3`, then the guest stack; integer/string/pointer/float conversions).
      With the fix, the first lookup text is now
      `menu_screen_dummy_slot0_00_node`; the dummy loader leaves its loop normally.
    - **Material parameter IDs:** the high-bit “global” IDs in several materials (for example
      material 43 entries `e66c`/`e888`) index empty or string-valued slots in
      `CGlobalMaterialParameterManager`. The constructor then dereferences a string as an object.
      The current host guard normalizes only high-bit IDs whose manager slot is not a valid guest
      object to `0`, and `grabInternal` (ELF `0x50b9f4`) refuses invalid slots. This is a
      **workaround**, not the final fix; it lets materials 43–50 construct and the scene load continue.
    - **JNI capability query:** `nativeCanAndroidPhonesUsingMenu3D` (ELF `0x29a0b4`) called
      `NVThreadGetCurrentJNIEnv`, which returned the uninitialized/ELF-header address `0x10000`; the
      JNI vtable call then read ELF magic. It is answered on the host as **1 (3D menus)**. The general
      JNI environment is still not initialized; other Java calls can hit the same class of fault.
    - **Guest heap size:** the 256 MiB arena was exhausted during menu loading (`gmalloc` had a 4 MiB
      request at `0x0fcba460`). The guest arena is now **512 MiB** (`main_win32.c`), and helper bounds
      use `guest_mem_size`. `gmalloc`/`gfree` also distinguish live (`GBLOCK_MAGIC`) from free
      (`GFREE_MAGIC`) blocks to reject double frees.
    - **Current runtime behaviour:** the `FAST5_FAKE_GPU=1` path loads the menu scene
      (`#menu_favelas04.max`, `All_merge_low_.max`), loads menu sounds, and reaches a continuous
      frame loop with **no guest fault**. Observed: **frame 6600**, about **32 FPS**, window responsive
      at the title, but only **2 GL draw calls** and a black game surface. The window capture used a
      screen copy and once captured the terminal behind the game, so visual screenshots are not yet
      trustworthy. The game is still loading/menu-transitioning or waiting on a missing worker.
    - **Diagnostic/workaround overrides currently in `runtime/overrides.c`:** `0x54fd10`
      (`CImageLoaderJPG::loadImage`, log + call original), `0x5dd314` (libjpeg row alloc, log + call
      original), `0x5dd5d4` (`jpeg_mem_available`, return max; never hit), `0x29a0b4`
      (`nativeCanAndroidPhonesUsingMenu3D -> 1`), `0x520a20` (parameter-ID normalization, shader-field
      normalization, material-43 ID dump), and `0x50b9f4` (`grabInternal` invalid-slot guard). Remove
      the JPEG/ID dumps and the unused `0x5dd5d4` hook before the next phase; keep/evaluate the rest.
      The verified fixes are `sprintf`/`snprintf`, the material-ID guard, the JNI answer, 512 MiB, and
      the allocator magic.
    - **Still stubbed:** `pthread_create` is a no-op; the only observed create is the persistent timer
      routine `0x68d694`. Menu loading may be waiting on that worker or on audio. This is the next
      scheduler/threading task.
41. **Title and main menu render with real textures (current)**:
    - **Black-menu root cause:** bionic's `_tolower_tab_` and `_toupper_tab_` are **16-bit**
      (`unsigned short[257]`, indexed by `c + 1`), but `runtime/guest_data.c` wrote **8-bit** entries.
      The engine's `guessShaderVertexAttribute` therefore lowercased every shader attribute name to
      garbage and returned **255** for `Vertex`, `TexCoord0`, and `Color0`. `CGLSLShader::linkProgram`
      skipped `glGetAttribLocation`, leaving all attribute locations at 0; `setupArrays` then pointed
      every stream at location 0 and the position arrays read zero.
    - **Fix:** `guest_data.c` now writes real 16-bit lowercase/uppercase tables at `0x9A1300` /
      `0x9A1600`, with ASCII entries. Negative-range entries are zero; filling them with signed
      values exposed a TiXml fault at `0x6cb110`, while zero-filling booted cleanly. `runtime/gl_win32.c`
      also deduplicates repeated `attribute` names from `#if` branches and binds declaration order
      (`Vertex=0`, `TexCoord0=1`, `Color0=2`).
    - **Verified result:** program 30 reports three active attributes and now calls
      `glVertexAttribPointer` for locations 0/1/2 with distinct pointers
      (`0x8031f20` / `0x8031f2c` / `0x8031f34`, stride 24); the first position is
      `394.000 480.000 0.000` instead of zero. `game_frame_30.bmp` is the Universal logo,
      `game_frame_120.bmp` is the **Fast Five title**, and after a Space key
      `build/clean_menu.png` shows **Quick Race, Career 0%, Multiplayer, Options, More games!**
      and Gameloft LIVE. No guest fault; about 30–35 FPS on ANGLE/D3D11. `build/clean_title.png`
      is the title capture.
    - **Texture status (updated 2026-11-07):** all 14 Favelas requests now resolve to full-header
      `.pvr` aliases in the derived overlay with zero `Can't find texture` logs. The `.LINEAR`
      payload is a valid engine PVR image (`0x34` header, `PVR!` at 44, format `0x19`); the
      `CImageLoaderPVR::loadImage` path is table `0x550c28`. The car body was later fixed by the
      3D body-atlas bind and row-order changes in the current pass; the Favelas environment still
      needs its own binding check. The failed PVRTCDecompress/TGA/ZIP experiments and the exact
      next trace are in the pass section below.
    - **GL error cleanup:** the two `GL_INVALID_OPERATION` reports came from
      `glGetProgramInfoLog` on objects 112/115, which were shader IDs, not created program IDs.
      `runtime/gl_win32.c` now tracks created programs and returns an empty log without calling GL
      for a non-program. A guarded run has **no `GL_INVALID_OPERATION`** and still reaches the menu
      (`build/final_boot_menu.png`).
    - **Quiet boot:** first-use import logging and the 3-second watchdog are now opt-in
      (`FAST5_IMPORT_TRACE=1`, `FAST5_WATCHDOG=1`). The default boot log contains the game's own
      logs only; `build/quiet_boot.err` is the clean reference. The unsupported PVRTC extension is
      no longer advertised; removing it did **not** change the game's requested texture names in
      either fake-PowerVR or real-ANGLE runs.

## Texture work in this pass (2026-11-07)

- **Car body binding (historical):** an early workaround substituted the tracked Highline body
  whenever an FBO pass bound GL texture id 8, which was `Sprites/menu_font_sm_highres`. That global
  substitution is **removed** (2026-11-08): it could overwrite unrelated textures during any
  off-screen pass, including reflection RTs. Texture binding is now resolved from the texture
  object's own GL name, and each car body gets its own batch material, so the menu car reads the
  correct body without a global override.
- **Body row order fixed:** body TGAs use descriptor `0x08` (bottom-left origin); the engine upload
  path leaves them inverted. The bridge now flips every tracked `*_Body*.tga` texture at upload
  (per GL id, cleared on delete) before `glTexImage2D`. Wheels, glass, interior, and UI textures are
  unchanged and still align.
- **Implemented earlier:** `gl_win32.c` converts BGRA/BGR uploads to RGBA/RGB; `overrides.c`
  rewrites `.PVRTC2` and `mm_*.PVRTC4` requests at `0x619238`/`0x619170`/`0x61a260`;
  `extract_archive_tgas.py` writes raw `.tga` plus full-header `.pvr` aliases;
  `extract_car_textures.py` extracts 148 car TGA/PVR basenames.
- **Verified:** car body and wheels are textured in `build/final_body.png`; all 14 Favelas requests
  resolve with zero not-found logs; `FAST5_AUTO_MENU=1` reaches the menu without a guest fault.
- **Rejected/removed:** standard-ZIP archive overlay (`0x464c458b` fault); PVRTC-marked `.tga`
  aliases; direct `PVRTCDecompress` at `0x5cd114` (transparent output); hardcoded texture-95 swap and
  unlit-pass skipping were test-only and removed.
- **Open:** the Favelas environment/background is still not visibly proven; `.LINEAR` is a valid
  full-header PVR (`0x34`, `PVR!` at 44, format `0x19`) but the visible environment needs its own
  binding check. The car body is now correct; remaining texture work is scene/background alignment
  and verifying the PVR loader path, not the car.

## In progress

- **PVR image path (current):** the Favelas `.LINEAR` payload is a valid engine PVR image. The
  engine parser `FUN_00560814` expects a `0x34`-byte header, `PVR!` at offset 44 and format byte
  `0x19`; `CImageLoaderPVR::loadImage` (`0x550c28` table entry) supports format `0x19`.
  `tools/extract_archive_tgas.py` writes full-header `.pvr` aliases into
  `build/overlay/Track/Favelas/`; the texture manager resolves all 14 Favelas names with **zero
  not-found logs**. The visible environment still needs a binding check; do not infer success from
  filename resolution alone.
- **Car textures:** `tools/extract_car_textures.py` extracts 148 TGA/PVR basenames, including
  `Highline_H3_1972_Body.NOPVR.tga`, `Cranky_M3_2011_Body.NOPVR.tga`, and
  `Comet_Rice_Rocket_2011_Body.NOPVR.tga`. The 3D body/material bind and row-order fixes above are
  verified; body livery should be compared against a reference frame.
- **BGRA upload fix:** `runtime/gl_win32.c` converts `GL_BGRA_EXT`/`GL_BGR_EXT` uploads to
  `GL_RGBA`/`GL_RGB` before ANGLE. The former path passed BGRA internal/format values to GLES2.
- Mouse click → guest touch works; left-button down/up carries normalized coordinates.
- Two Vox worker threads start; audio output/timing still needs a longer test.
- `FAST5_AUTO_MENU=1` deterministically enters the menu; `FAST5_AUTO_RACE` still needs
  coordinates/timing.

## Texture binding root-cause trace (2026-11-07, this pass) — FIXED

**Status: the striped band is FIXED.** The main menu now renders with the proper backdrop, the
Fast Five logo, all menu items, and both showcase cars correctly textured (red Highline + green
Cranky with its body/reflection textures). Zero guest faults. The fix is host-side in
`runtime/overrides.c` + `runtime/gl_win32.c` (`gl_bridge_force_sampler`).

**The actual root cause chain (fully verified with instrumented runs `fix2`–`fix6`):**

1. The menu garage renders into FBO 4/5 RTTs. Materials are committed per draw by
   `commitCurrentMaterialImpl` (0x634e84 emit / 0x644e84 Ghidra) via
   `commitCurrentMaterialParametersAux<CMaterial>` (0x62e300).
2. The pass binding buffer has **three groups**: `[params | samplers | globals]` with counts at
   pass+0x28 (group 1) and +0x2a (group 2). **The sampler bindings live in group 2.**
3. `commitCurrentMaterialImpl` commits **only group 1**. Group 2 is committed by
   `commitCurrentMaterialIndirectParameters` (0x62b770), which runs only via
   `commitPassParameters` (0x634d74) as a tail call from `drawImpl` (0x634640).
4. **The menu scene's environment materials are drawn through the *virtual*
   `commitCurrentMaterial` (0x633c58, 8 bytes, tail-jump to Impl only) — group 2 never runs for
   them.** Their group-2 bindings are correct ({uniformRef=sampler record, paramIndex=0} into a
   correct material def {cls=2, type=0xc, off=0}), verified byte-for-byte — they are simply never
   committed. Car materials go through `commitPassParameters` and bind fine.
5. Because group 2 was never committed, those materials sampled whatever was on unit 0 — during
   the menu that is the UI font atlas / logo — the striped vertical band.

**The fix:** in the `call_override` hook for 0x634e84, snapshot the pass binding groups before
the call; after `fn_634e84` returns, walk group 2's bindings and for each sampler-type material
def (type 0xc–0xf) read the texture pointer from the material value buffer (mat+0x18+def offset),
resolve its GL id via the bridge's `g_tex_ptr_map` (texture-object pointer → GL name captured at
`glBindTexture`), and call `gl_bridge_force_sampler(loc, unit, tex_ptr)` which does
`glActiveTexture + glBindTexture + glUniform1i` directly on ANGLE and updates the bridge's unit
tracking. Idempotent for the car path (setTexture skips same-texture binds).

**Verified result:** BUILDINGS_00/FAVELAS_STREET bind again, `fbo=4/5` draws sample real car
textures (81/92/93/94/95/96), the Cranky body + reflection textures bind (0x0ea37c18,
0x122772a0), no guest faults, menu renders correctly (see `build/fix3_menu.png`,
`build/fix5_menu.png`).

**Fix 2 — environment texture replay (2026-11-07, this pass, EXACT):** the scene builder assigns
each batch's texture onto ONE shared material (observed 0x08a5c908) and commits it per batch during
load. The final per-batch materials (0x08a9dee0, 0x10684xxx, 0x10bd9xxx) are created later by the
material-renderer manager and never receive the values — they keep a heap-stale `loading_anim`
pointer. The exact per-batch textures were recovered from the shipped scene data instead of
guessed:

- The 811 MB of `GloftF5HP/res/file*.dat` packs are ZIP archives; `file000078.dat` holds
  `Scene/MenuGarage/menu_staticSection1.bdae` and `menu_loopableSection1.bdae`.
- Each `.bdae` is itself a ZIP whose `little_endian_not_quantized.bdae` is the BRES scene graph.
- At runtime the deserializer materializes the whole definition table in guest memory
  (`Material__NN` blocks, param slots, and texture-attribute `Map__<id>__<file>` entries). The
  dump/analysis decoded it end to end: node list → batch material references
  (`#Object01-mesh → #Material__117849350`, …), and definition → params → attribute → texture
  (`Material__19 → service_02.tga`, `20 → service_03`, `18 → service_04`, `22 → service_01`,
  `117849364 → market_03`, `117849369 → service_01`, `117849370 → market_09`, `23 →
  service_aditiv`, `186 → market_alpha`, `21 → service_05`, `24 → service_06`, skybox →
  `sky_box_sky`).

`runtime/overrides.c` now carries that scene-derived list (`g_env_scene_tex`) and resolves each
draw-time stale material by NAME (recorded from the loader's `setTexture` binds, case-insensitive)
in scene-node order; materials beyond the list (the car) are left untouched. Verified mapping at
runtime (`sceneTex:` under `FAST5_TEXTURE_TRACE=1`): every garage material receives exactly its
scene-defined texture; 0 guest faults, 0 GL errors (`build/scene_tex3.png`).

**Remaining known flaw (next step):** the true root is that the scene loader never runs
`setParameter<ITexture>` for those materials (see below); the replay is a deterministic
host-side substitute. Once the loader assignment is fixed, remove the replay.

**Fix 4 — THE menu-wide root: `lrand48` was not implemented (2026-11-07, final).**
`glitch::video::CGenericBaker::getBatchMaterial` (Ghidra 0x648060 / `SDefaultEndOfBatchCallback::finalize`
0x4d8c94) builds every batch material's name from **20 random characters drawn with `lrand48()`**
(`"0123456789ABC…z"[lrand48() % 62]`). The port had no `lrand48`/`srand48` import, so every call
returned 0 and every batch material registered as `"00000000000000000000"` — one shared name for the
whole menu, collapsing the name-keyed material-renderer registry onto a single placeholder material
with a stale texture slot. `runtime/imports.c` now implements `lrand48`, `srand48`, `drand48` and
`seed48` (POSIX 48-bit LCG). With unique names, each batch gets its own material and
`initParameters` copies the source scene material's values (already correct via Fix 3), so every
surface renders with its own texture. Verified (`beginMatRender`/`forceSampler` under
`FAST5_TEXTURE_TRACE=1`): 0 zero-name registrations, distinct materials per surface, e.g.
`MENU_LOOPABLE_LIGHTINGMAP + MM_FAVELAS_STREET / MM_SIDE_WALK / MM_STONE / MM_GRASS /
MM_CEMENT_00 / MM_STUFF / MM_BUILDINGS_00 / MM_BUILDINGS_01 / MM_FAVELAS_REFLECTION /
MM_STREET_C_00`, skybox `SKY_BOX_SKY`, car materials unchanged; 0 guest faults, 0 GL errors
(`build/rand1_menu.png` — brick buildings, sky, stone street, correctly textured car).

**Fix 3 — REAL ROOT FIX for the missing scene textures (2026-11-07, final):** the engine's own
value application now runs. Chain (REA, Ghidra addresses):
`glitch::collada::createMaterial` (0x59c0b8) iterates the scene material's parameter list and
resolves each name with `CMaterialRenderer::getParameterID(char const*, u16)` (0x53157c). That
lookup interns the name through the shared-string heap (`SSharedStringHeapEntry::SData::get`);
when the heap lookup fails it returns `0xffff` and `createMaterial` **silently skips the value**
(only type-0x14 technique params have a fallback path). That is why `setParameter<ITexture>` was
never called for the environment materials and their slots kept the stale `loading_anim` pointer.
Runtime override at `0x52157c` (ELF space) replaces the heap lookup with an exact `strcmp`
against the renderer's def names (`def->SData + 4`). With that, `createMaterial` performs the type
check (`DAT_0073ae98[defType]` bitmask) and calls the engine's own
`IMaterialParameters<CMaterial>::setParameter<ITexture>` (0x52ac28) for every texture parameter —
138 applications observed, e.g. `LightMap→MARKET_LIGHTMAP`, `DiffuseMap→MARKET_04/06/07/05/03/
01/02…`, one per material definition from the scene data. The garage/menu now renders with the
scene's real textures; the white/untextured overlay is gone; 0 guest faults, 0 GL errors
(`build/fix7_menu.png`). All fallback/rank/replay texture assignment has been REMOVED — the only
texture path is the engine's own definition-driven assignment plus the previously verified
group-2 sampler commit (`gl_bridge_force_sampler`).

**Root cause of the missing assignment (measured, historical):** `IMaterialParameters<CMaterial>::setParameter<ITexture>`
(ELF 0x51ac28) is **called 0 times for these materials** (verified with a fully instrumented run:
3000+ calls logged, none for 0x08a9dee0/0x10684838/0x10bd97f0; 1 rejection total in the whole
run). The setter itself works (ret=1, defType=0x0c matches every texture's type bits). The
engine's `glitch::io::CTextureAttribute::getTexture` (0x504fc8) simply returns the attribute's
cached texture pointer — so the attribute's texture is NULL too: the scene loader's texture
resolution never runs for these garage materials. Their name/attribute data is not present in the
surviving material object (descriptor dump: shared defs at desc+0x20, pass data at desc+0x64, no
texture-name strings), so draw-time name-based recovery is not possible; the real fix must be in
the scene-loader assignment path (hook the material/attribute deserializer used by
`menu_favelas04.max` / `menu_staticSection1.bdae` and find where the texture attribute resolution
is skipped).

**Diagnostic instrumentation added this pass (all behind `FAST5_TEXTURE_TRACE=1`):**
`texmap:`/`texupload:` (GL id ↔ guest texture name via bindImpl r0), `setTexture:`,
`getPlaceHolder:`, `grabInternal:` (+ manager entry hex), `cmr-ctor/cmr-raw/cmr-defs:`,
`linkProgram:` (+ uniform records with size), `cmi:` (per-commit pass groups + material defs),
`aux:` (Aux entry args + stack window), `indirect[ENV/CAR]:`, `forceSampler:`,
`setTexParam:`/`-> ret=` (material texture assignments), `matDump`/`descStr` (material objects).
Also fixed the `uniform1i prog=30` log flood in `gl_win32.c`.


2. Compare the now-aligned car body livery and material colors with a reference frame, then remove
   any remaining body-binding workaround if the root material ID is identified.
3. Capture the menu at native resolution after the scene-texture check.
4. Run a longer Vox-thread test and verify no data races in audio/timing.
5. Resolve `CFast5Effect` `service_smoke`/`service_dust` names and keep the material-ID/JNI
   workarounds until their roots are fixed.




## Blockers / risks

| Item | Status |
|---|---|
| `GL_INVALID_OPERATION` from `glGetProgramInfoLog` on 112/115 | **resolved**: non-program objects are ignored; no GL error in the guarded run |
| Missing menu textures (`mm_*PVRTC2.tga`) / black-white car bands | **partial**: all 14 names resolve to full-header `.pvr` aliases with no not-found logs; car body/wheels are textured via the per-car body material and row flip, scene/background still under trace |
| `CFast5Effect` `service_smoke` / `service_dust` | **exact names absent from all archives**; optional effects or missing pack |
| `pthread_create` no-op; timer routine `0x68d694` never runs | **resolved**: `0x68d694` is `vox::funcUpdate`; two Vox threads start; join/mutex implementation is in place |
| PVRTC textures upload placeholder | no compressed upload observed; unsupported PVRTC extension removed; full `.pvr` loader path now selected by aliases |
| JNIEnv not initialized | open; one capability query bypassed, general Java calls unsafe |
| Material-ID normalization / grab guard | workaround in place; root registration still unknown |
| Car body texture binding | **resolved**: body textures resolve from the texture object's own GL name and each car body gets its own batch material; the old global GL-id-8 font-atlas substitution was removed; body rows are flipped per `*_Body*.tga` texture; compare livery against reference |
| Title-to-menu input | **resolved for menu**: `FAST5_AUTO_MENU=1`; Quick Race auto-tap still needs coordinates/timing |
| Host debug noise | **resolved**: imports/watchdog opt-in (`FAST5_IMPORT_TRACE`, `FAST5_WATCHDOG`) |
| Android device for reference runs | none attached (0 devices) — M7 needs one or an offline emulator |
| qemu-user oracle for differential tests | not verified yet; check `qemu-arm` in WSL |
| Window capture correctness | `PrintWindow` helper verified; old `capture_window.ps1` still screen-copy |
| Ghidra cold import of 8.36 MB ARM binary | likely 5–20 min; raise startup timeout before the first call |
| REA `adb` provider | not on REA's PATH; Craft's adb works, so device work uses Craft |

## Session conventions

- Evidence ids look like `ev_<sha256>`; record them in `docs/06-EVIDENCE.md`.
- Expensive analyses are done once; cite the evidence id afterwards.
- Never write outside this workspace; never modify the two immutable inputs.
- `build/link/` is disposable generated output: never edit it. Recovered functions go into
  `analysis/reports/pcode_extra.tsv`; host behavior goes in `runtime/`; per-function host overrides
  go in `runtime/overrides.c` via `call_override`, so regeneration cannot erase work.
- Windows builds are incremental: `python tools/build_win.py` compiles only stale sources and links
  only when needed; `--force` rebuilds everything. `link_all.py --emit-only` preserves unchanged
  generated chunks, so a runtime edit normally costs one object plus the link.
- The guest arena is currently **512 MiB** (`main_win32.c`, `size = 0x20000000`). Runtime helpers that
  classify pointers should use `guest_mem_size`, not the old `0x10000000`/`0x0f000000` literals.
