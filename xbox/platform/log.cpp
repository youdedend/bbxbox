// SPDX-License-Identifier: GPL-3.0-or-later
#include "src/host/core/bbx_fault_filter.h"
#include "xbox/platform/log.h"

#include <windows.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "xbox/platform/guest_memory.h"
#include "xbox/platform/host_fs.h"

namespace bbx::platform {
namespace {

std::mutex g_mutex;
// The log is a hostfs handle, not a FILE*: the static CRT's stdio reaches
// CreateFileW, which an AppContainer refuses on the dump's granted folders -
// and the same layer is what writes loader.log into LocalState. hostfs is the
// seam that is FromApp on the console (xbox/platform/host_fs.h), and it is what
// the USB mirror already used.
int g_file = -1;
int g_errors = -1;
Level g_level = Level::kInfo;
std::uint64_t g_start = 0;
char g_last_line[512] = {};
constexpr std::size_t kRecentLineCap = 16;
char g_recent_lines[kRecentLineCap][512] = {};
std::size_t g_recent_next = 0;
std::size_t g_recent_count = 0;
std::atomic<bool> g_installed{false};

// --- the black box on the player's stick ----------------------------------
//
// A run that dies on a console is a run nobody can read: LocalState is inside
// the package's own data and the console has no file browser. So as soon as a
// dump folder is granted, the same lines are mirrored, unbuffered, into
// `<dump>\bbxbox_logs\`, and a fault writes its whole report to `crash.log`
// there. The player copies one folder off a USB stick and the report is in it.
//
// The mirror goes through `xbox/platform/host_fs.h`, not `std::fopen`: the
// granted folder is reached through a `FutureAccessList` token, and a CRT
// stream is a `CreateFileW` stream, which the AppContainer refuses there.
int g_mirror = -1;         // <dump>\bbxbox_logs\loader.log, appended
int g_mirror_errors = -1;  // <dump>\bbxbox_logs\errors.log, appended
int g_crash = -1;          // <dump>\bbxbox_logs\crash.log, appended
std::mutex g_mirror_mu; // serialises the mirrors, never held across a fault path
char g_crumb[160] = {}; // the last thing the backend was doing
long g_device_reason = 0;
bool g_device_removed = false;

// The report is built here: the fault path allocates nothing and locks only
// what is free (see LastChance).
char g_report[8192] = {};

int MirrorError() { return hostfs::LastError(); }

void MirrorWrite(int handle, const char* text) {
    const std::size_t size = std::strlen(text);
    std::size_t put = 0;
    while (put < size) {
        const std::int64_t n = hostfs::Write(handle, text + put, size - put);
        if (n <= 0) return;
        put += static_cast<std::size_t>(n);
    }
}

void WriteAndFlush(int handle, const char* text) {
    if (handle < 0) return;
    MirrorWrite(handle, text);
    (void)hostfs::Flush(handle);
}

// The handler must not allocate or lock: take the mutex only if it is free.
void WriteRaw(const char* text, bool also_errors = false, bool to_debugger = true) {
    WriteAndFlush(g_file, text);
    if (also_errors) WriteAndFlush(g_errors, text);
    if (g_mirror >= 0 || (also_errors && g_mirror_errors >= 0)) {
        std::unique_lock<std::mutex> lock(g_mirror_mu, std::try_to_lock_t());
        if (lock.owns_lock()) {
            WriteAndFlush(g_mirror, text);
            if (also_errors) WriteAndFlush(g_mirror_errors, text);
        }
    }
    if (to_debugger) ::OutputDebugStringA(text);
}

// --- the crash report ------------------------------------------------------
//
// Everything here is bounded and on the stack or in `g_report`: no heap, no
// format function, no C++ string. A reporter that faults while reporting is
// worth nothing.

// Where `pc` lives: the module's file name and the offset from its base, the
// form `xbox/scripts/symbolicate.py` resolves against the build's .map.
void DescribePc(std::uintptr_t pc, char* out, std::size_t cap) {
    HMODULE module = nullptr;
    std::uintptr_t base = 0;
    if (::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                 GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             reinterpret_cast<LPCWSTR>(pc), &module) &&
        module != nullptr) {
        base = reinterpret_cast<std::uintptr_t>(module);
        wchar_t wide[MAX_PATH] = {};
        const DWORD got = ::GetModuleFileNameW(module, wide, MAX_PATH);
        char narrow[MAX_PATH] = {};
        if (got > 0) {
            const int need = ::WideCharToMultiByte(CP_UTF8, 0, wide, static_cast<int>(got), narrow,
                                                   MAX_PATH, nullptr, nullptr);
            if (need > 0) narrow[need < MAX_PATH ? need : MAX_PATH - 1] = '\0';
        }
        const char* slash = std::strrchr(narrow, '\\');
        std::snprintf(out, cap, "%s+0x%llx", slash != nullptr ? slash + 1 : narrow,
                      static_cast<unsigned long long>(pc - base));
        return;
    }
    std::snprintf(out, cap, "0x%llx", static_cast<unsigned long long>(pc));
}

// A return address read off the stack is only followed when the stack pointer
// that would be read looks like one: the alternate reporter's own frame is
// never trusted with a wild read.
bool PlausibleStack(std::uint64_t rsp) {
    return rsp >= 0x10000ull && (rsp & 0x7ull) == 0 && rsp < (1ull << 47);
}

// Up to `max` frames as module+offset, unwound with the image's own unwind
// data (RtlVirtualUnwind): the .pdata the linker emitted is what the console
// uses for its own crashes, so a stack here resolves the same way.
int AppendFrames(const CONTEXT& context, char* out, std::size_t cap, std::size_t* used) {
    CONTEXT frame = context;  // 1232 bytes: the stack is fine for one copy
    int frames = 0;
    for (; frames < 32; ++frames) {
        char desc[160];
        DescribePc(static_cast<std::uintptr_t>(frame.Rip), desc, sizeof(desc));
        const int wrote = std::snprintf(out + *used, cap - *used, "  #%02d %s\n", frames, desc);
        if (wrote <= 0 || static_cast<std::size_t>(wrote) >= cap - *used) break;
        *used += static_cast<std::size_t>(wrote);

        DWORD64 image_base = 0;
        const PRUNTIME_FUNCTION entry = ::RtlLookupFunctionEntry(frame.Rip, &image_base, nullptr);
        if (entry == nullptr) {
            // A leaf function has no unwind data: its caller's address is at
            // the top of the stack, and the stack is only read when it looks
            // like a real one.
            if (!PlausibleStack(frame.Rsp)) break;
            const auto next = *reinterpret_cast<const std::uint64_t*>(frame.Rsp);
            if (next == 0) break;
            frame.Rip = next;
            frame.Rsp += 8;
            continue;
        }
        if (!PlausibleStack(frame.Rsp)) break;
        PVOID handler = nullptr;
        DWORD64 establisher = 0;
        KNONVOLATILE_CONTEXT_POINTERS volatile_ctx = {};
        ::RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, frame.Rip, entry, &frame, &handler,
                           &establisher, &volatile_ctx);
        if (frame.Rip == 0) break;
    }
    return frames;
}

