# 02 — Architecture

How the shipped game is put together. Observed facts are cited; anything marked **[hypothesis]** is a
named prediction to be confirmed by decompilation, never used as a premise.

## 1. Layers

```
┌──────────────────────────────────────────────────────────────────────┐
│ Java (classes.dex, 101 classes)                                      │
│  GLGame (Activity) ─ GameGLSurfaceView ─ GameRenderer ─ GLResLoader  │
│  GLAsyncPlayer (audio)   GameInstaller   GLiveMain (WebView)         │
│  DRM.Gloft.GloftDRM      GLUtils.{Device,HTTP,XPlayer}               │
└───────────────────────────────┬──────────────────────────────────────┘
                                │ JNI (System.loadLibrary)
┌───────────────────────────────▼──────────────────────────────────────┐
│ libFast5.so  (libjni.so) — 8.36 MB, ARMv7-A, VFPv3, softfp, GCC 4.4.3│
│                                                                      │
│  glitch::  ── Irrlicht-derived engine                                │
│    scene/      ISceneNode, CBatchMesh, ICameraSceneNode, animators    │
│    video/      IVideoDriver, GLES2 driver, CImageLoaderPVR (+PVRTC)   │
│    collada/    scene + animation_track import                        │
│    io/         IReadFile, CLimitReadFile, CUnZipReader               │
│    core/       vectors, matrices, shared strings, allocators         │
│    CAndroidOSDevice  (postKeyEvent / postMouseEvent)                 │
│                                                                      │
│  spark::   particle system                                           │
│  vox::     audio engine (banks, decoders, MP3/ADPCM cursors)          │
│  game:     GS_* states, GP_* gameplay, PhysicCar, RaceCar,           │
│            NitroPickup, CustomTraffic, FastFiveMpManager,            │
│            GLXPlayerMP*, Camera, Comms, SoundManager, ALicenseCheck  │
└───────────────────────────────┬──────────────────────────────────────┘
                                │ GLES2 + libc/libm/libstdc++/liblog/libdl
┌───────────────────────────────▼──────────────────────────────────────┐
│ Android platform                                                     │
└──────────────────────────────────────────────────────────────────────┘
```

## 2. Boot sequence (confirmed)

Every step below is observed in decompiled code, not inferred.

```
GLGame.onCreate                          (Activity, Java)
 ├─ if !GameInstaller.a  →  launch GameInstaller activity, finish()
 ├─ GameGLSurfaceView created, setContentView
 ├─ GLResLoader.init()
 │    └─ GLResLoader.nativeInit()        (JNI export 0x29bf2c)
 │         ├─ log
 │         ├─ NewGlobalRef(env, GLResLoader class)
 │         └─ (rest hidden behind an indirect jump — Ghidra to resolve)
 ├─ Device.init()                        → GLUtils.Device.nativeInit (0x29a1b4)
 └─ TelephonyManager.listen, locale, wifi

GLSurfaceView thread:
GameRenderer.onSurfaceCreated
 ├─ reads Display width/height
 ├─ GLResLoader.init()                  (again)
 ├─ GLGame.nativeInit()                 (JNI 0x2996ec)
 │    ├─ log
 │    ├─ store env in a global
 │    └─ NewGlobalRef(env, GLGame activity)       // vtable index 21
 └─ waits until GameGLSurfaceView.a/b != -1 (set by onSizeChanged = width/height)
      then returns — the `nativeInit(w,h,lang)` call below the loop is unreachable in the
      normal path (only an exception during the wait can reach it)

GameGLSurfaceView.onSizeChanged → static a = w, b = h

GameRenderer.onSurfaceChanged
 ├─ if MyVideoView.isVideoCompleted() == 0 → start MyVideoView with
 │     /sdcard/gameloft/games/GloftF5HP/res/LOGO_Gameloft_480x800.mp4
 └─ if GameRenderer.b → nativeResize(w,h)   (observed: log-only, no engine effect)

GameRenderer.onDrawFrame                   (every frame, hard 20 fps cap via Thread.sleep)
 ├─ returns early while MyVideoView.isVideoCompleted() == 0
 └─ nativeRender()                       (JNI 0x299460) → appRender(0,0,0) at 0x29cb14
```

