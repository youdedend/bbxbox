# The shader pipeline: GCN → SPIR-V → DXIL

bbhost already turns the game's GCN machine code into typed, structured SPIR-V (the *lift*), and
prints that module as HLSL. On Windows that HLSL is compiled by DXC to **SPIR-V** for Vulkan. On
Xbox it has to become **DXIL**, which is a different target with different rules. This document is
what changes, what stays, and how the port proves the two produce the same pixels.

## 1. Where the shaders come from

| Source | What it holds | How it is read |
|---|---|---|
| `.shaderbnd.dcx` bundles (`dvdroot_ps4/shader/`) | BND4 archives of shader containers with an `OrbShdr` footer | `src/host/shader_bundles.cpp` (upstream) — the precompiler enumerates them without the game's help |
| The `eboot.bin` itself | YEBIS's, Scaleform's and the engine's full-screen programs, which GX never creates | `eboot_shaders()` (upstream) |
| GX at runtime | a program the game creates that no bundle names | the shader-creation hook; still works, just untranslated until it happens |

The precompiler and the runtime use the same container parser, so a bundle entry's footer hash names
the shader in the cache, in the log and in a quarantine entry.

## 2. Lifting is upstream's

```
GCN program
  → gcn::translate         (SPIR-V for the resource model: bindings, T#/S#, user data)
  → gcn::lift_pixel_shader / lift_vertex_shader / lift_compute_shader / lift_tess_* / lift_gs_*
        = typed structured SPIR-V, one function, structured selections and loops
```

The lifter is the IR of record; the port does not touch it. What the port needs from it is exactly
what it needs on Vulkan: a module whose interfaces are declared, whose operations are preserved, and
whose refusals are countable.

## 3. DXIL-targeted HLSL

`gcn/hlsl.cpp` prints the lift as HLSL with Vulkan conventions. `HlslOptions::target` gains a
`kDxil` value and the emitter changes in these places only:

