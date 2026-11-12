# 01 — Target Dossier

Everything in this file is observed. Evidence ids are in `docs/06-EVIDENCE.md`.

## 1. Package

| Field | Value |
|---|---|
| APK | `00742-Fast-5-HD-1.0.9-PowerVR.apk` |
| Size | 4,077,503 bytes |
| SHA-256 | `fb8e26a16e2e243c43df2b88c26eee5bab62b738eb2c134232353f7f1809e14a` |
| Package | `com.gameloft.android.GAND.GloftF5HP.ML` |
| Version | 1.0.9 (versionCode 109) |
| minSdk | 5 (Android 2.0) |
| targetSdk | not declared |
| Native ABI | `armeabi-v7a` only (one `.so`) |
| DEX | 1 file, 170,496 bytes, 101 classes |

Permissions: `WRITE_EXTERNAL_STORAGE`, `MODIFY_AUDIO_SETTINGS`, `INTERNET`, `READ_PHONE_STATE`,
`RESTART_PACKAGES`, `WAKE_LOCK`, `ACCESS_NETWORK_STATE`, `ACCESS_WIFI_STATE`,
`CHANGE_WIFI_STATE`, `CHANGE_NETWORK_STATE`.

### Activities (from manifest, evidence `ev_34df83eb…`)

| Activity | Role |
|---|---|
| `…ML.GLGame` | MAIN / LAUNCHER entry |
| `…ML.installer.GameInstaller` | expansion download / verify / install |
| `…ML.MyVideoView` | intro movie playback |
| `…ML.GLiveMain` | Gameloft Live web layer (WebView + JS bridge) |
| `…ML.IGPActivity` | in-game purchase flow |
| `…ML.MPOnlineLogin` | multiplayer login |

### Java layer (DEX, 101 classes)

- Boot: `GLGame` (Activity, 51 methods) → `GameGLSurfaceView` (`GLSurfaceView`, custom
  `ConfigChooser` + `ContextFactory`) → `GameRenderer` (16 methods) → `GLResLoader`.
- Audio: `GLAsyncPlayer` (Java-side player; no OpenSL ES import in the `.so`).
- Platform utils: `GLUtils.Device`, `HTTP`, `XPlayer`, `ZipFile`, `SUtils`, `ConnectionTimer`.
- Installer: `GameInstaller` (140 fields, 62 methods), `installer/Utils`, CRC, Tracker, XML parsing.
- DRM/licensing: `DRM.Gloft.GloftDRM`, `Base64`, `StringEncrypter`, stream filters `a`/`b`/`c`.
  License gate is native: `ALicenseCheck::Init(_JNIEnv*, _jclass*)` exists in the `.so`.
- Obfuscated classes `a…z` are thin wrappers (typically 1 field / 2 methods).

## 2. Native library

| Field | Value |
|---|---|
| Path in APK | `lib/armeabi-v7a/libFast5.so` |
| Size | 8,357,420 bytes |
| SHA-256 | `1c8d16f572c3b504edb2084b1a7887bfe8c7659a5d67f0690e813c1e483ccb12` |
| ELF | ELF32, little-endian, `ET_DYN`, `EM_ARM` |
| SONAME | `libjni.so` |
| Entry | `0x298b60` (first address of `.text`) |
| Arch | ARMv7-A (`Tag_CPU_name "7-A"`), **VFPv3-D32**, no NEON |
| Float ABI | **softfp** — no `Tag_ABI_VFP_args`; `Tag_ABI_HardFP_use: Deprecated` |
| Compiler | GCC 4.4.3 (NDK-era), `-O` aggressive speed |
| Linkage | non-PIE, NX, stack canary, no RELRO |
| Symbols | local symbols stripped; **23,688 dynamic symbols** (23,377 defined, incl. 19,023 functions) |

### Measured inventory (Craft `core_overview_show`, `ev_ef012ea6…`)

| Metric | Value |
|---|---|
| Functions identified | 20,211 |
| Instructions | 446,913 |
| Basic blocks | 72,758 |
| Cross-references | 143,013 |
| Strings | 31,008 |
| Imports | 308 from 6 libraries |
| Jump tables resolved | 0 (3,548 indirect jumps unresolved by static scan) |
| Functions discovered by symbol | 19,022 |

### Imported libraries

`libGLESv2.so`, `libc.so`, `libm.so`, `libstdc++.so`, `libdl.so`, `liblog.so`.
No OpenSL ES, no EGL (EGL is reached through Java/GLSurfaceView), no Android asset manager.

### Sections that matter to the recompiler

