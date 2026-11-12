# 06 — Evidence Ledger

Every factual claim in the docs resolves to a record here. Tool records live in the REA/Craft
session; exported bundles are stored in `analysis/evidence/` (local — not committed, because a
bundle contains material extracted from the shipped library).

> Evidence ids are **per server**: `ev_…` ids from Craft Anything are not valid in REA and vice
> versa. The ledger states which tool produced each row; when citing in a finding, only use ids from
> the same server.

## Full HD presentation (REA/Ghidra 12.1.4, 2026-11-11)

| Evidence id | Operation | Claim |
|---|---|---|
| `ev_54e64c0d…` | `procedure_pseudo_code` `appInit` | `SCR_HEIGHT = param_1`, `SCR_WIDTH = param_2`, swapped so the wider axis is the width |
| `ev_bbec1d13…` | `procedure_pseudo_code` `Application::Init` | scale factor 2.0 when width > 480, stored at `this+600`; logical size `this+0x250/0x254 = w/scale, h/scale`; `Game::NATIVE_SCREEN_WIDTH/HEIGHT` = the real screen size |
| `ev_7b3a8a3c…` | `procedure_pseudo_code` `CCommonGLDriverBase::set2DProjection` | the 2D ortho projection is built from the **current render target** size (`rt[0x1c]-rt[0x14]`, `rt[0x20]-rt[0x18]`), so 2D coordinates are render-target pixels |
| `ev_fc8c9565…` | `procedure_pseudo_code` `CCommonGLDriver::setViewportImpl` | `glViewport` is called with `fixUpScreenArea()`'s rect; the cached screen area is `this+0x21c..0x228` |
| `ev_541a2183…` | `procedure_pseudo_code` `CCommonGLDriverBase::fixUpScreenArea` | the viewport rect comes from the render target's screen rect; the device offset is `rt[0x30]+rt[0x10]` |
| `ev_c88e01f4…` | `procedure_pseudo_code` `CSprite::GetScaleFactor` | returns `Application+600` when `m_bPositionScale != 0`, else 1.0 |
| `ev_cd51e315…` | `procedure_pseudo_code` `CSprite::PaintModule` | the sprite position is multiplied by `GetScaleFactor()`; the drawn size is `module size * sprite scale(+0xa4)`, the high-res flag `+0xa8` removing the extra factor |
| `ev_2eccbce9…` | `procedure_pseudo_code` `CSprite::DrawRectangle` | the rect is multiplied by `Application+600` and drawn through `C2DDriver::draw2DRectangle` |
| `ev_3e79cb93…` | `procedure_pseudo_code` `CMenuManager::Render` | saves and clears `CSprite::m_bPositionScale` while the 2D menu layer is drawn |
| `ev_8fb7ddaa…` | `procedure_pseudo_code` `CMenuText::Render` | menu text is drawn with `CFont::DrawString` at the authored anchor |
| `ev_cae4ffa2…` | `procedure_pseudo_code` `CFont::DrawStringFlat` | the glyph advance and size come from the font sprite's module size and the font's own scale (`+0xa4`) |
| `ev_d02b7112…` | `procedure_pseudo_code` `CSpriteManager::GetFont` | the font is an ordinary sprite resolved by name |
| `ev_a9c3112a…` | `procedure_pseudo_code` `HudManager::InitButtons` | every HUD anchor is a ratio of `SCR_WIDTH`/`SCR_HEIGHT` (`-0.010416667`, `0.09375`, `-0.10416667`, `0.21875`, …) plus `this+0xd0/0xd4` offsets |
| `ev_be7c5d2b…` | `procedure_pseudo_code` `Camera::Camera_FixFov` | sets the FOV and takes the aspect ratio from the render target (`rt[0x0c]/rt[0x10]`) |
| `ev_68a1b22f…` | `procedure_pseudo_code` `CCameraSceneNode::setAspectRatio` | stores the aspect at `this+0x140` and recalculates the projection |
| `ev_f5a059f3…` | `procedure_pseudo_code` `buildProjectionMatrixPerspectiveFov` | the horizontal scale is `(1/tan(fovy/2))/aspect`; the aspect convention is width/height |
| `ev_76349035…` | `procedure_pseudo_code` `glitch::collada::CCameraSceneNode` | the COLLADA camera sets the FOV/aspect from the scene data, not from the screen |
| `ev_a30f082d…` | `procedure_pseudo_code` `IVideoDriver::fixUpProjectionMatrixOrientation` | the driver's orientation (`this+0x118`) rotates/flips the projection matrix: 1/3 transpose, 2/3 flip Y, 2/3 also flip X (180°) |
| `ev_864192d1…` | `procedure_pseudo_code` `IVideoDriver::setOrientation` | the only writer of the driver orientation besides `Game::OnOrientationChange` |
| `ev_eea59a4d…` | `procedure_pseudo_code` `Game::FromUIOrientToOrthoOrient` | maps UI orientation 2/3/4 through the table at `0x715210` |
| `ev_1e83a716…` | `read_bytes` `0x715210` | the table is `{2, 3, 1}` — UI orientation 2 means a 180° driver rotation |
| `ev_9a86485d…` | `procedure_pseudo_code` `Application::ReadOrientation` | turns the accelerometer z value into an orientation change (>30 → 1, <−30 → 2) |
| `ev_b373407a…` | `procedure_pseudo_code` `CCommonGLDriver::createRenderTarget` | a render target takes its size from an existing texture, not from width/height arguments |
| `ev_a763d682…` | `procedure_pseudo_code` `glitch::createDevice` | the device is created with the requested `dimension2d<int>` screen size |

Runtime verification (not MCP records):

* `build/v4_menu.png` — main menu at 1920×1080 with the engine running natively.
* `build/v4race_6600.png` — Rio race at 1920×1080 (native device, native render targets).
* `build/v4_native.png` — the 800×480 window is unchanged.
* `build/lb5_2400.png` — `FAST5_UI_FIT=letterbox`: uniform 2.25 scale, 60 px side strip.
* `build/v4long.log` — the screen viewport is 1920×1080 and the post-effect target 2304×1296
  (1.2x), i.e. native; the previous build used 960×576.
* `build/splash_120.png` — the Universal logo splash at 1920×1080.
* `build/v2_800_menu.png` — the 800×480 window is unchanged (identity transform).
* `build/v2_1920.log` — `resolution: design 800x480 -> frame 1920x1080 (fill) scale 2.4000/2.2500`,
  `gl: viewport 0 0 1920 1080`, `res: camera aspect 1.6667 -> 1.7778`.
* `tools/click_test.ps1` — a mouse click at window (1047, 695) arrives as design (436, 308).

Direct byte inspection (not MCP): the translated `fn_4d8228` (`build/link/chunk_033.c`) stores
`c->r1` into `this+0x140`, i.e. the soft-float ABI passes the aspect in a core register — the
host override has to write r1, not s0.

| `ev_c7138c8…` | `procedure_pseudo_code` `CMenuScreen::ScreenCoordsToProjectedScreen` | the 2D menu multiplies the touch by `Application[600]`; the 3D menu passes the position to `GetRayFromScreenCoordinates` |
| `ev_df0d560…` | `procedure_pseudo_code` (address probe) | REA address space for some functions in the JNI export region is unreliable; the ELF addresses are the ones that matter |


## Native texture path (REA/Ghidra 12.1.4, 2026-11-09)

| Evidence id | Operation | Claim |
|---|---|---|
| `ev_f34ba6f9…` | `procedure_pseudo_code` `DeviceConfig::SetupGameForDevice` | hardcodes `s_DeviceType = 0xB`, `s_GPUType = 0`, sets `s_bUsePVRBodyTexture = 1` |
| `ev_5f0aa64c…` | `procedure_pseudo_code` `MenuScene::LoadCars` | builds the 13-byte `SRaceCarTunning`; `local_33` (`+0x17d1`) = `m_bIsHighMemDevice == 0` |
| `ev_5c0455df…` | `procedure_pseudo_code` `TrackScene::LoadCars` | player `+0x17d1` = `m_bIsHighMemDevice == 0`; AI uses `s_bUsePVRBodyTexture` |
| `ev_7079f431…` | `procedure_pseudo_code` `RaceCar::RaceCar` | copies 13 tuning bytes to `this+0x17d0`; later calls `InitBodyTexture` |
| `ev_9689a96a…` | `procedure_pseudo_code` `RaceCar::InitCar` | swaps `s_bUsePVRBodyTexture` from `this[0x17d1]` around the car scene build |
| `ev_f944967d…` | `procedure_pseudo_code` `RaceCar::InitBodyTexture` | `+0x17d1 == 0` builds `_mask.NOPVR.tga`; else loads `_Mask.tga` and calls `SetPVRBodyMatParams` |
| `ev_9ea58cc0…` | `find_xrefs_to_name` `Game::s_bUsePVRBodyTexture` | writers/readers at `0x2dfcec/0x2dfcf0/0x2dfdf4` (`RaceCar::InitCar`), `0x38a190` (`CustomResFactory::getTexture`) |
| `ev_1fe0e1bc…` | `procedure_pseudo_code` `commitCurrentMaterialImpl` | commits only group 1 (direct params), then binds the program |
| `ev_a82c309d…` | `procedure_pseudo_code` `commitCurrentMaterialIndirectParameters` | commits group 2 (textures) and group 3 (globals) |
| `ev_ab4a4c66…` | `procedure_pseudo_code` `commitPassParameters` | calls `commitCurrentMaterialImpl` for pass != 0, then `commitCurrentMaterialIndirectParameters` |
| `ev_0a885402…` | `procedure_pseudo_code` `drawImpl` | calls `commitPassParameters(pass, vertex streams, attrib map)` before every draw |
| `ev_1c85e8a1…` | `procedure_pseudo_code` `CTexture::bindImpl` | generates `+0x40` GL name, binds it, calls `update()`; skips rebind when the per-unit cache says current |
| `ev_a45732d7…` | `procedure_pseudo_code` `CTexture::updateData` | `glTexImage2D`/`glTexSubImage2D` from the decoded image; no row flip |
| `ev_8f1dfa48…` | `procedure_pseudo_code` `CImageLoaderTGA::loadImage` | reads header/image type/bpp and pixel data; does **not** inspect the TGA descriptor or flip rows |
| `ev_de5c4e41…` | `procedure_pseudo_code` `CMaterialRenderer::getParameterID` | interns the name via `SData::get`, then compares `SData*` pointers against renderer defs |
| `ev_6770c48a…` | `procedure_pseudo_code` `SSharedStringHeapEntry::SData::get` | hash lookup in the global heap; insertion only when `param_2 != 0` |
| `ev_57f72847…` | `procedure_pseudo_code` `FUN_0060b1ec` | bucket walk with `strcmp`; returns the interned node or NULL |
| `ev_656c12c9…` | `procedure_pseudo_code` `PostEffects::PreDraw` | in `GS_Race`, returns when active effect == last-rendered; otherwise toggles RTT and binds it |
| `ev_38024d2f…` | `procedure_pseudo_code` `PostEffects::PostDraw` | same skip; composites the active RTT to screen |
| `ev_1ee4b084…` | `procedure_pseudo_code` `FUN_003a3978` | gate returns 1 only when the active state name is `GS_Race` |
| `ev_f58ab4e3…` | `procedure_pseudo_code` `PostEffects::GetPrevRT` | returns `this + this[4]*4 + 0x64` (RTT_1/RTT_2 texture) |
| `ev_598c79b3…` | `procedure_pseudo_code` `PostEffects::PostEffects` | creates RTT_1/RTT_2 at 1.2× screen, two render targets, current effect 0, last-rendered 0x12 |
| `ev_1c66d649…` | `procedure_pseudo_code` `PostEffects::ActivateEffect` | sets current effect and calls its `Init`; effect > 0x12 deactivates |
| `ev_f9416740…` | `procedure_pseudo_code` `PostEffects::ChangeStateEffect` | sets last-rendered = 0x12 |
| `ev_c397a3c4…` | `procedure_pseudo_code` `GP_RaceNormal::UpdatePostEffects` | only activates effect 9 when `s_GPUType == 1` (0 on this build) |
| `ev_81a24a3b…` | `procedure_pseudo_code` `RaceCar::UpdateShaderParams` | calls `PostEffects::GetPrevRT`, resolves `ReflSampler`, sets it per mesh |
| `ev_0534f8f3…` | `procedure_pseudo_code` `RaceCar::UpdateShaderParams(mesh, texture, factor)` | `getParameterID("ReflSampler")` then `setParameter<ITexture>` |
| `ev_a09f36f4…` | `procedure_pseudo_code` `RaceCar::SetPVRBodyMatParams` | sets `MaskSampler`/`Color1`/`Color2` only when the PVR branch runs |
| `ev_713febf7…` | `resolve_containing_procedure` `0x63aa54` | vtable slot 0x6c is `COpenGLES2Driver::getDriverType` |
| `ev_9ac1dea0…` | `procedure_pseudo_code` `COpenGLES2Driver::getDriverType` | returns 8 (not a boolean; engine only tests `!= 0`) |
| `ev_4e263edf…` | `xrefs` `"Can't find texture"` | logged in `CustomResFactory::getTexture` at `0x38a70c/0x38a724` |
| `ev_05086bb4…` | `procedure_pseudo_code` `CustomResFactory::getTexture` | tries original name, then suffixes from `__names__`, logs on failure |
| `ev_7535bb6c…` | `read_bytes` `0x7f0204` | four suffix pointers: `.TGA`, `.NOPVR.tga`, `.PVRTC4.tga`, `.PVRTC2.tga` |
| `ev_5d19f23d…` | `procedure_pseudo_code` `CResFactory::getTextureImpl` | `CTextureManager::getTexture(name)` then tries a memory-file variant |
| `ev_e7c70267…` | `procedure_pseudo_code` `Application::GetTextureHashname` | appends `m_TextureHashnameSuffix` when the pattern matches; optional uppercase |
| `ev_4cdf40ee…` / `ev_27c5099b…` | `search_procedures` | `CImageLoaderTGA::loadImage` at ELF `0x55157c`; `CImageLoaderPVR::loadImage` at ELF `0x550c28` |
| `ev_9ff088c7…` | `find_xrefs_to_name` `__names__` | suffix table used by `CustomResFactory::getTexture` |