// The guest's own address, when the fault is inside the arena: a guest
// pointer's offset is what the game's own crash reports use.
void DescribeTarget(std::uintptr_t address, char* out, std::size_t cap) {
    const bbx::guest::Arena& arena = bbx::guest::GlobalArena();
    if (arena.base != nullptr && address >= arena.slide &&
        address < arena.slide + arena.size) {
        std::snprintf(out, cap, " guest=0x%llx (arena+0x%llx)", static_cast<unsigned long long>(address),
                      static_cast<unsigned long long>(address - arena.slide));
        return;
    }
    const char* where = "host";
    if (address >= bbx::guest::kAddressLimit) where = "host (above the guest's 1 TiB)";
    std::snprintf(out, cap, " %s", where);
}

const char* FaultKind(ULONG_PTR code) {
    switch (code) {
        case 0: return "read";
        case 1: return "write";
        case 8: return "execute";
        default: return "?";
    }
}

void ReportFault(const EXCEPTION_POINTERS* info, const char* lead) {
    const auto* record = info != nullptr ? info->ExceptionRecord : nullptr;
    const auto* context = info != nullptr ? info->ContextRecord : nullptr;
    const auto fault = reinterpret_cast<std::uintptr_t>(
        record != nullptr ? record->ExceptionAddress : nullptr);
    std::size_t used = 0;
    const std::size_t cap = sizeof(g_report);
    auto advance = [&](int n) {
        if (n <= 0) return;
        const std::size_t avail = cap > used ? cap - used - 1 : 0;
        const std::size_t added = static_cast<std::size_t>(n);
        used += added < avail ? added : avail;
    };

    char at[192];
    DescribePc(fault, at, sizeof(at));
    int wrote = std::snprintf(g_report, cap,
                              "\n! %s code=0x%08lx at %s (0x%llx)\n", lead,
                              record != nullptr ? record->ExceptionCode : 0ul, at,
                              static_cast<unsigned long long>(fault));
    advance(wrote);

    if (record != nullptr && record->NumberParameters >= 2 &&
        (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION ||
         record->ExceptionCode == EXCEPTION_IN_PAGE_ERROR)) {
        char target[192];
        // ExceptionInformation[1] is a ULONG_PTR holding the faulting address.
        const auto address = static_cast<std::uintptr_t>(record->ExceptionInformation[1]);
        DescribeTarget(address, target, sizeof(target));
        wrote = std::snprintf(g_report + used, cap - used, "  %s of 0x%llx%s\n",
                              FaultKind(record->ExceptionInformation[0]),
                              static_cast<unsigned long long>(address), target);
        advance(wrote);
    }

    if (context != nullptr) {
        wrote = std::snprintf(g_report + used, cap - used,
                              "  rip=%016llx rsp=%016llx rbp=%016llx eflags=%08lx\n"
                              "  rax=%016llx rbx=%016llx rcx=%016llx rdx=%016llx\n"
                              "  rsi=%016llx rdi=%016llx r08=%016llx r09=%016llx\n"
                              "  r10=%016llx r11=%016llx r12=%016llx r13=%016llx\n"
                              "  r14=%016llx r15=%016llx\n",
                              static_cast<unsigned long long>(context->Rip),
                              static_cast<unsigned long long>(context->Rsp),
                              static_cast<unsigned long long>(context->Rbp),
                              static_cast<unsigned long>(context->EFlags),
                              static_cast<unsigned long long>(context->Rax),
                              static_cast<unsigned long long>(context->Rbx),
                              static_cast<unsigned long long>(context->Rcx),
                              static_cast<unsigned long long>(context->Rdx),
                              static_cast<unsigned long long>(context->Rsi),
                              static_cast<unsigned long long>(context->Rdi),
                              static_cast<unsigned long long>(context->R8),
                              static_cast<unsigned long long>(context->R9),
                              static_cast<unsigned long long>(context->R10),
                              static_cast<unsigned long long>(context->R11),
                              static_cast<unsigned long long>(context->R12),
                              static_cast<unsigned long long>(context->R13),
                              static_cast<unsigned long long>(context->R14),
                              static_cast<unsigned long long>(context->R15));
        advance(wrote);
        wrote = std::snprintf(g_report + used, cap - used, "  stack:\n");
        advance(wrote);
        const int frames = AppendFrames(*context, g_report, cap, &used);
        wrote = std::snprintf(g_report + used, cap - used, "  (%d of 32 frame(s))\n", frames);
        advance(wrote);
    }

    wrote = std::snprintf(g_report + used, cap - used,
                          "  backend: %s\n  device: %s%s\n  last: %s\n",
                          g_crumb[0] != '\0' ? g_crumb : "(nothing recorded)",
                          g_device_removed ? "removed" : "present",
                          g_device_removed ? "" : " ", g_last_line);
    if (wrote < 0) {
        g_report[used] = '\0';
    } else {
        advance(wrote);
    }
}

