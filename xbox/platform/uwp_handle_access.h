// SPDX-License-Identifier: GPL-3.0-or-later
// UWP COM interop declarations for IStorageItemHandleAccess and
// IStorageFolderHandleAccess (identical to Windows SDK's WindowsStorageCOM.h,
// which is gated behind WINAPI_PARTITION_DESKTOP even though the interfaces
// are implemented and supported on Xbox UWP / Windows 10+).
//
// Pulling a native Win32 HANDLE directly from a FutureAccessList StorageFile
// lets host_fs_uwp.cpp read granted USB dumps via ReadFile/SetFilePointerEx
// instead of bouncing every read through WinRT IRandomAccessStream::ReadAsync.
#pragma once

#include <windows.h>
#if __has_include(<unknwn.h>)
#include <unknwn.h>
#endif

#ifndef __IStorageItemHandleAccess_INTERFACE_DEFINED__
#define __IStorageItemHandleAccess_INTERFACE_DEFINED__

enum HANDLE_OPTIONS : unsigned int {
    HO_NONE = 0,
    HO_OPEN_REQUIRING_OPLOCK = 0x40000,
    HO_DELETE_ON_CLOSE = 0x4000000,
    HO_SEQUENTIAL_SCAN = 0x8000000,
    HO_RANDOM_ACCESS = 0x10000000,
    HO_NO_BUFFERING = 0x20000000,
    HO_OVERLAPPED = 0x40000000,
    HO_WRITE_THROUGH = 0x80000000,
};

enum HANDLE_ACCESS_OPTIONS : unsigned int {
    HAO_NONE = 0,
    HAO_READ_ATTRIBUTES = 0x80,
    HAO_READ = 0x120089,
    HAO_WRITE = 0x120116,
    HAO_DELETE = 0x10000,
};

enum HANDLE_SHARING_OPTIONS : unsigned int {
    HSO_SHARE_NONE = 0,
    HSO_SHARE_READ = 0x1,
    HSO_SHARE_WRITE = 0x2,
    HSO_SHARE_DELETE = 0x4,
};

enum HANDLE_CREATION_OPTIONS : unsigned int {
    HCO_CREATE_NEW = 0x1,
    HCO_CREATE_ALWAYS = 0x2,
    HCO_OPEN_EXISTING = 0x3,
    HCO_OPEN_ALWAYS = 0x4,
    HCO_TRUNCATE_EXISTING = 0x5,
};

struct __declspec(uuid("826ABE3D-3ACD-47D3-84F2-88AAEDCF6304")) IOplockBreakingHandler : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE OplockBreaking(void) = 0;
};

struct __declspec(uuid("5CA296B2-2C25-4D22-B785-B885C8201E6A")) IStorageItemHandleAccess : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Create(HANDLE_ACCESS_OPTIONS accessOptions,
                                             HANDLE_SHARING_OPTIONS sharingOptions,
                                             HANDLE_OPTIONS options,
                                             IOplockBreakingHandler* oplockBreakingHandler,
                                             HANDLE* interopHandle) = 0;
};

struct __declspec(uuid("DF19938F-5462-48A0-BE65-D2A3271A08D6")) IStorageFolderHandleAccess
    : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Create(LPCWSTR fileName,
                                             HANDLE_CREATION_OPTIONS creationOptions,
                                             HANDLE_ACCESS_OPTIONS accessOptions,
                                             HANDLE_SHARING_OPTIONS sharingOptions,
                                             HANDLE_OPTIONS options,
                                             IOplockBreakingHandler* oplockBreakingHandler,
                                             HANDLE* interopHandle) = 0;
};

#endif  // __IStorageItemHandleAccess_INTERFACE_DEFINED__
