// SPDX-License-Identifier: GPL-3.0-or-later
#include "xbox/platform/storage.h"

#include "xbox/platform/log.h"

#include <windows.h>

// The FromApp file APIs: the APP-partition surface for paths outside the
// package's own folders. They live in their own header, they are not in
// <windows.h>, and the SDK declares them only when NTDDI_VERSION is at least
// NTDDI_WIN10_RS3 (see xbox/CMakeLists.txt).
#include <fileapifromapp.h>

#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.AccessCache.h>
#include <winrt/Windows.Storage.Pickers.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.UI.Core.h>

#include <algorithm>
#include <cstdio>
#include <cwctype>
#include <mutex>
#include <thread>

namespace bbx::platform {
namespace {

StoragePaths g_paths;
std::once_flag g_once;

// GetFileAttributesExFromApp is the APP-partition way to ask about a path;
// GetFileAttributesW is desktop-only.
DWORD AttributesOf(const std::wstring& wide) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!::GetFileAttributesExFromAppW(wide.c_str(), GetFileExInfoStandard, &data)) {
        return INVALID_FILE_ATTRIBUTES;
    }
    return data.dwFileAttributes;
}

std::wstring Widen(const std::string& text) {
    if (text.empty()) return {};
    const int need = ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                           nullptr, 0);
    std::wstring out(static_cast<std::size_t>(need), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), need);
    return out;
}

std::string Narrow(const std::wstring& text) {
    if (text.empty()) return {};
    const int need = ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                           nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(need), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), need,
                          nullptr, nullptr);
    return out;
}

std::string Join(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    if (a.back() == '\\' || a.back() == '/') return a + b;
    return a + "\\" + b;
}

std::string Lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

}  // namespace

bool StorageInit() {
    bool ok = false;
    std::call_once(g_once, [&ok]() {
        try {
            const auto local = winrt::Windows::Storage::ApplicationData::Current().LocalFolder();
            const auto temp = winrt::Windows::Storage::ApplicationData::Current().TemporaryFolder();
            const auto roaming = winrt::Windows::Storage::ApplicationData::Current().RoamingFolder();
            g_paths.local_state = winrt::to_string(local.Path());
            g_paths.temp = winrt::to_string(temp.Path());
            g_paths.roaming = winrt::to_string(roaming.Path());
            g_paths.package =
                winrt::to_string(winrt::Windows::ApplicationModel::Package::Current().InstalledPath());
            ok = true;
        } catch (const winrt::hresult_error& error) {
            LogMessage(Level::kError, "storage: ApplicationData unavailable (0x%08x)",
                       static_cast<unsigned>(error.code().value));
        }
        if (ok) {
            ::CreateDirectoryFromAppW(Widen(Local("shadercache")).c_str(), nullptr);
            ::CreateDirectoryFromAppW(Widen(Local("crashes")).c_str(), nullptr);
            ::CreateDirectoryFromAppW(Widen(Local("guestswap")).c_str(), nullptr);
        }
    });
    return ok;
}

const StoragePaths& Paths() { return g_paths; }

std::string Local(const std::string& relative) {
    if (g_paths.local_state.empty()) return relative;
    return Join(g_paths.local_state, relative);
}

bool ReadBinary(const std::string& path, std::vector<std::uint8_t>& out, std::size_t cap) {
    out.clear();
    const std::wstring wide = Widen(path);
    HANDLE file = ::CreateFile2FromAppW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(file, &size) || size.QuadPart < 0 ||
        static_cast<std::uint64_t>(size.QuadPart) > cap) {
        ::CloseHandle(file);
        return false;
    }
    out.resize(static_cast<std::size_t>(size.QuadPart));
    DWORD read = 0;
    const bool ok = out.empty() || (::ReadFile(file, out.data(), static_cast<DWORD>(out.size()), &read,
                                               nullptr) &&
                                    read == out.size());
    ::CloseHandle(file);
    if (!ok) out.clear();
    return ok;
}