// The first-chance handler. The console run at 954a97c died here: it logged
// DBG_PRINTEXCEPTION_C (what OutputDebugString raises), logging raised another,
// and the nesting ended in a stack overflow. So the rules are bbx_fault_filter's
// and they are applied before anything is written: a debug print, a breakpoint, a
// C++ throw or a fault while this handler is already running is passed straight
// on; at most 32 faults are logged and then one line says the rest were dropped.
// Nothing here calls OutputDebugString, WinRT or LogMessage - one bounded write to
// the raw report and the crash mirror, and out.
LONG CALLBACK LastChance(EXCEPTION_POINTERS* info) {
    static bbx::core::FaultGate gate;
    const auto* record = info != nullptr ? info->ExceptionRecord : nullptr;
    const std::uint32_t code = record != nullptr ? record->ExceptionCode : 0u;
    if (!gate.Enter()) return EXCEPTION_CONTINUE_SEARCH;  // re-entered: never log
    const bbx::core::FaultAction action = gate.Admit(code);
    if (action == bbx::core::FaultAction::kPass) {
        gate.Leave();
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (action == bbx::core::FaultAction::kSuppressed) {
        WriteRaw("\n! further faults suppressed (only the first 32 are logged)\n", true,
                 /*to_debugger=*/false);
        gate.Leave();
        return EXCEPTION_CONTINUE_SEARCH;
    }
    ReportFault(info, "fault");
    WriteRaw(g_report, true, /*to_debugger=*/false);
    if (g_crash >= 0) {
        std::unique_lock<std::mutex> lock(g_mirror_mu, std::try_to_lock_t());
        if (lock.owns_lock()) {
            MirrorWrite(g_crash, g_report);
            (void)hostfs::Flush(g_crash);
        }
    }
    gate.Leave();
    return EXCEPTION_CONTINUE_SEARCH;  // a caught fault never ends the process
}

void OnTerminate() {
    WriteRaw("\n! std::terminate: unhandled exception\n", true, /*to_debugger=*/false);
    std::abort();
}

void FormatAndWrite(Level level, bool mirror_to_errors, const char* format, std::va_list args) {
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
        std::size_t recent_len = std::strlen(line);
        if (recent_len > 0 && line[recent_len - 1] == '\n') --recent_len;
        if (recent_len >= sizeof(g_recent_lines[0])) {
            recent_len = sizeof(g_recent_lines[0]) - 1;
        }
        std::memcpy(g_recent_lines[g_recent_next], line, recent_len);
        g_recent_lines[g_recent_next][recent_len] = '\0';
        g_recent_next = (g_recent_next + 1) % kRecentLineCap;
        if (g_recent_count < kRecentLineCap) ++g_recent_count;
        WriteRaw(line, mirror_to_errors || level >= Level::kError);
    }
}

}  // namespace

