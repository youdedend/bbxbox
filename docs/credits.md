# Credits and licences

## The port stands on

**[droogie/bbhost](https://github.com/droogie/bbhost)** (GPL-3.0-or-later) — the Bloodborne
compatibility layer and renderer this port is built on: the ELF loader and HLE, the GCN translator
and lifter, the GX/YEBIS hook model, the command stream and recorder, the translation cache and the
translator fingerprint. `LICENSE` and `LICENSE-EXCEPTION` are bbhost's, unchanged.

**[youdedend/Test-project](https://github.com/youdedend/Test-project)** (GPL-2.0-or-later) — the
hardware-verified Xbox Series S solutions carried into this port: the sub-1 TiB guest arena and
sparse `WC_GARLIC` swap pager, the `param.sfo` version rules, the update-overlay index, the launcher
panel text, `Windows.Gaming.Input` mapping with touchpad chords, the `XAudio2` sink, the
`FutureAccessList` USB flow and last-chance logger, the precompile breadcrumb/quarantine protocol,
and the UWP build, import-audit and packaging scripts. Adapted files keep an
`SPDX-License-Identifier: GPL-2.0-or-later` header and a note naming the original.

The prior art's renderer (shadPS4's Liverpool emulator and Mesa Dozen) is deliberately **not** used;
see `docs/porting-plan.md` §1.

## Third-party, linked or shipped

| Component | Licence | Used for | Shipped |
|---|---|---|---|
| DirectX Shader Compiler (`dxcompiler.dll`, `dxil.dll`) | MIT / NCSA | HLSL → DXIL, DXIL validation | yes, in the package |
| SPIRV-Cross | Apache-2.0 | fallback SPIR-V → HLSL for the shaders the lifter refuses | statically linked |
| LibAtrac9 | MIT | ATRAC9 decoding behind `sceAjm` | statically linked |
| Monocypher | BSD-2-Clause / CC0 | Ed25519 checks for official plugins | statically linked |
| zlib | zlib | DCX inflation | statically linked |
| FFmpeg | LGPL-2.1-or-later | movie decode (bboxed, static, no `--enable-gpl`) | statically linked |
| Windows App SDK CRT (`Microsoft.VCLibs.140.00`) | Microsoft | UWP runtime dependency | as an Appx dependency, not bundled |

The package's `ThirdParty\` folder carries each licence text, produced by the build
(`xbox/CMakeLists.txt`), and `check-imports.ps1` fails the build if a binary's imports are not in the
allowed set.

## Not included, ever

Game code, game data, `eboot.bin`, dumps, keys, ISOs, or anything derived from them. Shader HLSL and
DXIL built from the game's shaders are generated on the player's console and never committed or
shipped.

## Console platform dependencies

* [SDL3-uwp](https://github.com/XboxEmulationHub/SDL3-uwp) (`uwp-3.4.20`, zlib licence): SDL 3.4.20
  with WinRT/Xbox backends. Built unmodified by `xbox/scripts/build-deps.ps1`; the app links its C
  API. Every SDL3 call in bbhost's host code resolves to this build on the console.
* [libuwp](https://github.com/XboxEmulationHub/libuwp) (zlib licence): the TV display size and
  refresh rate, and `LoadPackagedLibrary`. Only `uwp_GetScreenSize`, `uwp_GetRefreshRate`,
  `uwp_LoadLibrary` and `uwp_GetWindowReference` are used; its ROM picker is not.
* [Test-project](https://github.com/youdedend/Test-project) (GPL-2.0-or-later): the eight
  hardware-verified Xbox lessons, `sfo_version.h`, `overlay_index.h` and `dump_panel.h`, adapted
  with SPDX headers.
