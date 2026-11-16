# Port status

Honest state of the port, milestone by milestone, with what is verified where. The exit criteria are
`docs/porting-plan.md` §6; this file says whether they have been met, and what has actually run on a
console versus only on the build machine.

Last updated with the Xbox hardware rules folded in (`docs/ARCHITECTURE_XBOX_D3D12.md`).

## Milestones

| Phase | Work | State |
|---|---|---|
| P0 | UWP shell, launcher, memory contract, packaging, CI, import audit | **Implemented; not yet run on console.** `.appx` layout, manifest (`codeGeneration` + `removableStorage` only), SDL3-uwp/libuwp cached dependency build, memory self-test, `check-imports.ps1`, `local_precheck.sh`, CI. |
| P1 | Guest boot + HLE + audio/input, no rendering | **Started.** The portable core (`elf`, `imports`, `thunk`, `tls_rewrite`, `sha256`, `paced_file`, `memory`, `bbx_self`) is compiled by the host test build and unit-tested (191 cases). The UWP arena adapter (`guest_memory_bbhost.h`) is what bbhost's `core/memory.h` resolves to under `BBX_UWP`. **The boot input is solved:** `src/host/core/bbx_self.{h,cpp}` decodes the fSELF wrapper (`4f 15 3d 1d`) into a flat ELF and is shared by `load_orbis_elf` and `gpu::eboot_shaders`, so a real `eboot.bin` from a retail disc dump is read instead of the encrypted bytes. Run against the user's own 93,825,704-byte `eboot.bin` it produces a 93,667,640-byte ELF whose `e_entry` lands on the x86-64 prologue in `ph0`, whose `PT_DYNAMIC` carries the Orbis tags, and that still holds all 5,493 `OrbShdr` containers the bake step needs; plain ELF input passes through untouched, and a retail-encrypted SELF is refused with a diagnostic instead of a garbage image. Segments are placed from the wrapper's own entry table (the file's offsets are not the image's), and `PT_LOAD`/`PT_DYNAMIC`/`PT_SCE_DYNLIB_DATA` that cannot be placed is a hard error, not a hole. **The guest's file system is routed off the CRT.** `hle/fs.cpp`, `hle/fios.cpp` and `hle/save_commit.cpp` no longer call `_open`/`_read`/`_stat64`/`FindFirstFileA`/`std::filesystem`/`CreateFileW`: they call `xbox/platform/host_fs.h`, one table of operations with two implementations — the CRT and POSIX for a desktop build (`upstream/bbhost/src/core/host_fs_desktop.cpp`, which the host tests exercise) and `CreateFile2FromApp`/`GetFileAttributesExFromApp`/`FindFirstFileExFromApp`/`MoveFileFromApp`/`RemoveDirectoryFromApp`/`ReplaceFileFromApp` in the AppContainer (`xbox/platform/host_fs_uwp.cpp`). The path policy — `/app0`, the update folder ahead of the base dump, the mods and plugin overlays, `/data`, mount points, the case-insensitive fallback — is unchanged and now runs unchanged on both; the save commit's moves, flushes and recoveries go through the same seam, and its protocol has a test of its own. The app runs the seam's self test once at boot and logs one line, so an AppContainer that refuses a call says so at launch instead of at the game's first save. **The console's memory layer is the AppContainer's.** Direct memory is a
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
Store SDK has no `<pthread_time.h>`). **The guest runtime compiles for the console, all but four files of it.** The
`bbhost_guest` library is forty translation units: all twenty of `src/core` that
a console build uses (every one except the Linux VM layer and the desktop host-fs
backend), the winpthreads shim, and nineteen of the twenty-three in `src/hle` —
the file system, `fios`, save commits, the kernel, pthreads, the
synchronisation primitives, the variadic shims (asm and all), libc, gnm's
resource tracking, the audio stack (`audio`, `ajm`, `avplayer`), `dialog`, the
network modules (`net`, `http`, `np_matching2`, `np_signaling`), `runtime` and
the guest console — compiled against the Store SDK and the AppContainer on every
Windows run. Getting there needed three things upstream's MinGW build had for
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

The three libraries are still **not on `bbxbox`'s link line**, and that is now
the only thing between the tree and a booting guest. The census machinery is in
place so the gap closes in one run rather than twenty: the app links with
`/ERRORLIMIT:0`, and `xbox/scripts/build.ps1` turns a link failure into one
deduplicated `undefined symbols` annotation (each symbol with the object that
wanted it, up to 50 KB) instead of the first eighty lines. The sandbox cannot
read job logs — `results-receiver.actions.githubusercontent.com` is not
reachable and the API's log endpoint is 404 — so the annotation *is* the
channel, and it now holds the whole list.

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

Not yet: the pipeline itself. A dispatch resolves its shader identity, and a key
the baked cache does not carry is counted and *refused once per key*, with the
reason in the log (`host_gpu_d3d12.cpp`, `GuestDispatchReport()`) — never dispatched
as something the console cannot run. Building the compute PSO from the cached
DXIL and issuing the dispatch, and the whole graphics half
(`host_gpu_draw`, render-target views, the blits) stay counted stubs until
Slice 5B-2, which is the next slice.

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

The import audit's own findings are recorded in the run's annotations: the only
notices are `LoadLibraryExW` (C++/WinRT's activation fallback, never called by
the port) and the desktop API imports inside the shipped third-party DLLs
(SDL3-uwp, libuwp, Microsoft's DXC), which the port does not own. `bbxbox.exe`
imports no desktop-only file, memory or loader API.

## Verified here (build machine)

* **226 host tests** — `./tools/build_host.sh`: format, budget, cache, quarantine, descriptors, present,
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
  rumble, the virtual keyboard, and D3D12 device creation.
* `src/host/d3d12`'s replay half: `replay.cpp` and `pipeline.cpp` now exist and compile against the
  real SDK, but no `ID3D12Device` has been created on a console, no root signature has been
  serialised by a real driver, no PSO has been created by one, nothing has presented, and the GPU
  self-test has never actually dispatched — so "the replay is correct" is a compile-time claim and
  nothing more. In particular, the untile dispatch and the read-only-DSV rule (G12) are not
  exercised by anything that runs here.
* bbhost's `src/hle` is not linked into the app, so no guest code has run: `load_orbis_elf` is
  compiled and unit-testable but is not called by any boot path yet, and `CUSA03173` has never been
  opened by this port.

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
