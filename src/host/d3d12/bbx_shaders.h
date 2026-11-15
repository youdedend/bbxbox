// SPDX-License-Identifier: GPL-3.0-or-later
// The port's own D3D12 shaders: what they are, and why they are compiled at
// build time.
//
// bbhost's `gcn/hlsl.cpp` is not this file's source. That emitter lowers guest
// GCN to SPIR-V flavoured HLSL - `[[vk::ext_instruction]]`,
// `[[vk::ext_builtin_input]]`, `[[vk::ext_execution_mode]]` - and its intended
// consumer is `dxc -spirv`. Feeding it to `-T ps_6_4` is a compile error, and
// shipping desktop `dxcompiler.dll` to the console to paper over that is exactly
// the single point of failure this port must not have: its imports
// (`CreateFileW`, `LoadLibraryA`, `MapViewOfFile`) are refused by the AppContainer.
//
// So the fixed internal passes - present, overlay - are compiled by
// `build-deps/prefix/dxc/dxc.exe` on the build machine, at `*_6_4`, and embedded
// as bytes (cmake/embed_shaders.cmake). The console app has no runtime shader
// compiler for them. The guest's shaders are P3's problem, and the architecture
// there is SPIR-V -> DXIL in memory (task 1.3/1.4 of the port plan), not this
// header.
//
// This header is portable: no d3d12.h, no Windows types. `bbx_shaders.cpp` in the
// console layer supplies the bytes from the generated header.
#pragma once

#include <cstddef>

namespace bbx::shaders {

struct Blob {
    const void* data = nullptr;
    std::size_t size = 0;
    const char* name = "";
};

// One entry per compiled entry point. The names are the contract between
// xbox/CMakeLists.txt (which runs dxc), xbox/shaders/*.hlsl (which define them)
// and replay.cpp (which creates the PSOs).
struct EntryPoint {
    const char* symbol;    // the C++ accessor below
    const char* entry;     // -E <entry>
    const char* profile;   // -T <profile>
    const char* source;    // file under xbox/shaders
};

inline constexpr EntryPoint kPresentVs{"PresentVs", "PresentVs", "vs_6_4", "present.hlsl"};
inline constexpr EntryPoint kPresentPs{"PresentPs", "PresentPs", "ps_6_4", "present.hlsl"};
inline constexpr EntryPoint kOverlayVs{"OverlayVs", "OverlayVs", "vs_6_4", "overlay.hlsl"};
inline constexpr EntryPoint kOverlayPs{"OverlayPs", "OverlayPs", "ps_6_4", "overlay.hlsl"};
// The compute passes. `UntileCs` detiles a guest texture's GCN micro tiles into a
// linear one; `SelftestCs` writes a pattern the CPU verifies, which is how a boot
// proves the queue, the fence and a readback buffer actually work on a console.
inline constexpr EntryPoint kUntileCs{"UntileCs", "UntileCs", "cs_6_4", "untile.hlsl"};
inline constexpr EntryPoint kSelftestCs{"SelftestCs", "SelftestCs", "cs_6_4", "stream_selftest.hlsl"};

struct EntryPointList {
    const EntryPoint* items;
    std::size_t count;
};

// Every entry point the build must produce. Inline so the host tests can check
// it without a console build; tests/shaders_test.cpp checks that the list is
// complete, that no profile is newer than SM 6.4 and that nothing is duplicated,
// and tools/local_precheck.sh checks it against xbox/CMakeLists.txt.
[[nodiscard]] inline EntryPointList Manifest() {
    static constexpr EntryPoint kAll[] = {kPresentVs,   kPresentPs, kOverlayVs,
                                          kOverlayPs,  kUntileCs,  kSelftestCs};
    return EntryPointList{kAll, sizeof(kAll) / sizeof(kAll[0])};
}

// The compiled bytes. Empty (size 0) only if the build did not produce them,
// which the console reports once and then draws nothing rather than crashing.
[[nodiscard]] Blob PresentVs();
[[nodiscard]] Blob PresentPs();
[[nodiscard]] Blob OverlayVs();
[[nodiscard]] Blob OverlayPs();
[[nodiscard]] Blob UntileCs();
[[nodiscard]] Blob SelftestCs();

// A one-line summary for the boot log: "6 shaders, 21,408 bytes, SM 6.4".
[[nodiscard]] const char* DescribeBlobs();

}  // namespace bbx::shaders
