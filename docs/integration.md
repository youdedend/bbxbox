# How the port sits on bbhost

bbxbox is not a fork of bbhost. It is a tree that is applied over a pinned bbhost checkout, and it
keeps the number of upstream edits as small as the platform allows. This document is the exact list
of touch points: what is added, what is excluded from the build, and what is edited — with the
reason for each.

## 1. The shape

```
<bbhost checkout>            pinned at UPSTREAM.md's commit
├── src/…                    upstream, edited only where §3 says
├── CMakeLists.txt           upstream, one option block added (§3.1)
└── bbxbox/                  this repository, as a subdirectory or a sibling
    ├── src/host/d3d12/      the backend (added to the target)
    ├── src/host/uwp/        host shims
    ├── xbox/                the UWP app target
    └── cmake/               the port's CMake modules

<build>/layout/              the .appx layout
```

`tools/apply_overlay.sh` (Linux/macOS) and `xbox/scripts/apply-overlay.ps1` (Windows) prepare a tree:
they verify the bbhost commit, copy nothing over upstream files, and write
`cmake/bbxbox-overlay.cmake` which the top-level build includes.

## 2. Files added (no upstream edit)

| Path | Purpose |
|---|---|
| `src/host/d3d12/device.*` | device, queues, heaps, fences, feature queries |
| `src/host/d3d12/swapchain.*` | `CoreWindow` flip model, pacing, tearing rules |
| `src/host/d3d12/descriptors.*` | bindless heaps, per-frame rings, retirement |
| `src/host/d3d12/rootsig.*` | root signature, and the heap-direct variant |
| `src/host/d3d12/pipeline.*` | PSO creation, input layouts, PSO library |
| `src/host/d3d12/replay.*` | `gpu::stream` records → D3D12 command lists |
| `src/host/d3d12/barriers.*` | stage/access tables, enhanced + legacy barriers |
| `src/host/d3d12/untile.*` | detile job planning and dispatch |
| `src/host/d3d12/dxil.*` | UWP-safe `dxcompiler.dll`/`dxil.dll` loading, compile, validate |
| `src/host/d3d12/cache.*` | cache file format, stamp validation, PSO keys |
| `src/host/d3d12/precompile.*` | the precompiler, inflight breadcrumb, quarantine |
| `src/host/d3d12/budget.*` | the 5,120 MiB plan, residency tiers, heap caps |
| `src/host/uwp/storage.*` | `LocalState`/`TemporaryFolder`, `*FromApp` file calls, overlay index |
| `src/host/uwp/thread.*` | thread creation with the console's stack policy |
| `src/gcn/hlsl_dxil.cpp` | the DXIL target for the HLSL emitter (§3.3) |
| `xbox/**` | the UWP app, launcher, packaging, scripts, tests |

## 3. Upstream edits

Each edit is small, guarded by `BBX_UWP`, and does not change desktop behaviour.

### 3.1 `CMakeLists.txt`

```cmake
option(BBX_UWP "build the Xbox UWP backend instead of the Vulkan one" OFF)
if(BBX_UWP)
  include(cmake/bbxbox-overlay.cmake)   # backend sources, excludes, link line
endif()
```

The overlay file drops `src/host/gpu.cpp`, `gxvk_*.cpp`, `window.cpp`, `upscaler_ffx.cpp`,
`overlay.cpp`, `launcher*.cpp`'s SDL parts, `present_pass.cpp`'s Vulkan parts and the SDL3/FFmpeg
desktop links, and adds the D3D12 sources plus `WindowsApp.lib`, `d3d12.lib`, `xaudio2.lib`.

### 3.2 `src/host/gpu.cpp` / `render.cpp`: the backend boundary

bbhost's draw path calls `vkCmd*` directly. The port introduces `src/host/backend.h`, an interface
with the operations the recorder and renderer need:

```cpp
struct Backend {
  virtual Pipeline* create_graphics_pipeline(const GfxPipeline&, const DxilPair&) = 0;
  virtual void bind_pipeline(Cmd&, Pipeline*) = 0;
  virtual void bind_descriptors(Cmd&, const DescriptorRef*, uint32_t) = 0;
  virtual void draw(Cmd&, const DrawArgs&) = 0;      // indexed/instanced/indirect
  virtual void dispatch(Cmd&, const DispatchArgs&) = 0;
  virtual void barrier(Cmd&, Span<const Barrier>) = 0;
  virtual void begin_rendering(Cmd&, const RenderingInfo&) = 0;
  virtual void copy(Cmd&, const CopyInfo&) = 0;
  virtual void clear(Cmd&, const ClearInfo&) = 0;
};
```

`src/host/gpu.cpp` becomes one implementation of it (`VulkanBackend`), `src/host/d3d12/` the other.
The `gpu::stream` records are translated by each backend's replay; the recorder is untouched. This
is a mechanical refactor: the functions already exist, they just gain a virtual call.

### 3.3 `src/gcn/hlsl.cpp`: the DXIL target

Add `HlslTarget { kVulkan, kDxil }` to `HlslOptions` and the spellings table in
`docs/shader-pipeline.md` §3. The lifter and the module's structure are untouched. The port's
`hlsl_interface_diff`/`hlsl_operation_diff` harness keeps running against the new target.

