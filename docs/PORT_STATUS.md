# Port status

Honest state of the port, milestone by milestone, with what is verified where. The exit criteria are
`docs/porting-plan.md` §6; this file says whether they have been met, and what has actually run on a
console versus only on the build machine.

Last updated with the Xbox hardware rules folded in (`docs/ARCHITECTURE_XBOX_D3D12.md`).

## Milestones

| Phase | Work | State |
|---|---|---|
| P0 | UWP shell, launcher, memory contract, packaging, CI, import audit | **Implemented; not yet run on console.** `.appx` layout, manifest (`codeGeneration` + `removableStorage` only), SDL3-uwp/libuwp cached dependency build, memory self-test, `check-imports.ps1`, `local_precheck.sh`, CI. |
| P1 | Guest boot + HLE + audio/input, no rendering | In progress: bbhost is **vendored and compiled** (`upstream/bbhost` @ `a317321`, `cmake/bbhost.cmake`: zlib, LibAtrac9, monocypher, the GCN library). Wiring `src/core` + `src/hle` into the app is the next step. |
| P2 | D3D12 device, swapchain, replay of clears/copies/barriers/markers | Headers and portable policy in place; `device.cpp`/`swapchain.cpp`/`replay.cpp` next. |
| P3 | GX draw path: pipelines, bindless descriptors, vertex fetch, buffer shadow | Design + `bbx_psocache`/`bbx_caps` landed. |
| P4 | YEBIS, Scaleform, compute tessellation, FSR | Policy landed (`bbx_tess`), kernels pending. |
| P5 | DXIL precompile + PSO cache + quarantine + USB sync | Cache and quarantine landed and tested; DXIL compile and USB sync pending. |
| P6 | 60 FPS pass | Pending. |

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
  `guest_memory.cpp` (the arena over `VirtualAlloc2FromApp`), `memory_selftest.cpp`;
* the D3D12 policy translation units in `src/host/d3d12/` (`bbx_caps`,
  `bbx_budget`, `bbx_psocache`, `bbx_tess`, `bbx_stream`, `bbx_quarantine`,
  `bbx_untile`, …);
* the vendored guest core's GCN library, LibAtrac9, monocypher and zlib;
* the package: `bbxbox.exe` + `SDL3.dll` + `libuwp.dll` + DXC's two DLLs, the
  manifest (`EntryPoint="bbxbox.App"`, activated through `bbxbox.def`), the six
  generated logos, a `.map` and a `.pdb`.

The import audit's own findings are recorded in the run's annotations: the only
notices are `LoadLibraryExW` (C++/WinRT's activation fallback, never called by
the port) and the desktop API imports inside the shipped third-party DLLs
(SDL3-uwp, libuwp, Microsoft's DXC), which the port does not own. `bbxbox.exe`
imports no desktop-only file, memory or loader API.

## Verified here (build machine)

* **146 host tests** — `./tools/build_host.sh`: format, budget, cache, quarantine, descriptors, present,
  stream, untile, precompile, barriers, dump layout, ini, launcher, chords/keyboard, overlay, spill,
  guest-memory ledger (W^X), caps, PSO cache, tessellation policy, display plan.
* `./tools/local_precheck.sh`: tests, AppContainer API rules (`*FromApp`, no
  `PAGE_EXECUTE_READWRITE`), `/STACK:8388608,65536`, no game files/build output/absolute paths tracked,
  docs/code number agreement.

## Not verified (no console, no device)

* Anything requiring a real console: package install, Game-mode memory limit, the memory self-test's
  numbers, 4K/HDR output, rumble, the virtual keyboard, DXC on Xbox, and D3D12 device creation.
* `src/host/d3d12`'s device/swapchain/replay/DXIL half is still being written: the policy layer
  compiles, but nothing has created a `ID3D12Device` yet.
* bbhost's `src/core` and `src/hle` are not linked into the app yet (P1): only the GCN library, zlib,
  LibAtrac9 and monocypher are compiled, and the guest arena is the port's own (`guest_memory.cpp`).

## Rules that must not regress

1. `/STACK:8388608,65536` — never 16 MiB (1.28 GiB across 82 threads).
2. Guest addresses < 1 TiB; strict W^X; `*FromApp` APIs only.
3. No `ID3D12PipelineLibrary`; no mass PSO creation at startup.
4. Shader targets `*_6_4` on console; no 16-bit types; descriptor tables, not heap indexing.
5. Tessellated draws: compute emulation or a counted, logged skip — never a hardware HS/DS draw.
6. Swapchain ≤ 3840×2160; TV-mode size via `HdmiDisplayInformation` + inverse matrix transform.
7. `precompile_inflight.txt` before every compile; quarantine on next boot.
8. Zero game files in git; `local_precheck.sh` before every push; CI 2–4 minutes incremental.
