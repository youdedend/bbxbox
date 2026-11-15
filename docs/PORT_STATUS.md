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

## Verified here (build machine)

* **146 host tests** — `./tools/build_host.sh`: format, budget, cache, quarantine, descriptors, present,
  stream, untile, precompile, barriers, dump layout, ini, launcher, chords/keyboard, overlay, spill,
  guest-memory ledger (W^X), caps, PSO cache, tessellation policy, display plan.
* `./tools/local_precheck.sh`: tests, AppContainer API rules (`*FromApp`, no
  `PAGE_EXECUTE_READWRITE`), `/STACK:8388608,65536`, no game files/build output/absolute paths tracked,
  docs/code number agreement.

## Not verified (no Windows toolchain in this environment)

* Everything under `xbox/` compiles nowhere yet: `xbox/CMakeLists.txt`, `main.cpp`, `storage.cpp`,
  `guest_memory.cpp`, `memory_selftest.cpp`, `display_winrt.cpp`, `log.cpp`.
* `Device`, `Swapchain`, `Dxil`, `Replay`, `Untile` (D3D12) — not written yet.
* `xbox/scripts/*.ps1` and the CI `console` job have never executed. Expect the first run to need
  fixes (toolset retarget, `SDL3.lib` output path, `appmodel.h` availability).
* Anything requiring a real console: install, Game-mode memory limit, 4K output, DXC on Xbox.

## Rules that must not regress

1. `/STACK:8388608,65536` — never 16 MiB (1.28 GiB across 82 threads).
2. Guest addresses < 1 TiB; strict W^X; `*FromApp` APIs only.
3. No `ID3D12PipelineLibrary`; no mass PSO creation at startup.
4. Shader targets `*_6_4` on console; no 16-bit types; descriptor tables, not heap indexing.
5. Tessellated draws: compute emulation or a counted, logged skip — never a hardware HS/DS draw.
6. Swapchain ≤ 3840×2160; TV-mode size via `HdmiDisplayInformation` + inverse matrix transform.
7. `precompile_inflight.txt` before every compile; quarantine on next boot.
8. Zero game files in git; `local_precheck.sh` before every push; CI 2–4 minutes incremental.
