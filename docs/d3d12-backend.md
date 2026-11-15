# The native Direct3D 12 backend

`src/host/d3d12/` replaces `src/host/gpu.cpp` (and `gxvk_*`, `window.cpp`, `upscaler_ffx.cpp`) on
Xbox. It consumes the same `gpu::stream` records the Vulkan backend does and produces the same
picture through `ID3D12Device`.

## 1. Device and queues

```cpp
D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device));
```

Feature level 12_0 is what Game mode grants. If it fails, the app reports *App mode is not
supported* and exits; there is no silent D3D11 fallback because bbhost's draw model needs 12_0
resources and typed UAVs.

Three queues, created from `D3D12_COMMAND_LIST_TYPE_*`:

| Queue | Priority | Carries | Fence |
|---|---|---|---|
| direct | normal | every draw, pass, barrier, copy that touches a render target | `direct_fence` |
| compute | high | texture untiling, particle dispatches that do not alias the frame's targets | `compute_fence` |
| copy | normal | uploads, readbacks, shadow-page uploads | `copy_fence` |

Each queue has a monotonically increasing timeline value. `Timeline` wraps a value, an
`ID3D12Fence` and a `HANDLE` event, and is the only thing the rest of the backend waits on.

**Why timeline fences and not `ID3D12Fence` per submission:** the label landing thread has to know
"everything at or before submission N has run". A single monotonic value per queue is exactly
`vkQueueSubmit`'s timeline semaphore that bbhost already relies on, and it is what the `bb-labels`
thread waits on.

Command lists: `kFramesInFlight = 3` allocators per queue, each with its own
`ID3D12GraphicsCommandList7`. Bundles are not used for draws (bbhost's list packets are already
flattened by the recorder); they are used for the untiling job prefixed to a copy.

## 2. Heaps, UMA and the budget

Xbox is UMA: `D3D12_HEAP_TYPE_DEFAULT` is physically the same memory the CPU uses, but D3D12 still
requires heap types and still does not let a shader dereference a CPU pointer. That single fact
shapes the backend:

* **There is no host-pointer import.** bbhost's Vulkan host imports guest pages with
  `VK_EXT_external_memory_host`. On D3D12 a guest buffer reaches the GPU only through a **shadow**:
  a `D3D12_HEAP_TYPE_DEFAULT` buffer whose contents are copied from guest memory. bbhost already
  has this path (`src/host/buffer_shadow.cpp`, `src/core/write_watch.cpp`) for discrete GPUs; the
  port makes it mandatory and page-granular:
  * `write_watch_arm_locked` marks guest pages read-only; the fault handler records the 4 KiB page
    in the owning shadow's dirty set;
  * at replay time the dirty ranges are copied into the upload ring once, then the draw reads the
    shadow;
  * a shadow is valid from the fence value at which its upload was recorded.
* **Heaps are budgeted objects.** Four heap owners, each with a hard cap from
  `docs/memory-budget.md`: `kBufferHeap`, `kTextureHeap`, `kTargetHeap` (render targets and depth),
  `kTransientHeap` (staging rings). A request that would cross a cap returns
  `D3D12_HEAP_FLAG`-less failure to the planner, which evicts LRU resources (texture pool first)
  and retries; a request that cannot be satisfied at all fails the frame loudly rather than
  letting the process die at the ceiling.
* **Placed resources, not committed ones.** Committed resources are opaque to the budget and
  fragment the address space; `ID3D12Device::CreatePlacedResource` into budget-owned heaps is what
  lets the port answer "how much is in textures right now" exactly.
* **Residency.** `ID3D12Device3::MakeResident`/`Evict` are not the constraint here (there is no
  separate video memory); the constraint is the process commit. `budget.cpp` therefore reports both
  `AppMemoryUsage` and the D3D12 heap totals, and the pager's thresholds are derived from the
  former.

`D3D12_HEAP_FLAG_CREATE_NOT_ZEROED` is used for transient heaps (staging, targets) because zeroing
1 GiB of staging costs milliseconds per frame; the port zeroes only what it hands out.