The engine entry is therefore:

| Symbol | Address | Role |
|---|---|---|
| `JNI_OnLoad` | `0x29c604` | `NVThreadInit()`, `VoxSetJavaVM(vm)`, `vm->GetEnv(JNI_VERSION_1_4)`; rest behind indirect jump |
| `appInit` | `0x29c830` | called once from `GameRenderer.nativeInit`; 8 blocks / 114 instructions / 12 callees |
| `appRender` | `0x29cb14` | called from `nativeRender`; 13 blocks / 143 instructions / 10 callees |
| `NVThreadInit` | `0x29c310` | thread/JNI bookkeeping |
| `NVThreadGetCurrentJNIEnv` | `0x29c324` | env lookup for native threads |
| `VoxSetJavaVM` | `0x6892fc` | gives the audio engine the JavaVM |
| `GetNativeResourceFull` | `0x29c0ac` | APK-resource bridge (Java callback) |
| `GetNativeResourceBytes` | `0x29c198` | APK-resource bridge |
| `GetNativeResourceLength` | `0x29c280` | APK-resource bridge |

`sub_2980e8` (273 call sites) and `sub_298844` (6 call sites) are 12-byte import thunks
(`adr ip, …; add ip, …; ldr pc, [ip, …]`), i.e. PLT entries — their real targets are resolved by
Ghidra in M1.

### Java-side surface the native code depends on

- APK resources are read through `GLResLoader.getResourceFull/Bytes/Length(String)` — the native code
  calls back into Java (`GetNativeResource*`), so the port must provide those callbacks (or replace
  them with direct reads of the same APK entries).
- Input arrives through `GLGame.nativeTouchPressed/Moved/Released`, `nativeSetOnKeyDown/Up`,
  `nativeAccelerometer`, `nativeOrientation`.
- Lifecycle: `GameGLSurfaceView.nativePause/nativeResume/nativePauseAllSounds`,
  `GameRenderer.nativeDestroy`, `GLGame.nativePauseInGame`.
- Audio: `GLMediaPlayer.nativeInit` (2,048 B) and friends are exported, but `GLMediaPlayer` is not in
  this DEX — **[unknown U8]** whether this is dead legacy code or reached through another class.

### Native boot internals (Ghidra, ELF addresses)

**Address convention:** Ghidra maps the ELF at image base `0x10000`, so every Ghidra address is
`ELF address + 0x10000`. All docs use ELF addresses unless marked "Ghidra".

#### `JNI_OnLoad` — 0x29c604

```c
NVThreadInit(vm);                 // stores JavaVM* in a global (0x7f6968 in .bss)
VoxSetJavaVM(vm);                 // audio engine gets the JavaVM
DBG_OUT("JNI_OnLoad called");
if (vm->GetEnv(&env, JNI_VERSION_1_4) == JNI_OK) {
    ALicenseCheck_setJavaVM(vm);
    mWaitForInit = 10;            // first 10 appRender calls are skipped
    return JNI_VERSION_1_4;
}
return -1;
```

#### `appInit(w, h, lang)` — 0x29c830

- `VersionGame = nativeGetVersion()` (string from Java).
- Landscape normalisation: `SCR_WIDTH = max(w,h)`, `SCR_HEIGHT = min(w,h)`.
- Builds `g_appPath` = `/sdcard/gameloft/games/GloftF5HP` **byte by byte** (so the string is not a
  single `.rodata` literal).
- Builds `g_saveDirPath` = `/data/data/com.gameloft.android.GAND.GloftF5HP.ML/`.
- Calls `Application::CreateInstance(g_appPath, 1, 0, SCR_WIDTH, SCR_HEIGHT, VersionGame)`.

#### `Application::CreateInstance`

- Destroys any previous instance.
- If `SCR_HEIGHT < 512`, halves every entry > 511 in the `aFlashFileData` table (14 × 24-byte
  entries) — a low-resolution asset/LOD adjustment. **This branch must be reproduced exactly.**
