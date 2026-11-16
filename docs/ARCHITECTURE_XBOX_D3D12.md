# Xbox Series X|S D3D12 architecture (hardware rules)

This is the console's GPU, as measured on real Series X hardware in UWP Developer Mode — not as
documented. Sources: KytyPS5's `docs/d3d12-backend.md` and `XBOX_CHANGES.md`, commit `988ceea`
(compute tessellation), `src/uwp/fidelityfx.cmake` and `src/uwp/src/app.cpp`, plus
`Cosmo/ext-ffmpeg-core-xbox`. Every rule below is enforced somewhere in this tree, and
`tools/local_precheck.sh` fails when the code stops agreeing with this document.

`src/host/d3d12/bbx_caps.h` is the machine-readable form: `XboxSeriesCaps()` returns the limits, and
the DXIL compile flags, the PSO cache and the tests all read it.

## 1. Device capabilities

| Property | Series X (SraKmd_arden) | Consequence in this port |
|---|---|---|
| Feature level | **11_0** | No FL12_0-only features; `bbx_d3d12.h` never asks for them. |
| Shader model | **6.4** (max) | `ShaderTarget()` emits `*_6_4`; `-T ps_6_4`, `cs_6_4`, … |
| `ResourceDescriptorHeap[...]` | **absent** (SM 6.6) | Descriptor **tables** + `spaceN` arrays + `NonUniformResourceIndex`; bindless is by slot index into a table, never heap indexing. |
| Mesh shaders | absent (SM 6.5) | Merged VS/GS (NGG) continues to be spliced to a vertex shader (`src/gcn/gs_vertex.h`), not a mesh shader. |
| Barycentrics | absent | Pixel shaders that read per-vertex values fall back to the interpolated value (speckles, documented limit). |
| Native 16-bit ALU | **absent** | Never `-enable-16bit-types`; 16-bit math is lowered to 32-bit (`CompileFlags::lower_16bit_ops`). |
| Resource binding tier | 3 | Unbounded descriptor arrays are legal; the port still sizes them. |
| Wave size | 64 lanes | `WaveActiveBallot` excludes helper lanes at 6.4 — **an empty ballot must not hang a loop** (see §6). |
| Typed UAV loads | yes | `kTypedUavLoad` is used where the translator emits it. |
| Group shared memory | **32 KiB** | The shader cache clamps the guest's LDS (guard G15). |
| Constant-alpha blend factors | **rejected** | The pipeline builder never emits them (guard G4). |
| `ID3D12PipelineLibrary` | **removes the device** | Never created; disk cache stores **signed DXIL blobs** only. |
| Hardware tessellator | **emits zero domain points** | Compute tessellation or skip (see §4). |
| Swapchain | 3840×2160 max | `xbox/platform/display.*` clamps (guard G8); CoreWindow is 1080p regardless of TV mode. |
| GPU memory | ~2.2 GiB of the 5,120 MiB app budget | Budget owners in `bbx_budget`; PSO/heap residency is enforced, not hoped for. |

## 2. Never a pipeline library, never all PSOs at once

Two independent failures, both verified:

1. `CreatePipelineLibrary` on this driver removes the device.
2. Thousands of live `ID3D12PipelineState` objects exhaust the 5,120 MiB cap (the prior art ran a game
   out of memory building them ahead of time).

The port therefore:

* precompiles `.shaderbnd.dcx` programs ahead of gameplay into a **compressed disk cache of signed
  DXIL blobs** in `LocalState\shadercache` (`docs/precompile-cache.md`), with `precompile_inflight.txt`
  written and flushed before every compile so a DXC/driver crash quarantines that unit instead of
  boot-looping the console;
* creates a PSO **lazily**, the first draw that needs it, from the cached blob;
* keeps live PSOs in a bounded LRU (`src/host/d3d12/bbx_psocache.*`) inside the `kPipelines`
  budget owner (192 MiB, plus a 2,048-object cap). Eviction hands the keys back so their objects are
  released; a re-create comes from the blob, not from a compile;
