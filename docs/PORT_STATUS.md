# Port status

Honest state of the port, milestone by milestone, with what is verified where. The exit criteria are
`docs/porting-plan.md` §6; this file says whether they have been met, and what has actually run on a
console versus only on the build machine.

Last updated with the Xbox hardware rules folded in (`docs/ARCHITECTURE_XBOX_D3D12.md`).

## Milestones

| Phase | Work | State |
|---|---|---|
| P0 | UWP shell, launcher, memory contract, packaging, CI, import audit | **Implemented; not yet run on console.** `.appx` layout, manifest (`codeGeneration` + `removableStorage` only), SDL3-uwp/libuwp cached dependency build, memory self-test, `check-imports.ps1`, `local_precheck.sh`, CI. |
| P1 | Guest boot + HLE + audio/input, no rendering | **Linked; not booted.** The portable core (`elf`, `imports`, `thunk`, `tls_rewrite`, `sha256`, `paced_file`, `memory`, `bbx_self`) is compiled by the host test build and unit-tested (191 cases). The UWP arena adapter (`guest_memory_bbhost.h`) is what bbhost's `core/memory.h` resolves to under `BBX_UWP`. **The boot input is solved:** `src/host/core/bbx_self.{h,cpp}` decodes the fSELF wrapper (`4f 15 3d 1d`) into a flat ELF and is shared by `load_orbis_elf` and `gpu::eboot_shaders`, so a real `eboot.bin` from a retail disc dump is read instead of the encrypted bytes. Run against the user's own 93,825,704-byte `eboot.bin` it produces a 93,667,640-byte ELF whose `e_entry` lands on the x86-64 prologue in `ph0`, whose `PT_DYNAMIC` carries the Orbis tags, and that still holds all 5,493 `OrbShdr` containers the bake step needs; plain ELF input passes through untouched, and a retail-encrypted SELF is refused with a diagnostic instead of a garbage image. Segments are placed from the wrapper's own entry table (the file's offsets are not the image's), and `PT_LOAD`/`PT_DYNAMIC`/`PT_SCE_DYNLIB_DATA` that cannot be placed is a hard error, not a hole. **The guest's file system is routed off the CRT.** `hle/fs.cpp`, `hle/fios.cpp` and `hle/save_commit.cpp` no longer call `_open`/`_read`/`_stat64`/`FindFirstFileA`/`std::filesystem`/`CreateFileW`: they call `xbox/platform/host_fs.h`, one table of operations with two implementations — the CRT and POSIX for a desktop build (`upstream/bbhost/src/core/host_fs_desktop.cpp`, which the host tests exercise) and `CreateFile2FromApp`/`GetFileAttributesExFromApp`/`FindFirstFileExFromApp`/`MoveFileFromApp`/`RemoveDirectoryFromApp`/`ReplaceFileFromApp` in the AppContainer (`xbox/platform/host_fs_uwp.cpp`). The path policy — `/app0`, the update folder ahead of the base dump, the mods and plugin overlays, `/data`, mount points, the case-insensitive fallback — is unchanged and now runs unchanged on both; the save commit's moves, flushes and recoveries go through the same seam, and its protocol has a test of its own. The app runs the seam's self test once at boot and logs one line, so an AppContainer that refuses a call says so at launch instead of at the game's first save. **The console's memory layer is the AppContainer's.** Direct memory is a
`CreateFileMappingFromApp(..., PAGE_READWRITE | SEC_RESERVE, ...)` section, not a
6 GiB RWX one: RWX is refused outright by the container, and without
`SEC_RESERVE` the whole size is charged at creation and Game Mode's 5,120 MiB
ceiling answers `ERROR_COMMITMENT_LIMIT`. The guest's two fixed windows
(4 GiB direct memory, `0xff00000000` GPU alias) are reserved at the first
statement of `main.cpp` (`xbox/platform/guest_windows.cpp`, through the same
`win_vm` layer), because a UWP title cannot relaunch itself when ASLR wins the
race the way a desktop build does. `core/win_vm.cpp` binds the container's
`VirtualAlloc2FromApp` and asks kernelbase for the view entry points by name —
the SDK's app-partition import library otherwise pulls in `MapViewOfFile2`,
which the audit refuses. The page protections in `write_watch.cpp`,
`win_watch.cpp` and `kernel.cpp` are `VirtualProtectFromApp`, and
`core/host_clock_shim.h` supplies `clock_gettime` over the Win32 clocks (the
Store SDK has no `<pthread_time.h>`). **The guest runtime compiles for the console — all 23 files of `src/hle` — and is linked into the app.** The
`bbhost_guest` library is forty-four translation units: all twenty of `src/core` that
a console build uses (every one except the Linux VM layer and the desktop host-fs
backend), the winpthreads shim, and all twenty-three in `src/hle` —
the file system, `fios`, save commits, the kernel, pthreads, the
synchronisation primitives, the variadic shims (asm and all), libc, gnm's
resource tracking, the audio stack (`audio`, `ajm`, `avplayer`), `dialog`, the
network modules (`net`, `http`, `np_matching2`, `np_signaling`), `runtime`, the
guest console, and the last four (`video`, `system`, `gnm_exec`, `gx_trace`) —
compiled against the Store SDK and the AppContainer on every Windows run, and
linked into `bbxbox` with `bbhost_gcn`/`bbhost_host`/`bbhost_engine`
(§"What the packaged app contains"). **Compiled and linked is not booted:** no
guest ELF has been loaded at run time and no HLE entry point has executed, so
this row is "the runtime is in the app", not "the game starts". Getting there needed three things upstream's MinGW build had for
free:

  * `xbox/winpthreads_shim/` — `<pthread.h>`, `<sched.h>` and `<sys/time.h>`
    over `SRWLOCK`, `CONDITION_VARIABLE`, `CreateThread` with an explicit stack
    *reservation*, and the Win32 clocks. A mutex is one pointer, allocated on
    first use, so a zero-initialised `pthread_mutex_t` is valid without an
    initialiser macro; `pthread_self()` makes a record for threads the shim did
    not create, so `main`, SDL's threads and the D3D12 worker are not special
    cases;
  * `thunk.cpp`'s module-level asm and `SYSV_VA_ENTRY` open with
    `.att_syntax prefix`, because clang-cl hands a module-level `asm("...")` to
    the assembler in Intel syntax — which is what `unknown token in expression`
    at `push %rbp` was. The GCC-style extended asm inside functions was always
    AT&T. `/arch:AVX2` for the vector save area, as the console's Zen 2 is;
  * strict W^X: `host_page_alloc(size, true)` comes back *writable* on the
    console (`VirtualAllocFromApp` refuses `PAGE_EXECUTE_READWRITE` even with
    the `codeGeneration` capability), and `host_page_make_writable` /
    `host_page_make_executable` are the pair every writer of generated code
    uses — the thunk stub page flips its pages, stores the bytes, flips to
    `PAGE_EXECUTE_READ` and flushes the instruction cache.

