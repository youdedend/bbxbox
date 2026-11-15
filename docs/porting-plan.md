# Porting plan

The port in one document: what the console forces, how bbhost is re-shaped to meet it, which of the
eight hardware-verified Xbox lessons are enforced where, and what "done" means.

## 1. What the platform forces

Xbox Series X|S, Retail Developer Mode, UWP `x64`, App type **Game**:

| Constraint | Consequence for the port |
|---|---|
| No Vulkan driver. `vulkan-1.dll` does not exist and cannot be shipped. | bbhost's Vulkan host (`src/host/gpu.cpp`, `gxvk_*`, `window.cpp`) does not build. A native D3D12 backend replaces it (`docs/d3d12-backend.md`). |
| `AppMemoryUsageLimit` = **5,120 MiB** (Game mode), shared by the process, the GPU allocations and the driver. | Everything is budgeted and enforced: `docs/memory-budget.md`. The guest, its GPU-visible shadows, the D3D12 heaps, DXIL/PSO caches and the compiler's working set all draw from the same 5 GiB. |
| App mode = 1 GiB and no D3D12 FL12_0. | The app refuses to start the game in App mode and says how to switch, instead of dying inside the driver. |
| 8 Zen 2 cores, 7 granted to a Game-mode app; SMT is not available to the app. | Thread budget: the game's own threads plus bbhost's (`bb-cp`, `bb-record`, `bb-submit`, `bb-labels`, `bb-hlsl`) must fit 7 cores, and DXC workers are clamped to 2. |
| No `runFullTrust`, no `broadFileSystemAccess`, no pickers that hang. | Dump access through `FutureAccessList` + `removableStorage`, auto-scan of known roots, and the overlay index that never copies the dump (`docs/console-ux.md`). |
| W^X in the AppContainer. | Guest code pages are mapped RW, then flipped to RX with `VirtualProtectFromApp`; guest addresses must stay under 1 TiB. |
| UWP suspends the process (PLM) with a 5 s deferral. | GPU drain, cache flush and checkpoint on `Suspending` (`docs/console-ux.md`). |

### Why not Mesa Dozen

bbhost's Vulkan host is not a generic Vulkan client that Dozen could satisfy:

* `VK_EXT_external_memory_host` is how guest pages enter the GPU. bbhost imports the game's
  GPU-visible memory (about 4 GB in the world) as host-pointer memory, so a shader reads the exact
  bytes the game's CPU code wrote, with no copies. Dozen has no host-pointer import on Xbox UWP;
  emulating it means copying every buffer per draw — the copies alone do not fit in 5,120 MiB.
* `VK_EXT_graphics_pipeline_library` + `VK_KHR_pipeline_library` are how bbhost links pipelines
  from precompiled stage libraries fast enough to avoid hitches at draw time.
* `VK_KHR_push_descriptor` is how a dispatch's storage buffers are bound without descriptor-set
  churn per draw.

Dozen supplies none of them cleanly, and a Dozen port would still need a D3D12 layer underneath —
one more translator, more memory, more places to lose the frame. The port therefore goes where
bbhost is already going: build the draw from the engine's own objects and compile DXIL directly.

## 2. Architecture

```
  guest eboot.bin (x86-64, unmodified)
        │  syscalls, libs
        ▼
  bbhost HLE (src/core, src/hle)          ← unchanged
        │
        ├──► GX / YEBIS / Scaleform hooks (src/host/render_hooks.cpp)
        │        │  typed draw records: GpuDrawInputs, GxDrawObjects
        │        ▼
        │    command stream  (recorder.cpp, stream_ops.h)   ← unchanged format
        │        │   kDrawState | kBarrier | kCopy* | kClear* | kBeginRendering | kMarker …
        │        ▼
        │    ┌───────────────────────────────────────────────┐
        │    │  bb-record: replay onto D3D12 command lists    │  ← this port
        │    │  src/host/d3d12/replay.cpp                     │
        │    └───────────────────────────────────────────────┘
        │              │
        │              ▼
        │      ID3D12Device / queues / heaps / PSOs / swapchain
        │
        └──► shader creation hook
                 │  GCN program (from .shaderbnd.dcx bundle or eboot)
                 ▼
             gcn::translate  → SPIR-V (bindings, resources)
             gcn::lift_*     → typed structured SPIR-V        (~3,206/3,209 PS, all VS/CS/tess/GS)
                 │
                 ├─ normal path:   DXIL-targeted HLSL → dxcompiler.dll → dxil.dll → DXIL blob
                 └─ refusals (~3): translate.cpp SPIR-V → SPIRV-Cross HLSL → DXC → DXIL blob
                 ▼
             PSO ← D3D12 pipeline state, cached by key (docs/precompile-cache.md)
```

