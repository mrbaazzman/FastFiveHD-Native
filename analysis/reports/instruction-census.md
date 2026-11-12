# Instruction census — `libFast5.so`

- Functions analysed: **19,022**
- Instructions decoded: **874,251**
- Mean instructions/function: **45**, median **18**, max **15,827**
- Entry point: `0x298b60`

## Condition codes

| CC | Instructions |
|---|---|
| AL | 774,622 |
| EQ | 50,800 |
| NE | 28,332 |
| LE | 3,068 |
| HI | 2,786 |
| LT | 2,483 |
| GT | 2,472 |
| LO | 2,468 |
| LS | 2,393 |
| HS | 1,641 |
| MI | 1,382 |
| GE | 1,358 |
| PL | 413 |
| VC | 30 |
| VS | 3 |

## Top 50 mnemonics

| Mnemonic | Count |
|---|---|
| `mov` | 155,981 |
| `ldr` | 136,663 |
| `add` | 75,296 |
| `bl` | 66,508 |
| `str` | 62,062 |
| `cmp` | 49,091 |
| `vldr` | 27,152 |
| `b` | 25,972 |
| `beq` | 25,564 |
| `vstr` | 25,264 |
| `pop` | 14,080 |
| `bne` | 12,631 |
| `push` | 12,254 |
| `sub` | 10,520 |
| `strb` | 10,122 |
| `ldrb` | 9,564 |
| `andeq` | 6,623 |
| `vmov` | 6,423 |
| `movw` | 5,728 |
| `bx` | 5,366 |
| `rsb` | 5,305 |
| `vmul.f32` | 4,404 |
| `ldrne` | 4,126 |
| `vmla.f32` | 3,915 |
| `vmrs` | 3,537 |
| `ldrh` | 3,410 |
| `vmov.f32` | 3,072 |
| `strne` | 3,016 |
| `mvn` | 2,944 |
| `vcmpe.f32` | 2,913 |
| `subs` | 2,865 |
| `moveq` | 2,810 |
| `strh` | 2,681 |
| `addne` | 2,568 |
| `lsl` | 2,506 |
| `andseq` | 2,324 |
| `movt` | 2,312 |
| `movne` | 2,226 |
| `vsub.f32` | 2,217 |
| `ldm` | 2,206 |
| `asr` | 2,123 |
| `vadd.f32` | 2,090 |
| `ble` | 2,040 |
| `tst` | 1,875 |
| `vcvt.f32.s32` | 1,822 |
| `orr` | 1,685 |
| `mul` | 1,604 |
| `stm` | 1,535 |
| `eorseq` | 1,357 |
| `eoreq` | 1,332 |

## Categories

| Category | Instructions | Functions using it |
|---|---|---|
| VFP (any `v*`) | 93,908 | 3,118 |
| NEON-like | 8,949 | 1,107 |
| branches `b/bl` | 92,480 |  |
| indirect `bx/blx` | 6,339 |  |
| load/store `ldr/str` | 239,945 |  |
| block `ldm/stm` | 5,338 |  |
| stack `push/pop` | 26,334 |  |
| multiply `mul/mla/umull/smull` | 3,471 |  |
| atomics `ldrex/strex` | 0 | 0 |
| syscall `svc` | 0 | 0 |
| coprocessor `mcr/mrc` | 0 | 0 |
| saturating `q*` | 1 |  |
| bitfield `ubfx/sbfx/bfi/bfc` | 831 |  |

## Calls

| Kind | Count |
|---|---|
| Direct `bl` (immediate) | 66,508 |
| Indirect `blx`/register | 973 |
| `bx` / `ldr pc` sites | 33,999 |

## Most-called internal symbols (direct calls)

| Symbol | Calls |
|---|---|
| `sub_6f5d88` | 4,567 |
| `_ZdlPv` | 2,609 |
| `_ZNK6glitch17IReferenceCounted4dropEv` | 2,172 |
| `sub_298694` | 919 |
| `_ZN5boost13intrusive_ptrIN6glitch5video9CMaterialEED1Ev` | 819 |
| `_ZNSbIcSt11char_traitsIcEN6glitch4core10SAllocatorIcLNS1_6memory13E_MEMORY_HINTE0EEEE4_Rep10_M_disposeERKS6_` | 785 |
| `_Znwj15MemoryHintState` | 775 |
| `_ZdlPv15MemoryHintState` | 673 |
| `_ZdaPv` | 617 |
| `_ZNSbIcSt11char_traitsIcEN6glitch4core10SAllocatorIcLNS1_6memory13E_MEMORY_HINTE0EEEED1Ev` | 560 |
| `_ZNSsD1Ev` | 547 |
| `_Z10GlitchFreePv` | 544 |
| `sub_2980e8` | 509 |
| `_ZN4Game15GetSoundManagerEv` | 492 |
| `sub_298340` | 448 |
| `__cxa_begin_catch` | 417 |
| `__cxa_end_catch` | 415 |
| `_ZnwjN6glitch6memory13E_MEMORY_HINTE` | 413 |
| `_ZN4Game14GetMenuManagerEv` | 392 |
| `sub_29888c` | 390 |
| `_ZdlPvN6glitch6memory13E_MEMORY_HINTE` | 368 |
| `XP_API_MEMSET` | 360 |
| `_ZN4Game9GetPlayerEi` | 357 |
| `sub_29825c` | 346 |
| `_Z7VoxFreePv` | 344 |
| `_ZN5boost13intrusive_ptrIN6glitch5video7IBufferEED1Ev` | 324 |
| `_Znaj` | 312 |
| `__cxa_rethrow` | 311 |
| `sub_298250` | 310 |
| `_ZNSbIcSt11char_traitsIcEN6glitch4core10SAllocatorIcLNS1_6memory13E_MEMORY_HINTE0EEEEC1EPKcRKS6_` | 281 |
| `_ZNSs4_Rep10_M_disposeERKSaIcE` | 269 |
| `_Znwj` | 259 |
| `_ZNSt5dequeISt4pairIPKcN6glitch5video23E_SHADER_PARAMETER_TYPEEESaIS6_EE16_M_push_back_auxERKS6_` | 256 |
| `_ZNSsC1EPKcRKSaIcE` | 254 |
| `_ZNK6glitch5video15CCommonGLDriverINS0_21CProgrammableGLDriverINS0_18CGLSLShaderHandlerEEENS0_6detail33CProgrammableGLFunctionPointerSetEE11testGLErrorEv` | 237 |
| `_ZN5boost13intrusive_ptrIN6glitch5video8ITextureEED1Ev` | 212 |
| `_ZN9MpManager8GetCommsEv` | 209 |
| `_ZN6glitch5video14CVertexStreamsD1Ev` | 208 |
| `XP_API_STRLEN` | 203 |
| `_ZN5Comms8CommsLogEiPKcz` | 201 |

## Compiler / runtime helper calls

| Group | Direct calls |
|---|---|
| `__aeabi_*` | 59 |
| `__cxa_*` | 1,502 |
| `sqrt*` | 1 |
| `sin*` | 39 |
| `cos*` | 35 |
| `atan*` | 20 |
| `pow*` | 6 |
| `exp*` | 1 |
| `floor*` | 17 |
| `fmod*` | 29 |

## Functions using 63 k+ byte bodies are excluded only from size buckets, not counts.