**All of `src/hle` compiles for the console: 23 of 23.** The last four
(`video`, `system`, `gnm_exec`, `gx_trace`) needed three small local changes and
no host-header surgery: `host/ingame_menu.h` now declares opaque handles under
`BBX_UWP` (it was the only one of the six "Vulkan headers" that carried a single
`#include <vulkan/vulkan.h>`; the `gxvk*`/`cb_capture`/`list_resolve` headers
only ever had the name), `hle/gnm_exec.cpp` needed `<array>`, and
`hle/system.cpp`'s save-directory `::stat`/`S_ISDIR`/`fopen` became the host-fs
seam — the save folder on a granted USB stick is not the CRT's to open.
`host/window.h`'s `host_pad_rumble(std::uint8_t small, ...)` lost a parameter
name to `<rpcndr.h>`'s `#define small char`. Forty-four translation units total:
`20/20` console `src/core`, `23/23` `src/hle`, and the winpthreads shim.

**The `<dump>\mods` overlay is a launcher switch.** The Arabic localisation for
CUSA03173 v1.09 is loose files under `msg/`, `font/` and `menu/` plus a patched
`eboot.bin`. `LauncherModel` now carries `mods_present` (asked of the dump root
through the dump's own file seam), `mods_enabled` (`[dump] mods`, on by default
when the folder exists) and `modded_eboot_ok` (`[dump] modded_eboot`, off by
default, offered *only* with the overlay on, and marked dangerous because it
loads a v1.09 eboot whose SHA-256 is not the pinned one — `BBHOST_ANY_EBOOT`'s
meaning, and the digest is logged either way). Four new tests cover the four
states. 195 host tests green.
**The host layer and the guest's own code compile too, as two more libraries.**
`bbhost_host` is thirty-one translation units of `src/host` (upstream's own
`host/gpu_stub.cpp` covers the entire `host_gpu_*` surface with no Vulkan at
build time; then the audio device and its ATRAC9 path, the bindings, the frame
stats, the list plumbing, `gxvk_*`'s bookkeeping, the shader override and
bundle tables) plus `replay/json.cpp`. `bbhost_engine` is fifty-eight: the whole
of `src/decomp`, all of `src/engine` except the six that reach the frame
generator, the two upscalers or the ImGui menu, and `src/net`. That is **133
translation units compiled for the console**, and the last two libraries also
needed the `.att_syntax prefix` fix (the engine's `frame_ret` and friends are
module-level asm too).