### 3.4 `src/host/dxc.cpp`: UWP loading

`Dxc::load` gains a `BBX_UWP` branch: `LoadPackagedLibrary` from the package folder, and validation
through `IDxcValidator` (the desktop path deliberately does not sign DXIL; the console requires it).

### 3.5 `src/host/translation_cache.cpp`: two new uses

`TranslationUse::kDxil` (the DXIL blob, keyed on the lifted module + DXC version) and
`kPsoDesc` (the PSO description, keyed on the DXIL bytecode hash + state). Both are entries in the
existing file format, so the cache's replacement policy and its fingerprint handling are reused
rather than reimplemented.

### 3.6 `src/host/main.cpp`: platform startup

The port owns startup: no SDL window, no setup window, no command line. `main.cpp` gains an
`#ifdef BBX_UWP` entry that calls `bbxbox::platform::run()` (the UWP shell), which calls back into
bbhost's `host_main(argc, argv)` once the launcher has resolved the dump. The guest loader, HLE and
configuration paths are unchanged.

### 3.7 `src/host/config.cpp` / paths

`BB_USER_DIR`, `BB_CONFIG` and the data folder become `LocalState` subdirectories on UWP; the
port sets them before bbhost reads them, so this is configuration, not an edit.

### 3.8 `src/host/window.cpp`, `launcher*.cpp`

bbhost keeps its SDL3 calls, and on the console they are served by
[SDL3-uwp](https://github.com/XboxEmulationHub/SDL3-uwp) (`uwp-3.4.20`): a full SDL 3.4.20 whose
WinRT backends provide the CoreWindow video device, WinRT WASAPI audio, `Windows.Gaming.Input`
gamepads and the WinRT pickers. It is built **once** by `xbox/scripts/build-deps.ps1` into
`build-deps/prefix/sdl3` (MSVC, because its WinRT code is C++/CX) and cached; the app itself is
clang-cl and meets it at SDL's C ABI. `src/host/window.cpp`'s Vulkan window is still excluded — the
console window is `SDL_CreateWindow` over the view's CoreWindow, and the swapchain is created from
that CoreWindow by `src/host/d3d12/swapchain.cpp`.

The Xbox-specific helpers SDL does not expose — the TV output size and refresh rate from
`HdmiDisplayInformation` (the CoreWindow bounds are not the output resolution on Series S) and
`LoadPackagedLibrary` for packaged DLLs such as `dxcompiler.dll` — come from
[libuwp](https://github.com/XboxEmulationHub/libuwp) (`src/libuwp.cpp`), also built by
`build-deps.ps1`. Its file-copying `uwp_pick_rom` is deliberately **not** used: the dump is never
copied, it is granted once through the `FutureAccessList` and read in place (`docs/console-ux.md`).

The guest arena replaces bbhost's `src/core/memory.h`/`memory.cpp` with
`xbox/platform/guest_memory_bbhost.h` + `guest_memory.inc`: `VirtualAlloc2FromApp` below the 1 TiB
line, strict W^X (a write maps RW and the execute grant is re-taken with `VirtualProtectFromApp` +
`FlushInstructionCache`).

## 4. What is deliberately reused unchanged

* `src/core/*` — ELF loader, imports, thunks, memory, write watch, clocks.
* `src/hle/*` — the 660 import implementations.
* `src/gcn/{decode,container,translate,lift,...}` — the translator and lifter.
* `src/host/recorder.cpp`, `stream_ops.h` — the stream format and the recorder's threading.
* `src/host/render_hooks.cpp`, `gpu.h` — the GX draw-record model.
* `src/host/translation_cache.cpp` — the cache (extended with uses).
* `src/host/shader_bundles.cpp` — bundle parsing for the precompiler.
* `src/host/buffer_shadow.cpp`, `src/core/write_watch.cpp` — now on the only path instead of an
  alternative one.
* `src/host/frame_rate.cpp`, `frame_tick.cpp` — 30/60/90/120 pacing.

## 5. Keeping the edits reviewable

Every edit is a **single-purpose commit** with a `[bbxbox]` prefix and, where it touches shared
code, a rationale in the commit message naming the affected platform:

```
[bbxbox] hlsl: add a DXIL target to the emitter          (desktop unchanged)
[bbxbox] dxc: load from the package and validate DXIL    (BBX_UWP only)
[bbxbox] gpu: route draw submission through Backend       (mechanical, no behaviour change)
```

The goal is that upstream can take the backend boundary and the DXIL target on their own merits; the
Xbox-specific memory and launcher code stays in `bbxbox/`.

## 6. Verifying an overlay

`tools/verify_overlay.sh <bbhost-tree>` checks:

1. the tree's `HEAD` is the pinned commit;
2. every file §3 names exists and contains the expected anchor (a symbol or comment), so a silent
   upstream move fails loudly;
3. no file listed as excluded in `bbxbox-overlay.cmake` is still in the target's source list;
4. the port's headers compile standalone (`tools/syntax_check.sh`).