| Vulkan HLSL today | DXIL target |
|---|---|
| `[[vk::binding(binding, set)]]` on resources | explicit `register(tN, space0)` / `register(uN, space0)` / `register(sN, space0)`, or `ResourceDescriptorHeap[index]` when bindless-direct is on |
| `[[vk::push_constant]]` `ConstantBuffer<Params>` at a byte offset | `[[vk::constant_buffer]]`? No: a plain `cbuffer Params : register(b0)`, with the root constants' 32 dwords laid out at the same offsets |
| `[[vk::location(n)]]` on varyings | `TEXCOORDn` semantics with the same ordering the input layout expects |
| `[[vk::builtin("FragCoord")]]` | `SV_Position` in the pixel stage |
| `[[vk::ext_instruction(...)]]` for `FSign`, `Ldexp`, `FMin/FMax`, `Fma`, bitfield ops | `asfloat(asuint(x) & 0x80000000 | …)`-style integer spellings, `f32tof16`/`f16tof32` via `D3D` intrinsics, `firstbit_*`, or an inline `cs_*` helper; **`mad()` stays banned** (it is a contraction, the lift's `Fma` is not) and is written as `a * b + c` with `precise` |
| `[[vk::ext_execution_mode]]` (denorm/rounding) | dropped: D3D12's shader model has no per-module rounding mode, and the game's GCN rounds to nearest for f32/f16. The lifter already chooses `native_half_rtz=false` for D3D12's half behaviour; the port asserts it (`HlslOptions::half_rtz = false`) |
| `discard` / `OpKill` | unchanged (`discard` is `discard`, not `IsHelperLane`) |
| quad ops via `QuadReadLaneAt` / `QuadReadAcross*` | unchanged — these are HLSL intrinsics, not Vulkan |
| tessellation: one function, two phases | `[outputcontrolpoints(N)]`, `[patchconstantfunc]`, `[domain("tri")]` with the same body split; the port checks the same "patch values written by invocation 0" precondition the lift checks |
| `vk::ext_decorate(18)` on the position output (invariant) | `precise` on the position, and `D3D12_RASTERIZER_DESC::ForcedSampleCount` does not change it; invariance is a PSO flag rather than a decoration |
| descriptor-set indexing (`set` ≠ 0) | the port assigns every set to one of three spaces: `space0` = draw resources (bindless table), `space1` = YEBIS/Scaleform resources, `space2` = compute storage. The assignment is in `bindings.cpp` and is part of the PSO key |

Rules that do **not** change: every float value stays `precise` (so DXC marks arithmetic
`NoContraction` as the lift does), integer operations stay bit-exact, `FMin`/`FMax` stay the
`NMin`/`NMax` spellings rather than `min()`/`max()`, and half-precision arithmetic is still not
printed (conversions only).

The emitter change is deliberately narrow: it is a new set of *spellings* for the same module, so
`hlsl_interface_diff` and `hlsl_operation_diff` (upstream) can still compare the emitted module
against the lift, and the port keeps that check as a build gate.

## 4. Compilation: `dxcompiler.dll` and `dxil.dll`

UWP cannot load a compiler from `PATH`, and cannot use the desktop `dxcompiler.dll` search rules.
The package carries both DLLs from the DXC release matching the Xbox GDK's shader model support:

```
bbxbox.exe
dxcompiler.dll      ← IDxcCompiler3, IDxcLibrary, IDxcValidator (untrusted sign)
dxil.dll            ← IDxcValidator's signer, required for signed DXIL
```

Loading is explicit and versioned (`src/host/d3d12/dxil.cpp`):

```cpp
const std::wstring dir = Package::Current().InstalledLocation().Path().c_str();
LoadPackagedLibrary((dir + L"\\dxcompiler.dll").c_str(), 0);
DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler));
DxcCreateInstance(CLSID_DxcValidator, IID_PPV_ARGS(&validator));
```

* Compile: `-T <profile> -E <entry> -HV 2021 -enable-16bit-types -O3 -Qstrip_reflect
  -Qstrip_debug -Fo <blob>`; profile is `vs_6_6`/`ps_6_6`/`cs_6_6`/`hs_6_6`/`ds_6_6`/`gs_6_6`
  (6.6 because bindless-direct and `ResourceDescriptorHeap` need it; the fallback profile is 6_0
  with classic tables).
* Validate: `IDxcValidator::Validate` with `DxcValidatorFlags_InPlaceEdit`; an unsigned DXIL blob
  fails PSO creation on the console with a bare `E_INVALIDARG`, so this step is mandatory and its
  result is part of the cache entry.
* DXC's version (`1.9.2609.x`) and `dxil.dll`'s are recorded in the cache fingerprint; a different
  compiler invalidates the DXIL cache, as a different translator does the translation cache.

**Two instances, never shared.** `IDxcCompiler3` is not thread-safe across concurrent
`Compile()` calls in every DXC release; each worker thread gets its own instance. Workers are
clamped to 2 (`docs/memory-budget.md` §5) so DXC's arena cannot starve the audio or submission
threads.

## 5. The fallback that keeps 100% of draws native

The lifter refuses a handful of programs (upstream reports 3 of 3,209 pixel shaders at the measured
places; the exact set depends on the build). Those programs have a translation — `gcn::translate`'s
straightforward SPIR-V — but no lift, and sameness is not guaranteed at the bit level.

```
translate.cpp SPIR-V
  → SPBV-Cross (spirv_hlsl) with Vulkan semantics 1.2, -fvk-t-shift / -fvk-u-shift to place resources
  → HLSL (SM 6.0)
  → dxcompiler → DXIL
```

Differences from the lift path are recorded per shader in
`LocalState\bbxbox\shader-report.txt` (`fallback` vs `lift`, the reason, the hash), so a picture
difference in one draw can be attributed to one shader in one line. SPIRV-Cross is vendored as a
build dependency (`ThirdParty/spirv-cross`, Apache-2.0); it is not exported or shipped as a
library, it is linked in.

## 6. Interface hashes and the PSO key

Every compiled shader gets three hashes, all stable across runs:

| Hash | Over | Used for |
|---|---|---|
| `program` | the GCN container bytes (footer hash + code) | naming in logs and quarantine |
| `interface` | stage, inputs (semantic, register, type), outputs, resource slots (space/register/type), root-constant layout | PSO matching: a VS and PS can be linked only if the PS's inputs match the VS's outputs |
| `bytecode` | the validated DXIL blob (minus the validator's signature section, which carries a timestamp) | the PSO cache: identical bytecode reuses the same blob |

The PSO key is

```
PSO key = H( vs.bytecode, ps.bytecode, [gs/hs/ds], blend, depthStencil, rasterizer,
             input layout hash, render target formats, sample desc, root signature id,
             bindless mode, feature level, pso library version )
```

`src/host/d3d12/cache.cpp` implements the canonical serialisation and the key; the test pins the
exact bytes for a fixed input so an accidental field reorder is a test failure, not a cache that
looks valid and draws garbage.

## 7. Cache locations

| Data | Location | Why |
|---|---|---|
| DXIL blobs, PSO library blob, manifests | `LocalState\bbxbox\cache\<serial>\` | survives reboots and updates; portable to USB by the sync tool |
| In-flight breadcrumb and quarantine | `LocalState\precompile_inflight.txt`, `LocalState\precompile_quarantine.txt` | must be written and flushed *before* the first compile; see `docs/precompile-cache.md` |
| DXC scratch, temporary untiling buffers | `TemporaryFolder\bbxbox\` | regenerable; PLM can drop it; keeps LocalState small (a Dev Mode backup quota lesson) |

## 8. Proving equivalence

Before the fast path is enabled for a title a player can reach, the port runs upstream's check at
the 12 measured places:

1. the emitted DXIL's interface is compared field for field with the lift's (`hlsl_interface_diff`
   equivalent on the DXIL reflection);
2. a GPU capture of the same draw in the port and in bbhost's Vulkan build is compared
   numerically (mean absolute error ≤ 1/255 on RGB, no NaN/Inf, and the depth buffer identical);
3. the operation diff counts every substituted spelling; a shader with an unsubstituted Vulkan-only
   operation is quarantined rather than compiled wrong.

Until step 2 passes for a given shader, that shader stays on the fallback path.