The three libraries are **on `bbxbox`'s link line**. `xbox/CMakeLists.txt`
links `bbhost_gcn bbhost_guest bbhost_host bbhost_engine` (plus `bbx_atrac9` and
`bbx_monocypher`) whenever `BBX_GUEST_LINK` is on — which is the default, and the
configuration CI builds, because neither `xbox/scripts/build.ps1` nor the
workflow passes `-DBBX_GUEST_LINK` — and defines `BBX_GUEST_BOOT`, so
`platform/bbhost_bridge.cpp` compiles its real `StartGuest` instead of the arm
that logs `BBX_GUEST_LINK=OFF` and returns. "What the packaged app contains"
below is the three facts that say the packaged binary carries the guest, and the
check that keeps them true. Linked is not booted: no guest ELF has been loaded
at run time and nothing has run on a console.

The census machinery that got the link there is still worth keeping, because the
same channel serves whatever gap comes next: the app links with `/ERRORLIMIT:0`,
and `xbox/scripts/build.ps1` turns a link failure into one deduplicated
`undefined symbols` annotation (each symbol with the object that wanted it, up
to 50 KB) instead of the first eighty lines. The sandbox cannot read job logs —
`results-receiver.actions.githubusercontent.com` is not reachable and the API's
log endpoint is 404 — so the annotation *is* the channel. It is also, in
hindsight, the same evidence from the other side: a symbol that is not on the
link line can neither collide nor go missing, so the two diagnostic runs before
the fix (`duplicate symbol host_gpu_*`, `undefined symbols
PipelineSet::CreateGuestRootSignature / host_render_worker_jobs`) could only
speak about objects that were linked.

`src/hle` is **not** linked into `bbxbox` yet: `runtime.cpp` pulls `thunk_wrap`, the guest-thread entry points, the fs HLE and the audio stack, so the HLE/VM hook routing (`win_vm.cpp`, `hle/fs.cpp`, `hle/fios.cpp`, `hle/audio.cpp`) is the next milestone step. Nothing has booted a game. |
| P2 | D3D12 device, swapchain, replay of clears/copies/barriers/markers | **Written and compile-verified; not yet run on a console.** `bbx_d3d12.cpp`, `device.cpp` (11_0 device, WARP refused, caps validated, one queue + fence + three frame allocators, bounded heaps, device-loss capture), `swapchain.cpp` (CoreWindow flip-discard ×3, plan-sized, inverse-scaled, RTVs, resize, present), `pipeline.cpp` (one root signature per pass: 32-bit constants in `b0`, Tier-3 typed tables in space 0, `t0`/`u0` for the compute passes; PSOs created on first use from the build-time DXIL and accounted through `bbx_psocache`; **no `CreatePipelineLibrary`**) and `replay.cpp` (WalkStream before emitting, clears, copies, barriers, markers as PIX events, the untile dispatch from `bbx_untile`'s plan, present + overlay passes, and the boot GPU self-test through a READBACK heap) are compiled by the Windows job against the real SDK and type-checked locally by `tools/syntax_check.sh`. `xbox/platform/hud.cpp` lays the boot report out as solid quads (a 3x5 font, so no atlas is needed for P2) and `xbox/platform/d3d12_session.cpp` owns device + swapchain + pipelines + replay behind an `IUnknown*`, which is what `main.cpp` calls — the app therefore **links and packages** with the
whole P2 path in it (run `43734984657`). Six shader entry points are embedded as SM 6.4 DXIL; no compiler ships. **What P2 does not do:** image copies (`kCopyImage`, `kCopyBufferToImage`, `kCopyImageToBuffer`) are counted and deferred to P3's resource table, per-draw state from the stream is not replayed, and the launcher's own UI is still SDL's — the GPU path currently shows the boot report and the self-test row. No console has run any of it. |
| P3 | GX draw path: pipelines, bindless descriptors, vertex fetch, buffer shadow | Design + `bbx_psocache`/`bbx_caps` landed. |
| P4 | YEBIS, Scaleform, compute tessellation, FSR | Policy landed (`bbx_tess`), kernels pending. |
| P5 | DXIL precompile + PSO cache + quarantine + USB sync | Cache and quarantine landed and tested; DXIL compile and USB sync pending. |
| P6 | 60 FPS pass | Pending. |

**The guest's GPU is the port's device, not the stub (Slice 5B-1).** `host/gpu.h` is the
surface `hle/gnm_exec.cpp` and `hle/video.cpp` call; upstream answers it from
`host/gpu_stub.cpp` when Vulkan is not at build time, which means a boot never
waits on a fence and never draws. The console now answers it from
`src/host/d3d12/host_gpu_d3d12.cpp`, which is a seam over the port's own
`D3d12Session` rather than a second device: the session installs a
`GuestGpuBinding` (`src/host/d3d12/host_gpu_d3d12.h`) at `Create` and clears it
before it destroys the device. What that makes real now:

* **the timeline fence** — `host_gpu_submit`/`host_gpu_flush` signal the device's
  fence and `host_gpu_submissions_completed`/`host_gpu_work_needs`/
  `host_gpu_wait_completed` read it, so an EOP or label wait is a wait on work
  that was actually submitted, and `g_flip_after_gpu` tracks submissions;
* **the guest's memory** — `bbx_guest_mem` (`GuestPlan`, `GuestMemory`) builds the
  two-level page table and the 256 MiB slabs `space3 t0`/`u0..u31` name, slab `0`
  being the 64 KiB guard page every unmapped table entry points at; slabs are
  resident only where the plan puts a window, so the console pays for what the
  guest maps and not for 32 * 256 MiB out of Game Mode's 5,120 MiB. Every
  unmapped L1/L2 entry points at slab 0 offset 0, so a stray lane reads the guard
  page instead of faulting (or of corrupting a live one);
* **the guest root signature** — `pipeline.cpp` creates a console 1.0 signature of
  4 DWORDs (`bbx_guest_root.h`: the `StageParams` CBV at `b0, space1`, the
  space-1 SRV/UAV/sampler table the emitted shaders declare, and the space-3
  table for the page table and the 32 slabs), with no static samplers and no
  `ID3D12PipelineLibrary`.

**And the compute pipeline on top of it (Slice 5B-2a).** `platform/guest_compute.cpp` now
runs a dispatch rather than refusing it:

* the program at `COMPUTE_PGM_LO/HI` is read out of the guest's own memory (the
  `OrbShdr` footer gives its length, a program without one is decoded to its
  `s_endpgm`) and patched exactly as the bake patches it;
