# bbxbox

**Bloodborne (PS4 v1.09) on Xbox Series X|S, Retail Developer Mode, UWP `x64`, with a native
Direct3D 12 backend.**

`bbxbox` is a port of [`droogie/bbhost`](https://github.com/droogie/bbhost) — the native x86-64
Bloodborne compatibility layer and FromSoftware GX/YEBIS renderer — to the Xbox Series X|S UWP
sandbox. The game's own 1.09 `eboot.bin` runs unmodified on the console's Zen 2 cores; bbhost's HLE
answers its PS4 system calls; and the renderer is rebuilt on `ID3D12Device` instead of Vulkan,
because Xbox UWP has no Vulkan driver and Mesa Dozen cannot supply the three extensions bbhost's
Vulkan host needs (`VK_EXT_external_memory_host`, `VK_EXT_graphics_pipeline_library`,
`VK_KHR_push_descriptor`) inside the 5,120 MiB Game Mode ceiling.

The repository contains **no game code or data**. You need your own dump of Bloodborne with the
1.09 update and its decrypted `eboot.bin` (SHA-256
`941f887a562aae054fac35af8cc8f27cf075f3d4cc2e029fb5ae2a663aaa5ae7`).

---

## What is where

| Path | What it is |
|---|---|
| `docs/porting-plan.md` | The port in one document: architecture, the eight hardware lessons from `Test-project`, phases and acceptance. |
| `docs/d3d12-backend.md` | The native D3D12 backend: device, UMA heaps, bindless root signature, descriptor heaps, CoreWindow flip model, GPU untiling, timeline fences → `bb-labels`. |
| `docs/shader-pipeline.md` | GCN → lifter SPIR-V → DXIL-targeted HLSL → `dxcompiler.dll` → `dxil.dll`, plus the fallback that keeps 100% of draws native. |
| `docs/precompile-cache.md` | Ahead-of-gameplay DXIL/PSO precompile, translator fingerprints, `precompile_inflight.txt`, quarantine, USB sync. |
| `docs/memory-budget.md` | The 5,120 MiB partition, the 1.28 GiB thread-stack fix, the sub-1 TiB guest arena, the sparse `WC_GARLIC` swap pager. |
| `docs/console-ux.md` | Launcher, dump discovery and the split base/update overlay, `FutureAccessList` USB tokens, gamepad/touchpad chords, virtual keyboard, `XAudio2`, last-chance logger, PLM. |
| `docs/integration.md` | Exactly which upstream bbhost files change, and how the overlay is applied. |
| `docs/validation.md` | CI gates, on-console acceptance criteria, and how to report a run. |
| `src/host/d3d12/` | The D3D12 backend, the memory/residency planner, the PSO/DXIL cache and the precompiler. |
| `src/host/uwp/` | Host shims for the UWP sandbox (storage, Win32 surface the guest needs, timing). |
| `xbox/` | The UWP app: `CoreApplication` shell, launcher, manifest, packaging, import audit, symbolication. |
| `tests/` | Host-side unit tests for everything that is pure logic (runs on Linux CI and on the build machine). |

## Status

Honest, per subsystem. The port is being brought up; nothing has run on a console yet.

| Subsystem | State |
|---|---|
| UWP build, packaging, import audit, symbolication | implemented (`xbox/`) |
| Launcher, dump/version/overlay detection, USB tokens | implemented (`xbox/platform/`) |
| Guest memory (<1 TiB arena, commit-on-touch, sparse swap) | implemented (`xbox/platform/guest_memory.inc`) |
| Input, audio, virtual keyboard, last-chance logger, PLM | implemented (`xbox/platform/`) |
| D3D12 device, heaps, descriptor heaps, swapchain, fences | implemented (`src/host/d3d12/device*.cpp`) |
| Memory/residency planner, budget accounting | implemented and unit-tested (`src/host/d3d12/budget.*`) |
| PSO/DXIL cache, fingerprint validation, quarantine | implemented and unit-tested (`src/host/d3d12/cache.*`, `precompile.*`) |
| Texture untiling compute | implemented (`src/host/d3d12/untile.*` + `shaders/untile.hlsl`) |
| Stream replay (bbhost `gpu::stream` → D3D12 command lists) | implemented for passes, barriers, copies, clears, queries, markers (`src/host/d3d12/replay.*`) |
| DXIL-targeted HLSL emitter | **specified, not written** — upstream `gcn/hlsl.cpp` emits Vulkan HLSL; the port needs a DXIL target (see `docs/shader-pipeline.md`) |
| Draw packets (GX-native draws) on D3D12 | **in progress** — pipeline/descriptor model done, `render.cpp` draw glue pending |
| 60 FPS on Series X / Series S | **not yet measured** |

The 60 FPS goal is a target, not a claim: `docs/porting-plan.md` records the frame budget it has to
fit in and `docs/validation.md` the run that decides it.

## Building

Requirements: Windows 10/11 x64, Visual Studio 2022 (17.10+), the Universal Windows Platform
workload, the C++ Clang tools (`clang-cl` is mandatory — bbhost implements guest System V functions
with `__attribute__((sysv_abi))`), Windows SDK 10.0.22621+, and a checkout of bbhost at the commit in
[`UPSTREAM.md`](UPSTREAM.md).

```powershell
git submodule update --init --recursive
xbox\scripts\build-deps.ps1 -Bbhost ..\bbhost
xbox\scripts\build.ps1 -Configuration Release -Bbhost ..\bbhost
xbox\scripts\package.ps1
```

The build lays out a package under `xbox\.local\build\layout`, audits every PE import against
`WindowsApp.lib` (`xbox\scripts\check-imports.ps1`), then signs with `MakeAppx`/`SignTool`.

On Linux (CI and development) the pure-logic half builds and tests without any Microsoft toolchain:

```sh
cmake -S . -B build -DBBX_HOST_ONLY=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

## Installing on the console

1. Activate Developer Mode, open Xbox Device Portal (`https://<xbox-ip>:11443`).
2. Add `bbxbox_<version>_x64.appx` and `Dependencies\Microsoft.VCLibs.x64.14.00.appx`.
3. **Set App type to `Game`** (Dev Home → highlight the app → View → View details). In `App` mode
   Xbox caps the process at 1 GiB and D3D12 feature level 12_0 is not available; the app detects
   this and says so instead of failing inside the driver.
4. Put your dump on a USB SSD (`E:\dumps\Bloodborne`) or in `D:\DevelopmentFiles\Bloodborne`, so
   that `eboot.bin`, `sce_module\`, `sce_sys\param.sfo` and `dvdroot_ps4\` are in one folder. A
   separate 1.09 update folder beside it (`Bloodborne-patch\`) is detected and overlaid without
   copying either folder.
5. Launch, pick the dump once (the choice survives in `LocalState\bbxbox.ini` and in the
   `FutureAccessList`), run **Precompile shaders** once, then **Start game**.

## Controls

Standard Soulsborne mapping on the Xbox controller, with the DualShock 4's two touchpad clicks on
chords because the Xbox pad has no touchpad:

| PS4 | Xbox |
|---|---|
| Cross / Circle / Square / Triangle | A / B / X / Y |
| L1 / L2 / R1 / R2 | LB / LT / RB / RT |
| Options | Menu |
| Touchpad left click (Gestures) | **View + D-Pad Left** |
| Touchpad right click (Personal Effects) | **View** (tap), or **View + D-Pad Right** |
| Settings overlay | **L3 + R3** |

Hunter Name entry uses the system virtual keyboard (`Windows.UI.ViewManagement.UIViewSettings` /
`CoreWindow` text input), because a 10-foot UI cannot type on a gamepad.

## Licence

GPL-3.0-or-later, the same as bbhost. The Xbox platform files adapted from
[`youdedend/Test-project`](https://github.com/youdedend/Test-project) are GPL-2.0-or-later, which is
compatible with GPLv3 and is marked in each file's SPDX header. See
[`docs/credits.md`](docs/credits.md).

## Status

Milestone **P0** (UWP shell) is in the tree: the console app (`xbox/`) with the launcher, dump
discovery, the guest memory contract (below 1 TiB, strict W^X), the memory self-test, packaging and
the import audit; the platform layer is [SDL3-uwp](https://github.com/XboxEmulationHub/SDL3-uwp)
`uwp-3.4.20` plus [libuwp](https://github.com/XboxEmulationHub/libuwp), built once into
`build-deps/prefix` and cached by CI. The portable D3D12 core and the platform model carry **146
host-side tests**, run by `./tools/build_host.sh` and by `./tools/local_precheck.sh` (the same
script CI runs before every push). The D3D12 device, swapchain and replay are P2 (their console rules — SM 6.4, no pipeline library,
PSO LRU, compute tessellation, 4K output — are already recorded in
`docs/ARCHITECTURE_XBOX_D3D12.md` and `docs/PORT_STATUS.md`); the headers and
the portable policy they use are already here.