- `new Application`, call constructor, `Init(...)`, then `g_nRandomSeed = 0; g_nMPRandomSeed = 0;`
  (deterministic seeds — important for reproducible races).

#### `Application::Init` — order matters

1. `m_DriverType = 1`.
2. `nativePutDeviceToSleep()`.
3. Scale factor: `2.0` if `SCR_WIDTH > 480`, else `1.0`; stored at `this+600`.
   `this+0x254 = height/scale`, `this+0x250 = width/scale`.
4. `Game::NATIVE_SCREEN_HEIGHT/ WIDTH = h / w`.
5. `Game::CreateGame()`.
6. `glitch::core::setProcessBufferHeapSize(0x80000)` (512 KiB).
7. `SetUIOrientation(0, 1)`; `this+0x22c = -1`.
8. **`DeviceConfig::SetupGameForDevice()`**.
9. `Game::Init(Game::s_pInstance)`.
10. `this[0x215] = 1` (running), timestamps at `this+0x21c/0x220`.
11. `ProfileManager::SetCurrentProfile(0)` → load profile → language; if `< 0`, `nativeDetectDeviceLang()`
    and persist the choice.
12. `StringManager::RefreshPacks(1)`, `IGPSendGameInfo(lang)`.
13. Virtual call `(*(Game::s_pInstance+0x10c)+0x10)->vtable[0xb0](0x100)` — subsystem configured with 256.
14. `SetVersion(this, VersionGame)`.
15. `new XPlayerManager()`.

#### `DeviceConfig::SetupGameForDevice` — device profile is fixed

- Logs `"PLATFORM_I9000"` and sets `s_DeviceType = 0xB` (11) **unconditionally**. The build always uses
  the Samsung I9000 (Galaxy S) profile, independent of the actual device. This is ideal for a port:
  the reference profile is a constant.
- `CheckHighMemDevice()` then selects body-texture and damage-resolution flags.
- Five LOD/clip values are read from `s_DeviceType`-indexed tables (`.rodata` at ELF `0x7058ac`,
  `0x705960`, `0x70599c`): menu car LOD, gameplay car LOD, gameplay factor LOD, default car LOD,
  gameplay far clip.
- `nativeGetDeviceID()` is consulted only in the high-memory branch; the reference ID is `9000`
  (Samsung I9000). The port must supply this value through the JNI shim.

#### `appRender` — 0x29cb14

- Returns immediately if `mbAppRunning == 0`.
- First 10 calls: decrement `mWaitForInit` and log (init grace period).
- If `m_timerForResume > 0`: count down; at zero call `Application::ResumeGame()`.
- Otherwise `Application::Update()`, `Application::ReadAcceleration()`, `Application::ReadOrientation()`.

#### `ALicenseCheck` path

- `ALicenseCheck::Init(env, jclass)` caches four static methods on the Java class
  (`d()V`, `db()[B`, `dc()[B`, `da()Landroid/app/Activity;`) with `GetStaticMethodID`, finds
  `android/content/Context`, clears `EKEY`/`EPID`/`SERVER`, and calls `ALicenseCheck::LoadConfig()`.
- `ALicenseCheck_InitLicense(env, clazz)` is called from **`GLGame.nativeInit`** (ELF `0x2996ec`).
  `ALicenseCheck_InitLicenseVM` (ELF `0x6bd094`) has no resolved callers and no data xref; it is
  either dead in this build or reached by an unresolved indirect path — recorded as U6.
- `ALicenseCheck::LoadConfig` calls `db()` into a 256 KiB stack buffer and runs `ConfigFile::Decode`.
  The blob is `res/raw/igli.bin` (262,144 bytes) when present, matching the buffer size exactly.
- `ConfigFile::Decode` reads a fixed layout (lengths at `0xC00`/`0x4800`/`0x19000`, data at
  `0x2000`/`0x5C00`/`0x19800`), applies the substitution table
  `GW45-19S.WK$RP0#O^3LTX_ 23U3SEDGF0 4A @#   . .. $# INVALIDEXCS57/8*`, then XORs with the
  `dc()` key to produce `EKEY`, `EPID` and `SERVER`.