* replays the three guard fixes the hardware needs: no constant-alpha blend factors (G4), the
  guest's LDS clamped to 32 KiB (G15), and a read-only DSV on any depth-sampled draw (G12).

`PsoKeyInput::pso_library` exists in `bbx_cache.h` only to be **0** on the console; a key with it set
can never be produced by the Xbox path.

## 3. Shader compilation

Two different problems, with two different answers: the port's own passes, and the guest's shaders.

### 3.1 The port's passes: DXIL at build time, no compiler in the package

`xbox/shaders/present.hlsl` and `overlay.hlsl` are compiled by `build-deps/prefix/dxc/dxc.exe` on the
build machine, at `-T vs_6_4` / `-T ps_6_4`, `-O3 -WX`, and embedded as byte arrays by
`cmake/embed_shaders.cmake` into `bbx_shaders_generated.h`. `src/host/d3d12/bbx_shaders.h` is the
manifest of entry points; `tests/shaders_test.cpp` checks it and `tools/local_precheck.sh` checks it
against `xbox/CMakeLists.txt`. The consequence that matters: **no shader compiler ships**, so
`LoadPackagedLibrary` on a desktop `dxcompiler.dll` is not a run-time dependency of anything.

That DLL was a real single point of failure, not a hypothetical one: `dxcompiler.dll` imports
`CreateFileW`, `LoadLibraryA` and `MapViewOfFile`, which the AppContainer refuses, so whether the
console could load it depended on an OS detail nobody controls. Compiling at build time also moves a
shader error from the TV to CI.

Rules the compiled passes follow, enforced by `-WX` plus the precheck greps:

* `*_6_4`, `-enable-16bit-types` never (`lower_16bit_ops` always, §1), no `ResourceDescriptorHeap`
  (SM 6.6), no mesh shaders.
* Bindings: one SRV/UAV descriptor table per kind in its own `space`, one sampler table; guest
  T#/S# slots become table indices. Non-uniform access goes through `NonUniformResourceIndex`, and
  where the driver needs it a `WaveReadLaneFirst` waterfall.
* Loops with `break` carry `[loop]` (guard G6); a switch that cannot express the guest's flow is a
  skip with a log line, not a hang.

### 3.2 The guest's shaders: SPIR-V is the IR of record, DXIL is the target

`gcn/lift.cpp` lifts guest GCN to SPIR-V, and all 22 `src/host/shaders/*.spv.h` are SPIR-V.
`gcn/hlsl.cpp` is **not** on that path: it emits SPIR-V flavoured HLSL (`[[vk::ext_instruction]]`,
`[[vk::ext_builtin_input]]`, `[[vk::ext_execution_mode]]`) for `dxc -spirv`, and DXC rejects that
source outright at `-T *_6_4`. Nothing may feed it to a DXIL profile; the precheck asserts the port's
own HLSL contains no `[[vk::` for the same reason.

One exception to that rule exists and is measured: `gcn/hlsl.cpp`'s console dialect
(`HlslOptions::dxil_sm64`, §3.3), which the prebake bakes to DXIL with `-T *_6_4`. It carries no
`[[vk::` attribute and no inline SPIR-V, and the precheck and the bake both refuse a source that
does.