* its key is built the way the bake builds it — the same words, the same
  `gcn::translate`, the same `predict_image_dims`, the same `ConsoleStageKeyOf`
  — so a look-up hits the blob the bake wrote instead of a shader this build
  invented;
* the blob is inflated from `<dump>/bbx_shaders.cache` (validated once against
  its stamp: `dxil-6.4`, `rs-1.0`, `tables`, `11_0`), and the compute PSO is
  created on first use against the guest compute root signature, with `PsoCache`
  owning the budget and the eviction;
* the dispatch is recorded with one shader-visible CBV/SRV/UAV heap and one
  sampler heap, the 512-byte `StageParams` block as the root CBV at `b0, space1`
  (its `user_sgpr` from the dispatch's own user data — with `kCbSsbo = false`,
  `kBindless = false` and the space-3 lowering, that is the whole contract a
  console compute shader has, not a placeholder), the space-1 tables, the space-3
  page table and slabs, then `Dispatch` or `ExecuteIndirect` (the arguments are
  the guest's own three dwords, in the slab the page table puts them in — no
  copy) and a UAV barrier.

The four D3D12 rules that decide the signatures are stated in
`docs/ARCHITECTURE_XBOX_D3D12.md` §3.4 and enforced by `bbx_guest_root.h`: a
sampler never shares a table with a CBV/SRV/UAV range, `b1..b63`/`t0..t63`/
`u0..u63`/`s0..s63` are covered, vertex and pixel get their own tables (compute is
5 DWORDs, graphics 9, ceiling 64), and the guest arena is an index space over the
device's *one* shader-visible heap with a `FLAG_NONE` source heap, because
`CopyDescriptorsSimple` refuses a shader-visible source.