| Section | Addr | Size | Role |
|---|---|---|---|
| `.text` | `0x298b60` | 4,626,296 | code |
| `.rodata` | `0x7022d8` | 306,592 | literals, vtables-in-data |
| `.ARM.extab` | `0x74d078` | 170,728 | exception unwind tables |
| `.ARM.exidx` | `0x776b60` | 161,072 | exception index (20,134 rows) |
| `.init_array` | `0x79f090` | 1,732 | 433 global constructors |
| `.data.rel.ro` | `0x79f758` | 221,992 | vtables/RTTI |
| `.got` | `0x7d5b68` | 7,292 | GOT |
| `.data` | `0x7d77e4` | 126,312 | globals |
| `.bss` | `0x7f6550` | 794,732 | zero-initialised |

Runtime reservation: `LOAD0` r-x `0x0–0x79e090` (7.99 MB), `LOAD1` rw- `0x79f090–0x8b85bc` (1.15 MB).

### Symbol and code census

From `tools/elf_census.py` and `tools/instruction_census.py`
(report: `analysis/reports/elf-census.md`, `instruction-census.md`):

| Metric | Value |
|---|---|
| Function symbols defined | 19,023 |
| ARM entry points | **19,023 (100 %)** |
| Thumb entry points | **0** |
| Functions with non-zero size | 19,022 |
| Mean / median function size | 189 B / 72 B |
| Largest function | 63,308 B |
| Data objects exported | 4,344 |
| Instructions decoded in function extents | 874,251 (upper bound; includes literal pools) |
| Mean / median instructions per function | 45 / 18 |
| Conditional instructions | ~99,600 (≈11 %); `AL` 774,622 |
| VFP instructions | 93,908 across 3,118 functions |
| VFP register file | `s0–s18` and **`d7–d20` observed** → D16–D31 exist → VFPv3-D32 |
| NEON | none: no `Tag_Advanced_SIMD_arch`, no NEON mnemonics in the census |
| Direct calls (`bl`) | 66,508 |
| Indirect calls (`blx reg`) | 973 |
| `bx` / `ldr pc` sites | 33,999 (mostly returns) |
| Atomics (`ldrex/strex`) | 0 |
| `svc` / coprocessor | 0 / 0 |
| Bitfield ops (`ubfx/sbfx/bfi/bfc`) | 831 |
| C++ exception call sites (`__cxa_*`) | 1,502 |

**Why this is favourable:** one ISA mode (ARM), no Thumb interworking, no atomics, no syscalls,
almost no indirect dispatch. VFP is single-precision dominated (`vmul.f32`, `vmla.f32`,
`vcmpe.f32`), consistent with the VFPv3-only attribute. The `andeq`/`eoreq` words seen in the
histogram are literal-pool data, not executed code — the emitter follows the CFG, not a linear sweep.

## 3. Game data

Path: `GloftF5HP/` — 659 files, 775.17 MB.

```
GloftF5HP/
├─ InsTime                         11 B   "1304060310/"   (install timestamp marker)
└─ res/
   ├─ .nomedia                     3 B
   ├─ oconf.bar                    78 B   text: "GGI: 25498\r\nXPPHPVerNo: 4\r\nGAME_PARAM_COUNT: 6\r\nGAME_PARAM_TYPE: 0|0|0|0|0|0\r\n"
   ├─ profile1.sav                 657 B  binary profile; contains "John Doe", float fields
   ├─ LOGO_Gameloft_480x800.mp4    1.5 MB intro movie
   └─ file000000.dat … file000652.dat   653 files, 775 MB total
```

Largest data files: `file000548.dat` 165.8 MB, `file000188.dat` 62.5 MB, `file000060.dat` 55.4 MB,
`file000093.dat` 52.2 MB, `file000181.dat` 46.1 MB.

### `.dat` files (decoded)

The 653 `.dat` files are **per-asset payloads**, renamed from their original asset names.
`file000000.dat` is the **obfuscation map** that restores the names:

| Property | Value |
|---|---|
| Size | 25,582 bytes |
| Key | `0x4F` |
| Trailer marker | `c|-|3k` (required, checked by the engine) |
| Entries | **651** (`fileNNNNNN.dat` ↔ original asset name) |

The map was decoded by executing the shipped decode loop from
`glitch::io::CFileSystem::addObfuscationFileMap` (ELF `0x6297dc`) under Unicorn, then reproduced
byte-for-byte by a static model in `tools/deobfuscate_map.py`. Verified artifact:
`analysis/reports/dat-map.tsv`, raw buffer `analysis/reports/dat-map.raw`.

