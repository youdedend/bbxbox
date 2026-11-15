// SPDX-License-Identifier: GPL-3.0-or-later
// Supplies the build-time compiled DXIL bytes (see bbx_shaders.h for why there is
// no runtime compiler here).
#include "src/host/d3d12/bbx_shaders.h"

#include <cstdio>

// Produced by cmake/embed_shaders.cmake; the include directory is added by
// xbox/CMakeLists.txt.
#include "bbx_shaders_generated.h"

namespace bbx::shaders {
namespace {

template <std::size_t N>
constexpr Blob MakeBlob(const unsigned char (&bytes)[N], const char* name) {
    return Blob{bytes, N, name};
}

}  // namespace

Blob PresentVs() { return MakeBlob(generated::kPresentVs, "present.vs_6_4"); }
Blob PresentPs() { return MakeBlob(generated::kPresentPs, "present.ps_6_4"); }
Blob OverlayVs() { return MakeBlob(generated::kOverlayVs, "overlay.vs_6_4"); }
Blob OverlayPs() { return MakeBlob(generated::kOverlayPs, "overlay.ps_6_4"); }
Blob UntileCs() { return MakeBlob(generated::kUntileCs, "untile.cs_6_4"); }
Blob SelftestCs() { return MakeBlob(generated::kSelftestCs, "stream_selftest.cs_6_4"); }

const char* DescribeBlobs() {
    // Static: the boot log writes it once, and this must not allocate.
    static char text[128];
    if (text[0] == '\0') {
        const Blob blobs[] = {PresentVs(), PresentPs(), OverlayVs(),
                              OverlayPs(), UntileCs(),  SelftestCs()};
        const std::size_t count = sizeof(blobs) / sizeof(blobs[0]);
        std::size_t total = 0;
        for (const Blob& blob : blobs) { total += blob.size; }
        std::snprintf(text, sizeof(text), "%zu shaders, %zu bytes, SM 6.4 (%zu entry points)", count,
                      total, Manifest().count);
    }
    return text;
}

}  // namespace bbx::shaders