## 3. Root signature and descriptors

The root signature mirrors GCN's binding model rather than Vulkan's:

```
root constants: b0, 32 dwords            ← GCN user data (SGPRs) copied as-is
descriptor table (SRV/UAV): t0..tN       ← shader-visible heap, 65,536 entries
descriptor table (sampler): s0..s15      ← shader-visible heap, 2,048 entries
```

* A draw's resources are **bindless indices**, written once when the resource is created, not per
  draw. That is the same shape as GCN's T#/S# user-data slots, so a bbhost draw packet maps to a
  root-constant write plus a pipeline (see `replay.cpp`).
* When `D3D12_RESOURCE_BINDING_TIER_3` and shader model 6.6 are both available, the tables are
  compiled out and the HLSL indexes `ResourceDescriptorHeap[]` directly
  (`CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED`). The two shapes share one key space; `descriptors.cpp`
  decides at device creation and the shader cache records which one it compiled for.
* The sampler heap is fixed-size and pre-populated with the game's 16 sampler states (GCN's S#
  filter/wrap combinations), so a sampler bind is an index, not a heap write.
* Root signature flags: deny unnecessary root access, allow IA layout, and
  `D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED` when bindless-direct is on.

Descriptor allocation (`descriptors.cpp`) is a per-frame ring over the shader-visible heap: 3 rings,
each `kRingSize = 16,384` entries, with a free-list of released tables that carries the fence value
that frees them. A table released in frame N is reusable in frame N+3; the assert is that no table
is reused before its fence.

## 4. Pipelines

`ID3D12PipelineState` per (shader pair, render state) exactly like bbhost's `GfxPipeline`, with two
differences forced by D3D12:

* **PSO creation is the expensive step**, not shader compilation. The precompiler therefore caches
  PSO descriptions *and* the `ID3D12PipelineLibrary` blob, so a warm start creates the library, not
  the PSOs.
* **A PSO needs the input layout at creation.** bbhost computes vertex input from the GX fetch
  shader per draw; the port keys PSOs on the resulting `D3D12_INPUT_LAYOUT_DESC` hash, so two draws
  with the same shaders but different layouts get different PSOs. This is why the precompiler also
  records the layouts the manifest's pipelines used.

Tessellation and geometry stages map directly: GCN's LS/HS/DS become D3D12's HS/DS patches with
`SV_TessFactor`/`SV_InsideTessFactor`; bbhost's `GsLift` becomes a D3D12 GS pipeline with the copy
shader as the GS's output. The port relies on bbhost having lifted all of those
(`docs/shader-pipeline.md`), because D3D12 has no fallbacks for them either.

## 5. Swapchain and presentation

```cpp
DXGI_SWAP_CHAIN_DESC1 d{};
d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
d.BufferCount = 3;
d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
d.Scaling = DXGI_SCALING_STRETCH;
d.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
d.SampleDesc = {1, 0};
factory->CreateSwapChainForCoreWindow(queue, core_window, &d, nullptr, &swapchain);
```

* `SetMaximumFrameLatency(2)` and pacing on `GetFrameLatencyWaitableObject()`, which is the UWP
  equivalent of a present fence and the only reliable 60 Hz pacer on the console.
* Tearing (`DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING` + `DXGI_PRESENT_ALLOW_TEARING`) is set **only**
  when `CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING)` succeeds *and* the user's vsync
  setting is off. Passing the flag otherwise is an `DXGI_ERROR_INVALID_CALL` crash — a lesson from
  every UWP port that ever shipped.
* `ApplicationViewBoundsMode::UseCoreWindow` (no TV overscan crop) and
  `RequiresPointerMode::WhenRequested` (no phantom mouse cursor) are set at launch.
* A present is skipped only when the swapchain returns `DXGI_STATUS_OCCLUDED`; on suspend the
  backend drains the queue and calls `IDXGIDevice3::Trim()`.

## 6. Barriers

The stream's `kBarrier`, `kTransferBarrier`, `kCopyOrderBarrier` and `kPassBarrier` records are
translated by `barriers.cpp` into either:

