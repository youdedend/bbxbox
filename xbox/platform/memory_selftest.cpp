// SPDX-License-Identifier: GPL-3.0-or-later
#include "xbox/platform/memory_selftest.h"

#include "xbox/platform/guest_memory.inc"
#include "xbox/platform/log.h"
#include "xbox/platform/storage.h"

#include <windows.h>

#include <winrt/Windows.System.h>

#include <cstdio>
#include <cstring>

namespace bbx::platform {
namespace {

// The app's commitment, via Windows.System.MemoryManager: the supported UWP way
// to ask what Game Mode has handed the title and what it is using. The desktop
// GetAppMemoryUsage/GetAppMemoryUsageLimit are not in the APP partition's
// appmodel.h, and psapi is desktop-only.
bool QueryAppMemory(std::uint64_t& used, std::uint64_t& limit) {
    used = 0;
    limit = 0;
    try {
        // MemoryManager is the UWP surface built for exactly this question.
        used = winrt::Windows::System::MemoryManager::AppMemoryUsage();
        limit = winrt::Windows::System::MemoryManager::AppMemoryUsageLimit();
    } catch (const winrt::hresult_error&) {
        return false;
    }
    return used != 0 && limit != 0;
}

struct SysvProbe {
    bool ok = false;
    int value = 0;
};

SysvProbe RunSysvProbe() {
    SysvProbe probe;
    void* page = ::VirtualAlloc2FromApp(::GetCurrentProcess(), nullptr, 4096, MEM_RESERVE | MEM_COMMIT,
                                       PAGE_READWRITE, nullptr, 0);
    if (page == nullptr) return probe;
    // mov eax, 42; ret
    const unsigned char code[] = {0xb8, 0x2a, 0x00, 0x00, 0x00, 0xc3};
    std::memcpy(page, code, sizeof(code));
    DWORD old = 0;
    if (!::VirtualProtectFromApp(page, 4096, PAGE_EXECUTE_READ, &old)) {
        ::VirtualFree(page, 0, MEM_RELEASE);
        return probe;
    }
    ::FlushInstructionCache(::GetCurrentProcess(), page, 4096);
    using Fn = int(__attribute__((sysv_abi))*)();
    probe.value = reinterpret_cast<Fn>(page)();
    probe.ok = probe.value == 42;
    ::VirtualFree(page, 0, MEM_RELEASE);
    return probe;
}

// Strict W^X: RW, then RX, and a request for RWX must not produce an executable
// writable page. The check inspects the protection the OS actually reports.
bool CheckWx() {
    void* page = ::VirtualAlloc2FromApp(::GetCurrentProcess(), nullptr, 4096, MEM_RESERVE | MEM_COMMIT,
                                       PAGE_READWRITE, nullptr, 0);
    if (page == nullptr) return false;
    DWORD old = 0;
    bool ok = ::VirtualProtectFromApp(page, 4096, PAGE_EXECUTE_READ, &old) != 0;
    ::FlushInstructionCache(::GetCurrentProcess(), page, 4096);
    MEMORY_BASIC_INFORMATION info{};
    if (::VirtualQuery(page, &info, sizeof(info)) != 0) {
        ok = ok && (info.Protect == PAGE_EXECUTE_READ || info.Protect == PAGE_EXECUTE);
        ok = ok && info.Protect != PAGE_EXECUTE_READWRITE;
    } else {
        ok = false;
    }
    ::VirtualFree(page, 0, MEM_RELEASE);
    return ok;
}

}  // namespace

SelfTestResult RunMemorySelfTest(bool guest_arena_probe) {
    SelfTestResult result;

    const SysvProbe probe = RunSysvProbe();
    result.code_generation = probe.ok;
    result.detail += "codeGeneration + sysv_abi: ";
    result.detail += probe.ok ? "PASS (42)" : "FAIL";

    result.wx = CheckWx();
    result.detail += "\nW^X (no PAGE_EXECUTE_READWRITE, RX after flush): ";
    result.detail += result.wx ? "PASS" : "FAIL";

    // The arena: either the guest's, or a probe of the same shape.
    // The probe must not use 0x100000000: the launcher has already reserved the
    // guest dmem window there (guest windows: dmem [0x100000000,+0x1f0000000)),
    // so an exact-slide allocation at that address always fails and the report
    // said "guest arena below 1 TiB: FAIL" on every boot. Probe in free guest
    // space that is still under the 1 TiB line, and name the address used.
    bbx::guest::Arena arena;
    constexpr std::uint64_t kProbeSize = 64ull << 20;
    constexpr std::uint64_t kProbeSlides[] = {0x2000000000ull, 0x4000000000ull, 0x8000000000ull};
    std::uint64_t probed_at = 0;
    for (const std::uint64_t slide : kProbeSlides) {
        if (bbx::guest::Alloc(arena, slide, kProbeSize)) {
            probed_at = slide;
            break;
        }
    }
    if (probed_at != 0) {
        result.guest_arena = bbx::guest::RangeIsGuest(arena, probed_at, kProbeSize) &&
                            (probed_at + kProbeSize) <= bbx::guest::kAddressLimit;
        result.guest_mib = bbx::guest::CommittedMib(arena);
        bbx::guest::Free(arena);
    } else {
        result.detail += "\n(no probe slide below 1 TiB could be reserved)";
    }
    if (guest_arena_probe) {
        bbx::guest::Arena& global = bbx::guest::GlobalArena();
        if (global.base != nullptr) {
            result.guest_arena = result.guest_arena &&
                                 (global.slide + global.size) <= bbx::guest::kAddressLimit;
            result.guest_mib = bbx::guest::CommittedMib(global);
        } else {
            // No guest has been loaded this boot, which is the normal state when
            // no dump folder has been chosen yet. The probe above is what the P0
            // contract checks; failing the whole line for an arena nobody asked
            // for reported "guest arena below 1 TiB: FAIL" on every boot without
            // a game and hid the real result.
            result.detail += "\n(global arena not allocated: no guest loaded this boot)";
        }
    }
    result.detail += "\nguest arena below 1 TiB: ";
    result.detail += result.guest_arena ? "PASS" : "FAIL";

    std::uint64_t used = 0;
    std::uint64_t limit = 0;
    const bool queried = QueryAppMemory(used, limit);
    result.app_mib = used >> 20;
    result.limit_mib = limit >> 20;
    // The 5,120 MiB Game-mode ceiling is the plan's input; a different limit
    // means the app was launched in App mode or the console is not in Game mode.
    result.budget = queried && limit >= (4096ull << 20);
    result.detail += "\nAppMemoryUsage: ";
    if (queried) {
        char line[160];
        std::snprintf(line, sizeof(line), "%llu MiB of %llu MiB", static_cast<unsigned long long>(result.app_mib),
                      static_cast<unsigned long long>(result.limit_mib));
        result.detail += line;
    } else {
        result.detail += "unavailable";
    }
    result.under_allowance = queried && result.app_mib <= kStartupAllowanceMib;
    result.detail += result.under_allowance ? " (under the P0 allowance of " : " (over the P0 allowance of ";
    result.detail += std::to_string(kStartupAllowanceMib) + " MiB)";

    LogMessage(result.Passed() ? Level::kInfo : Level::kError, "memory self-test: %s",
               result.Passed() ? "PASS" : "FAIL");
    const std::string report = WriteSelfTestReport(result);
    if (!report.empty()) LogMessage(Level::kInfo, "memory self-test report: %s", report.c_str());
    return result;
}

std::string WriteSelfTestReport(const SelfTestResult& result) {
    const std::string path = Local("memory-selftest.txt");
    std::string text =
        "bbxbox memory self-test\n"
        "P0 contract: code generation, W^X, guest arena below 1 TiB, budget, startup allowance\n\n";
    text += result.detail;
    text += "\n\nresult: ";
    text += result.Passed() ? "PASS\n" : "FAIL\n";
    text += "limits: Game Mode 5,120 MiB; guest cap 2,048 MiB; allocatable 4,480 MiB\n";
    if (!WriteText(path, text)) return {};
    return path;
}

}  // namespace bbx::platform