Examples:

```
file000001.dat -> sfx_murcielago_offrumble.wav
file000013.dat -> trackTextures.zip
file000548.dat -> (largest payload, 165.8 MB)
```

Most payloads are `.wav` sound effects; there are also `.zip`-named bundles (e.g. track textures).
The payload encoding **inside** each `.dat` is still **U2** (open): `file000013.dat` starts
`51 4d 06 08 14 00 00 00 …` followed by internal paths such as
`Data/#temp/TrackCommonTga/lens_fla…`, which is a custom container header, not a raw ZIP.

Decoding stays deferred by design: the port runs the game's *own* `CUnZipReader`/`CPakReader`
code, so the true reader is whatever the shipped functions implement.

### Variant identity

`res/raw/gi_settings.xml` sets `<pvrt_textures>1</pvrt_textures>`.
`res/raw/data.txt` records the upstream expansion URL:

```
PVRT:http://dl.gameloft.com/hdplus/android/Fast5/Samsung_i9000/ml/1.0.5/Fast5_Samsung_i9000_Ver105_PVRT.zip
```

So these are the **PowerVR/PVRTC** assets, not the ETC1/ATITC variants. The binary ships
`PVRTCDecompress(const char*, int, int, int, unsigned char*)`, so the decoder itself is available to
the port.

## 4. Identity of the engine

Confirmed from exported symbol names and embedded strings:

| Layer | Identity | Evidence anchor |
|---|---|---|
| Engine | **Glitch** — a heavily modified **Irrlicht** fork | embedded `"Found unknown element in irrlicht scene file"`, `"__irrlicht_Diffuse_color"`; namespaces `glitch::scene`, `glitch::video`, `glitch::video::CImageLoaderPVR` |
| Scene/animation | `glitch::collada::*` (COLLADA scene + animation tracks) | exported `CAnimationTrackEx::getValue`, `CSceneNodeAnimator*` |
| Particles | **Spark** (`spark::CEmitter_Point`, …) | exported symbols |
| Audio | **Vox** (`vox::VoxEngineInternal`, `vox::PriorityBankManager`, decoders) | exported symbols |
| Support | Boost (intrusive_ptr), GNU libstdc++ (C++03 era, exceptions, RTTI) | exported symbols, `.ARM.exidx` |
| Math/physics | in-house (`PhysicCar`, `RaceCar`, `Camera`) | exported symbols |
| Game systems | `GS_*` (game states), `GP_*` (gameplay), `FastFiveMpManager`, `GLXPlayerMP*` | exported symbols |

Representative game symbols proving the gameplay layer is fully named:

```
PhysicCar::PhysicCar(int, eControlType, CARTYPE, unsigned char)
PhysicCar::CalculateCameraAccelDistanceFactor()
RaceCar::SetPVRBodyMatParams(boost::intrusive_ptr<glitch::scene::IMesh>, …)
NitroPickup::Update()
BlackMarketObject::~BlackMarketObject()
GS_MenuMain_Load::OnUnLoad3DScene()
GS_Debrief::ShowDebrief()
GP_RaceStartSimple::ExecuteGPUpdate(int)
Camera::IsInViewFrustrum(glitch::core::aabbox3d<float> const&)
RoadReflectionSceneNode::~RoadReflectionSceneNode()
CustomTraffic::SceneObjGetAttributeRespawnTime()
FastFiveMpManager::MP_ServerAddRabbitScoreMessages()
GLXPlayerMPLobby::mpSendLeaveTeam()
ALicenseCheck::Init(_JNIEnv*, _jclass*)
```

## 5. Known unknowns

| # | Unknown | Probe that settles it |
|---|---|---|
| U2 | Exact `.dat` container format(s) | decompile `glitch::io::CUnZipReader` / archive loader; trace open paths |
| U3 | Which GLES2 feature set is actually used | enumerate 308 imports + driver code paths |
| U4 | Exception usage in hot paths | 1,502 `__cxa_*` call sites counted; per-function distribution still open |
| U5 | Audio path (Java `GLAsyncPlayer` vs native Vox callbacks) | decompile `GLAsyncPlayer` + JNI bridge |
| U6 | DRM/licence gate behaviour | decompile `ALicenseCheck::Init`, `GloftDRM` |
| U7 | Whether a device-side reference run is available | check adb device availability (currently 0 attached) |

Resolved: **U1** — `tools/elf_census.py` proves 19,023 of 19,023 function symbols have even entry
addresses, i.e. the library is 100 % ARM mode with no Thumb interworking.
