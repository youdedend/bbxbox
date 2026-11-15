// SPDX-License-Identifier: GPL-3.0-or-later
// The error currency, the log bridge and object naming: the things every D3D12
// translation unit in the port needs and none of them owns.
#include "src/host/d3d12/bbx_d3d12.h"

#include <cstdio>

#include "xbox/platform/log.h"

namespace bbx::d3d12 {
namespace {

LogFn g_log = nullptr;

// DXGI's device-loss family. A port that treats any of these as "the call failed,
// try again" loops forever on a removed device, so they map to one status that the
// launcher shows and the crash report explains.
constexpr HRESULT kDxgiDeviceRemoved = static_cast<HRESULT>(0x887A0005L);
constexpr HRESULT kDxgiDeviceHung = static_cast<HRESULT>(0x887A0006L);
constexpr HRESULT kDxgiDeviceReset = static_cast<HRESULT>(0x887A0007L);
constexpr HRESULT kDxgiDriverInternalError = static_cast<HRESULT>(0x887A0020L);
constexpr HRESULT kDxgiOutOfMemory = static_cast<HRESULT>(0x887A0002L);
// D3D12CreateDevice's "this adapter is not 12_0", which is not a failure of the
// device but of the pick.
constexpr HRESULT kDxgiUnsupported = static_cast<HRESULT>(0x887A0004L);

}  // namespace

const char* StatusName(Status status) {
    switch (status) {
        case Status::kOk: return "ok";
        case Status::kUnavailable: return "unavailable";
        case Status::kOutOfMemory: return "out of memory";
        case Status::kDeviceLost: return "device lost";
        case Status::kInvalidArgument: return "invalid argument";
        case Status::kQuarantined: return "quarantined";
    }
    return "unknown";
}

Status FromHresult(HRESULT hr) {
    if (SUCCEEDED(hr)) { return Status::kOk; }
    switch (hr) {
        case kDxgiDeviceRemoved:
        case kDxgiDeviceHung:
        case kDxgiDeviceReset:
        case kDxgiDriverInternalError:
            return Status::kDeviceLost;
        case kDxgiOutOfMemory:
        case E_OUTOFMEMORY:
            return Status::kOutOfMemory;
        case E_INVALIDARG:
            return Status::kInvalidArgument;
        default:
            return Status::kUnavailable;
    }
}

void SetLog(LogFn fn) { g_log = fn; }

void Log(const char* format, ...) {
    // The backend never prints on its own: a line that bypasses the last-chance
    // logger is a line a crash takes with it.
    char buffer[1024];
    std::va_list args;
    va_start(args, format);
    const int written = std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    if (written <= 0) { return; }
    if (g_log) {
        g_log(buffer);
    } else {
        bbx::platform::LogMessage(bbx::platform::Level::kInfo, "d3d12: %s", buffer);
    }
}

void NameObject(ID3D12Object* object, const wchar_t* name) {
    if (object == nullptr || name == nullptr) { return; }
    // SetName is a debug-layer aid; a failure here must not fail the caller.
    (void)object->SetName(name);
}

}  // namespace bbx::d3d12
