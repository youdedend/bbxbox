# Pinned upstreams

Everything in this repository is written against these two trees. Neither is vendored: the build
takes `-DBbhost=<path>`, and `tools/fetch_upstream.sh` checks the trees out at the exact commits
below.

| Tree | Commit | Date | What is taken from it |
|---|---|---|---|
| [`droogie/bbhost`](https://github.com/droogie/bbhost) | `a317321c7fea9f34cb002a4796d492f461ff52dd` | 2026-11-14 | The whole host: guest loader and HLE (`src/core`, `src/hle`), the GCN translator and lifter (`src/gcn`), GX/YEBIS hooks and the draw-record model (`src/host/render*.cpp`, `render_hooks.*`, `gpu.h`), the typed command stream (`recorder.cpp`, `stream_ops.h`), the translator fingerprint (`cmake/translator_fingerprint.cmake`), the translation cache (`translation_cache.*`), shader bundles (`shader_bundles.*`), DXC loading (`dxc.cpp`), frame pacing and the label landing thread. |
| [`youdedend/Test-project`](https://github.com/youdedend/Test-project) | `36e388d1123b7a46272043577826c86297d81712` | 2026-10-27 | The hardware-verified Xbox UWP solutions only: `<1` TiB guest arena and sparse `WC_GARLIC` swap (`xbox/platform/guest_memory.inc`, `guest_swap.inc`), `param.sfo` version rules (`sfo_version.h`), the update-overlay index (`overlay_index.h`), the launcher panel strings (`dump_panel.h`), `Windows.Gaming.Input` mapping with touchpad chords (`input.cpp`), `XAudio2` sink (`audio.cpp`), `FutureAccessList` USB redemption and the last-chance logger (`xbox/platform/main.cpp`), the precompile breadcrumb/quarantine protocol (`gpu/shim/bb_precompile.cpp`), and the UWP build/import-audit/packaging scripts (`xbox/CMakeLists.txt`, `AppxManifest.xml`, `xbox/scripts/`). |

**Not** taken from `Test-project`: its shadPS4 Liverpool emulator, its Mesa Dozen
(`vulkan_dzn.dll`) renderer, or its `prepare.py`/`prepare_game.cpp` PS4 SELF-unpacking pipeline.
bbhost loads a decrypted ELF itself (`src/core/elf.cpp`) and lifts shaders from GCN machine code, so
none of that is needed, and Dozen is exactly what this port replaces.

## Why the commit pin matters

* **Translator fingerprint.** `bbhost` stamps its caches with a SHA-256 over `src/gcn/*` plus
  `translation_cache.cpp`. Porting changes files under `src/gcn` (a DXIL emitter target), so the
  fingerprint changes, which by design throws away every translation and pipeline built by a
  previous build. `bbxbox` extends that fingerprint with the D3D12-side identity
  (`docs/precompile-cache.md`), so a DXIL/PSO cache from another `dxc.dll`, another root signature
  or another feature level is rejected rather than reused.
* **Draw-record layout.** `src/host/gpu.h`'s `GpuDrawInputs` and `stream_ops.h`'s records are read
  field for field by the D3D12 replay. A commit bump that changes either must be followed by a
  matching port change; `tests/stream_walk_test.cpp` fails loudly on a record-size change.

## Refreshing the pin

```sh
tools/fetch_upstream.sh            # clone/checkout the commits above into .upstream/
tools/diff_portable_surface.sh     # what of the port's assumptions moved
```

The second script compares the declarations the port depends on (record layouts, `GfxPipeline`'s
state, `gcn::LiftResult`, the fingerprint macro) against the pinned tree and prints a unified diff,
so an upstream bump is a reviewable change rather than a surprise at link time.

## Vendoring

`droogie/bbhost` at `a317321c7fea9f34cb002a4796d492f461ff52dd` is **vendored** in
`upstream/bbhost/` (`src/`, `third_party/libatrac9`, `third_party/monocypher`, both licence
files, and `UPSTREAM-PIN`). It is compiled by `cmake/bbhost.cmake` on every CI run — no network
access at build time, no stand-in. zlib 1.3.1 is vendored next to it in `upstream/third_party/zlib`
(bbhost's `src/gcn/container.cpp` inflates `.shaderbnd.dcx` with it).

Local changes are marked `BBX_UWP`. Today that is `src/core/memory.h` and `src/core/memory.cpp`
only: on the console the guest arena comes from `xbox/platform/guest_memory_bbhost.h`
(`VirtualAlloc2FromApp` below 1 TiB, strict W^X). `tools/fetch_upstream.sh` re-copies the pin,
refuses to overwrite edits outside those two files, and `--diff` lists the markers.
