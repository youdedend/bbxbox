// SPDX-License-Identifier: GPL-3.0-or-later
// The guest's fixed address windows, reserved at the first instruction of the
// process.
//
// bbhost's guest puts direct memory at a fixed address (the CPU window at
// 4 GiB) and maps the same pages a second time in the GPU's alias window at
// 0xff00000000. Those ranges have to be *ours* before anything else can take
// them: bottom-up ASLR places the heap, the stack and every DLL before `main`
// runs, and one of them landing in a window turns every guest mapping into a
// best-effort hint. On a desktop that is handled by relaunching the process
// when the reservation loses (`hle_kernel_reserve_guest_windows`).
//
// A UWP title cannot relaunch itself - there is no CreateProcess in an
// AppContainer, and a second launch of the same package is a second game - so
// the reservation is made here instead, at the top of `main.cpp`, before SDL
// starts its threads and long before D3D12 creates a device with reservations
// of its own.
//
// The constants are bbhost's own (hle/kernel.cpp: kDmemVaBase, kDmemMaxSize,
// kGpuDmemBase, kImageFallbackBase); hle/kernel.cpp reserves the same three
// ranges when it starts, and finds them already held by this process, which is
// exactly what it wants: win_vm_reserve is idempotent for a range it owns.
#include "xbox/platform/guest_windows.h"

#include "core/win_vm.h"
#include "xbox/platform/log.h"

namespace bbx::platform {
namespace {

constexpr std::uint64_t kDmemVaBase = 0x100000000ull;
constexpr std::uint64_t kDmemMaxSize = 0x1f0000000ull;
constexpr std::uint64_t kGpuDmemBase = 0xff00000000ull;
constexpr std::uint64_t kGpuDmemSize = 0x100000000ull;
constexpr std::uint64_t kImageFallbackBase = 0x300000000ull;
constexpr std::uint64_t kImageFallbackSize = 0x10000000ull;

}  // namespace

bool ReserveGuestWindows() {
#if defined(_WIN32)
    const bool dmem = win_vm_reserve(kDmemVaBase, kDmemMaxSize);
    const bool alias = win_vm_reserve(kGpuDmemBase, kGpuDmemSize);
    const bool image = win_vm_reserve(kImageFallbackBase, kImageFallbackSize);
    LogF("guest windows: dmem [0x%llx,+0x%llx) %s, gpu alias [0x%llx,+4 GiB) %s, image fallback %s",
         static_cast<unsigned long long>(kDmemVaBase), static_cast<unsigned long long>(kDmemMaxSize),
         dmem ? "reserved" : "TAKEN", static_cast<unsigned long long>(kGpuDmemBase),
         alias ? "reserved" : "TAKEN", image ? "reserved" : "taken");
    if (!dmem || !alias) {
        LogMessage(Level::kError,
                   "guest windows: a fixed address was taken before the port ran; guest mappings are "
                   "best effort this session");
    }
    return dmem && alias;
#else
    return true;
#endif
}

}  // namespace bbx::platform
