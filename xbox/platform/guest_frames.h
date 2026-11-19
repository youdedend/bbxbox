// SPDX-License-Identifier: GPL-3.0-or-later
// The guest's scanout frames, from the flip to the window.
//
// The guest's video HLE calls host_present on its own thread, between two waits
// (an EOP and a label), so whatever runs there has to be bounded: a bounds check
// and a memcpy, never a wait for the GPU. The window's session is on the
// launcher's thread and is the only thing that can put the pixels on the
// television, and one thread cannot call the other, so the hand-off is a sink
// function: the session registers one when its device comes up, host_present
// calls it with the newest frame, and a sink that is not registered (no device,
// launcher menu) simply drops the frame.
//
// What crosses the line is the buffer's own attribute, not a promise about its
// bytes. A display buffer is whatever sceVideoOutSetBufferAttribute said it is:
// 8-bit channels or 10, sRGB or not, four bytes a pixel or eight, rows or GCN
// micro tiles, and a row pitch in pixels that need not be the width. The seam
// carries the console's words and lets the presenter decide what they mean
// (bbx_scanout), because a frame described as "RGBA8, tightly packed, rows" is
// three guesses about a buffer the console described exactly.
//
// Deliberately narrow: no D3D12 type crosses this line, so the bridge does not
// have to include the session's header (and the two translation units do not
// have to agree about <d3d12.h>'s macros).
#pragma once

#include <atomic>
#include <cstdint>

namespace bbx::platform {

// One scanout buffer, as the guest registered it.
struct GuestFrame {
    const std::uint8_t* pixels = nullptr;  // the guest's bytes, in its own layout
    std::uint64_t display_va = 0;          // guest virtual address for zero-copy GPU scanout lookup
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t format = 0;  // SceVideoOutPixelFormat word (bit 31 set)
    std::uint32_t tiling = 0;  // SceVideoOutTilingMode: 0 TILE, 1 LINEAR
    std::uint32_t pitch = 0;   // row pitch in pixels; 0 is the width
    std::uint64_t bytes = 0;   // how much of the buffer the copy reads
};

// Called on the guest's thread; must not block.
using GuestFrameSink = void (*)(const GuestFrame& frame);

inline std::atomic<GuestFrameSink> g_guest_frame_sink{nullptr};

inline void SetGuestFrameSink(GuestFrameSink sink) { g_guest_frame_sink.store(sink, std::memory_order_release); }

inline void SubmitGuestFrame(const GuestFrame& frame) {
    const GuestFrameSink sink = g_guest_frame_sink.load(std::memory_order_acquire);
    if (sink == nullptr || frame.width == 0 || frame.height == 0) {
        return;
    }
    if (frame.display_va == 0 && (frame.pixels == nullptr || frame.bytes == 0)) {
        return;
    }
    sink(frame);
}

}  // namespace bbx::platform
