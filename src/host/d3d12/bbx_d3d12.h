// SPDX-License-Identifier: GPL-3.0-or-later
// The one place the port includes D3D12, and the error currency the rest of the
// console backend uses.
//
// The portable half of `src/host/d3d12` (bbx_format, bbx_budget, bbx_cache,
// bbx_quarantine, bbx_precompile, bbx_descriptors, bbx_present, bbx_stream,
// bbx_untile) does not include this header and is compiled on the build machine.
// Everything else here is Windows-only and only checked by
// tools/syntax_check.sh and a run on the console.
#pragma once

#ifndef _WIN32
#error "the D3D12 backend is Windows-only; the portable core is in bbx_*.h"
#endif

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdarg>
#include <cstdint>
#include <string>

namespace bbx::d3d12 {

using Microsoft::WRL::ComPtr;

enum class Status : std::uint8_t {
    kOk = 0,
    kUnavailable,      // the device/adapter or a component is missing
    kOutOfMemory,      // the budget said no, or the allocator failed
    kDeviceLost,       // HRESULT removed; the title must be restarted
    kInvalidArgument,
    kQuarantined,      // a compile crashed last boot; skipped on purpose
};

[[nodiscard]] const char* StatusName(Status status);
[[nodiscard]] Status FromHresult(HRESULT hr);

// The platform supplies logging (`uwp_last_chance_logger`'s sink and
// `loader.log.txt`). The backend never prints on its own: a log line that
// bypasses the last-chance logger is a line a crash takes with it.
using LogFn = void (*)(const char* line);
void SetLog(LogFn fn);
void Log(const char* format, ...);

// A name on every object: the debug layer and PIX both quote it, and a crash
// dump of a resource called "depth" is worth ten called "0x1".
void NameObject(ID3D12Object* object, const wchar_t* name);

}  // namespace bbx::d3d12