Runtime verification (not an MCP record): `build/native_default.log` — 1501 frames, no guest fault,
no host exception, `postfx: frames=1501 preDraw=1501 postDraw=1500 getPrevRT=4507`; `build/clean_smoke.log`
— menu load to frame 1320, no signal/GL error/missing texture; `build/live_refl.log` — RTT_2 dumps at
frames 1500/1800/2100/2700/3300/3900 have six distinct SHA-256 values; `build/alias_safe.log` — menu
with `FAST5_TEXTURE_TRACE=1`, zero missing textures, every resolver rewrite targets an existing
Favelas `.LINEAR` asset, no guest fault and no GL error.

## Resolution work (historical; reverted to native 800x480 on 2026-11-10)

| Evidence id | Operation | Claim |
|---|---|---|
| `ev_871cd899…` | `procedure_pseudo_code` `appInit` | sets `SCR_HEIGHT=param_1`, `SCR_WIDTH=param_2`, swaps to landscape, calls `Application::CreateInstance(..., SCR_WIDTH, SCR_HEIGHT, ...)` |
| `ev_8b6f7762…` | `procedure_pseudo_code` `Application::CreateInstance` | forwards the requested screen size; halves flash files only when width < 0x200 |
| `ev_bbec1d13…` | `procedure_pseudo_code` `Application::Init` | scale 2.0 when width > 480; UI space `w/2,h/2`; stores `Game::NATIVE_SCREEN_WIDTH/HEIGHT` = width,height |
| `ev_ccd1e8fd…` / `ev_5215e4ab…` | `find_xrefs_to_name` `SCR_WIDTH` / `SCR_HEIGHT` | HUD, menu, debrief and IGM layout readers use the real screen size |
| `ev_cb395d87…` | `procedure_pseudo_code` `CSprite::PaintFrame` | frame loop; each module goes through `PaintFModule` |
| `ev_1c2591c4…` | `procedure_pseudo_code` `CSprite::GetFrameRect` | authored frame rect at `sprite+0x5c + frame*8`; optional divide by app scale |
| `ev_6f0f38bb…` / `ev_f8b6411a…` | `procedure_pseudo_code` `GetFrameWidth` / `GetFrameHeight` | dimensions derived from `GetFrameRect` |
| `ev_cd51e315…` | `procedure_pseudo_code` `CSprite::PaintModule` | builds the destination rect and calls `C2DDriver::draw2DRectangle` |
| `ev_2eccbce9…` | `procedure_pseudo_code` `CSprite::DrawRectangle` | solid fills pass the same rect as source and destination; scaled by `Application+600` |
| `ev_6d5c7c33…` | `procedure_pseudo_code` `Loading::DisplayFrame` | splash sprite is `PaintFrame(splash,0,0,...)`; loading spinner is a separate sprite |
| `ev_d0b7dfb8…` | `procedure_pseudo_code` `GS_MenuMain_Load::StateRender` | loading text positioned from `Application+0x250/0x254` |
| `ev_18b2b299…` / `ev_f529d110…` | `procedure_pseudo_code` `GS_Video::StateRender` / `GS_UniversalLogo::StateRender` | cinematic and Universal-logo 2D paths |
| `ev_3e79cb93…` | `procedure_pseudo_code` `CMenuManager::Render` | saves then sets `CSprite::m_bPositionScale = 0`, drawing the menu 2D layer in authored 800x480 pixels |
| `ev_6ddfe331…` | `find_xrefs_to_name` `CSprite::m_bPositionScale` | global at Ghidra `0x7f2254`; read by `GetScaleFactor`, written by `CMenuManager` |
| `ev_d46c075e…` / `ev_6b4b1aca…` | `procedure_pseudo_code` `CMenuManager::RenderCashFrame` / `RenderXPFrame` | cash/XP widgets draw their frames and text through the 2D driver directly |
| `ev_c196507a…` | `search_strings` `_highres` | the engine has a `_highres` suffix at Ghidra `0x720c70` |
| `ev_32bdc2e1…` | `procedure_pseudo_code` `CSpriteManager::GetSpriteFilename` | appends `_highres` when `Application+600 == 2.0`, probes the file and reports whether the high-res variant exists |
| `ev_7837af99…` | `procedure_pseudo_code` `CSpriteManager::LoadSprite` | stores the high-res selection at `sprite+0xa8`; low-res is the fallback |
| `ev_8ae2b8d9…` / `ev_d02b7112…` | `search_procedures` / `GetFont` | sprite and font loaders share the high-res path |
| `ev_13d9cd42…` | `procedure_pseudo_code` `CMenuSprite::Render` | menu 2D sprite `PaintAFrame` at its authored position |
| `ev_native1320…` | runtime capture after revert | All custom 2D/menu transforms removed; `game_frame_1320_native.png` shows the native authored 800x480 2D menu over the 1920x1080 3D scene. |
| `ev_8fb7ddaa…` | `procedure_pseudo_code` `CMenuText::Render` | menu 2D text uses `GetTextAnchorX/Y` and `CFont::DrawString` |
| `ev_00898e2b…` | `procedure_pseudo_code` `CMenuScreen2d::Render` | 2D menu screen renders its child container |

The Full HD/2D scaling entries below are historical. The shipped build is back to the native 800x480
window and the 2D menus are left in their authored layout; only the 3D scene and original HUD remain.

Direct verification (not MCP): `build/hd_highres.log` shows `platform: test window hidden
(FAST5_TEST_MINIMIZED)`, the historical 1920x1080 client/EGL, and `texmap` loads for
`Sprites/menu_font_medium_highres.NOMIPMAP.NOPVR.tga`, `menu_font_sm_highres`, `menu_font_big_highres`
and `menu_font_currency_highres`. A direct scan of `GloftF5HP/res/file000548.dat` (`Sprites.zip`) shows
the shipped high-res set: `2D_menu_elements_highres` 2048x1024, `2D_track_screens_highres` 2048x2048,
`interface_highres` 1024x512, `menu_font_medium_highres` 512x512, `menu_font_big_highres` 1024x1024,
`game_title_*_highres` 512x256 and the high-res storyboard/splash variants. `build/game_frame_240_highres2.png`
is the full-bleed high-res splash; `game_frame_30_universal2.png` / `game_frame_60_universal2.png`
the high-res Universal logo; `game_frame_1320_highres2.png` the crisp main menu;
`game_frame_1080_highres_story.png` Select Event; `game_frame_2100_highres_story.png` the cinematic;
`game_frame_3600_highres_race.png` the Rio race HUD. The run logs contain no guest fault or GL error.
The Universal logo is a 1024x512 static frame drawn at 0,0; it now uses a true 2x integer centered
mapping (2048x1024; `game_frame_30_2xdefault.png`), while the 800x480 splash keeps the full-bleed
1920 mapping (`game_frame_240_2xdefault.png`). `FAST5_UI_2X=1` also 2x-scales the 800x480 static
frames for A/B (`game_frame_240_2x.png` shows the resulting 1600x960 letterbox).
The host adapter skips extra 800-design scaling when `sprite+0xa8` marks the shipped high-res variant,
so the 2x high-res assets stay crisp; low-res-only 800x480 full-screen frames still map to the window.
Cash/XP get their scoped overlay scale, so native menu text is not moved.

## Effects and character-reference audit (REA/Ghidra 12.1.4, 2026-11-09)