- `ALicenseCheck_ValidateLicense(flag)` formats `SERVER` with `KEY`, `EPID`, `IMEI`, and when
  `flag != 0` calls `d()` (Tracking initialisation). It is called from
  `GS_MenuMain_Load::StateCreate`, `GS_Race_Load::StateCreate`, `GS_MPOnlineLobbyRoom::StateCreate`.

#### Original source layout (leaked `__FILE__` strings)

The binary was built on Windows from `D:/HD_Prj/_F5/Working/publishing/trunk/...` and keeps five
source paths:

```
.../Projects/Android/Libs_HTC/jni/jni/../../../../Android/GameSpecific/jni/./jni_GameRenderer.c
.../Projects/Android/Libs_HTC/jni/jni/../../../../Android/GameSpecific/jni/./app_Android.cpp
.../Projects/Android/Libs_HTC/vox/jni/../../../../../Externals/Vox/project/vs9/../../src/stb_vorbis.cpp
.../Projects/Android/Libs_HTC/vox/jni/../../../../../Externals/Vox/project/vs9/../../src/mpc/mpc_demux.c
.../Projects/Android/Libs_HTC/vox/jni/../../../../../Externals/Vox/project/vs9/../../src/mpc/mpc_decoder.c
```

Vox therefore decodes Vorbis (stb_vorbis) and Musepack (MPC) in addition to the Ogg seen in the APK.

### Virtual file system and data pack (confirmed)

`Game::InitResourcePath` builds the VFS in this exact order:

1. Append `"/res/"` to `g_appPath` and store it in the Application.
2. `addFolderFileArchive(<app>/res/, 1, 1)` — mount the data directory.
3. `changeWorkingDirectoryTo(<app>/res/)`.
4. `addObfuscationFileMap("file000000.dat", 0x4F)` — load the 651-entry name map.
5. Mount `Glitch/`, `Effects/`, `SWF/`, `strings/`, `Scene/`, `Audio/`.
6. Mount eleven archives: `Car.zip`, `TrafficCar.zip`, `Career.zip`, `Effects.zip`, `Fonts.zip`,
   `Glitch.zip`, `Objects.zip`, `Particles.zip`, `Scene.zip`, `Sprites.zip`, `strings.zip`.

Offsets were read from the real `CFileSystem` vtable (ELF `0x7ced20`): `+0x18` =
`addZipFileArchive`, `+0x1c` = `addFolderFileArchive`, `+0x24` = `addObfuscationFileMap`,
`+0x30` = `changeWorkingDirectoryTo`.

The map file is 25,582 bytes, key `0x4F`, and must end with `c|-|3k`. Its 651 entries map
`fileNNNNNN.dat` to the original asset name (mostly `.wav`, plus bundles such as
`trackTextures.zip`). Decoding was verified by executing the shipped loop under Unicorn and by a
byte-identical static model (`tools/deobfuscate_map.py`); the artifact is
`analysis/reports/dat-map.tsv`.

### Data root (confirmed)

`/sdcard/gameloft/games/GloftF5HP/res/` — the intro-video intent uses exactly that path, and the
project's `GloftF5HP/res/` folder matches it file-for-file (653 `.dat` + `oconf.bar` + `profile1.sav`
+ the `.mp4`). The engine's native data access is plain `fopen`-class I/O rooted at
`/data/data/com.gameloft.android.GAND.GloftF5HP.ML` plus this external path.

## 3. Remaining hypotheses

**[hypothesis H2]** Audio is produced by native Vox and played through Java `GLAsyncPlayer`
(`AudioTrack`). *Probe:* decompile `GLAsyncPlayer`, find the native buffer-fill export. No OpenSL ES
import exists (observed), so audio must cross the JNI boundary either way.

**[hypothesis H3]** `ALicenseCheck::Init` gates startup on `GloftDRM` data read from the APK
(`serialkey.txt`, `igli.bin`). *Probe:* decompile `ALicenseCheck::Init` + `GloftDRM`.

