// SPDX-License-Identifier: GPL-3.0-or-later
#include "xbox/platform/log.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <mutex>

namespace bbx::platform {
namespace {

std::mutex g_mutex;
FILE* g_file = nullptr;
Level g_level = Level::kInfo;
std::uint64_t g_start = 0;
char g_last_line[512] = {};
std::atomic<bool> g_installed{false};

// The handler must not allocate or lock: take the mutex only if it is free.
void WriteRaw(const char* text) {
    if (g_file != nullptr) {
        std::fputs(text, g_file);
        std::fflush(g_file);
    }
    ::OutputDebugStringA(text);
}

LONG CALLBACK LastChance(EXCEPTION_POINTERS* info) {
    const auto* record = info->ExceptionRecord;
    char line[512];
    const auto fault = reinterpret_cast<std::uintptr_t>(record ? record->ExceptionAddress : nullptr);
    const unsigned long long address = static_cast<unsigned long long>(fault);
    HMODULE module = nullptr;
    if (::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                 GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             reinterpret_cast<LPCWSTR>(record ? record->ExceptionAddress : nullptr),
                             &module) &&
        module != nullptr) {
        const unsigned long long base = static_cast<unsigned long long>(
            reinterpret_cast<std::uintptr_t>(module));
        std::snprintf(line, sizeof(line),
                      "\n! fault code=0x%08lx at 0x%llx (RVA 0x%llx); last: %s\n",
                      record ? record->ExceptionCode : 0ul, address, address - base, g_last_line);
    } else {
        std::snprintf(line, sizeof(line), "\n! fault code=0x%08lx at 0x%llx; last: %s\n",
                      record ? record->ExceptionCode : 0ul, address, g_last_line);
    }
    WriteRaw(line);
    return EXCEPTION_CONTINUE_SEARCH;
}

void OnTerminate() {
    WriteRaw("\n! std::terminate: unhandled exception\n");
    std::abort();
}

}  // namespace

bool LogOpen(const std::string& local_state) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_start = ::GetTickCount64();
    const std::string current = local_state + "\\loader.log";
    const std::string previous = local_state + "\\loader.prev.log";
    std::remove(previous.c_str());
    std::rename(current.c_str(), previous.c_str());
    g_file = std::fopen(current.c_str(), "w");
    if (g_file == nullptr) return false;
    std::setvbuf(g_file, nullptr, _IONBF, 0);
    return true;
}

void LogClose() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file != nullptr) std::fclose(g_file);
    g_file = nullptr;
}

void LogSetLevel(Level level) { g_level = level; }

void LogVF(Level level, const char* format, std::va_list args) {
    if (level < g_level) return;
    char message[1024];
    std::vsnprintf(message, sizeof(message), format, args);
    char line[1200];
    const unsigned long long ms = g_start == 0 ? 0 : ::GetTickCount64() - g_start;
    static const char* kNames[] = {"D", "I", "W", "E"};
    std::snprintf(line, sizeof(line), "[%7llu.%03llu %s] %s\n", ms / 1000, ms % 1000,
                  kNames[static_cast<int>(level)], message);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        std::snprintf(g_last_line, sizeof(g_last_line), "%s", message);
        WriteRaw(line);
    }
}

void LogMessage(Level level, const char* format, ...) {
    std::va_list args;
    va_start(args, format);
    LogVF(level, format, args);
    va_end(args);
}

void LogLastChance(const char* what, std::uintptr_t address, std::uintptr_t module_base) {
    LogMessage(Level::kError, "last chance: %s at 0x%llx (RVA 0x%llx)", what,
               static_cast<unsigned long long>(address),
               static_cast<unsigned long long>(address - module_base));
}

void InstallLastChanceLogger() {
    if (g_installed.exchange(true)) return;
    // AddVectoredExceptionHandler is desktop-partition only in the SDK's family
    // filter, but kernel32 exports it for apps too (and on the console). Resolve
    // it by name instead of losing the last-chance log.
    using AddVectoredFn = PVOID(WINAPI*)(ULONG, PVECTORED_EXCEPTION_HANDLER);
    const HMODULE kernel = ::GetModuleHandleW(L"kernel32.dll");
    const auto add_vectored = kernel == nullptr
                                  ? nullptr
                                  : reinterpret_cast<AddVectoredFn>(
                                        ::GetProcAddress(kernel, "AddVectoredExceptionHandler"));
    if (add_vectored != nullptr) {
        add_vectored(1, &LastChance);
    } else {
        LogMessage(Level::kWarn, "log: no vectored exception handler; first-chance faults unseen");
    }
    ::SetUnhandledExceptionFilter([](EXCEPTION_POINTERS* info) -> LONG {
        const auto* record = info->ExceptionRecord;
        char line[256];
        std::snprintf(line, sizeof(line), "\n! unhandled: code=0x%08lx at 0x%llx; last: %s\n",
                      record ? record->ExceptionCode : 0ul,
                      static_cast<unsigned long long>(
                          reinterpret_cast<std::uintptr_t>(record ? record->ExceptionAddress : nullptr)),
                      g_last_line);
        WriteRaw(line);
        return EXCEPTION_EXECUTE_HANDLER;
    });
    std::set_terminate(&OnTerminate);
}

const char* LastLine() { return g_last_line; }

void LogFlush() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file != nullptr) std::fflush(g_file);
}

}  // namespace bbx::platform