| Evidence id | Operation | Claim |
|---|---|---|
| `ev_85730c06…` | `search_procedures` `Blur` | only `RadialBlur` / `RadialBlurMap` post-effect classes; no `MotionBlur` procedure |
| `ev_b421aad9…` | `search_strings` `blur` | `blur_map.tga` and radial-blur shader/effect names; no motion-blur asset |
| `ev_d2d3493b…` | `search_strings` `motion` | slow-motion, track and particle strings only; no motion-blur effect |
| `ev_e107f235…` | `procedure_pseudo_code` `RadialBlur::Init` | parameters at `+8` center_x, `+0xc` center_y, `+0x10` scale |
| `ev_4970ec58…` | `procedure_pseudo_code` `RadialBlurMap::Init` | texture slot 1 resolves `blur_map` |
| `ev_3bfeab4e…` / `ev_4d213710…` | `procedure_pseudo_code` `RadialBlur::Apply` / `RadialBlurMap::Apply` | the two post-effect draw implementations |
| `ev_d692a3d8…` | `xrefs` `PostEffects::ActivateEffect` | callers `GP_RaceCrash::ExecuteGPInit` `0x45cafc`, `GP_RaceCrashAI::ExecuteGPInit` `0x45e91c`, `GP_RaceNormal::UpdatePostEffects` `0x464bf8` |
| `ev_db6844c7…` | `procedure_pseudo_code` `GP_RaceCrash::ExecuteGPInit` | activates effect 14 (Desaturate), sets its six floats; not a blur |
| `ev_afd89e0a…` | `procedure_pseudo_code` `GP_RaceCrashAI::ExecuteGPInit` | activates effect 15 (DesaturateTakeDown); not a blur |
| `ev_003df835…` / `ev_ba0cb49c…` | `procedure_pseudo_code` `GP_RaceCrash::UpdatePostEffects` / `GP_RaceShowOff::UpdatePostEffects` | both are no-ops |
| `ev_c397a3c4…` | `procedure_pseudo_code` `GP_RaceNormal::UpdatePostEffects` | effect 9 (RadialBlurMap) only when `s_GPUType == 1`; 0 on this build |

Direct verification (not MCP): `objectsTextures.zip` (`file000247.dat`) holds
`Data/#temp/ObjectsTga/afro_female.tga` (first record, offset 0x34) and `girl_color.tga`;
`Objects/start_chick_rio.bdae` names `girl_color`, `afro_female`, `sam_dark`, `sam_difuse` and
`civilian_coste`, but no `.dat` contains a record for the last three, and the BDAE has no embedded
PVR/JPEG/PNG/DDS image data. `build/race_tex.log` shows `afro_female.tga`/`girl_color.tga` opening
successfully and the other three failing every shipped suffix. That fallback is the shipped
engine's own behavior, so it is kept 1:1.

## Confirmed observations

| Evidence id | Operation | Claim |
|---|---|---|
| `ev_380f446a…` | `core_target_triage` (APK) | APK is a valid Android package, 4,077,503 bytes, sha256 `fb8e26a1…e14a` |
| `ev_34df83eb…` | `inspect_android_package` | package `com.gameloft.android.GAND.GloftF5HP.ML`, v1.0.9/109, minSdk 5, 99 classes, 88 resources, 6 activities |
| `ev_8997597d…` | `android_classes_find` | 101 classes in `classes.dex`; boot/audio/installer/DRM class inventory |
| `ev_f281405b…` | `core_archive_show` (APK, off 0) | 88 entries; `lib/armeabi-v7a/libFast5.so`, `classes.dex`, `res/raw/igli.bin`, `raw_000.ogg`, `serialkey.txt` |
| `ev_82facdc2…` | `core_archive_show` (APK, off 60) | `resources.arsc`, layouts, `gi_settings.xml`, `data.txt`, `infoversion.txt` |
| `ev_8e3c4063…` | `core_target_triage` (`.so`) | ELF32 ARM, Android, shared library, JNI, stripped locals, entry `0x298b60` |
| `ev_e2059a63…` | `native_layout_show` | 19 sections, 23,685 symbols, 62,432 relocations, NX yes, PIE no, RELRO none, canary yes |
| `ev_b97de6d7…` | `native_library_calls_list` | 308 imports from 6 libs: `libGLESv2`, `libc`, `libm`, `libstdc++`, `libdl`, `liblog` (no OpenSL, no EGL) |
| `ev_ac6c6756…` | `native_offered_symbols_list` | 23,377 exports with full C++ names (`glitch::`, `spark::`, `vox::`, `PhysicCar`, `GS_*`) |
| `ev_ef012ea6…` | `core_overview_show` | 20,211 functions, 446,913 instructions, 72,758 blocks, 143,013 xrefs, 31,008 strings, 3,548 unresolved indirect jumps |
| `ev_d8ddc53b…` | `native_strings_find` "Irrlicht" | `"Found unknown element in irrlicht scene file"`, `"__irrlicht_Diffuse_color"` → engine is an Irrlicht fork |
| `ev_6659a078…` | `native_strings_find` ".dat" | `"file000000.dat"`, `"tracklog.dat"` |
| `ev_870f2b89…` | `native_strings_find` "PVR" | `PVRTCDecompress`, `glitch::video::CImageLoaderPVR`, PVR body-texture flags |
| `ev_6944264a…` | `native_strings_find` "Data/" | engine root `/data/data/com.gameloft.android.GAND.GloftF5HP.ML` |
| `ev_33d4ddcb…` | `native_functions_list` | first 30 functions are 12-byte PLT thunks at `0x297d4c…` |

### Audio (REA/Ghidra 12.1.4, 2026-11-08)

| Evidence id | Operation | Claim |
|---|---|---|
| `ev_9b9f77dd…` | `procedure_pseudo_code` | `DriverCallbackInterface::FillBuffer` = `Mutex::Lock(this+4)`; `if (this[8]) _FillBuffer(this,out,frames)`; `Unlock` → `driver+0x08` is the "started" gate |
| `ev_436dcbfe…` | `procedure_pseudo_code` | `DriverCallbackSourceInterface::FillBuffer` gates on `this+0x50 == 1` (playing), `this+0x18 == 0x10` (16-bit) and `this+0x10 ∈ {1,2}` (mono/stereo); resample step = `doppler * (pos * pitch >> 14) >> 14` |
| `ev_ec3dae39…` | `procedure_pseudo_code` | `DriverCallbackInterface::_FillBuffer` mixes the circular list at `driver+0x10` into a global int32 buffer (`frames*8` bytes), clamps, downconverts to int16 stereo |
| `ev_8b52dd8b…` | `procedure_pseudo_code` | `DriverAndroid::UpdateThreadedAT` creates the AudioTrack, then sets `driver+8 = 1` and loops `DoCallbackAT` — it does not tick the engine |
| `ev_f66b5372…` | `procedure_pseudo_code` | `VoxEngine::Initialize` requires `m_internal`, registers decoders, and creates two `VoxThread`s (`UpdateEmittersThreaded`, `UpdateSourcesThreaded`) |
| `ev_eee8c6d4…` | `procedure_pseudo_code` | `SoundManager::Play3DInternal` returns an empty handle unless `VoxEngine::IsValid(DataHandle)`; otherwise `CreateEmitterAsync` → `Play` |
| `ev_e7ea3606…` | `procedure_pseudo_code` | `SoundManager::Update` only sets the 3D listener position/velocity/orientation — it does not drive the engine |
| `ev_d00e6d61…` | `resolve_containing_procedure` `0x44a900` | caller of `SoundManager::Update` is `GS_Race::StateUpdate` (Ghidra `0x44a5d4`) |
| `ev_61762700…` | `resolve_containing_procedure` `0x44e598` | second caller is `GS_Tutorial::StateUpdate` |
| `ev_732eece6…` | `procedure_callers` `0x66e4b0` | `vox::VoxEngine::Update` has no callers — the engine is driven by its own `VoxThread`s |
| `ev_30eb6556…` | `search_procedures` `vox::Driver` | full vox driver/driver-source API with Ghidra addresses (`CreateDriverSource` `0x699b08`, `SetDriverSampleRate` `0x69ca80`, …) |
| `ev_6d39eab5…` | `search_procedures` `vox::VoxEngine::` | full vox engine API (`IsValid` `0x66e868`, `Play` `0x66eb74`, `UpdateThreaded` `0x66f5ec`, …) |
| `ev_b36258a7…` | `procedure_pseudo_code` | `SoundManager::Play2D` is a thin forward to `PlayRelative3D` |

### Save / player profile (REA/Ghidra 12.1.4, 2026-11-08)

| Evidence id | Operation | Claim |
|---|---|---|
| `ev_ec3e54df…` | `procedure_pseudo_code` `0x3be460` | `PlayerProfile::AwardCash` adds to `this+0x5c` (cash) and `this+0x118` (lifetime cash) |
| `ev_9418201f…` | `procedure_pseudo_code` `0x35c7a4` | `FormatMoney(int, char*, int)` = `sprintf(dst, "%s%s%s", sign, StringManager::GetString(0x400db), FormatNumber(v))` |
| `ev_aa5cf586…` | `search_procedures` `Profile::` | `PlayerProfile` API including `AwardCash`, `AwardFamePoints`, `UpdateUnlocks`, `MPProfile::Compute` |
| `ev_7fd5fccabe…` | `procedure_pseudo_code` `0x35d508` | `FormatNumber(int, char*, bool)` = `sprintf("%d")` then a loop appending digits with **`strncat`** and separators with `memcpy`; separator glyph and grouping depend on `*StringManager::s_pStringManagerInstance` |
| `ev_2135768e…` | `procedure_callers` `0x35c7a4` | 12 call sites of `FormatMoney` (ELF `0x34c7a4`), e.g. Ghidra `0x2e79c0`, `0x2e8224`, `0x3f6d1c`, `0x436720`, `0x454814` |

`profile1.sav` is a struct dump: `"John Doe"` at file offset `0x10`; the cash word at `0x5c` is 0 in
`GloftF5HP/res/profile1.sav` and 14132 in `build/userdata/profile1.sav` after a race.

**Resolved by import fix:** `strncat` was an unimplemented import (no-op), which dropped every digit
`FormatNumber` produced. Measured with a `FormatMoney` hook: `'$,'` → `'$14,132'` after implementing
`strncat` in `runtime/imports.c`.

**Open item (probe): `vsnprintf` / `vsprintf` are unimplemented.** Found by the `FAST5_IMPORT_TRACE=1`
audit, which also reports `basename`, `btowc`, `wctob`, `wctype`, `gethostbyname`, `sigaction`,
`uname`, `wcslen` as unimplemented and actually reached. Any code path that formats through a
`va_list` currently produces an empty string. Probe to settle it: implement `vsprintf`/`vsnprintf`
over the ARM EABI `va_list` (`__ap` points at the contiguous argument save area) and diff against
`guest_format`.

### M1 boot trace (REA/Ghidra 12.1.4)

