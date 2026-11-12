# ELF census — `libFast5.so`

- SHA-256 reference: see `docs/01-TARGET.md`
- ELF type `3`, machine `40`, entry `0x298b60`, flags `0x5000002`

## Sections

| Section | Address | Size | Role |
|---|---|---|---|
| `.hash` | `0xd4` | 160,404 | symbol hash |
| `.dynsym` | `0x27368` | 379,008 | dynamic symbols |
| `.dynstr` | `0x83be8` | 1,679,952 | dynamic strings |
| `.rel.dyn` | `0x21de38` | 497,056 | relocations |
| `.rel.plt` | `0x2973d8` | 2,400 | PLT relocations |
| `.plt` | `0x297d38` | 3,620 | PLT stubs |
| `.text` | `0x298b60` | 4,626,296 | code |
| `.rodata` | `0x7022d8` | 306,592 | read-only data |
| `.ARM.extab` | `0x74d078` | 170,728 | exception unwind tables |
| `.ARM.exidx` | `0x776b60` | 161,072 | exception index |
| `.init_array` | `0x79f090` | 1,732 | global constructors |
| `.data.rel.ro` | `0x79f758` | 221,992 | vtables / RTTI |
| `.dynamic` | `0x7d5a80` | 232 |  |
| `.got` | `0x7d5b68` | 7,292 | global offset table |
| `.data` | `0x7d77e4` | 126,312 | initialised globals |
| `.bss` | `0x7f6550` | 794,732 | zero-initialised globals |
| `.comment` | `0x0` | 11,502 |  |
| `.ARM.attributes` | `0x0` | 47 |  |
| `.shstrtab` | `0x0` | 162 |  |

## Dynamic symbols

| Kind | Count |
|---|---|
| Functions (defined) | 19,023 |
| — ARM entry points (even) | 19,023 |
| — Thumb entry points (odd) | 0 |
| — with non-zero size | 19,022 |
| — zero-size (aliases/thunks) | 1 |
| Objects (defined) | 4,344 |
| Undefined symbols | 309 |

## Function sizes (non-zero)

- total 3,602,084 bytes, mean 189 B, median 72 B
- largest 63,308 B

| Size bucket (bytes) | Functions | Bytes |
|---|---|---|
| <=16 | 4,233 | 39,000 |
| 17-64 | 4,851 | 184,824 |
| 65-256 | 6,605 | 909,244 |
| 257-1024 | 2,772 | 1,280,120 |
| 1025-4096 | 529 | 879,924 |
| >4096 | 32 | 308,972 |

## Namespace groups (top 25)

| Namespace | Functions |
|---|---|
| `(mangled)` | 4,013 |
| `(c symbols)` | 762 |
| `glitch::collada::animation_track` | 560 |
| `std::std::vector` | 503 |
| `std` | 398 |
| `glitch::video::detail` | 246 |
| `glitch::io::CAttributes` | 222 |
| `std::std::_Rb_tree` | 161 |
| `glitch::core::detail` | 123 |
| `std::std::__copy_move_backward` | 115 |
| `glitch::video::CCommonGLDriver` | 109 |
| `glitch::video::CNullDriver` | 101 |
| `__gnu_cxx::new_allocator` | 93 |
| `std::std::__copy_move` | 78 |
| `std::std::basic_streambuf` | 78 |
| `std::std` | 70 |
| `glitch::scene::CSceneManager` | 68 |
| `glitch::io::IAttribute` | 65 |
| `glitch::io::CNumbersAttribute` | 64 |
| `glitch::video::IVideoDriver` | 60 |
| `std::std::basic_istream` | 57 |
| `Singleton` | 55 |
| `glitch::scene::ISceneNode` | 54 |
| `glitch::collada::detail` | 52 |
| `std::std::basic_filebuf` | 52 |

## Runtime structures

| Structure | Count / size |
|---|---|
| `.ARM.exidx` rows | 20,134 |
| `.ARM.extab` size | 170,728 B |
| `.init_array` constructors | 433 |

## Largest functions

| Address | Size | Symbol |
|---|---|---|
| `0x3d0a70` | 63,308 | `_ZN16CMainMenuObjects10LoadScreenEi` |
| `0x553bf0` | 24,872 | `_ZN6glitch5video24guessShaderParameterTypeEPKc` |
| `0x302b20` | 20,504 | `_ZN10PhysObject11SceneObjAddEP11TrackObject` |
| `0x53db50` | 16,652 | `_ZN6glitch5video12pixel_format7convertENS0_14E_PIXEL_FORMATEPKvjS2_Pvjjjb` |
| `0x4b6418` | 11,468 | `_ZN11CIGMObjects10LoadScreenEi` |
| `0x2acef4` | 11,352 | `_ZN11GS_MenuMain11StateUpdateEi` |
| `0x2bb490` | 11,216 | `_ZN6Camera20Camera_UpdateRaceCamEi` |
| `0x43ea98` | 10,956 | `_ZN11GS_Tutorial18StateMachineUpdateEi` |
| `0x2e8bb4` | 10,700 | `_ZN10TrackScene4LoadERi` |
| `0x2ef0d8` | 7,816 | `_ZN10TrackScene15UpdateCarSoundsEi` |
| `0x5bf6bc` | 7,400 | `_ZN6glitch7collada18ISceneNodeAnimator9forceBindEv` |
| `0x3f1a8c` | 7,100 | `_ZN10HudManager11InitButtonsEv` |
| `0x42a20c` | 6,780 | `_ZN9GS_Garage11StateUpdateEi` |
| `0x5cef78` | 6,560 | `inflate` |
| `0x38fabc` | 6,348 | `_ZN11PostEffectsC1EPN6glitch5scene13CSceneManagerE` |
| `0x38e1f0` | 6,348 | `_ZN11PostEffectsC2EPN6glitch5scene13CSceneManagerE` |
| `0x3f543c` | 6,100 | `_ZN10HudManager6RenderEv` |
| `0x62bd3c` | 5,928 | `_ZN6glitch5video21CProgrammableGLDriverINS0_18CGLSLShaderHandlerEE40commitCurrentMaterialAutomaticParametersEPKNS0_11CGLSLShaderEPKNS0_14CVertexStreamsEPKh` |
| `0x6846ec` | 5,748 | `_ZN3vox15VoxSoundPackXML7LoadXMLEPKc` |
| `0x55d590` | 5,644 | `_ZN6glitch5video12CBatchDriver15thisAppendBatchERKN5boost13intrusive_ptrIKNS0_14CVertexStreamsEEERKNS0_16CPrimitiveStreamERKNS3_IKNS_5scene11CMeshBufferEEE` |

## Undefined symbols by prefix

| Prefix | Count |
|---|---|
| `__a` | 23 |
| `pthread` | 18 |
| `__s` | 3 |
| `inet` | 2 |
| `` | 1 |
| `iscntrl` | 1 |
| `glCompressedTexSubImage2D` | 1 |
| `glDrawArrays` | 1 |
| `_toupper_tab_` | 1 |
| `wcscoll` | 1 |
| `__e` | 1 |
| `glSampleCoverage` | 1 |
| `glBindBuffer` | 1 |
| `connect` | 1 |
| `towupper` | 1 |