bool LogOpen(const std::string& local_state) {
    std::lock_guard<std::mutex> lock(g_mutex);
    // Idempotent, and it has to be: wWinMain opens the log before anything that
    // can fail, and the framework view opens it again when the platform enters
    // it. Rotating twice would move the lines written between the two into
    // `loader.prev.log` - exactly the lines a launch that died needs to keep.
    if (g_file >= 0) return true;
    g_start = ::GetTickCount64();
    g_recent_next = 0;
    g_recent_count = 0;
    const std::string current = local_state + "\\loader.log";
    const std::string previous = local_state + "\\loader.prev.log";
    (void)hostfs::Unlink(previous);
    (void)hostfs::Rename(current, previous, true);
    g_file = hostfs::Open(current, hostfs::kWrite | hostfs::kCreate | hostfs::kTruncate);

    const std::string errors_current = local_state + "\\errors.log";
    const std::string errors_previous = local_state + "\\errors.prev.log";
    (void)hostfs::Unlink(errors_previous);
    (void)hostfs::Rename(errors_current, errors_previous, true);
    g_errors = hostfs::Open(errors_current, hostfs::kWrite | hostfs::kCreate | hostfs::kTruncate);
    return g_file >= 0;
}

void LogClose() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file >= 0) (void)hostfs::Close(g_file);
    g_file = -1;
    if (g_errors >= 0) (void)hostfs::Close(g_errors);
    g_errors = -1;
    std::lock_guard<std::mutex> mirror(g_mirror_mu);
    if (g_mirror >= 0) (void)hostfs::Close(g_mirror);
    g_mirror = -1;
    if (g_mirror_errors >= 0) (void)hostfs::Close(g_mirror_errors);
    g_mirror_errors = -1;
    if (g_crash >= 0) (void)hostfs::Close(g_crash);
    g_crash = -1;
}

void LogSetLevel(Level level) { g_level = level; }

void LogVF(Level level, const char* format, std::va_list args) {
    FormatAndWrite(level, /*mirror_to_errors=*/false, format, args);
}

void LogMessage(Level level, const char* format, ...) {
    std::va_list args;
    va_start(args, format);
    LogVF(level, format, args);
    va_end(args);
}

void LogMirroredMessage(Level level, const char* format, ...) {
    std::va_list args;
    va_start(args, format);
    FormatAndWrite(level, /*mirror_to_errors=*/true, format, args);
    va_end(args);
    LogFlush();
}

std::uint64_t LogElapsedMs() {
    return g_start == 0 ? 0 : static_cast<std::uint64_t>(::GetTickCount64() - g_start);
}

void LogLastChance(const char* what, std::uintptr_t address, std::uintptr_t module_base) {
    LogMessage(Level::kError, "last chance: %s at 0x%llx (RVA 0x%llx)", what,
               static_cast<unsigned long long>(address),
               static_cast<unsigned long long>(address - module_base));
}