| Evidence id | Operation | Claim |
|---|---|---|
| `ev_f1badfec…` | `binary_overview` | Ghidra image base `0x10000`; 20,929 procedures; 30,435 defined strings; segment map |
| `ev_65513fe0…` | `procedure_pseudo_code` JNI_OnLoad | `NVThreadInit(vm)`, `VoxSetJavaVM(vm)`, `GetEnv(0x10004)`, `ALicenseCheck_setJavaVM(vm)`, `mWaitForInit = 10`, returns `JNI_VERSION_1_4` |
| `ev_03f0c515…` | `procedure_pseudo_code` NVThreadInit | stores the JavaVM pointer in a global (`DAT_00806968` = ELF `0x7f6968`) |
| `ev_e7b4effa…` | `procedure_pseudo_code` appInit | landscape normalisation, builds `g_appPath`/`g_saveDirPath` byte-wise, calls `Application::CreateInstance(g_appPath,1,0,SCR_W,SCR_H,VersionGame)` |
| `ev_bb2de333…` | `procedure_pseudo_code` appRender | `mbAppRunning` gate, `mWaitForInit`/`m_timerForResume` countdowns, `Application::Update` + `ReadAcceleration` + `ReadOrientation` |
| `ev_0600f94d…` | `procedure_pseudo_code` Application::CreateInstance | low-res `aFlashFileData` halving when height < 512; seeds `g_nRandomSeed = g_nMPRandomSeed = 0` |
| `ev_9f2ea36e…` | `procedure_pseudo_code` Application::Init | exact subsystem order; scale 2.0 when width > 480; `setProcessBufferHeapSize(0x80000)`; profile/language; `XPlayerManager` |
| `ev_c1a89015…` | `procedure_pseudo_code` DeviceConfig::SetupGameForDevice | `s_DeviceType = 0xB` hardcoded, logs `PLATFORM_I9000`; LOD/clip tables at ELF `0x7058ac/0x705960/0x70599c` |
| `ev_2cc0505c…` | `procedure_pseudo_code` ALicenseCheck::Init | caches static members (`GetStaticMethodID`), `FindClass("android/content/Context")`, clears EKEY/EPID/SERVER, `LoadConfig()` |
| `ev_46b0773d…` | `procedure_callers` ALicenseCheck::Init | single caller at Ghidra `0x6cd094` |
| `ev_6ba32aa5…` | `resolve_containing_procedure` | caller is `ALicenseCheck_InitLicenseVM`, ELF `0x6bd094`, 36 bytes |
| `ev_1e10ea5b…` | `native_strings_find` "D:/HD_Prj" (Craft) | five original source paths: `app_Android.cpp`, `jni_GameRenderer.c`, `stb_vorbis.cpp`, `mpc_demux.c`, `mpc_decoder.c` |

### M1 data-pack and license trace (REA/Ghidra 12.1.4)

| Evidence id | Operation | Claim |
|---|---|---|
| `ev_ca5b12c7…` | `procedure_pseudo_code` Game::InitResourcePath | builds `<app>/res/`, mounts it, chdir, loads `file000000.dat`, mounts six subfolders, mounts eleven ZIPs |
| `ev_3b495db2…` | `read_bytes` 0x712c7c | the sixth mount name is `Audio` (others: `Glitch`, `Effects`, `SWF`, `strings`, `Scene`) |
| `ev_8523628d…` | `read_bytes` 0x7af8c8 | six mount-name pointers (table at ELF `0x79f8c8`) |
| `ev_e0193d91…` | `procedure_pseudo_code` addObfuscationFileMap | decodes the whole file with a rolling key, requires marker `c\|-\|3k`, parses `A:B` lines |
| `ev_9e893dd3…` | `procedure_assembly` addObfuscationFileMap | exact signed loop; `__aeabi_idivmod(q2*bit, param_2)` uses the original key as divisor |
| `ev_600240da…` / `ev_f225f286…` | `address_name` | PLT targets `__aeabi_idivmod` and `strcmp` |
| `ev_42912ab2…` / `ev_60c9b669…` / `ev_06d653f1…` | `procedure_pseudo_code` | `addPakFileArchive`, `addZipFileArchive(…,uint)`, `addFolderFileArchive` signatures |
| `ev_2c85be…` (Craft `ev_39256506…`) | vtable read | object offsets: `+0x18` zip, `+0x1c` folder, `+0x24` obfuscation map, `+0x30` chdir |
| `ev_3a363ec7…` | `procedure_pseudo_code` ALicenseCheck_ValidateLicense | builds `sprintf(SERVER, KEY, EPID, IMEI)`, optionally calls `d()` |
| `ev_090aa016…` | `procedure_callers` ALicenseCheck_InitLicense | called from `GLGame.nativeInit` (ELF `0x2996ec`) |
| `ev_4746503b…` | `procedure_pseudo_code` ALicenseCheck::LoadConfig | 256 KiB buffer from `db()`, then `ConfigFile::Decode` |
| `ev_7d5e70bb…` | `read_bytes` 0x7570e0 | cached methods: `d()V`, `db()[B`, `dc()[B`, `da()Activity` |
| `ev_2c85bcbc…` | `procedure_callers` ValidateLicense | three call sites: menu load, race load, MP lobby |
| `ev_58d149ee…` | `procedure_pseudo_code` Game::Init | shader defines, `DISABLE_ROAD_LIGHTING` for device 11, PostEffects, `Particles/effects.bin`, `LoadFonts` |
| `ev_ef733551…` | `procedure_pseudo_code` Application::Update | 1 ms/33 ms/80 ms timestep rules and `GameUpdate`/`GameRender` order |
| `ev_a93a5b96…` | `procedure_pseudo_code` CheckHighMemDevice | returns true unless `s_DeviceType == 13` |

Direct verification (not an MCP record): `tools/emulate_map.py` executed the shipped loop under
Unicorn on `file000000.dat` with key `0x4F` and produced the required `c|-|3k` trailer; the static
model in `tools/deobfuscate_map.py` then reproduced the entire 25,582-byte buffer identically.

Direct verification (not an MCP record): `recomp/elf_image.py` mapped both `PT_LOAD` segments at
their link addresses, parsed 23,688 `.dynsym` entries, applied all 62,432 relocations, and read all
433 `.init_array` entries from the real library; the loader exits `OK`.

Direct verification (not an MCP record): `recomp/ghidra_scripts/DumpPcode.java` produced
`analysis/reports/pcode.tsv` (35.7 MB) covering 14,185 functions ≤128 bytes, 151,473 instructions and
780,747 P-code ops. `recomp/diff_test.py` compared translated C against the shipped ARM instructions
under Unicorn on identical memory/register state. Latest sweep: **40/40 integer functions, 12/12
setters, 4/4 VFP functions — 0 register/memory mismatches**, with guest faults compared by address.
The harness caught and fixed three translation bugs (decimal constants, Ghidra→ELF address base,
`STORE` width). Known host difference: NaN sign/payload from x86 SSE vs ARM VFP.

Direct verification (not an MCP record): `readelf -A` reports `Tag_FP_arch: VFPv3` and no
`Tag_Advanced_SIMD_arch`; the P-code for small functions uses `d16–d20`, so the target is
**VFPv3-D32 with no NEON**. Ghidra userop names in the dump are `setISAMode`,
`VectorSignedToFloat`, `VectorUnsignedToFloat`.

Direct verification (not an MCP record): the full dump contains **20,627 functions** (223 MB).
`recomp/coverage.py` emitted **20,325/20,325** non-empty functions (100 %, 0 failures) and
`recomp/emit_all.py` compiled all of them in 102 chunks: **102/102 clean, 0 errors, 0 warnings**
(`gcc -O1 -Wall -Wextra`, 8 jobs). Report: `analysis/reports/emit-all.md`.

Direct verification (not an MCP record): `recomp/link_all.py` generated a 20,325-entry function
table and linked **`build/fast5_recomp_test`** with no undefined symbols (104 translation units,
`-lm`). Executing translated `nativeGetTotalSounds` (ELF `0x29b014`) returned `r0 = 635` and
`pc = 0xdeadbee0` (the harness return address), proving the full translate → compile → link → run
path.

Direct verification (not an MCP record): `recomp/gen_imports.py` maps **300/300** PLT entries to
import names from `.rel.plt`. With `runtime/imports.c` (softfp helpers, libc subset, guest heap,
pthread/math/log stubs), the linked binary executes **all 433 `.init_array` constructors** without a
fault and still returns 635 from the test function. A PLT stub merged into a function body was
detected by fault 0x70373c (a string address used as a function pointer) and fixed by treating PLT
ranges as imports.

Direct verification (not an MCP record): `build/fast5_boot_test` (`recomp/link_all.py ... --boot`)
runs the 433 constructors, logs `PLATFORM_I9000` and `m_bIsHighMemDevice TRUE`, loads `profile1.sav`,
`appInit` returns `0x09390CD8`, `appResume` runs, and `appRender` executes 40 frames (~200 ms). The
log reaches `Android:: resuming game` and `glFlush`, then
`Texture not found Sprites/universal_logo.tga !!!`; `StateRender` (`0x4460F0`) faults at `0x446194`
dereferencing a NULL object. This is the current boot frontier.

Data verification: scanning every archive (`analysis/notes/scan_archives.py`) finds only
`Sprites/universal_logo_highres.bsprite` and `.tga`, both inside `file000548.dat` (a `QM` container
whose embedded ZIP starts at file offset 38). No plain `universal_logo` exists in the whole tree.
`GetSpriteFilename` (`0x3CDA84`) appends `_highres` when `[obj+592] != 512`; the runtime value is
`0x190` (400) and the computed pointer is `0x710C70` (`_highres`, printed at runtime), so the plain
request comes from a different path than this function. REA xrefs (Ghidra 12.1.4): `_highres`
(`0x720C70`) → `0x3DDB54`; `Texture not found` (`0x720CA8`) → `0x3DE328`.

Direct verification (not an MCP record): the PVR header parser read the file at offset 8 because
`strncmp` was unimplemented and returned 0. `FUN_00560814` (`0x560814`) branches on
`strncmp(buffer, "BTEXpvr", 8)`; with the false match it read 0x34 bytes from the current position
(after the 8-byte probe), so the header buffer began at PVR offset 8 and was rejected. Implementing
`strncmp` in `runtime/imports.c` fixed it; the `Sprites/universal_logo_highres.tga` entry then loads
(archive indices 275 `.bsprite`, 276 `.tga`; entry offset `0x09d7175f`, size `0x000aab34`).

Direct verification (not an MCP record): after `strncmp`, the PVRTC path called the untranslated
callback `0x553208` (`FUN_00563208`, real `cmp/cmple/push {r4,r5,r6,r7,r8,lr}`). Adding it via
`tools/add_missing_functions.sh` and rebuilding yields a clean **200-frame** run: touch press at
frame 60 / release at 70 are accepted, the Universal Studios legal text renders, and the menu scene
`#menu_favelas04.max` + `menu_loopableSection1.bdae` loads. The next fault is
`CMaterial::getTechnique` (`0x5177fc`, Ghidra `0x5277fc`) reading `0xe590a17c` from a garbage
material pointer during `CSceneManager::compileInternal` → `sortBatches`. REA pseudocode for
`getTechnique` shows the full `CMaterial` pointer chain that must be valid.