Two things are deliberately **not** changed:

1. **The guest and the HLE.** bbhost's ELF loader, its 660 import bindings, its memory model and its
   frame pacing are reused as is; the `sysv_abi` constraint is why the build is `clang-cl`
   (`docs/memory-budget.md` §4).
2. **The command-stream format.** The D3D12 backend consumes the same records the Vulkan backend
   does, so `recorder.cpp`'s ordering, the recorder's phases and the marker windows are unchanged.
   What changes is the sink: `Replay` instead of Vulkan calls.

## 3. The eight Xbox lessons, enforced

The eight solutions `Test-project` proved on Series S hardware, and where each one lives here:

| # | Lesson | Enforced in |
|---|---|---|
| 1 | **Guest stack fix.** `/STACK:16777216,16777216` committed 16 MiB per OS thread (82 threads ≈ 1.28 GiB). Use `/STACK:8388608,65536` and let stacks grow. | `xbox/CMakeLists.txt`; verified by the startup `AppMemoryUsage` acceptance line. |
| 2 | **Sub-1 TiB guest arena, commit-on-touch.** Guest mappings are placeholders (`VirtualAlloc2FromApp` + `MEM_RESERVE_PLACEHOLDER`), backed when touched, aliased with `MapViewOfFile3FromApp`; every guest-visible address stays below 1 TiB. | `xbox/platform/guest_memory.inc` (strict W^X; `guest_swap.inc` is the spill the residency pager uses, since bbhost does not map the dump). |
| 3 | **Sparse `WC_GARLIC`-only swap.** Only cold, non-executable GPU-visible pages page out, to a sparse `LocalState` file, with a live-view registry so a view the GPU may still read is never recycled. | `xbox/platform/guest_swap.inc`. |
| 4 | **5,120 MiB Game Mode budget.** Memory is a plan, not a hope: guest, shadows, heaps, caches, compiler and reserve, each capped, with the pager's thresholds derived from the limit. | `docs/memory-budget.md`, `src/host/d3d12/budget.*`. |
| 5 | **Split dump support.** Base 01.00 + update 01.09 as two folders, version read from each `param.sfo`, never guessed; the update is overlaid in place (no copying tens of GB). | `xbox/platform/sfo_version.h`, `overlay_index.h`, `dump_panel.h`, `src/host/uwp/storage.cpp`. |
| 6 | **Crash-proof precompile.** `LocalState\precompile_inflight.txt` is written and flushed before each shader/PSO compile; on the next boot it is consumed into `precompile_quarantine.txt` so a malformed shader cannot boot-loop the console. | `src/host/d3d12/precompile.cpp`, `xbox/platform/main.cpp`. |
| 7 | **Console UX.** `Windows.Gaming.Input` with touchpad chords on `View`, system virtual keyboard for the Hunter Name, `XAudio2` 7.1, `uwp_last_chance_logger` writing the faulting module+RVA. | `xbox/platform/input.cpp`, `audio.cpp`, `keyboard.cpp`, `main.cpp`. |
| 8 | **UWP compliance and diagnosability.** Strict manifest (`codeGeneration` + `removableStorage` only), every import audited against `WindowsApp.lib`, `/MAP` + `symbolicate.py` so `bbxbox.exe+0xNNNNNN` resolves for the exact package. | `xbox/AppxManifest.xml`, `xbox/scripts/check-imports.ps1`, `symbolicate.py`. |