**[hypothesis H4]** PVRTC textures are decompressed at load time by the binary's own
`PVRTCDecompress`, not uploaded as compressed textures. *Probe:* decompile `CImageLoaderPVR::loadImage`
and `PVRTCDecompress`; check whether `glCompressedTexImage2D` is called with PVRTC formats.

## 3. Subsystems and their port cost

| Subsystem | What exists | Port implication |
|---|---|---|
| Window/GL context | created by Java (`GLSurfaceView`), never by native EGL | runtime creates Win32 window + WGL context; no EGL needed |
| Rendering | GLES2 via `gl*` imports; `glCompressedTexImage2D`, FBOs, shaders | map to desktop GL 3.3 core / GL 4.1; PVRTC stays PVRTC (desktop GL has `GL_COMPRESSED_RGB_PVRTC_4BPPV1_EXT` only with extension) → **[hypothesis H4]** we will decompress PVRTC with the binary's own `PVRTCDecompress` and upload RGBA, or expose the EXT texture compression path if the driver supports it. Fidelity note: decompressed RGBA is pixel-identical to what the Android driver would sample minus hardware filtering differences. |
| Scene graph | Irrlicht-style nodes, batched meshes, COLLADA import | translated, unchanged |
| Animation | `glitch::collada::animation_track` interpreters | translated, unchanged |
| Audio | Java `GLAsyncPlayer` + native `vox` decoders | runtime implements the Java-side player via WASAPI/XAudio2 while native decoders are translated |
| Input | `CAndroidOSDevice::postKeyEvent/postMouseEvent`; Device class exposes touch/key | runtime maps keyboard/mouse/gamepad to the same `SEvent` structures |
| Filesystem | native opens absolute paths under `/data/data/com.gameloft…`; Java installer places data | runtime mounts a virtual root: `/data/data/com.gameloft…` → project data dir (read-only) |
| Threads/time | pthread imports (`pthread_create` etc.), `gettimeofday` | runtime provides pthread-compatible layer over Win32; deterministic clock option |
| Networking | BSD sockets (`connect`, `getaddrinfo`, `accept`) — multiplayer/GLive | translated; online services are dead, so failures must degrade exactly like the original offline |
| DRM | `ALicenseCheck`, `GloftDRM`, `StringEncrypter` | translated; must satisfy the same checks without the Android installer |
| Exceptions | `.ARM.exidx` 161 KB / `.ARM.extab` 171 KB | runtime unwinder + translated landing pads (see strategy) |

## 4. Data access model

The engine never uses Android `AAssetManager` (no such import). It builds absolute paths from
`/data/data/…` (string at `.rodata 0x702774`) and uses plain `fopen`/`open`/`fread`
(all present in imports). Therefore the port can satisfy it with a **path redirection layer**:
guest path prefix `/data/data/com.gameloft.android.GAND.GloftF5HP.ML` and the SD/external path both
resolve into this project's immutable data tree. No repacking, no extraction.

## 5. Rendering contract to preserve

The Android build targets GLES2. Preserve:

- shader source and uniforms exactly as the game submits them (translate GLSL ES 1.00 → GLSL 1.50
  with a mechanical prefix/attribute rewrite; keep the game's uniform names);
- framebuffer sizes/format choices as chosen by `ConfigChooser` (RGB565/RGBA8888 + depth);
- texture formats (PVRTC 2/4bpp), wrap modes and mip generation;
- the fixed-function state machine emulation the engine implements itself (`glitch::video` has its own
  material renderer; it calls GL directly).

## 6. What "native" means here

At runtime the process is a normal Win64 executable. It has:

- no ARM code, no interpreter loop, no JIT;
- our runtime library statically linked;
- the original data files read in place;
- OS APIs only (kernel32/user32/gdi32/opengl32/xaudio2/ws2_32).

The translated engine code is ordinary C compiled by the host compiler. The only "emulation" left is
the guest address space and the C++ exception unwinder, both implemented as plain runtime code.