* **Enhanced barriers** (`D3D12_BARRIER_GROUP`, `SYNC_*`/`ACCESS_*`) when the device supports them
  — the mapping from Vulkan's stage/access masks is a table, not a guess; or
* **Legacy `ResourceBarrier`** transitions derived from the same table when they are not.

`tests/barrier_test.cpp` pins the table: every (Vulkan stage mask, access mask) pair the recorder
can emit maps to a specific D3D12 state set, and the test fails on an unmapped pair rather than
letting the replay silently drop a barrier. Dropping a barrier is the classic way a port produces
flickering geometry that looks like a shader bug for weeks.

## 7. Texture untiling

The game's textures are GCN-tiled: 8×8 micro tiles, thin/thick, pipe-interleaved with 256-byte
blocks, bank-swizzled by macro-tile mode. The port detiles on the GPU:

1. `untile.cpp` plans a job per mip (tile mode, dimensions, format, source offset, destination
   offset).
2. A compute shader (`shaders/untile.hlsl`, one thread per 8×8 micro tile) reads the tiled source
   from the upload buffer and writes linear texels into a staging texture.
3. `CopyTextureRegion` moves the staging texture into the real one on the copy queue, and the
   compute fence is waited on by the direct queue once.

Tiling modes the game uses and the shader handles: `ARRAY_2D_TILED_THIN1`, `ARRAY_2D_TILED_THICK`,
`ARRAY_1D_TILED_THIN1`, `ARRAY_2D_TILED_THIN1` with `PIPE_ALIGNED`, and linear (copied straight).
`tests/untile_test.cpp` checks the address math against a CPU reference for every mode and a set of
sizes, including non-multiple-of-tile dimensions, because an off-by-one here produces a subtly
skewed texture that reads as "the port has a memory bug".

## 8. Fences, labels and `bb-labels`

* Every submission signals its direct-queue timeline with a value that is also recorded in the
  stream as the submission's serial.
* `labels.cpp` keeps the per-submission label list (guest VA, 64-bit value, optional OR-mask).
  When the fence passes, `bb-labels` writes the values into guest memory in stream order, exactly as
  bbhost's Vulkan path does.
* Where a label must be visible to the GPU itself (a GPU particle counter a later dispatch reads),
  the replay emits `WriteBufferImmediate` on the command list instead, and the CPU landing is
  skipped for that label. The label record's `kMarker`/`kFill` shape decides which.
* `bb-submit` merges queued submissions when the queue falls behind, as upstream does; the port
  keeps that because command-list recording is cheap and submission is not.

## 9. Debugging

* `BBX_D3D12_VALIDATION=1` enables the D3D12 debug layer (dev builds only; it costs memory).
* `BBX_D3D12_MARKERS=1` emits PIX BeginEvent/EndEvent around each pass and submission, named by the
  stream's marker windows, so a GPU capture lines up with `bb-labels`.
* `BBX_D3D12_STATS=1` prints per-second heap use, PSO count, descriptor ring occupancy and
  submission latency — the console's substitute for a profiler.
* On any device-removed (`DXGI_ERROR_DEVICE_REMOVED`) the backend writes
  `LocalState\d3d12-device-removed.txt` with the DRED breadcrumbs (`ID3D12DeviceRemovedExtendedData`)
  before the last-chance logger fires.

## The console's device, in one paragraph

On Xbox the device is feature level 11_0 and shader model **6.4**: descriptor tables and `spaceN`
arrays, never shader model 6.6 `ResourceDescriptorHeap` indexing; no mesh shaders, no barycentrics, no
native 16-bit arithmetic (`*_6_4` targets, 16-bit math lowered); 32 KiB of group shared memory; no
`ID3D12PipelineLibrary` (it removes the device) so the disk cache holds signed DXIL blobs and live
PSOs come from an LRU (`bbx_psocache`); and no working hardware tessellator, so hull/domain draws run
as compute passes or are skipped and counted. The full rule set, with sources, is
`docs/ARCHITECTURE_XBOX_D3D12.md`.