The plan for P3 (guest shaders, not yet implemented) follows KytyPS5: translate SPIR-V to DXIL
in memory with Mesa's MIT-licensed `spirv_to_dxil`, built with `-Db_vscrt=mt
-Dmin-windows-version=10` and a local patch for

* linked-stage signatures (the guest's VS/PS pair is one SPIR-V module, DXIL wants separate stages
  with matching signatures),
* 16-bit half-to-int lowering, because the console has no native 16-bit ALU (§1),
* `NonUniform` descriptor handles, and
* SM 6.4 helper-lane wave fixes.

The result is validated in memory with `dxil.dll`'s `IDxcValidator` (present on the console; also
MIT-adjacent and already part of the platform) — again zero `dxcompiler.dll`. If `spirv_to_dxil`
proves too lossy for Bloodborne's compute, the fallback is a D3D12-native SM 6.4 emitter from the same
SPIR-V, not a packaged desktop compiler.

### 3.3 The console's dialect and its guest memory

The prebake (`tools/bbx_bake_cache.cpp`) compiles guest shaders on the desktop, through
`gcn/hlsl.cpp` with `dxil_sm64`, and ships the DXIL. Two of §1's rules shape that dialect:

* **no 64-bit integers.** `Int64ShaderOps` is not something the console's shader compiler provides
  for a title that could have used it, so every `uint64_t` in the generated source is a refusal; a
  `uint64_t`/`int64_t` block member the shader never reads is rewritten to `uint2` (the standing
  64-bit rule), and a shader that *reads* one is refused by name.
* **no `float16_t`.** `-enable-16bit-types` is never passed, and a module that needs it is refused.

A compute shader's guest memory is the case that forces this. The Vulkan path walks the page table
itself and dereferences a `PhysicalStorageBuffer` pointer (`gcn/lift.cpp`, `declare_guest_memory`);
DXIL has neither those pointers nor a 64-bit integer to hold a VA in, and a D3D12 `RWByteAddressBuffer`
is addressed by a **32-bit** byte offset, so no single buffer covers the guest's address space (5 GiB
of RAM plus the GPU's `0xff00000000` alias, in sparse 64 KiB pages). `TranslateOptions::console_memory`
therefore lowers a guest VA to **two dwords** - `(lo, hi)` - with the carry made by hand:

```hlsl
uint lo2 = lo + off;
uint hi2 = hi + (lo2 < lo ? 1u : 0u);   // DXIL 6.4 has no 64-bit add
```

and every load, store and atomic to a call into a prelude that `gcn/hlsl.cpp` prints once per shader
(`console_memory_prelude`). The root signature contract, which Slice 5B implements:

| binding | contents |
|---|---|
| `t0, space3` — `ByteAddressBuffer g_page_table` | L1: one dword per 4 GiB region, `4096` entries, `min(va_hi, 4095) * 4 -> l2_base` |
| | L2: one 8-byte entry per 64 KiB page, `65536` per table, `(l2_base + ((va_lo >> 16) & 0xffff)) * 8 -> uint2(slab, byte offset of the page)` |
| `u0..u31, space3` — `RWByteAddressBuffer g_guest_slabs[32]` | 32 slabs of 256 MiB (8 GiB), each under 4 GiB, indexed with `NonUniformResourceIndex(slab)` |

A shader's address is the page's slab and offset plus `va_lo & 0xffff`, and the access is `.Load`,
`.Store`, `InterlockedAdd`, `InterlockedAnd` or `InterlockedOr` on that slab. The constants live in
`src/host/d3d12/bbx_guest_mem.h` (`kGuestSpace`, `kGuestPageTableRegister`, `kGuestSlabRegister`,
`kGuestSlabCount`, `kGuestSlabBytes`, `kPageL1Entries`, `kPageL2Entries`) so the launcher and the bake
cannot disagree about them.

### 3.4 The root signature those shaders need, and the four rules it obeys

`src/host/d3d12/bbx_guest_root.*` describes it as data and `pipeline.cpp` turns that into D3D12:
`b0, space1` is the `StageParams` root CBV, `space1` carries the stage's own Vulkan bindings flattened
into registers (`b1..b63` constant buffers, `t0..t63` SRVs, `u0..u63` UAVs, `s0..s63` samplers), and
`space3` carries the guest memory above. Four facts about D3D12 decide its shape:

1. **A descriptor table is all samplers or no samplers.** `D3D12SerializeRootSignature` rejects a table
   that mixes a `SAMPLER` range with `CBV`/`SRV`/`UAV` ranges, so `s0..s63` has a table of its own in
   every signature - and a sampler lives in the sampler heap, not the CBV/SRV/UAV one.
2. **CBV, SRV and UAV may share a table; they may not share a register file.** Three ranges, because a
   guest's flat Vulkan binding numbers collide across classes (`b3` and `t3` are different resources and
   both exist). `b0` is absent from the table on purpose: it is the root CBV.
3. **One root parameter has one visibility.** A vertex/pixel draw needs *both* stages' space-1 tables, so
   the graphics signature carries them separately (`VISIBILITY_VERTEX`, `VISIBILITY_PIXEL`); sharing one
   table would bind the VS's `t0` and the PS's `t0` to the same descriptor. The compute signature is one
   stage, so its tables are `VISIBILITY_ALL`. Same ranges, same baked DXIL, only the visibility differs:
   **compute is 5 DWORDs** (root CBV + resources + samplers + space 3) and **graphics is 9** (4 per stage
   + space 3), against the console's ceiling of 64.
4. **One shader-visible CBV/SRV/UAV heap, and no copying from it.** `SetDescriptorHeaps` takes one such
   heap per command list, and `CopyDescriptorsSimple` refuses a `SHADER_VISIBLE` *source*. The guest
   arena is therefore an index space (`GuestHeapLayout`): views are made once in a `FLAG_NONE` heap
   (`bbx_guest_mem.cpp`), copied into the device's single shader-visible heap at fixed offsets - space 3
   first, then one region per stage - and the dispatcher's tables are GPU handles into that region.

No static samplers and no `ID3D12PipelineLibrary`, for the reasons in `bbx_cache.h`
(`kConsolePsoLibrary`): a static sampler cannot carry an aniso/border combination per binding, and the
pipeline library removes the device on a console.

Two sites still name a *host* address in the params block, and cannot be spelled on the console: a GDS
counter's address (`ds_append`/`ds_consume`) and a captured position (`capture_position`). Both are
refused by name in console mode rather than approximated.

`bbx_bake_cache --self-test`'s fourth container (`cs_mem`) is a real one: a V# in `s[0:3]`, an `offen`
dword load at `0x1000`, a `s_waitcnt`, and a store of what it loaded at `0x1004`, so both bindings,
the two-dword address and the prelude's load and store are exercised by every run. Every baked blob is
checked for `DXBC`, a non-zero digest at `bytes[4..20]`, and - from the container's own `SFI0` part -
feature flags without `Int64ShaderOps` (bit 1) or the 16-bit types (bit 7).

## 4. Tessellation: zero domain points

The Series X tessellator produces no domain points, so a hardware HS/DS draw is invisible or hangs.
Bloodborne's particle export+geometry stage is already spliced into a vertex shader upstream
(`src/gcn/gs_vertex.h`), which leaves the true hull/domain draws (Forbidden Woods and friends).

Policy in `src/host/d3d12/bbx_tess.*` (`ChooseTessPath`), modelled on KytyPS5 `988ceea`:

| Path | When | How |
|---|---|---|
| `kHardware` | desktop only | normal HS/DS pipeline |
| `kCompute` | console, emulation on, no isoline, LDS ≤ 32 KiB | capture pass (LS as VS) → HS as compute (one workgroup per patch) → `gpu_tess_scan.comp` + `gpu_tess_tessellate.comp` (D3D11 reference tessellator, MIT) → TES as the VS of an indirect draw; patches beyond the scratch chunk split across draws |
| `kSkip` | emulation off, isoline, LDS too large, or HS/DS unpaired | the draw is skipped, counted, and named once in the log |

`TessCounters` prints `tessellation: N hardware, N compute, N skipped (off/isoline/LDS)` in the exit
report, so a black screen is attributable. The toggle lives with the level's settings; default on,
because a skipped draw is worse than a slow one only if it is silent — and it is not.

## 5. TV output and upscaling

* The CoreWindow is 1920×1080 whatever the TV does. `QueryTvMode()` reads
  `HdmiDisplayInformation::GetForCurrentView().GetCurrentDisplayMode()`; `PlanOutput()` sizes the
  swapchain to the TV mode, clamped to 3840×2160 (G8), and returns the inverse matrix for
  `IDXGISwapChain2::SetMatrixTransform` so the picture is 1:1 on the panel.
* "Output resolution: 1080p" keeps the window's pixels (plan scale 1.0).
* Upscaling: AMD FSR 1 (MIT) for non-HDR pictures, FSR 3.1 (MIT, FidelityFX SDK v1.1.4) where a
  temporal hint is available — both compiled statically into the app (`src/uwp/fidelityfx.cmake` in
  KytyPS5 is the reference recipe: strip PIX and the desktop swapchain wrapper, keep AMD's licence
  next to the binaries). The port's own presentation is a fullscreen-triangle blit; FSR slots in
  where that blit is, once rendering is live (P4).

## 6. The wave/ballot trap

At SM 6.4 `WaveActiveBallot` excludes helper lanes. A pixel shader loop that waits for "all lanes
done" therefore sees an empty ballot when the remaining lanes are helpers, and spins forever — a GPU
hang the console reports by ending the game, not by a device removal. Rules the port follows:

* a ballot-driven loop terminates on the ballot's own result, never on a count that can be zero;
* lane selection uses `WaveReadLaneFirst` over the lanes still active, with a documented fallback for
  an empty set;
* an empty ballot is treated as "this lane is done", which is what 6.4 means.

## 7. FFmpeg and video

`Cosmo/ext-ffmpeg-core-xbox` builds FFmpeg as static libraries with clang-cl (so inline-assembly
optimisations are available, which MSVC cannot do) and UWP-clean imports, with D3D12 hardware decode
for H.264/HEVC/VP9. `xbox/scripts/build-deps.ps1` builds it into the cached `build-deps/prefix`
alongside SDL3-uwp; nothing in this repository is fetched at run time.

## 8. Guards G1–G20 as the port applies them

| Guard | Rule | Where |
|---|---|---|
| G1 | 96-bit RGB32 textures must not be created | `bbx_format` rejects; the draw is skipped |
| G2 | No format casts across typeless families | format family recorded in `bbx_format`; copies instead of casts |
| G3 | uint view of a BC texture | unhandled: skipped with a log line, not run on console |
| G4 | No constant-alpha blend factors | `bbx_psocache`/pipeline builder refuses; blend falls back to one/zero-alpha |
| G5 | `EvaluateAttribute*` needs barycentrics | absent on console; interpolated value fallback |
| G6 | `break` in a loop needs `[loop]` | `CompileFlags::loop_attribute`; HLSL emitter annotates |
| G7 | Stencil plane copy to a buffer | not supported; logged once |
| G8 | Presented image ≤ 3840×2160 | `xbox/platform/display.*` |
| G9, G11, G16–G20 | Guest memory: lazy commit, no huge reservations, red-zone protection | `xbox/platform/guest_memory.inc` (< 1 TiB arena, commit-on-alloc, strict W^X) |
| G10 | No frame-statistics pacing | `bbx_present` uses the present interval only |
| G12 | Depth sampled and written in one draw | read-only DSV on depth-sampled draws |
| G13 | Typed UAV loads | used; no format casting through UAVs |
| G14 | Per-lane array index in DXBC | not applicable: the port emits DXIL |
| G15 | 32 KiB group shared memory | `GroupSharedFits()` clamps and warns once |

## 9. What this costs 60 FPS

Nothing here is free, so the budget says where it goes: no compile on the render thread, no descriptor
writes per draw (bindless table indices), no pipeline library load, no tessellation hang, and a
swapchain at the TV's size instead of a 1080p one upscaled twice. The frame accounting is in
`docs/memory-budget.md` §6 and `docs/porting-plan.md` §5.