Direct byte inspection (PowerShell, this session):
- `oconf.bar`: `GGI: 25498 / XPPHPVerNo: 4 / GAME_PARAM_COUNT: 6 / GAME_PARAM_TYPE: 0|0|0|0|0|0`
- `InsTime`: `1304060310/`
- `profile1.sav`: binary, `John Doe`, float fields (`0.7`, `1.0`, …)
- `file000013.dat`: header `51 4d 06 08 …` + ASCII path `Data/#temp/TrackCommonTga/lens_fla…`
- `file000000.dat`: high-entropy, no readable structure
- `raw_000.ogg`: valid Ogg Vorbis (`OggS`, `vorbis`)
- `gi_settings.xml`: `pvrt_textures=1`
- `data.txt`: expansion URL ending `Fast5_Samsung_i9000_Ver105_PVRT.zip`

`readelf` (WSL, direct):

- ELF32 ARM, `ET_DYN`, entry `0x298b60`, flags `0x5000002`
- `Tag_CPU_name "7-A"`, VFPv3, `Tag_ABI_HardFP_use: Deprecated`, **no `Tag_ABI_VFP_args`** → softfp
- Sections: `.text` 4,626,296; `.ARM.extab` 170,728; `.ARM.exidx` 161,072; `.init_array` 1,732;
  `.data.rel.ro` 221,992; `.got` 7,292; `.bss` 794,732
- Compiler: GCC 4.4.3

`tools/elf_census.py` + `tools/instruction_census.py` (WSL python3 + Capstone 4.0.2):

- 19,023 function symbols; **19,023 ARM / 0 Thumb**; 19,022 with non-zero size; 4,344 data objects
- sizes: 3,602,084 B total, mean 189 B, median 72 B, largest 63,308 B
- 874,251 instructions decoded within function extents (upper bound — includes literal pools)
- condition codes: AL 774,622; EQ 50,800; NE 28,332; all others ≈ 20,400
- VFP: 93,908 instructions in 3,118 functions; `vmul.f32` 4,404, `vmla.f32` 3,915,
  `vcmpe.f32` 2,913, `vcvt.f32.s32` 1,822
- calls: 66,508 direct `bl`, 973 indirect `blx reg`, 33,999 `bx`/`ldr pc` (mostly returns)
- 0 `ldrex/strex`, 0 `svc`, 0 coprocessor ops, 831 bitfield ops
- 1,502 `__cxa_*` call sites (417 `__cxa_begin_catch`, 311 `__cxa_rethrow`)
- most-called internal symbol: `sub_6f5d88` (4,567), then `operator delete` (2,609),
  `glitch::IReferenceCounted::drop` (2,172)

## Native Windows window / real GL (this session)

Direct verification (not an MCP record): `tools/win32_angle_test.c` was compiled locally with MinGW
GCC 16.2.0 and run. It loaded the vendored x64 ANGLE pair from `third_party/angle/`, created a real
Win32 window and EGL surface, and reported:

    EGL 1.5
    GL_VERSION  OpenGL ES 3.0 (ANGLE 2.1.28027 git hash: d8fcfe6b0c0d)
    GL_VENDOR   Google Inc. (AMD)
    GL_RENDERER ANGLE (AMD, AMD Radeon RX 9060 XT (0x00007590) Direct3D11 vs_5_0 ps_5_0, D3D11-...)

It captured a real 800×480 frame to `build/angle_test.bmp` (solid blue clear), proving this is a
rendering window, not an offscreen stub.

Direct verification (not an MCP record): `python tools/build_win.py` compiled 109 local units (101
generated chunks, `table.c`, and 7 runtime files) with MinGW and linked `build/fast5_win.exe`.
`runtime/platform_win32.c` creates the window/EGL context before constructors and `appInit`, so the
game's GL driver initialization sees the real driver. `runtime/gl_win32.c` dispatches all 91 GL
imports; guest memory is passed directly to ANGLE after translating guest offsets to host pointers.

Direct verification (not an MCP record): the shipped shader templates contain a stray `]` after
preprocessor directives. `build/shader_80_65.txt` shows `#endif]`. ANGLE's compiler rejected it as
`ERROR: 0:16: ']' : unexpected token after conditional expression`; a 25 s run logged 54 shader
compile failures. `combine_and_clean_shader()` in `runtime/gl_win32.c` strips `]` only on lines whose
first non-space character is `#`, concatenates the source parts, and uploads one string. The next run
logged **0 shader compile failures and 0 program link failures**.

Direct verification (not an MCP record): the missing virtual method at ELF `0x552e8c` was recovered
with the existing Ghidra script:

    tools/add_missing_functions.sh 0x553b70 0x552e68 0x6c8660 0x6f65f8 0x6f725c 0x6f728c 0x553208 0x552e8c

Output: `OK 0x552e8c -> FUN_00562e8c size=272`; `analysis/reports/pcode_extra.tsv` now holds **8**
recovered functions. `recomp/link_all.py` was extended to merge that file automatically and to
support `--emit-only` (write chunks/table/main without a Linux compile). `build/link/table.c` now has
`case 0x552e8c`, and `build/link/chunk_100.c` contains `fn_552e8c`.

Native run evidence (not an MCP record): before the recovery, `build/fast5_win.exe` logged
`unmapped target 0x00552e8c ... lr=0x00552bd4`, `guest fault at 0x00552e8c (write)`, after rendering
the Universal Studios legal text and calling `glDrawElements`. After the recovery, the same run no
longer faults there. It enters the game's statically linked **libjpeg**:
`watchdog pc=0x005dd388 lr=0x005dd3b0 ...` inside `fn_5dd314`, which calls
`FUN_005ed250`/`FUN_005ed0cc` (the libjpeg memory manager: `jpeg_get_large`, `jpeg_get_small`, the
`0x3b9ac9f0` = 1,000,000,000-byte chunk limit). The run continues advancing through texture levels
(`TEXTURE LEVEL = 11`, then `9`) without a fault; this is slow interpreter execution, not a missing
translation. The native harness watchdog and host exception filter are in `runtime/main_win32.c`.

REA pseudocode evidence for the hot path (Ghidra 12.1.4, ephemeral immutable-source import):

- `FUN_005ed314` (`ev_fb75dc3dd191eefe566709f119527dfca1365fd8c3e04ebbe257aa6d0dfc2c20`):
  resource array loader; divides `0x3b9ac9f0` by the element size, allocates `count*4`, and calls the
  two helpers below.
- `FUN_005ed250` (`ev_2df643aa9f5be1b1a757d39ac805ce066810709b40e7342020e18745840637c8`):
  `jpeg_get_large`-backed chunk allocator/copy helper.
- `FUN_005ed0cc` (`ev_4b290242a3dde227cba3a996c6acd18df297f147e6f7da54ebd4c91851d392be`):
  `jpeg_get_small`/`jpeg_get_large` pool allocator.
- `FUN_00562af0` (`ev_df0ba362db1d5024fa65b3e5dd09a4ef105728d541b1b12ab97467b4a82829c8`):
  `ITextureDataLoading` entry that drives `pixel_format::convert` and the recovered virtual method.

### Native render follow-up

- **Compatibility identity:** `FAST5_FAKE_GPU=1` makes `glGetString` report `Imagination
  Technologies` / `PowerVR SGX 540` / `OpenGL ES 2.0` while rendering through real ANGLE. In this
  mode the game skips the CPU JPEG path and reaches `CSceneManager::renderLists` / `finalize=33`;
  with real ANGLE strings it decodes JPEGs on the main thread (the libjpeg path documented above).
- **Attributes:** the game never calls `glGetAttribLocation` or `glBindAttribLocation`. `gl_win32.c`
  now scans each attached vertex shader for `attribute` declarations and calls
  `glBindAttribLocation(program, 0..n, name)` before `glLinkProgram`. Observed bindings include
  `Vertex`, `TexCoord0`, `Color0` (and a duplicate `TexCoord0` in one generated shader).
- **Textures are real:** `glTexImage2D` for the first 256×256 RGB texture produced
  `build/tex_0_256x256.bmp`, a colourful gradient/circle image. Asset reads and uploads are not the
  cause of the black frame.
- **Frames are black:** `build/game_frame_30.bmp`, `60.bmp`, `120.bmp` are all black. A `prog=30`
  menu draw has 12/996 indices but its client vertex array (stride 36, pointer `0x8031f30`) reads
  `0.000 0.000 0.000` at draw time.
- **External window capture:** `tools/capture_window.ps1` captured the actual 816×519 window to
  `build/window_capture.png`; it is black and the title bar says “Not Responding” during the long
  frame. This confirms the black is not a `glReadPixels`/FBO readback artifact.
- **Threads:** `pthread_create` remains a no-op. The only observed start routine is the persistent
  timer at `0x68d694` (it calls `usleep`); a synchronous-thread experiment was removed.
- **New fault:** after `menu_staticSection1.bdae` and `DefaultEffects.bdae`, the fast path logs
  `guest fault at 0x5f383065 (write)`, registers `r3=0x5f38304d`, `lr=0x530c34`, `pc=0x5313d4`.
  REA pseudocode at Ghidra `0x5413d4` is
  `IMaterialTechniqueMapsReader::processMapGroup` (`ev_37c8e473b28cdb1b92a2327c581e9017356b261a891a49a339cb65df7f207c64`);
  `load` is `ev_1a4088936df1f177383a6408248757460b07967587419ae104bdbf2fcf98c412`;
  `setError` is `ev_b846b7cbdd2034b05c73bdc25bd6b8f87a599276617062e676070262d5c965f2`.
  The bad pointer behaves like a string/branch-table address, not a heap object.

### Dispatch performance and material fault

- **Dispatch:** `build/link/table.c` previously contained a 20,000-case switch; MinGW emitted a very
  large comparison tree inside `call_target`. `recomp/link_all.py` now emits a sorted
  `TargetEntry { uint32_t addr; void (*fn)(Cpu*,uint8_t*); }` table and `call_target` performs a
  binary search; `table.c` is compiled with `-O2`. Measured locally: **frames 30/60/120 complete in
  ~15 s**, versus ~30 s for only 30/60 before.
- **Incremental build:** `python tools/build_win.py` compiles only stale objects and relinks only if
  needed. No-change build: “all 110 objects are up to date”. Touching `runtime/gl_win32.c` alone:
  “compiling 1 of 110 … linked” in **1.9 s**. `--force` performs a full rebuild.
  `recomp/link_all.py --emit-only` preserves unchanged chunk mtimes; a no-change emit reports
  `0 file(s) changed`.
- **JPEG loader override negative result:** a host override at
  `CImageLoaderJPG::loadImage` (`0x54fd10`) was never invoked during the real or fake runs. REA
  resolves that symbol at Ghidra `0x55fd10` (`ev_3063f93abc9b87190d6d4d83dd2c227ed9bb1912170a644547f9bfefdb8d7cbd`);
  its only `jpeg_read_header`/`jpeg_start_decompress` xrefs are `0x55fea8`/`0x55fec8`, inside that
  function. `CTexture::updateData` (`0x640d1c`, `ev_a45732d7ed272065cdc5d2bb9feb5e88438b2b40182709365acba0e7aa28f517`)
  only logs `TEXTURE LEVEL` and uploads already-decoded data.
