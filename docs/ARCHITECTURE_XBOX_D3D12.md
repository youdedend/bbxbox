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

* Target `*_6_4`, `-enable-16bit-types` never, `lower_16bit_ops` always (§1).
* Bindings: one SRV/UAV descriptor table per kind in its own `space`, one sampler table; guest
  T#/S# slots become table indices. Non-uniform access goes through `NonUniformResourceIndex`, and
  where the driver needs it a `WaveReadLaneFirst` waterfall.
* Signed with the SDK's `dxil.dll`, loaded from the package with `LoadPackagedLibrary` (libuwp), never
  from the working directory.
* Loops with `break` carry `[loop]` (guard G6); a switch that cannot express the guest's flow is a
  skip with a log line, not a hang.

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