void LogCrumb(const char* what) {
    // No lock, no allocation: the backend calls this between commands, and a
    // torn crumb is better than a deadlock in the reporter.
    if (what == nullptr) {
        g_crumb[0] = '\0';
        return;
    }
    std::snprintf(g_crumb, sizeof(g_crumb), "%s", what);
}

void LogDeviceRemoved(long reason) {
    g_device_reason = reason;
    g_device_removed = true;
    LogMessage(Level::kError, "d3d12: device removed (reason 0x%lx)%s", reason,
               g_crumb[0] != '\0' ? "" : " ");
}

long LogDeviceReason() { return g_device_reason; }

bool LogMirrorToUsb(const std::string& dump_root) {
    if (dump_root.empty()) return false;
    const std::string dir = hostfs::JoinPath(dump_root, "bbxbox_logs");
    if (!hostfs::MkDir(dir, true)) {
        LogMessage(Level::kWarn, "log mirror: cannot make %s (errno %d)", dir.c_str(),
                   MirrorError());
        return false;
    }
    const std::string live = hostfs::JoinPath(dir, "loader.log");
    const std::string errors_live = hostfs::JoinPath(dir, "errors.log");
    const std::string crash = hostfs::JoinPath(dir, "crash.log");

    bool opened = false;
    int open_err = 0;
    {
        std::lock_guard<std::mutex> lock(g_mirror_mu);
        if (g_mirror >= 0) {  // already mirroring: the folder did not change
            return true;
        }
        g_mirror = hostfs::Open(live, hostfs::kWrite | hostfs::kCreate | hostfs::kAppend);
        if (g_mirror < 0) {
            open_err = MirrorError();
        } else {
            opened = true;
            g_mirror_errors =
                hostfs::Open(errors_live, hostfs::kWrite | hostfs::kCreate | hostfs::kAppend);
            g_crash = hostfs::Open(crash, hostfs::kWrite | hostfs::kCreate | hostfs::kAppend);
            MirrorWrite(g_mirror,
                        "\n=== bbxbox log mirror: written on the console, readable on a PC ===\n");
            (void)hostfs::Flush(g_mirror);
            if (g_mirror_errors >= 0) {
                MirrorWrite(
                    g_mirror_errors,
                    "\n=== bbxbox errors.log mirror: steps, heartbeats, shutdowns, errors ===\n");
                (void)hostfs::Flush(g_mirror_errors);
            }
        }
    }
    if (!opened) {
        LogMessage(Level::kWarn, "log mirror: cannot open %s (errno %d)", live.c_str(), open_err);
        return false;
    }
    LogMessage(Level::kInfo, "log mirror: %s", live.c_str());
    return true;
}

bool LogMirrorActive() { return g_mirror >= 0; }

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
        ReportFault(info, "unhandled");
        WriteRaw(g_report, true, /*to_debugger=*/false);
        if (g_crash >= 0) {
            std::unique_lock<std::mutex> lock(g_mirror_mu, std::try_to_lock_t());
            if (lock.owns_lock()) {
                MirrorWrite(g_crash, g_report);
                (void)hostfs::Flush(g_crash);
            }
        }
        return EXCEPTION_EXECUTE_HANDLER;
    });
    std::set_terminate(&OnTerminate);
}

const char* LastLine() { return g_last_line; }

std::vector<std::string> LogRecentLines() {
    std::lock_guard<std::mutex> lock(g_mutex);
    std::vector<std::string> out;
    out.reserve(g_recent_count);
    const std::size_t start =
        (g_recent_next + kRecentLineCap - g_recent_count) % kRecentLineCap;
    for (std::size_t i = 0; i < g_recent_count; ++i) {
        const std::size_t idx = (start + i) % kRecentLineCap;
        out.emplace_back(g_recent_lines[idx]);
    }
    return out;
}

void LogFlush() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file >= 0) (void)hostfs::Flush(g_file);
    if (g_errors >= 0) (void)hostfs::Flush(g_errors);
    if (g_mirror >= 0 || g_mirror_errors >= 0) {
        std::unique_lock<std::mutex> mirror(g_mirror_mu, std::try_to_lock_t());
        if (mirror.owns_lock()) {
            if (g_mirror >= 0) (void)hostfs::Flush(g_mirror);
            if (g_mirror_errors >= 0) (void)hostfs::Flush(g_mirror_errors);
        }
    }
}

}  // namespace bbx::platform