- **Material fault isolation:** at `CMaterialRenderer` constructor entry the arguments are correctly
  ordered: `r0=this`, `r1=driver`, `r2=material index`, `r3=material name`, `stack[0]=technique list`.
  Example log:
  `cmr-ctor r0=08504848 r1=0802f328 r2=00000000 r3=0839dbc8 stack=0839dbe0 00000001 00000002 0839dd70 text=Null`.
  The fault is deeper in the technique/pass data: a pointer field points at
  `0x703bb8`, the start of the ASCII string `"sh_us.bsprite"`; reading that string as a pointer gives
  `0x755f6873` (`"sh_u"`) and the next dereference faults at `0x755f688b` (`+0x18`).
  REA: `CMaterialRenderer::allocate`
  (`ev_12f613e8bbd85288370c59d419f827a83cc421e44818c0fae3b039483ca6da0c`),
  `CMaterialRenderer` constructor
  (`ev_561e524ea8bef63fab2760a434cb39722092456f3071b188f7ecc12cdd853ebd`),
  `CMaterialRendererManager::endMaterialRenderer`
  (`ev_391549c0a223d7ccd9106355fb9d63c290235c8115bed944f490c8a07a0f0933`),
  `createMaterialRendererForProfile`
  (`ev_cc1b9eee5d781aecb8e9873e5c3596e4ed8cecf9dd640a3e615a92fdec0d0e0a`).
- **Material 43 boundary:** the last successful `CMaterialRenderer` constructor is material 43
  (`ProfileCOMMON_Material__117849376...menu_staticSection1`). Its allocate entry has 7 technique
  nodes, one pass each, and all `f7` shader pointers and all 22 parameter pointers are valid guest
  heap addresses. The fault is inside the constructor while following a shader/pass field at
  `+0x18`; the value is still a texture-string pointer (`sh_us.bsprite`). This narrows the defect to
  the `IShader`/render-pass field layout written by `addRenderPass`/`createShader`, not the list
  heads or parameter array.
- **Material parameter-ID workaround (observed):** material 43’s 63-entry ID buffer is valid except
  for high-bit entries `e66c`/`e888`; their `CGlobalMaterialParameterManager` slots are empty or
  contain string data. `CGlobalMaterialParameterManager::grabInternal`
  (`ev_848d10e4daaf4cb5b6e2ca2c790c9229f28bbd96ba31097c2c981d92f19c3a50`) indexes
  `this+0x18 + id*0x14 + 0x10` and dereferences the result at `+0x18`. The current host guard maps
  only invalid high-bit IDs to 0 and refuses invalid `grabInternal` slots; materials 43–50 now
  construct. This is a workaround, not a root registration fix.
- **Guest `sprintf` root fix (runtime observation):** `runtime/imports.c` had no `sprintf` handler;
  the import fell through to the unknown-import path. In `CMainMenuObjects::ReloadDummies`
  (REA `ev_7ff6548c7ac7115f459732462715f6841ddee59b97f3a3de773c1a4799216ffb`), the formatted
  lookup name was therefore empty. After adding guest-ABI `sprintf`/`snprintf`, the runtime lookup
  log is `text=menu_screen_dummy_slot0_00_node` and the loader exits its loop.
- **JNI capability path (observed):** `nativeCanAndroidPhonesUsingMenu3D` (REA
  `ev_bae6f32aa4106e4f2684ef596728f4dbbc125edb8ce83b857a71e830701d1cbf`) calls
  `NVThreadGetCurrentJNIEnv` and then a `JNIEnv` vtable entry at `+0x204`. The uninitialized env
  resolves to guest address `0x10000` (ELF header), so the vtable read sees `0x464c457f` and faults
  at `0x464c45cb`. It is currently answered directly on the host as 3D-capable.
- **Heap exhaustion (runtime observation):** at the 256 MiB arena, `gmalloc` was asked for
  `0x400000` bytes with the bump pointer at `0x0fcba460`; the bounds check called
  `guest_fault(0x0fcba460)` (write). The arena is now 512 MiB and runtime pointer bounds use
  `guest_mem_size`. A 512 MiB run reached `frame 6600` with no guest fault.
- **Menu state before the ctype fix (historical):** the 512 MiB fake-GPU run loaded
  `#menu_favelas04.max`, `All_merge_low_.max` and menu sounds, reached `frame 6600` at about 32 FPS,
  but had only **2 GL draw calls** and no menu draw calls. `game_frame_240/480/720.bmp` were black.
  `pthread_create` remains a no-op; the only observed create is timer routine `0x68d694`. This was
  the render frontier before item 41; the fixed build now renders the title and main menu.
- **Bionic ctype table was the real attribute blocker (runtime + REA):** the engine's
  `guessShaderVertexAttribute` (`ev_53747f86309383f8aa895692e5bb5151b9a738e4894ea53e43be1fec00cab08d`)
  lowercases each name through `_tolower_tab_` and reads a **16-bit** entry at
  `_tolower_tab_ + c*2 + 2`. `runtime/guest_data.c` had written only 8-bit entries, so the names
  became garbage. The runtime override log before the fix was
  `guess attr name=Vertex -> 255`, `TexCoord0 -> 255`, `Color0 -> 255`; after writing
  `unsigned short[257]` ASCII tables the log is `Vertex -> 0`, `TexCoord0 -> 1`, `Color0 -> 18`.
  `CGLSLShader::linkProgram`
  (`ev_b4bef6319e947126097335a340c4db861d7348e67cae9d313ff4268c7f6cee34`) only calls
  `glGetAttribLocation` when that result is below `0x1e`, so the bad table left every location at 0.
  `CProgrammableGLDriver::setupArrays`
  (`ev_5b8bd6c16e1a15bc75763f8b9f46bd8d279f2d257625407fb30630ae3a21e160`) then pointed every stream
  at location 0. After the fix program 30 reports three active attributes and the log shows distinct
  pointers at locations 0/1/2: `0x8031f20` / `0x8031f2c` / `0x8031f34`, with first position
  `394.000 480.000 0.000`.
- **Table layout:** `_tolower_tab_` is 257 × 16-bit starting at `0x9A1300`; `_toupper_tab_` was moved
  to `0x9A1600` so it cannot overlap. ASCII entries are filled; signed-range entries are zero.
  Filling negative entries with signed values caused a new fault in `TiXmlNode::Identify` at
  `0x6cb110` (`r3=0xff801300`); zero-filling them booted correctly. The exact TiXml consumer of the
  negative entry was not isolated, so the zero-fill is documented as an observed workaround.
- **Attribute names are deduplicated:** shader source contains both branches of `#if` attribute
  declarations; the old binding consumed locations for both `TexCoord0` occurrences and bound
  `TexCoord0` twice, then `Color0` at 3. The bridge now binds only the first occurrence in
  declaration order: `Vertex=0`, `TexCoord0=1`, `Color0=2`.
- **Visible frames (runtime observation):** `game_frame_30.bmp` is the Universal logo,
  `game_frame_120.bmp` is the Fast Five title with real textures, and after a posted Space key the
  `build/clean_menu.png` capture shows Quick Race, Career 0%, Multiplayer, Options, More games!, and
  Gameloft LIVE. `build/clean_title.png` is the title. The clean run reached `frame 2400` with no
  guest fault at about 30–35 FPS on ANGLE/D3D11.
- **Texture variant and derived overlay:** the menu requests `.PVRTC2.tga` names but the Favelas
  archive carries `Track/Favelas/*.LINEAR.tga`; `file000609.dat` carries scene `mm_*` names.
  `MenuScene::LoadTexture` hashes the exact requested name.
  `runtime/overrides.c` rewrites resolved names at `CFileSystem` entry points Elf `0x619238`,
  `0x619170` and `0x61a260`; the exact name is tried first. `tools/extract_archive_tgas.py`
  extracts the shipped records into `build/overlay/Track/Favelas/` as raw `.tga` names and
  complete-header `.pvr` aliases. The VFS overlay serves them after the immutable tree; the
  original `GloftF5HP/` data is never written. `tools/extract_car_textures.py` likewise extracts
  148 `carTextures.zip` TGA/PVR basenames.
- **PVR header path (runtime observation):** `FUN_00560814` parses the `.LINEAR` payload as a
  0x34-byte PVR header with `PVR!` at offset 44 and format byte `0x19`;
  `CImageLoaderPVR::loadImage` (table `0x550c28`) explicitly supports format 0x19. The full-header
  `.pvr` aliases resolve all 14 Favelas names with zero `Can't find texture` logs. The car body is
  now textured via the separate body-atlas bind/row-order fix below; the visible environment still
  needs its own binding check. A direct `PVRTCDecompress` probe (`0x5cd114`, offsets 0/44/96/148,
  bpp 2/4) produced transparent output and was removed.
- **Car texture bind trace (runtime observation):** binding names at the GL boundary identify
  texture 8 as `Sprites/menu_font_sm_highres.NOMIPMAP.NOPVR.tga`, texture 81 as
  `Highline_H3_1972_Body.NOPVR.tga`, texture 90 as `Comet_Rice_Rocket_2011_Body.NOPVR.tga`,
  texture 95 as `Cranky_M3_2011_Body.NOPVR.tga`, and texture 82 as
  `Car_Body_Reflection_Fixed.PVRTC4.tga`. The 3D car body was being drawn with the font atlas on
  FBO 4/5 (`prog=30`), which produced the black/white body. The bridge now tracks the Highline body
  (texture 81) and falls back to the last `*_Body*.tga` GL texture, substituting it for texture 8
  on 3D FBO binds. The menu car renders red with the Highline body in `build/final_body.png`.
- **Body row order (runtime implementation):** the car body TGA files use image descriptor `0x08`
  (bottom-left origin). The engine upload path left the body vertically inverted even though the
  wheels/interior were acceptable. `runtime/gl_win32.c` now flips only tracked `*_Body*.tga` rows
  before upload; `build/final_body.png` shows the aligned car body and correct wheels.
- **Visible menu (runtime observation):** with `FAST5_AUTO_MENU=1`, the main menu renders with the
  textured body and wheels and no guest fault. A later `FAST5_AUTO_RACE=1` tap at frame 600 did not
  leave the menu in `build/auto_race3.png`; that is an input/timing issue, not a texture failure.
- **3D upload trace:** a `FAST5_GL_TRACE=1` menu load produced uncompressed `glTexImage2D` uploads
  for UI and car textures, including 1024×1024 body atlases. The visible car body is now correctly
  textured after the body-bind and row-order fixes; the Favelas environment still needs a separate
  binding check. No `glCompressedTexImage2D` call was observed.
- **Vox worker threads:** the startup `pthread_create` calls pass ELF entry `0x68d694`, resolved by
  REA as `vox::funcUpdate` → `vox::VoxThread::Update` → `_Update`; `VoxThread::Stop` calls
  `pthread_join`, and `AccessController` calls `pthread_mutex_init/lock/unlock` (REA evidence
  `ev_4a1514a7…`, `ev_b9d1ebfb…`, `ev_94d788c8…`, `ev_df972ab0…`, `ev_c98a9102…`,
  `ev_c4200c5a…`). With `FAST5_THREAD_TRACE=1`, two guest threads start at ids 2/3, entries
  `0x0068d694`, stacks `0x00a00000`/`0x00b00000`; no guest fault or GL error occurs.