### The platform layer is SDL3-uwp, not a shim

The console build does not grow its own SDL: [SDL3-uwp](https://github.com/XboxEmulationHub/SDL3-uwp)
(`uwp-3.4.20`) is a complete SDL 3.4.20 with WinRT video, WASAPI audio,
`Windows.Gaming.Input` gamepads and WinRT pickers, so bbhost's existing SDL calls keep working.
It is compiled once with MSVC (its WinRT code is C++/CX) into `build-deps/prefix/sdl3` and cached
by CI; the app stays clang-cl and meets it through SDL's C API. [libuwp](https://github.com/XboxEmulationHub/libuwp)
supplies the two helpers SDL lacks: the TV output size and refresh rate from `HdmiDisplayInformation`
(CoreWindow bounds are not the output resolution on Series S) and `LoadPackagedLibrary` for
`dxcompiler.dll`/`dxil.dll`. The one dependency surface deliberately avoided is libuwp's
`uwp_pick_rom`, which copies a dump into local storage — the port grants the dump folder once through
the `FutureAccessList` and reads it where it lies. The launcher's own UI is SDL's renderer plus
`SDL_RenderDebugText` for P0; from P2 the frame comes from `src/host/d3d12` on the same CoreWindow.

## 4. The D3D12 backend in one page

Details in `docs/d3d12-backend.md`. The shape:

* **One device, three queues.** `ID3D12Device` at feature level 12_0. A direct queue (draws), a
  compute queue (texture untiling, and the game's dispatches when they do not alias the direct
  queue's work), a copy queue (uploads/readbacks). Each has its own timeline fence; a resource is
  free when its queue's fence has passed the value recorded when it was last used.
* **UMA heaps, placed resources.** Everything is `D3D12_HEAP_TYPE_DEFAULT` in a few big heaps the
  budget owns (one for buffers, one for textures, one for render-target/depth slices), so the
  5 GiB plan is visible in one place. Upload/readback rings are their own fixed heaps.
* **Bindless by default.** Root signature: 32 dwords of root constants (GCN user data as-is), one
  SRV/UAV descriptor table and one sampler table into shader-visible heaps, plus
  `CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED` when binding tier 3 and SM 6.6 allow it. Draws bind resources
  by index, exactly like GCN binds T#/S# slots, so per-draw descriptor writes disappear.
* **No host-pointer import.** D3D12 cannot alias guest pages, so the buffer shadow that bbhost uses
  on discrete GPUs becomes the only path: guest buffers have a GPU twin, and `core/write_watch.cpp`
  tells the backend which 4 KiB pages the game wrote so only those ranges are re-uploaded.
* **Flip model on `CoreWindow`.** `CreateSwapChainForCoreWindow`, `DXGI_SWAP_EFFECT_FLIP_DISCARD`,
  3 buffers, `SetMaximumFrameLatency(2)`, waitable object for pacing, tearing only when
  `CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING)` and vsync is off.
* **GPU untiling.** GCN tiled (thin/thick, pipe-interleaved, bank-swizzled) textures are detiled by
  a compute shader into a linear staging buffer, then copied into the texture; the existing
  `GpuUntile` census is reused to decide when a texture is read linear already.
* **Fences feed `bb-labels`.** Each submission signals a timeline value; a `bb-labels` thread waits
  on the fence and then writes the submission's label values into guest memory in order. GPU
  `WriteBufferImmediate` is used where a label must land on the GPU's own timeline.

## 5. Frame budget for 60 FPS

16.67 ms per frame, and the target is not to sit on it:

| Stage | Budget (Series X, 1080p) | Budget (Series S, 900p–1080p dynamic) |
|---|---|---|
| Game main + job threads (7 cores) | 8.0 ms | 9.5 ms |
| bbhost command processor (`bb-cp`) | 3.0 ms | 3.5 ms |
| `bb-record` replay + submit | 2.0 ms | 2.5 ms |
| GPU | 13.0 ms | 14.0 ms |
| Present (waitable, 2-deep) | 0.3 ms | 0.3 ms |

How the budget is defended:

* **PSOs never compile during gameplay** after the first visit to a place: the precompiler builds
  the bundles' shaders and the manifest's pipelines ahead of gameplay, and anything new is built on
  the precompile thread while the frame runs (hitches, not stalls). `docs/precompile-cache.md`.
* **Draws cost no descriptor writes.** Bindless indices are written once when a resource is
  created; a draw only writes 32 root constants and one pipeline index.
* **No per-draw copies.** Buffer shadows upload dirty 4 KiB pages once per frame at most, before
  the draw that needs them.
* **Resolution is a lever, not a constant.** Dynamic 1080p→900p (Series X) and 1080p→810p (Series
  S) on a 2-second GPU-time and frame-time controller; the HUD and UI stay native.
* **The frame limiter is the game's.** bbhost's frame-time manager already runs at 30/60/90/120; the
  port adds a present-side latch so a missed deadline is made up once and not twice.

## 6. Phases

Each phase ends with something that can be checked, not with "should work".

| Phase | Work | Exit criterion |
|---|---|---|
| P0 | UWP shell, launcher, memory contract, packaging, CI, import audit | `.appx` installs in Game mode; the memory self-test passes on console; startup `AppMemoryUsage` ≤ 1,100 MiB |
| P1 | Guest boot + HLE + audio/input (no rendering) | The title's logo/menu code runs; input reaches the game; audio starts; no fault in 10 min |
| P2 | D3D12 device, swapchain, replay of clears/copies/barriers/markers; show the game's own framebuffer | A frame reaches the screen: menu drawn by the game's own commands |
| P3 | GX draw path: pipelines, bindless descriptors, vertex fetch, buffer shadow | Title screen renders correctly at 30 FPS, `presents > 10,000`, `corrupt=0` |
| P4 | YEBIS post, Scaleform UI, tessellation/GS paths, compute dispatches | Menus, HUD, effects and GPU particles correct; Central Yharnam streams and draws |
| P5 | DXIL precompile + PSO cache + quarantine + USB sync | Second launch compiles nothing at gameplay; zero quarantines on a clean dump |
| P6 | 60 FPS pass (resolution controller, residency, queue overlap) | Acceptance in `docs/validation.md`: sustained 60 FPS in two areas, 1% low ≥ 55 |

## 7. Risks

| Risk | Why it is real | Mitigation |
|---|---|---|
| Guest memory + shadows do not fit 5,120 MiB | Bloodborne's world is ~4 GB of GPU-visible guest memory; D3D12 needs its own copies | Shadows are windowed (only what a frame reads), textures are BCn and streamed, and the pager covers the rest; worst case the resolution controller buys memory, not quality |
| No host-pointer import | Changes the data flow everywhere | The buffer shadow is already bbhost's discrete-GPU path; the port makes it mandatory and adds page-granular dirty tracking |
| PSO creation stalls | D3D12 compiles pipelines synchronously | Ahead-of-gameplay precompile, remembered-manifest, and a bounded compile thread; never on the render thread mid-frame |
| DXIL-targeted HLSL emitter diverges from the lifter | The emitted HLSL must preserve operations bit for bit | The port keeps bbhost's `hlsl_interface_diff`/`hlsl_operation_diff` harness and compares shader outputs at the 12 measured places before enabling the fast path |
| Console-only bugs | No local repro | `uwp_last_chance_logger` with module+RVA, `/MAP` symbolication in CI, and the precompile breadcrumb so a crash is attributable to one shader hash |

## 8. Non-goals

* Not an emulator: the guest runs natively, as it does in bbhost.
* Not a Vulkan compatibility layer over D3D12: there is no `vulkan-1.dll` shim, no Dozen, no
  translation of Vulkan commands. The stream records are consumed directly.
* Not a re-implementation of bbhost's translator: the GCN lifter, decomps and HLE are upstream's.
* No game data, no keys, no dumps in Git, ever.
