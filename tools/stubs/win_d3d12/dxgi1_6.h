// SPDX-License-Identifier: GPL-3.0-or-later
// A stub of <dxgi1_6.h> (and the <dxgi.h> types it re-exports), for
// tools/syntax_check.sh only. Only what the port calls is declared.
#pragma once

#include <windows.h>

struct DXGI_SAMPLE_DESC {
    UINT Count;
    UINT Quality;
};

struct DXGI_RATIONAL {
    UINT Numerator;
    UINT Denominator;
};

enum DXGI_FORMAT : UINT {
    DXGI_FORMAT_UNKNOWN = 0,
    DXGI_FORMAT_R8G8B8A8_UNORM = 28,
    DXGI_FORMAT_R8G8B8A8_UNORM_SRGB = 29,
    DXGI_FORMAT_B8G8R8A8_UNORM = 87,
    DXGI_FORMAT_R10G10B10A2_UNORM = 24,
    DXGI_FORMAT_R16G16B16A16_FLOAT = 10,
    DXGI_FORMAT_D32_FLOAT = 40,
};

enum DXGI_USAGE : UINT {
    DXGI_USAGE_SHADER_INPUT = 0x10,
    DXGI_USAGE_RENDER_TARGET_OUTPUT = 0x20,
    DXGI_USAGE_BACK_BUFFER = 0x40,
};

enum DXGI_SWAP_EFFECT : UINT {
    DXGI_SWAP_EFFECT_DISCARD = 0,
    DXGI_SWAP_EFFECT_SEQUENTIAL = 1,
    DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL = 3,
    DXGI_SWAP_EFFECT_FLIP_DISCARD = 4,
};

enum DXGI_SCALING : UINT {
    DXGI_SCALING_STRETCH = 0,
    DXGI_SCALING_NONE = 1,
    DXGI_SCALING_ASPECT_RATIO_STRETCH = 2,
};

enum DXGI_ALPHA_MODE : UINT {
    DXGI_ALPHA_MODE_UNSPECIFIED = 0,
    DXGI_ALPHA_MODE_PREMULTIPLIED = 1,
    DXGI_ALPHA_MODE_STRAIGHT = 2,
    DXGI_ALPHA_MODE_IGNORE = 3,
};

enum DXGI_SWAP_CHAIN_FLAG : UINT {
    DXGI_SWAP_CHAIN_FLAG_NONPREROTATED = 1,
    DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH = 2,
    DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT = 0x40,
    DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING = 0x200,
};

enum DXGI_GPU_PREFERENCE : UINT {
    DXGI_GPU_PREFERENCE_UNSPECIFIED = 0,
    DXGI_GPU_PREFERENCE_MINIMUM_POWER = 1,
    DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE = 2,
};

enum DXGI_ADAPTER_FLAG : UINT {
    DXGI_ADAPTER_FLAG_NONE = 0,
    DXGI_ADAPTER_FLAG_SOFTWARE = 2,
};

struct DXGI_MATRIX_3X2_F {
    float _11;
    float _12;
    float _21;
    float _22;
    float _31;
    float _32;
};

struct DXGI_ADAPTER_DESC1 {
    wchar_t Description[128];
    UINT VendorId;
    UINT DeviceId;
    UINT SubSysId;
    UINT Revision;
    SIZE_T DedicatedVideoMemory;
    SIZE_T DedicatedSystemMemory;
    SIZE_T SharedSystemMemory;
    // LUID, as the SDK spells it: a struct of high/low parts.
    struct {
        DWORD LowPart;
        LONG HighPart;
    } AdapterLuid;
    UINT Flags;
};

struct DXGI_SWAP_CHAIN_DESC1 {
    UINT Width;
    UINT Height;
    DXGI_FORMAT Format;
    BOOL Stereo;
    DXGI_SAMPLE_DESC SampleDesc;
    DXGI_USAGE BufferUsage;
    UINT BufferCount;
    DXGI_SCALING Scaling;
    DXGI_SWAP_EFFECT SwapEffect;
    DXGI_ALPHA_MODE AlphaMode;
    UINT Flags;
};

