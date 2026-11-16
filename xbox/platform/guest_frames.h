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
// Deliberately narrow: no D3D12 type crosses this line, so the bridge does not
// have to include the session's header (and the two translation units do not
// have to agree about <d3d12.h>'s macros).
#pragma once

#include <atomic>
#include <cstdint>

namespace bbx::platform {

// (rgba8 pixels, width, height). Called on the guest's thread; must not block.
using GuestFrameSink = void (*)(const std::uint8_t* rgba, std::uint32_t width, std::uint32_t height);

inline std::atomic<GuestFrameSink> g_guest_frame_sink{nullptr};

inline void SetGuestFrameSink(GuestFrameSink sink) { g_guest_frame_sink.store(sink, std::memory_order_release); }

inline void SubmitGuestFrame(const std::uint8_t* rgba, std::uint32_t width, std::uint32_t height) {
    const GuestFrameSink sink = g_guest_frame_sink.load(std::memory_order_acquire);
    if (sink == nullptr || rgba == nullptr || width == 0 || height == 0) { return; }
    sink(rgba, width, height);
}

}  // namespace bbx::platform
