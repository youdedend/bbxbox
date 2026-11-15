// SPDX-License-Identifier: GPL-3.0-or-later
// A stub of <windows.h>, for tools/syntax_check.sh only.
//
// It is not a Windows emulator and never gets linked: `clang++ -fsyntax-only`
// reads it so that the console translation units - which cannot be compiled on a
// Linux machine - are still type-checked in under a second on every precheck, and
// so that a wrong parameter list is caught here rather than in a 3-minute CI run.
//
// The declarations below are the ones the port's Windows code actually uses. When
// a translation unit needs one that is missing, add it here: that is cheaper than
// discovering the mismatch from a Windows runner. The real SDK remains the
// authority - `UWP build + import audit` compiles everything against it.
#pragma once

#ifndef _WIN32
#define _WIN32 1
#endif

// --- scalar types ------------------------------------------------------------
using BOOL = int;
using BOOLEAN = unsigned char;
using BYTE = unsigned char;
using UINT = unsigned int;
using UINT8 = unsigned char;
using UINT16 = unsigned short;
using UINT32 = unsigned int;
using UINT64 = unsigned long long;
using INT = int;
using INT32 = int;
using LONG = long;
using ULONG = unsigned long;
using DWORD = unsigned long;
using WORD = unsigned short;
using HRESULT = long;
using LPCSTR = const char*;
using LPCWSTR = const wchar_t*;
using LPWSTR = wchar_t*;
using PCWSTR = const wchar_t*;
using WCHAR = wchar_t;
using SIZE_T = unsigned long long;
using UINT_PTR = unsigned long long;
using INT_PTR = long long;
using LPVOID = void*;
using HANDLE = void*;
using HWND = void*;
using HMODULE = void*;
using HINSTANCE = void*;

#define WINAPI
#define APIENTRY
#define STDMETHODCALLTYPE
#define STDAPICALLTYPE
#define CALLBACK
#define TRUE 1
#define FALSE 0
#define IN
#define OUT
#define CONST const
#define VOID void
#define MAX_PATH 260
#define INFINITE 0xFFFFFFFFu
#define WAIT_OBJECT_0 0
#define WAIT_TIMEOUT 258
#define WAIT_FAILED 0xFFFFFFFFu
#define DLL_PROCESS_ATTACH 1
#define EXCEPTION_EXECUTE_HANDLER 1

// --- COM ---------------------------------------------------------------------
struct IUnknown {
    virtual HRESULT QueryInterface(const void* riid, void** out) = 0;
    virtual ULONG AddRef() = 0;
    virtual ULONG Release() = 0;
};

using REFIID = const void*;
using REFCLSID = const void*;

// clang models the MSVC calling convention and __uuidof without the real SDK.
#ifndef GUID_DEFINED
#define GUID_DEFINED
struct GUID {
    unsigned long Data1;
    unsigned short Data2;
    unsigned short Data3;
    unsigned char Data4[8];
};
struct IID : GUID {};
struct CLSID : GUID {};
#endif

// clang and MSVC have __uuidof; a GCC syntax check does not. There the interface
// ids are irrelevant (nothing is linked and no QueryInterface runs), so they
// collapse to a null REFIID, which every stub parameter accepts.
#if defined(__clang__) || defined(_MSC_VER)
#define DECLSPEC_UUID(x) __declspec(uuid(x))
#else
#define DECLSPEC_UUID(x)
#ifndef __uuidof
#define __uuidof(x) (static_cast<const void*>(nullptr))
#endif
#endif
#define MIDL_INTERFACE(x) struct DECLSPEC_UUID(x)

template <class T>
constexpr const void* __bbx_iid_of() {
    return nullptr;
}

#ifndef IID_PPV_ARGS
#define IID_PPV_ARGS(ppType) __uuidof(**(ppType)), reinterpret_cast<void**>(ppType)
#endif

// --- errors ------------------------------------------------------------------
constexpr HRESULT S_OK = 0;
constexpr HRESULT S_FALSE = 1;
constexpr HRESULT E_FAIL = static_cast<HRESULT>(0x80004005L);
constexpr HRESULT E_INVALIDARG = static_cast<HRESULT>(0x80070057L);
constexpr HRESULT E_OUTOFMEMORY = static_cast<HRESULT>(0x8007000EL);
constexpr HRESULT E_NOTIMPL = static_cast<HRESULT>(0x80004001L);
constexpr HRESULT E_NOINTERFACE = static_cast<HRESULT>(0x80004002L);
constexpr HRESULT E_ABORT = static_cast<HRESULT>(0x80004004L);
constexpr HRESULT E_ACCESSDENIED = static_cast<HRESULT>(0x80070005L);
constexpr HRESULT E_UNEXPECTED = static_cast<HRESULT>(0x8000FFFFL);

constexpr bool SUCCEEDED(HRESULT hr) { return hr >= 0; }
constexpr bool FAILED(HRESULT hr) { return hr < 0; }

constexpr HRESULT HRESULT_FROM_WIN32(unsigned long code) {
    return static_cast<HRESULT>(code <= 0 ? code : (0x80070000L | code));
}
constexpr unsigned long HRESULT_CODE(HRESULT hr) { return static_cast<unsigned long>(hr) & 0xFFFFu; }

// --- handles, sync, memory ---------------------------------------------------
struct SECURITY_ATTRIBUTES;
using LPSECURITY_ATTRIBUTES = SECURITY_ATTRIBUTES*;

struct LARGE_INTEGER {
    long long QuadPart;
};

HANDLE CreateEventExW(LPSECURITY_ATTRIBUTES attributes, LPCWSTR name, DWORD flags, DWORD access);
HANDLE CreateEventW(LPSECURITY_ATTRIBUTES attributes, BOOL manual_reset, BOOL initial_state, LPCWSTR name);
BOOL SetEvent(HANDLE event);
BOOL CloseHandle(HANDLE handle);
DWORD WaitForSingleObject(HANDLE handle, DWORD milliseconds);
DWORD WaitForSingleObjectEx(HANDLE handle, DWORD milliseconds, BOOL alertable);
BOOL GetLastErrorAsBool();
DWORD GetLastError();
void Sleep(DWORD milliseconds);
DWORD GetCurrentThreadId();

template <class T>
void SecureZeroMemory(T* destination, SIZE_T bytes) {
    auto* bytes_ptr = reinterpret_cast<volatile unsigned char*>(destination);
    while (bytes > 0) {
        *bytes_ptr++ = 0;
        --bytes;
    }
}
void ZeroMemory(void* destination, SIZE_T bytes);
void CopyMemory(void* destination, const void* source, SIZE_T bytes);