constexpr UINT DXGI_CREATE_FACTORY_FLAGS_NONE = 0;
constexpr UINT DXGI_PRESENT_TEST = 0x1;
constexpr UINT DXGI_PRESENT_ALLOW_TEARING = 0x200;
constexpr UINT DXGI_MAX_SWAP_CHAIN_BUFFERS = 16;
constexpr UINT DXGI_CREATE_FACTORY_DEBUG = 0x1;

DECLSPEC_UUID("3d12eda4-1416-4809-9a2b-2f7b9c60f8bf")
struct IDXGIObject : IUnknown {
    virtual HRESULT SetPrivateData(const GUID& name, UINT size, const void* data) = 0;
    virtual HRESULT SetPrivateDataInterface(const GUID& name, const IUnknown* object) = 0;
    virtual HRESULT GetPrivateData(const GUID& name, UINT* size, void* data) = 0;
    virtual HRESULT GetParent(REFIID riid, void** parent) = 0;
};

DECLSPEC_UUID("aec22fb8-76f3-4639-9be0-28eb43a67a2e")
struct IDXGIAdapter : IDXGIObject {
    virtual HRESULT EnumOutputs(UINT index, void** output) = 0;
    virtual HRESULT GetDesc(DXGI_ADAPTER_DESC1* desc) = 0;
    virtual HRESULT CheckInterfaceSupport(const GUID& name, LARGE_INTEGER* umd_version) = 0;
};

DECLSPEC_UUID("29038f61-3839-4626-91fd-086879011a05")
struct IDXGIAdapter1 : IDXGIAdapter {};

DECLSPEC_UUID("3c8d99d1-4fbf-4181-a41a-ef2c0d8cc50e")
struct IDXGIAdapter4 : IDXGIAdapter1 {
    virtual HRESULT GetDesc3(DXGI_ADAPTER_DESC1* desc) = 0;
};

struct IDXGIOutput;
struct IDXGISwapChain1;

DECLSPEC_UUID("1b1a9c2f-3f7f-4b8b-9a3e-2b8a6f5d2d3f")
struct IDXGIFactory : IDXGIObject {};

DECLSPEC_UUID("50c83a1c-e072-4c48-87b0-3630fa36a6d0")
struct IDXGIFactory2 : IDXGIFactory {
    virtual HRESULT CreateSwapChainForCoreWindow(IUnknown* queue, IUnknown* window,
                                                const DXGI_SWAP_CHAIN_DESC1* desc,
                                                IDXGIOutput* restrict_to_output,
                                                IDXGISwapChain1** swap_chain) = 0;
    virtual HRESULT CreateSwapChainForHwnd(IUnknown* queue, HWND window,
                                           const DXGI_SWAP_CHAIN_DESC1* desc,
                                           const void* full_screen_desc,
                                           IDXGIOutput* restrict_to_output,
                                           IDXGISwapChain1** swap_chain) = 0;
    virtual HRESULT CreateSwapChainForComposition(IUnknown* queue, const DXGI_SWAP_CHAIN_DESC1* desc,
                                                  IDXGIOutput* restrict_to_output,
                                                  IDXGISwapChain1** swap_chain) = 0;
    virtual BOOL IsCurrent() = 0;
};

DECLSPEC_UUID("c1b6694f-ff09-44a9-b03c-77900a0a1d17")
struct IDXGISwapChain : IDXGIObject {
    virtual HRESULT Present(UINT sync_interval, UINT flags) = 0;
    virtual HRESULT GetBuffer(UINT index, REFIID riid, void** surface) = 0;
    virtual HRESULT SetFullscreenState(BOOL fullscreen, IDXGIOutput* target) = 0;
    virtual HRESULT GetFullscreenState(BOOL* fullscreen, IDXGIOutput** target) = 0;
    virtual HRESULT GetDesc(DXGI_SWAP_CHAIN_DESC1* desc) = 0;
    virtual HRESULT ResizeBuffers(UINT count, UINT width, UINT height, DXGI_FORMAT format,
                                 UINT flags) = 0;
    virtual HRESULT ResizeTarget(const void* new_target_parameters) = 0;
    virtual HRESULT GetContainingOutput(IDXGIOutput** output) = 0;
    virtual HRESULT GetFrameStatistics(void* stats) = 0;
    virtual HRESULT GetLastPresentCount(UINT* last_present_count) = 0;
};