What is still counted and refused, rather than guessed (Slice 5B-2b): resolving a
space-1 descriptor from the dispatch's user data (which V#/T# a baked binding
names is the translator's `ResourcePath`, and binding the wrong buffer is worse
than binding none — the slots are null descriptors today), the whole graphics
half (`host_gpu_draw`, render-target views, the blits), and sampled resources.
Every dispatch that cannot run says so once by key, with the reason in the log
(`host_gpu_d3d12.cpp`'s `GuestDispatchReport()` and `guest_compute.Report()`).

## Verified on hardware (inherited, and what it changes here)

| Hardware fact | Source | Enforced in this tree |
|---|---|---|
| FL 11_0, SM 6.4 max, binding tier 3, 64-lane waves | KytyPS5 `docs/d3d12-backend.md` | `src/host/d3d12/bbx_caps.h`, `tests/caps_test.cpp` |
| No native 16-bit ALU; no heap indexing; no mesh shaders; no barycentrics | same | `FlagsFor()`, `ShaderTarget()`; `-enable-16bit-types` never emitted |
| `CreatePipelineLibrary` removes the device | `XBOX_CHANGES.md` | no pipeline library anywhere; `Caps::pipeline_library=false`, asserted in `Validate()` |
| Thousands of live PSOs exhaust 5,120 MiB | same | `bbx_psocache` (LRU + object cap + byte budget), `tests/psocache_test.cpp` |
| Constant-alpha blend factors fail the PSO (G4) | `d3d12-backend.md` guards | blend builder refuses, logs once |
| 32 KiB group shared limit (G15) | same | `GroupSharedFits()`, tessellation skip reason |
| Depth sampled + written needs a read-only DSV (G12) | same | replay/pipeline rule |
| Tessellator emits zero domain points | `988ceea` | `bbx_tess`: compute path or explicit skip, counters in the exit report |
| CoreWindow is 1080p; TV mode needs `HdmiDisplayInformation` + inverse matrix; clamp 3840×2160 (G8) | KytyPS5 `src/uwp/src/app.cpp` | `xbox/platform/display.{h,cpp}`, `tests/display_test.cpp` |
| Empty `WaveActiveBallot` (helper lanes) hangs pixel loops at 6.4 | `XBOX_CHANGES.md` | documented rule in `ARCHITECTURE_XBOX_D3D12.md` §6; HLSL emitter obligation |
| UWP FFmpeg with D3D12 hardware decode builds with clang-cl | `Cosmo/ext-ffmpeg-core-xbox` | `build-deps.ps1` stage (cached) |

## Verified by CI (the console job actually compiles and packages)

The `console` job on `windows-2025` is green end to end: cached dependencies
(`build-deps.ps1`), the app (`build.ps1`, Ninja + clang-cl + WindowsStore + the
Store CRT), the import audit and `makeappx`. That means, for the first time, the
following are compile-verified rather than hoped for:

* every `xbox/platform` translation unit: `main.cpp` (C++/WinRT view, activation
  factory), `storage.cpp` (WinRT picker + `*FromApp` file APIs), `dump.cpp`,
  `ux.cpp`, `launcher.cpp`, `log.cpp` (last-chance logger), `display*.cpp`,
  `guest_memory.cpp` (the arena over `VirtualAlloc2FromApp`), `memory_selftest.cpp`,
  `hud.cpp` (the overlay's quads), `d3d12_session.cpp` (device + swapchain +
  pipelines + replay behind an `IUnknown*`), `display.cpp`/`display_winrt.cpp`
  (the TV mode query and the output plan);
* the D3D12 backend translation units in `src/host/d3d12/` (`device.cpp`,
  `swapchain.cpp`, `pipeline.cpp`, `replay.cpp`, `bbx_d3d12.cpp`) and the one
  platform TU that avoids C++/WinRT (`d3d12_session.cpp`);
* the D3D12 policy translation units in `src/host/d3d12/` (`bbx_caps`,
  `bbx_budget`, `bbx_psocache`, `bbx_tess`, `bbx_stream`, `bbx_quarantine`,
  `bbx_untile`, `bbx_hud`'s model is in `xbox/platform/hud.cpp`, …);
* the vendored guest core's GCN library, LibAtrac9, monocypher and zlib;
* the package: `bbxbox.exe` + `SDL3.dll` + `libuwp.dll` (and no shader compiler at
  all: the six DXIL blobs are compiled on the runner and embedded in the exe), the
  manifest (`EntryPoint="bbxbox.App"`, activated through `bbxbox.def`, with the
  `Microsoft.VCLibs.140.00` dependency the console requires), the six generated
  logos, a `.map` and a `.pdb`.

The `.appx` is signed on the runner with an ephemeral self-signed certificate whose
subject matches the manifest's publisher (`CN=bbxbox`), and `bbxbox.cer` plus the
`Microsoft.VCLibs.x64.14.00.appx` framework package are produced next to it, so the
artifact is the three files a console wants.

The import audit's own findings are recorded in the run's annotations: the
notices are `LoadLibraryExW` (C++/WinRT's activation fallback, never called by
the port), the desktop API imports and the `*_APP` CRT imports inside the shipped
third-party DLLs (SDL3-uwp, libuwp), which the port does not own and does not
allow in its own image, and the seven symbols listed in
`check-imports.ps1`'s `$KnownUncalledSymbols` with the owner named in each.

**The CRT is static (`/MT`), and that is a console lesson rather than a size
decision.** The first packaged build that reached a Series X was installed,
launched and left LocalState empty: the process died in the loader with
`0xC0000135` because `bbxbox.exe` imported `VCRUNTIME140_APP.dll` and
`MSVCP140_APP.dll` from the `Microsoft.VCLibs` framework package, and nothing of
the port's had run, so nothing of the port's could say so. With `/MT` the CRT is
in the image and the import table is the system's libraries plus `SDL3.dll` (and
`libuwp.dll`), both of which the package carries. `SDL3.dll` itself still needs
VCLibs - it is `/MD` + `/ZW` - so the dependency stays; but a console without
the framework now produces a *running* app that logs "SDL unavailable" instead
of an image the loader refuses.

The audit got stricter in the same commit, because it can now be believed:
`user32`, `gdi32`, `ole32`, `advapi32`, `xaudio2_9`, `vcruntime140(_app)` and
`msvcp140(_app)` are no longer allowed modules for `bbxbox.exe`, and a DLL-CRT
configure is refused outright. What the static CRT brings with it is its own
file layer, written against `CreateFileW` and friends; the port's own paths were
moved off it (`log.cpp` now writes `loader.log` through `hostfs`, i.e.
`CreateFile2FromApp`), and the one caller left is bbhost's `core/paced_file.cpp`
- see "known gaps" below.

### What the packaged app contains (and what keeps it so)

"Compiled" and "linked" and "booted" are three different claims, and only the
first two are evidenced. As of run `43856060763` (`7e2d757`), the app that CI
packages contains the guest runtime:

* **The cache said so.** That run's `build/xbox/CMakeCache.txt` had
  `BBX_GUEST_LINK:BOOL=ON` and `BBX_BBHOST:BOOL=ON` — the defaults of
  `xbox/CMakeLists.txt`, since nothing overrides them — so `bbxbox` was linked
  against the four archives and `BBX_GUEST_BOOT` was defined for the bridge.
* **The link said so, twice over.** A successful link is silent, but its
  failures are not: the two runs that failed while the bridge's first real
  dispatch landed reported `duplicate symbol` collisions among `host_gpu_*` and
  `undefined symbol` for `PipelineSet::CreateGuestRootSignature` and
  `host_render_worker_jobs`. Those are link-time statements about archive
  members, and they exist only if the archives are on the line.
* **The binary said so, in size.** `bbxbox.exe` is 5,218,304 B in that run
  (pdb 36,286,464 B, `.appx` 2,468,134 B), against 4,451,328–4,467,200 B in the
  four green runs from before the guest link landed: ~750 KB of guest, host
  layer and `hle`. The `.map`/`.pdb` from *that* run could not be downloaded here
  (the artifact blob URL answers `EOF`), so at that point the claim rested on the
  configure defaults, the build succeeding and the size.
* **The map says so now, per archive.** Run `43861803683` (`6797e7b`) produced
  `bbxbox.map` (4,510,484 B) and reported its own account of the link as an
  annotation: **11,948 symbols came out of `bbhost_guest`**, 2,225 out of
  `bbhost_host`, 4,951 out of `bbhost_engine`, with `load_orbis_elf` and
  `register_hle` among them and the loader's `eboot sha256` text in the exe. That
  is direct evidence, in the run, instead of an inference from a size delta — and
  it is the evidence a later artifact download cannot take away.

Two changes hold that in place, so the claim cannot quietly stop being true:

* `xbox/CMakeLists.txt` now passes `/MAP`, so the `bbxbox.map` that
  `xbox/scripts/package.ps1` already copied beside the `.appx` and the workflow
  already uploaded is actually generated. Before this, every green run warned
  `no bbxbox.map under build: a crash report from the console cannot be
  symbolised` — a real gap in first-console debugging, not a cosmetic one, and
  the warning is gone from the run now.
* The workflow's `guest link evidence` step fails the run unless the cache has
  `BBX_GUEST_LINK:BOOL=ON`, the Ninja link line names all four archives,
  `bbxbox.exe` contains the guest loader's own `eboot sha256` literal, the map
  carries `load_orbis_elf` and `register_hle` (free functions, reached from the
  bridge's `StartGuest`, so decoration cannot hide them and `/OPT:REF` cannot
  drop them), and the map attributes at least twenty symbols to `bbhost_guest`
  and one each to `bbhost_host`/`bbhost_engine` — which is the part a flag and a
  link line cannot say, since an archive nothing references sits on the line and
  contributes nothing. That last distinction is not academic: the first version
  of this check also named `hle_gnm_execute`, and the linker had legitimately
  dead-stripped it, because nothing on the boot path reaches the GCN command
  processor until the draw path exists. It emits the sizes and the per-archive
  counts as a notice so the evidence survives in the annotation even though a
  later artifact download may not. This matters because a guestless build passes
  every other check in this workflow — it compiles, it packages, it imports
  cleanly, its self-test runs — and boots nothing.
* At configure time, `-DBBX_GUEST_LINK=OFF` is now a `FATAL_ERROR` unless it is
  accompanied by `-DBBX_GUESTLESS=ON`: dropping the link is a deliberate act for
  renderer iteration, not a thing to reach by forgetting a flag.

None of this is a boot. It is the statement that the shipped binary has the
guest's code in it; §"First physical-console run" lists what the first console
run has to show before "boots" may be written down.

## Verified here (build machine)

* **229 host tests** — `./tools/build_host.sh`: format, budget, cache, quarantine, descriptors, present,
  stream, untile, precompile, barriers, dump layout, ini, launcher, chords/keyboard, overlay, spill,
  guest-memory ledger (W^X), caps, PSO cache, tessellation policy, display plan, the shader manifest,
  and the guest core's own invariants (SHA-256 against the NIST vectors, the NID table, the TEB slot
  arithmetic, the `fs:[0]` → `gs:[disp]` rewrite over a synthetic segment including the stack-cookie
  read it must not touch, and `HostPacedFile` committing by rename), and the console's guest-memory
  map (`tests/guest_mem_test.cpp`: every window's pages walk to the slab and offset the shaders compute,
  the guard page catches the unmapped ones, the plan stays inside the slab table, and the root signature
  is 4 DWORDs with the registers the emitted shaders declare).