- **Mouse input:** `WM_LBUTTONDOWN`/`WM_LBUTTONUP` are queued with client coordinates normalized to
  the 800×480 surface and dispatched through the existing touch press/release imports. A posted
  left-button event reached the menu and the log shows `mouse down/up ... -> touch` entries.
- **GL error cleanup (runtime observation):** `glGetProgramInfoLog` was called on objects 112 and
  115. The bridge's create-program log shows programs `…108, 111, 114, 117…`; 112/115 were never
  created as programs, so the GL call returned `GL_INVALID_OPERATION` (`0x502`). Tracking created
  programs and returning an empty log for non-program objects removed the error; the guarded run
  still reaches the title/menu (`build/final_boot_menu.png`).
- **PVRTC extension removal:** the bridge previously appended `GL_IMG_texture_compression_pvrtc` to
  `GL_EXTENSIONS` while uploading a placeholder. Removing it did **not** change the requested
  `mm_*PVRTC2.tga` names in either fake-PowerVR or real-ANGLE runs, confirming the suffix is selected
  by the game's scene/config path, not the advertised extension.
- **Quiet boot:** first-use import traces and the watchdog thread are now opt-in
  (`FAST5_IMPORT_TRACE=1`, `FAST5_WATCHDOG=1`); `build/quiet_boot.err` contains only the game logs
  and the confirmed missing-asset/effect lines. The game's own missing-asset messages were not
  suppressed.
- **Capture caution:** `tools/capture_window.ps1` uses `CopyFromScreen`; when the game window is not
  foreground it captures the terminal behind it. One 240 s capture showed Command Code chat text,
  not the game. Use `PrintWindow`/topmost capture before treating a screenshot as game evidence.
- **D3D11 is active.** The external stats overlay during a live run reported `D3D11 32 FPS`; ANGLE is
  using the real GPU. A black game surface is therefore not evidence that the GPU path is absent.
- **Shader/pass investigation:** `IShader::IShader` initializes the fields at `+0x18`–`+0x24` to zero
  (`ev_6dddcaa7bbf8d8c5391c074d2b7b0940fef465bd0cf0d165c7678b1c91f61144`). At constructor entry
  material 43’s shaders show `f18=00040000`, `f1a=4`, `f20=0`, `f22=0`, so the parameter length is
  4; forcing shader flags was unnecessary and was removed.
- **JPEG diagnostic entries:** `CImageLoaderJPG::loadImage` is Ghidra `0x55fd10`
  (`ev_bc6a13a1455cf4c47b06a9ea3c5ddc7546f650e5e37694793f06a5484ced0615`), with pseudo-code
  `ev_3063f93abc9b87190d6d4d83dd2c227ed9bb1912170a644547f9bfefdb8d7cbd`; the row allocator
  `FUN_005ed314` is `ev_fb75dc3dd191eefe566709f119527dfca1365fd8c3e04ebbe257aa6d0dfc2c20`;
  `FUN_005ed5d4` (memory query) is
  `ev_9ee2de05ad5d7a43a7b8dd38209dcab802798f09f07820e6126c3a1d5265eda7`. The current
  log-only overrides for `0x54fd10`/`0x5dd314` did **not** fire in the long menu run; the 0x5dd
  watchdogs are stale guest PC samples, not proof of JPEG activity.
- **Rejected menu experiments this session:** returning 0 from
  `nativeCanAndroidPhonesUsingMenu3D` (2D menus) reached `InitIntro` and faulted on an uninitialized
  storyboard pointer `0x10000` (`0x464c45cb`). Skipping `ReloadDummies`, or forcing dummy scene-node
  lookups to NULL, produced the same later `0x10000` object fault. Skipping `InitIntro` never fired.
  The `jpeg_mem_available` return-max hook never fired. The actual blocker at that point was the
  missing `sprintf`; the 512 MiB arena removed the later heap-exhaustion fault.
- **False leads, reverted:** forcing the technique list to an empty self-loop moved the fault to
  `0xa8787715`. Changing the `stmdb` writeback from `sp=mult_addr+4` to `sp=mult_addr` made the
  constructors abort in `_Unwind_Resume_or_Rethrow`; the emitter change was reverted. The observed
  argument order is correct, so the remaining defect is in the shader/pass field mapping, not a
  general stack writeback shift.
- **Guest allocator:** live blocks carry `GBLOCK_MAGIC`; freed blocks carry `GFREE_MAGIC`.
  `free`/`realloc` validate magic and size before reuse, so a double free cannot insert the same
  block twice and create a cyclic guest free list. This did not by itself change the menu/JPEG
  behaviour. (Real and fake paths produce deterministic, different bad string pointers.)

## Wheel rotation / math imports (2026-11-07)

| Evidence id | Operation | Claim |
|---|---|---|
| `ev_5261c8a0…` | `procedure_pseudo_code` MenuScene::Update (ELF 0x2ca50c) | calls `UpdateCars(this, delta)` unconditionally; scene `+0x54/+0x58` are the rolling-road offsets |
| `ev_8a0070da…` | `procedure_pseudo_code` MenuScene::UpdateCars (0x2caff8) | loops the three car pointers at scene `+0x60..0x68`; calls `BaseCar::UpdateCar` and `RaceCar::UpdateMeshes` per non-null car |
| `ev_23b7c284…` | `procedure_pseudo_code` MenuScene::UpdateCarsWheelSpeed (0x2cb430) | writes `20.0f` to `car+0x40/+0x44` when `scene+0x5c != 0` |
| `ev_bd89b5e7…` | `procedure_pseudo_code` BaseCar::UpdateCar (0x483acc) | `car+0x48 += car+0x44*0.001*delta`, `car+0x4c += car+0x40*0.001*delta`, gated on `Game::s_pInstance+0x1b3 == 0` |
| `ev_4dcd0fdc…` | `procedure_pseudo_code` RaceCar::UpdateFakeWheelRotation (0x2d1c88) | dead helper (`+delta*0.015707964`); not on the menu path |
| `ev_0ff08edf…` | `procedure_pseudo_code` RaceCar::UpdateMeshes (0x2d2810) | feeds `car+0x48/+0x4c` to `quaternion::fromAngleAxis` and applies the resulting quaternions to wheel nodes at `car+0x1140..0x115c` |
| `ev_70222118…` | `procedure_pseudo_code` RaceCar::UpdateWheelsPosition (0x2d2254) | repositions wheel nodes from the wheel records at `car+0x9a8` |
| `ev_96c47986…` | `procedure_pseudo_code` quaternion::fromAngleAxis (0x2b8080) | output `(sin(angle/2)*axis, cos(angle/2))` |
| `ev_302288e2…` / `ev_5db0c95e…` | `procedure_pseudo_code` sinf / cosf | static wrappers: `__aeabi_f2d` → double `sin`/`cos` → `__aeabi_d2f` |
| `ev_2da2f8d6…` | `find_xrefs_to_name` Game::s_pInstance | global at Ghidra `0x80980c` (ELF `0x7f980c`) |

Direct verification (runtime observation, `build/wheel10.err`, `build/clean_math1.err`):

- Before the fix, the wrapper trace showed `f2d(wrapper): in=3c23d70b f=0` (the float 0.01 arrived
  as 0), `sin` received the raw double 0, and `fromAngleAxis` returned `(0,0,0,0)`. All eight wheel
  nodes were set to a zero quaternion every frame.
- After the fix, `fromAngleAxis` returns `(-0.0100,0,0,0.9999)` for angle 0.02 and
  `(-0.7243,0,0,0.6895)` for angle 1.62. The same wheel node `0x13190d40` was sampled 280 times
  with changing quaternions (`wheelSetRot sample=...`).
- `BaseCar::UpdateCar` ran every frame for all three cars (rates 20.0 on cars 1/2; `Game+0x1b3=0`;
  LOD 3). The wheel angle fields advanced `0 → 0.02 → 1.62 → 3.22 → …` and wrap at ±2π.
- Clean default run: quiet log (no trace env), menu at frame 240, captures at 480/720, no guest
  fault, no GL error (`build/clean_math1.err`).

## First playable race / cinematic XML (2026-11-07)

| Evidence id | Operation | Claim |
|---|---|---|
| `ev_07d5687d…` | `procedure_pseudo_code` CCinematicThread::update (0x4a7e18) | searches all 30 `ScriptManager` objects for `thread+0x24`; when nothing matches it calls `CCinematicEvent::start` with `+0x28/+0x2c` still `-1` |
| `ev_fa2ff2f5…` | `procedure_pseudo_code` CCinematicEvent::start (0x4d439c) | iterates its command vector and calls `CCinematicCommand::gogo(cmd, objIdx, subIdx, eventIdx)` |
| `ev_2e45859c…` | `procedure_pseudo_code` CCinematicCommand::gogo (0x4d0b70) | dispatches by command name; `Object_SetEnabled` receives `param_1` as the object index |
| `ev_2f366742…` | `procedure_pseudo_code` Object_SetEnabled (0x4d11ac) | `ScriptManager::GetObj(mgr, idx)` then `(*(obj+0x80))(obj, enabled, attrs->get("Enabled"))` with no NULL check |
| `ev_fc974dfa…` | `procedure_pseudo_code` ScriptManager::GetObj (0x332494) | unsigned `idx < 0x1e` bounds check, else 0 — so `-1` returns NULL correctly |
| `ev_643b891d…` | `procedure_pseudo_code` CCinematicObject::SceneObjItemInit (0x4a5d08) | parses each `*_cinematic.cff`; stores XML `object` into `CCinematicThread+0x24` and `type` into `+0x18` |
| `ev_a69fa80b…` | `procedure_pseudo_code` CCinematicThread constructor (0x4a7ae4) | initializes `+0x28 = +0x2c = -1` |
| `ev_26b83aa2…` | `procedure_pseudo_code` ScriptManager::ScriptUpdate (0x332038) | dispatches per-script slots; slot 0x18 receives the state delta and active player |

Direct byte inspection (not an MCP record): `GloftF5HP/res/file000060.dat` is a `QM` container whose
embedded ZIP holds `Track/Rio_night/cinematics/*_cinematic.cff`. The XML uses integer attributes,
e.g. `<cinematicThread type="0" name="Object Thread" object="21">` and `object="161"/"128"/"129"…`.
None of the 30 thread targets in the failing run was 0, matching the dump in `build/race13.err`.

Direct verification (runtime observation):
- Before the fix: `cineThread ... target=0x00000000 objIdx=-1`, `getObj entry idx=-1`,
  `getObj -> 0x00000000`, then `guest fault at 0x464c45ff (read)` — the NULL object's `+0x80`
  vtable read landing on the ELF header.
- After implementing `atoi`/`strtol`/`strtod`/`strtok`: the same trace shows
  `target=0x147 → objIdx=24`, `0x145 → 19`, `0x185 → 11`, and the commands run on valid objects.
- Full race: `Loading track rio.track`, Rio scene graphs (`#rio_city_sunset_refl_323...`,
  `#situatii_speciale_01`, `#purple_arrow01`), 38 loading draw-call steps, HUD and minimap, no
  guest fault through frame 3960 (`build/race14.err`); clean default run reaches frame 2880+
  (`build/race_clean1.err`). Captures `build/game_frame_2100.bmp` (countdown `GO`),
  `2400`/`2700`/`3600` (driving).