bool ReadText(const std::string& path, std::string& out, std::size_t cap) {
    std::vector<std::uint8_t> bytes;
    if (!ReadBinary(path, bytes, cap)) return false;
    out.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    return true;
}

bool WriteBinary(const std::string& path, std::span<const std::uint8_t> bytes) {
    const std::wstring wide = Widen(path);
    const std::wstring temporary = Widen(path + ".tmp");
    HANDLE file = ::CreateFile2FromAppW(temporary.c_str(), GENERIC_WRITE, 0, CREATE_ALWAYS, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = ::WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
              written == bytes.size();
    // The breadcrumb has to survive a power cut, not just a crash.
    ok = ok && ::FlushFileBuffers(file);
    ::CloseHandle(file);
    if (!ok) {
        ::DeleteFileFromAppW(temporary.c_str());
        return false;
    }
    ::DeleteFileFromAppW(wide.c_str());
    return ::MoveFileFromAppW(temporary.c_str(), wide.c_str()) != 0;
}

bool WriteText(const std::string& path, const std::string& text) {
    return WriteBinary(path, std::span<const std::uint8_t>(
                                  reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
}

bool AppendText(const std::string& path, const std::string& text) {
    std::string existing;
    // Append never fails on a missing file: ReadText reports false and `existing`
    // stays empty, which is exactly the file-to-create case.
    (void)ReadText(path, existing, 4u << 20);
    return WriteText(path, existing + text);
}

bool Exists(const std::string& path) {
    const std::wstring wide = Widen(path);
    const DWORD attributes = AttributesOf(wide);
    return attributes != INVALID_FILE_ATTRIBUTES;
}

bool IsDir(const std::string& path) {
    const DWORD attributes = AttributesOf(Widen(path));
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

std::uint64_t FileSize(const std::string& path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!::GetFileAttributesExW(Widen(path).c_str(), GetFileExInfoStandard, &data)) return 0;
    return (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
}

namespace {

std::vector<std::string> ListEntries(const std::string& path, bool want_dirs) {
    std::vector<std::string> out;
    const std::string pattern = Join(path.empty() ? "." : path, "*");
    WIN32_FIND_DATAW data{};
    HANDLE find = ::FindFirstFileExFromAppW(Widen(pattern).c_str(), FindExInfoBasic, &data,
                                           FindExSearchNameMatch, nullptr, 0);
    if (find == INVALID_HANDLE_VALUE) return out;
    do {
        const std::wstring name = data.cFileName;
        if (name == L"." || name == L"..") continue;
        const bool is_dir = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (is_dir != want_dirs) continue;
        out.push_back(Narrow(name));
    } while (::FindNextFileW(find, &data));
    ::FindClose(find);
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace

std::vector<std::string> ListDirs(const std::string& path) { return ListEntries(path, true); }
std::vector<std::string> ListFiles(const std::string& path) { return ListEntries(path, false); }

bool PickFolderGrant(std::string& path_out, std::string& token_out) {
    // The picker is a UI-thread call. Run it there and wait here, which is the
    // pattern the SDL3-uwp picker uses; calling .get() on the UI thread would
    // deadlock the only thread that can complete it.
    struct Result {
        std::string path;
        std::string token;
    } result;
    HANDLE done = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (done == nullptr) return false;

    std::thread([&result, done]() {
        auto dispatcher = winrt::Windows::ApplicationModel::Core::CoreApplication::MainView()
                              .CoreWindow()
                              .Dispatcher();
        dispatcher.RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal, [&result, done]() {
            try {
                using winrt::Windows::Storage::Pickers::FolderPicker;
                namespace Collections = winrt::Windows::Foundation::Collections;
                namespace AccessCache = winrt::Windows::Storage::AccessCache;
                FolderPicker picker;
                picker.SuggestedStartLocation(
                    winrt::Windows::Storage::Pickers::PickerLocationId::ComputerFolder);
                // A FolderPicker shows nothing until its filter has an entry; the
                // filter is an IVector<hstring>, so the '*' is a real hstring.
                Collections::IVector<winrt::hstring> filters = picker.FileTypeFilter();
                const winrt::hstring any_file = L"*";
                filters.Append(any_file);
                winrt::Windows::Storage::StorageFolder folder = picker.PickSingleFolderAsync().get();
                if (folder) {
                    result.path = winrt::to_string(folder.Path());
                    // The token is what survives a restart; the path is what
                    // this run streams from.
                    AccessCache::StorageItemAccessList recent =
                        AccessCache::StorageApplicationPermissions::FutureAccessList();
                    const winrt::Windows::Storage::IStorageItem item = folder;
                    // Add, not AddOrReplace: clang's C++/WinRT projection does not
                    // resolve the two-argument AddOrReplace overload here, and an
                    // extra entry in a 1,000-entry list costs nothing. The list is
                    // pruned on load (see RestoreDump's ContainsItem check).
                    result.token = winrt::to_string(recent.Add(item));
                }
            } catch (const winrt::hresult_error& error) {
                LogMessage(Level::kWarn, "storage: folder picker failed (0x%08x)",
                           static_cast<unsigned>(error.code().value));
            }
            ::SetEvent(done);
        });
    }).detach();

    const DWORD waited = ::WaitForSingleObject(done, 10 * 60 * 1000);
    ::CloseHandle(done);
    if (waited != WAIT_OBJECT_0 || result.path.empty()) return false;
    path_out = result.path;
    token_out = result.token;
    return true;
}

bool ResolveGrant(const std::string& token, std::string& path_out) {
    if (token.empty()) return false;
    try {
        using namespace winrt::Windows::Storage::AccessCache;
        if (!StorageApplicationPermissions::FutureAccessList().ContainsItem(winrt::to_hstring(token))) {
            return false;
        }
        const auto item = StorageApplicationPermissions::FutureAccessList().GetFolderAsync(
            winrt::to_hstring(token)).get();
        if (!item) return false;
        path_out = winrt::to_string(item.Path());
        return !path_out.empty();
    } catch (const winrt::hresult_error&) {
        return false;
    }
}

bool ReleaseGrant(const std::string& token) {
    if (token.empty()) return false;
    try {
        winrt::Windows::Storage::AccessCache::StorageApplicationPermissions::FutureAccessList()
            .Remove(winrt::to_hstring(token));
        return true;
    } catch (const winrt::hresult_error&) {
        return false;
    }
}

std::vector<std::string> CandidateDumpRoots() {
    std::vector<std::string> roots;
    // Removable drives: the manifest's `removableStorage` capability plus the
    // FromApp find API is the only way to see them, and only the drives the
    // console has handed to this app appear.
    for (wchar_t letter = L'D'; letter <= L'Z'; ++letter) {
        const std::wstring root{letter, L':', L'\\'};
        WIN32_FIND_DATAW data{};
        HANDLE find = ::FindFirstFileExFromAppW((root + L"*").c_str(), FindExInfoBasic, &data,
                                               FindExSearchNameMatch, nullptr, 0);
        if (find == INVALID_HANDLE_VALUE) continue;
        ::FindClose(find);
        roots.push_back(Narrow(root));
    }
    if (!g_paths.local_state.empty()) roots.push_back(g_paths.local_state);
    return roots;
}

DumpFs MakeDumpFs() {
    DumpFs fs;
    fs.exists = [](const std::string& path) { return Exists(path); };
    fs.is_dir = [](const std::string& path) { return IsDir(path); };
    fs.read = [](const std::string& path, std::size_t cap) {
        std::vector<std::uint8_t> bytes;
        const bool ok = ReadBinary(path, bytes, cap);
        if (!ok) return std::vector<std::uint8_t>{};
        return bytes;
    };
    fs.list_dirs = [](const std::string& path) { return ListDirs(path); };
    return fs;
}

bool OpenUsbCacheTarget(const std::string& token, UsbCacheTarget& out) {
    std::string root;
    if (!ResolveGrant(token, root)) return false;
    if (!IsDir(root)) return false;
    out.root = root;
    out.token = token;
    return true;
}

}  // namespace bbx::platform