* **The fSELF unwrapper is checked against the real dump, not only fixtures.** Eleven cases cover plain ELF,
  stored and zlib segments, blocked zlib segments, a corrupt stream, a truncated wrapper, a wrapper whose
  segments are already in place, no-inflate builds and a retail-signed SELF; each synthetic fixture zeroes the
  bytes at image coordinates, so a reading that trusts `p_offset` on the *file* fails instead of quietly copying
  zeros. `tools/local_precheck.sh` also refuses an untracked dump (`eboot.bin`, `Bb.zip`, `*.dcx`,
  `bbx_shaders.cache`) in the work tree, so the input cannot drift into git by accident.
* **The DXIL for the port's passes is compiled here, not on the console.** `dxc.exe -T vs_6_4/ps_6_4
  -O3 -WX` (no `-enable-16bit-types`, no `ResourceDescriptorHeap`) runs on the build machine via
  `xbox/scripts/dxc-run.ps1`; the blobs are embedded in `bbx_shaders_generated.h`. The package carries
  no `dxcompiler.dll`/`dxil.dll` at all, and `check-imports.ps1` no longer allows them as modules.
* **`./tools/syntax_check.sh`** — the Windows-only translation units
  (`bbx_d3d12.cpp`, `device.cpp`, `swapchain.cpp`, `pipeline.cpp`, `replay.cpp`,
  `host_gpu_d3d12.cpp`, `bbx_guest_mem.cpp`, `bbx_guest_root.cpp` and
  `xbox/platform/d3d12_session.cpp`) are type-checked on Linux against declaration-only stubs of
  `<windows.h>`, `<d3d12.h>`, `<dxgi1_6.h>` and `<wrl/client.h>` in
  `tools/stubs/win_d3d12/`. This is a type-and-signature check, not an emulator: it
  caught `DXGI_CREATE_FACTORY_FLAGS_NONE` (which does not exist), a wrong
  `GetDesc`/`GetDesc1` pair, an `int` where `D3D12_RTV_DIMENSION` was required and
  a `D3D12_ROOT_SIGNATURE_VERSION_1` the SDK does not have (its name is
  `D3D_ROOT_SIGNATURE_VERSION_1`) before CI did - and the Windows job still compiles
  the same files with clang-cl. The stub is included with `-isystem` so its own
  unused constants are not silenced, which is how an unused constant and an unused
  helper in `replay.cpp` were caught locally instead of on CI.
* `./tools/local_precheck.sh`: tests, AppContainer API rules (`*FromApp`, no
  `PAGE_EXECUTE_READWRITE`), `/STACK:8388608,65536`, no game files/build output/absolute paths tracked,
  docs/code number agreement.

## Not verified (no console, no device)

* Anything requiring a real console: package install (the `.appx` is signed with an ephemeral
  `CN=bbxbox` certificate and the VCLibs framework package is staged beside it, but neither has been
  installed on a console), Game-mode memory limit, the memory self-test's numbers, 4K/HDR output,
  rumble, the virtual keyboard, and D3D12 device creation. One package *was* installed and launched
  and died in the loader (`0xC0000135`); that is what the static CRT answers. Whether it answers it
  is a fact for the next console run, not for this file.
* `src/host/d3d12`'s replay half: `replay.cpp` and `pipeline.cpp` now exist and compile against the
  real SDK, but no `ID3D12Device` has been created on a console, no root signature has been
  serialised by a real driver, no PSO has been created by one, nothing has presented, and the GPU
  self-test has never actually dispatched — so "the replay is correct" is a compile-time claim and
  nothing more. In particular, the untile dispatch and the read-only-DSV rule (G12) are not
  exercised by anything that runs here.
* **Guest execution.** `load_orbis_elf` and the HLE are *in* the packaged app (see "What the
  packaged app contains"), and `main.cpp` calls `bbx::bridge::StartGuest`, but no guest ELF has been
  loaded at run time and no HLE entry point has executed: `CUSA03173` has never been opened by this
  port, and the ELF fixtures the tests use are synthetic. "Linked" is the claim; "boots" is the one
  the first console run has to make.

## Known gaps

Recorded, not hoped away. Each one is a call that compiles, packages and is
refused (or wrong) only on the console, and each one has an owner in the tree.

* **bbhost's `core/paced_file.cpp` still writes through `std::fopen`.** The static
  CRT turns that into an import of `CreateFileW`, which an AppContainer accepts
  for LocalState and refuses on a granted folder — and a paced write is the
  guest's save path. Everything else in the port is on the `hostfs` seam
  (`CreateFile2FromApp` and friends), including `log.cpp`'s `loader.log`. This is
  the reason five symbols are listed in `check-imports.ps1`'s
  `$KnownUncalledSymbols` instead of failing the audit, and removing it is what
  lets them leave that list.
* **`src/host/d3d12`'s sampled-resource and graphics paths** are still the counted
  refusals described under "Not verified"; the compute dispatch is the only
  D3D12 path that runs.

## First physical-console run

The order is not negotiable, because each step's failure mode is different and
only the previous step's log tells you which one you are looking at.

1. **Install.** Sideload `bbxbox.cer` into *Trusted People*, then
   `Microsoft.VCLibs.x64.14.00.appx`, then `bbxbox.appx` (Device Portal →
   Apps, or Dev Mode). Pass: the tile appears and the app activates — that is
   `bbxbox.App` through `bbxbox.def`, the manifest, and the framework
   dependency, nothing of the port yet.
2. **Launch with no dump.** Pass: the view and the HUD draw, the launcher says
   there is no dump, and `bbxbox_logs\loader.log` exists on a granted path
   without a `crash.log`. This is where a D3D12 device, the swapchain and the
   overlay are first exercised for real — `syntax_check.sh` type-checked them,
   no driver has created them.
3. **Point it at the dump and press start.** This is the first time guest code
   can run. Pass, in this order in `loader.log`:
   `ImageBase`/segment mapping for the unwrapped image, `relocs: N relative, …`,
   the `eboot sha256 …` line **matching the pinned digest** (`BBHOST_ANY_EBOOT`
   off, so a modded eboot is refused, not silently accepted), the thread
   creation, and the HLE's first output for the descriptor tables. The
   guest's own `main` running *and* the first frame presenting is the point at
   which "boot verified" may be written down — not before, and not for a
   partial sequence.
4. **Capture on failure, before retrying.** `bbxbox_logs\crash.log` from that
   boot, the `bbxbox.map`/`bbxbox.pdb` **from the same build**
   (`xbox/scripts/symbolicate.py` turns `module+0xRVA` into names), and the
   run's artifact, so the report is reproducible; a retry with a different
   build throws the addresses away.
5. **Deferred, and known to be deferred:** audio (LibAtrac9 is linked but no
   ATRAC9 stream has been decoded on a console), 4K/HDR output, rumble, the
   virtual keyboard, the memory self-test's numbers under the Game-mode limit,
   and every D3D12 claim past device creation.

## Rules that must not regress

1. `/STACK:8388608,65536` — never 16 MiB (1.28 GiB across 82 threads).
2. Guest addresses < 1 TiB; strict W^X; `*FromApp` APIs only.
3. No `ID3D12PipelineLibrary`; no mass PSO creation at startup.
4. Shader targets `*_6_4` on console; no 16-bit types; descriptor tables, not heap indexing.
5. Tessellated draws: compute emulation or a counted, logged skip — never a hardware HS/DS draw.
6. Swapchain ≤ 3840×2160; TV-mode size via `HdmiDisplayInformation` + inverse matrix transform.
7. `precompile_inflight.txt` before every compile; quarantine on next boot.
8. Zero game files in git; `local_precheck.sh` before every push; CI 2–4 minutes incremental.
9. No shader compiler in the package: every fixed pass is DXIL compiled at build time, and the guest's
   shaders go SPIR-V → DXIL in memory (P3). `gcn/hlsl.cpp` output is never fed to a `-T *_6_4` profile.