## Back-to-menu crashes and guest memory (2026-11-07)

| Evidence id | Operation | Claim |
|---|---|---|
| `ev_229b55bb…` | `address_name` ELF 0x29a0e4 | the faulting frame in the race→menu backtrace is `nativeGC` |
| `ev_30d99880…` | `procedure_pseudo_code` nativeGC | `NVThreadGetCurrentJNIEnv()` then a JNIEnv vtable call at `+0x204` — the JNI env is uninitialized (U11) |
| `ev_d0630363…` / `ev_8d8f0c91…` | `address_name` | ELF `0x29be34` is `AndroidOS_IsFinishLoadBackground`, not the frame that faulted |
| `ev_7b45acd4…` | `procedure_pseudo_code` `CAndroidOSDevice::postKeyEvent` | returns its argument unchanged — the key path is a stub; keys reach the engine only via the `nativeSetOnKey*` exports |
| `ev_51c94bce…` | `procedure_pseudo_code` notifyKeyPressed | handles only 4/BACK, 0x52/MENU, 0x50 — confirms there is no keyboard driving |
| `ev_0738fef1…` | `procedure_pseudo_code` nativeAccelerometer | stores args into `mAccelerometerX/Y/Z`; the driving path is tilt + touch |

**Guest heap measurement (runtime, `guest_heap_stats`, logged every 120 frames).**
Before the allocator rewrite, over one race → menu → race session:

    frame     bump     free  largest   blocks     live
      120    142.1      9.5      4.0    23223      4.6
     1560    310.1    168.6      4.7    64522     13.5
     2520    443.4    274.6      4.7   295323     40.8
     3480    744.7    573.0      4.7   625704     43.7
     4920    912.7    765.9      4.7   733236     18.9

Live memory never exceeded 43.8 MiB, yet 765.9 MiB sat in 733k free fragments whose largest member
was 4.7 MiB — so the 5,592,408-byte allocation reported by `guest heap exhausted` could not be
satisfied. This is fragmentation, not a leak.

After replacing the allocator with a boundary-tagged, coalescing one (same 512 MiB arena), four
full race → pause → menu → race cycles gave: max bump **192.2 MiB**, max free blocks **16,067**
(steady ~4,000), largest free block **29.8 MiB**, peak live 40.9 MiB, **0 guest faults, 0 host
exceptions**. Artifacts: `build/fast5.log`.

Two implementation defects were found and fixed during bring-up: the free-list links are two
pointers (16 bytes on x64) so a free-capable block needs a 16-byte minimum payload (8 corrupted the
next block's header), and the bin index must clamp to `NBINS`.

## Input mapping (2026-11-07)

| Evidence id | Operation | Claim |
|---|---|---|
| `ev_51c94bce…` | `procedure_pseudo_code` notifyKeyPressed (Ghidra 0x2ad338) | handles only key codes 4 (BACK), 0x52 (MENU) and 0x50; every other code is ignored — there is no keyboard driving in this build |
| `ev_a7a3434d…` | `procedure_pseudo_code` nativeSetOnKeyUp (Ghidra 0x2a9cf4) | JNI export: `notifyKeyReleased(param_3)` |
| `ev_0738fef1…` | `procedure_pseudo_code` nativeAccelerometer (Ghidra 0x2a9e28) | stores arg3/arg4/arg5 into `mAccelerometerX/Y/Z` |
| `ev_f6dc076e…` / `ev_9baf99b9…` | `procedure_pseudo_code` nativeTouchPressed / nativeTouchReleased | forward to `notifyTouchPress`/`notifyTouchReleased` with x, y, pointer id |
| `ev_d0cff3a7…` | `procedure_pseudo_code` Game::OnAcceleration | forwards to the accelerometer object's vtable `+8` — the same object the engine polls on device |
| `ev_1861b345…` | `procedure_pseudo_code` Application::ReadAcceleration | calls `Game::OnAcceleration` each frame |

Direct verification: `runtime/platform_win32.c` maps Win32 keys to Android key codes and the harness
dispatches them through `fn_299d28`/`fn_299cf4`; tilt is fed through `fn_299e28` from the arrow keys
(`x` = steer, `y` = pitch, `z = 9.81`). Mouse→touch is unchanged.

## Loading stalls (2026-11-07)

Measured with `FAST5_PROFILE=1`: an import profiler (per-PLT call counts and self time) plus a
`Cpu::pc` sampler, both dumped on any frame slower than 250 ms.

| Evidence id | Operation | Claim |
|---|---|---|
| import profile, one menu load | measured | `usleep` was 37.5 s of 41.1 s total import time across 4,375 calls (≈8.6 ms each) |
| sleep duration histogram | measured | **115,614 of 120,217** `usleep` calls requested <1 ms; another 4,595 requested 17–33 ms+. A `<1 ms` request costs a full 1 ms `Sleep` |
| import profile after fix | measured | total import time fell to 0.7 s; `usleep` no longer dominates |
| `nm` + host-RIP sampling of the f199 frame | measured | ~92 % of the single 14–19 s frame is `fn_5cdb7c`, reached from `CColladaDatabase::constructScene` (`menu_favelas04.max`) |
| `FAST5_THREAD_TRACE=1` | measured | only guest threads are two `vox::funcUpdate` (`0x68d694`) audio threads — no worker performs the scene build |
| `glTexImage2D` dump guard | measured | BMP writes (`build/tex_*.bmp`, `build/alpha_*.bmp`) ran unconditionally on first upload of matching textures; now require `FAST5_TEX_DUMP=1` |

Frame counts over a fixed 75 s auto-menu run: 600 (before) → 1200 (sub-ms sleeps fixed) →
1560 (import fast paths, `-O2` chunks, override bitmap). Total stall 66.6 s → 21.2 s.

## Unknowns

| # | Question | Probe | Status |
|---|---|---|---|
| U1 | ARM vs Thumb split | symbol LSB census | **resolved: 100 % ARM, 0 Thumb** |
| U2 | `.dat` payload encoding | traced map + VFS; payload header still open | **partial**: names decoded; inner container open |
| U3 | GLES2 feature usage | classify 308 imports; inspect driver init | open |
| U4 | Exception use in hot paths | count `__cxa_throw` sites; exidx rows | partial: 1,502 `__cxa_*` sites, distribution open |
| U5 | Audio path | decompile `GLAsyncPlayer` + `Java_*` exports | open; Vox uses stb_vorbis + Musepack (source paths observed) |
| U6 | DRM gate behaviour | decompile `ValidateLicense` network path + `ConfigFile` consumers | partial: init/config/validate call sites located |
| U7 | Reference device availability | adb devices (0 attached) | open |
| U8 | `GLMediaPlayer` exists in `.so` but not in `classes.dex` | search all DEX / registration path | open |
| U9 | Why is the menu surface black after a fault-free frame loop? | trace `GL` draws/state, state machine, timer worker | **resolved**: 8-bit ctype table made `guessShaderVertexAttribute` fail; all attributes used location 0. Fixed with 16-bit tables + deduplicated bindings. |
| U10 | Does menu load need the `0x68d694` timer thread or audio worker? | implement pthread/timer safely and compare state | **implemented**: `0x68d694` is `vox::funcUpdate`; two Vox threads start and the menu stays stable; audio/timing still needs a longer test |
| U11 | General JNIEnv initialization | `NVThreadInit`/fake JavaVM + used JNI vtable methods | open; one capability query bypassed |
| U12 | Guest PVRTC decode vs placeholder upload | trace `glCompressedTexImage2D` formats; guest decoder vs ARM decoder | open; no compressed upload observed in the menu trace |
| U13 | Why are `mm_forest_street.PVRTC2.tga` / `mm_rock.PVRTC2.tga` missing? | trace `CImageLoaderPVR`/`CFileSystem` name resolution + archive directory | **partial**: all 14 names resolve to full-header `.pvr` aliases (`0x619238`/`0x619170`/`0x61a260`) with zero not-found logs; visible environment binding still under trace |
| U14 | Why does the menu car/garage show black/white bands? | capture per-material texture/attribute state; compare with official frame | **mostly resolved**: the car body was binding the font atlas (`Sprites/menu_font_sm...`, GL 8) on FBO 4/5; the bridge substitutes the tracked `*_Body*.tga` texture and flips body TGA rows (`descriptor 0x08`). Body and wheels are textured in `build/final_body.png`; compare livery against a reference |
| U15 | Deterministic title-to-menu input | add scripted second touch; compare screenshots | **resolved**: `FAST5_AUTO_MENU=1` enters the menu and `build/auto_menu.png` captures it; `FAST5_AUTO_RACE` still needs coordinate/timing work |
| U16 | Why did `glGetProgramInfoLog` see object 112/115? | track GL program creation and call sites | **explained**: IDs are not created programs; bridge now ignores non-program info-log requests |

## Contradictions / cautions

- Craft reports 0 jump tables but 3,548 unresolved indirect jumps. This is a *coverage* statement,
  not evidence that no jump tables exist. The Capstone census finds only 973 indirect `blx` sites;
  the two numbers measure different things (Craft counts unresolved flows, not call sites).
- Symbol reconciliation (now closed): the ELF `.dynsym` section is **23,688** entries
  (`0x5c880 / 16`) = 19,023 functions + 4,344 objects + 308 named undefined + 1 null + 10 other
  defined + 2 unnamed undefined. Craft's `native_layout_show` reports 23,685 symbols / 23,367
  defined exports — its own classification, not a loader discrepancy. `recomp/elf_image.py` reads
  the authoritative count and reproduces exactly 62,432 relocations and 433 constructors.
- Instruction counts differ by scope: Craft's CFG walk = 446,913; the Capstone linear sweep inside
  symbol extents = 874,251 because it also decodes literal pools. **Translation scope is the CFG
  set**; the linear number is only an upper bound for budgeting.
- 1,107 functions show "NEON-like" mnemonics in the census while `Tag_Advanced_SIMD_arch` is absent.
  This is a prefix-matching artefact in the census script, not evidence of NEON. Confirm with a
  targeted disassembly before trusting either way.
- **PVR upload route caution:** the menu trace produced no `glCompressedTexImage2D`/PVRTC upload
  log. The `.LINEAR` files are full-header `.pvr` assets that must be routed through
  `CImageLoaderPVR`, not through the TGA loader or a generic compressed-texture upload. Verify the
  actual `CImageLoaderPVR::loadImage` result before changing the GL decoder.
- **Derived archives must preserve the shipped container:** the `.dat` files carry a custom prefix
  and a central directory whose first record is a directory entry with local offset 0; the actual
  local ZIP records begin after the prefix. A standard ZIP replacement is not accepted by the
  engine's `CUnZipReader`; do not put a generic ZIP in `build/overlay` for these archive names.
  The active overlay is a folder tree of extracted original bytes (raw `.tga` plus full-header
  `.pvr` aliases), not a replacement archive.
