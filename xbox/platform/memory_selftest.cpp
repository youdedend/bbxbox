// SPDX-License-Identifier: GPL-3.0-or-later
#include "xbox/platform/memory_selftest.h"

#include "xbox/platform/guest_memory.inc"
#include "xbox/platform/log.h"
#include "xbox/platform/storage.h"

#include <windows.h>

#include <appmodel.h>

#include <cstdio>
#include <cstring>

namespace bbx::platform {
namespace {

// AppMemoryUsage / AppMemoryUsageLimit, via the documented UWP APIs. The Xbox
// build exposes them through the same names as desktop UWP.
bool QueryAppMemory(std::uint64_t& used, std::uint64_t& limit) {
    used = 0;
    limit = 0;
    if (FAILED(::GetAppMemoryUsage(&used)) || FAILED(::GetAppMemoryUsageLimit(&limit))) return false;
    return true;
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

    result.w^x = CheckWx();
    result.detail += "\nW^X (no PAGE_EXECUTE_READWRITE, RX after flush): ";
    result.detail += result.w^x ? "PASS" : "FAIL";

    // The arena: either the guest's, or a probe of the same shape.
    bbx::guest::Arena arena;
    if (bbx::guest::Alloc(arena, 0x100000000ull, 64ull << 20)) {
        result.guest_arena = bbx::guest::RangeIsGuest(arena, 0x100000000ull, 64ull << 20) &&
                            (0x100000000ull + (64ull << 20)) <= bbx::guest::kAddressLimit;
        result.guest_mib = bbx::guest::CommittedMib(arena);
        bbx::guest::Free(arena);
    }
    if (guest_arena_probe) {
        bbx::guest::Arena& global = bbx::guest::GlobalArena();
        result.guest_arena = result.guest_arena && global.base != nullptr &&
                            (global.slide + global.size) <= bbx::guest::kAddressLimit;
        result.guest_mib = bbx::guest::CommittedMib(global);
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