DECLSPEC_UUID("790a45f7-0d42-4876-983a-0a55cfe6f4aa")
struct IDXGISwapChain1 : IDXGISwapChain {
    virtual HRESULT GetDesc1(DXGI_SWAP_CHAIN_DESC1* desc) = 0;
    virtual HRESULT GetFullscreenDesc(void* desc) = 0;
    virtual HRESULT GetHwnd(HWND* hwnd) = 0;
    virtual HRESULT GetCoreWindow(REFIID riid, void** core_window) = 0;
    virtual HRESULT Present1(UINT sync_interval, UINT flags, const void* parameters) = 0;
    virtual BOOL IsTemporaryMonoSupported() = 0;
    virtual HRESULT GetRestrictToOutput(IDXGIOutput** output) = 0;
    virtual HRESULT SetBackgroundColor(const float* color) = 0;
    virtual HRESULT GetBackgroundColor(float* color) = 0;
    virtual HRESULT SetRotation(UINT rotation) = 0;
    virtual HRESULT GetRotation(UINT* rotation) = 0;
};

DECLSPEC_UUID("94d99bdb-f1f8-4ab0-b236-7da0170edab1")
struct IDXGISwapChain2 : IDXGISwapChain1 {
    virtual HRESULT SetSourceSize(UINT width, UINT height) = 0;
    virtual HRESULT GetSourceSize(UINT* width, UINT* height) = 0;
    virtual HRESULT SetMaximumFrameLatency(UINT max_latency) = 0;
    virtual HRESULT GetMaximumFrameLatency(UINT* max_latency) = 0;
    virtual HANDLE GetFrameLatencyWaitableObject() = 0;
    virtual HRESULT SetMatrixTransform(const DXGI_MATRIX_3X2_F* matrix) = 0;
    virtual HRESULT GetMatrixTransform(DXGI_MATRIX_3X2_F* matrix) = 0;
};

DECLSPEC_UUID("a8be2ac4-199f-4946-b331-79599fb98de7")
struct IDXGISwapChain3 : IDXGISwapChain2 {
    virtual UINT GetCurrentBackBufferIndex() = 0;
    virtual HRESULT CheckColorSpaceSupport(DXGI_FORMAT format, UINT* support) = 0;
    virtual HRESULT SetColorSpace1(DXGI_FORMAT format) = 0;
    virtual HRESULT ResizeBuffers1(UINT count, UINT width, UINT height, DXGI_FORMAT format,
                                  UINT flags, const UINT* node_mask, const IUnknown* const* queues) = 0;
};

DECLSPEC_UUID("1bc6ea02-ef36-464f-bf0c-21ca39e5168a")
struct IDXGISwapChain4 : IDXGISwapChain3 {
    virtual HRESULT SetHDRMetaData(UINT type, UINT size, void* metadata) = 0;
};

DECLSPEC_UUID("3c9d1d1a-3f8a-4f4b-8b0f-7a0d0b1c9e2f")
struct IDXGIFactory6 : IDXGIFactory2 {
    virtual HRESULT EnumAdapterByGpuPreference(UINT index, DXGI_GPU_PREFERENCE preference,
                                               REFIID riid, void** adapter) = 0;
    virtual HRESULT EnumAdapterByLuid(const GUID& luid, REFIID riid, void** adapter) = 0;
    virtual HRESULT EnumWarpAdapter(REFIID riid, void** adapter) = 0;
};

constexpr GUID IDXGIFactory6_Uuid = {0xc1b6694f, 0xff09, 0x44a9, {0xb0, 0x3c, 0x77, 0x90, 0x0a, 0x0a, 0x1d, 0x17}};

HRESULT CreateDXGIFactory2(UINT flags, REFIID riid, void** factory);
HRESULT DXGIGetDebugInterface1(UINT flags, REFIID riid, void** debug);
constexpr UINT DXGI_DEBUG_ALL_INDEX = 0;
