// SPDX-License-Identifier: GPL-3.0-or-later
// A stub of <d3d12.h>, for tools/syntax_check.sh only. It declares the interfaces
// and enums the port's D3D12 code uses, with the SDK's member order, so a wrong
// argument list or a missing include fails on a Linux precheck.
//
// Deliberately absent: ID3D12Device::CreatePipelineLibrary's use in the port -
// the interface exists here because the SDK has it, but no translation unit calls
// it (CreatePipelineLibrary removes the device on a console).
#pragma once

#include <windows.h>
#include <dxgi1_6.h>

// Forward declarations first: the interfaces reference each other in the SDK's
// own order.
struct ID3D12Object;
struct ID3D12DeviceChild;
struct ID3D12Pageable;
struct ID3D12Device;
struct ID3D12CommandQueue;
struct ID3D12CommandAllocator;
struct ID3D12CommandList;
struct ID3D12GraphicsCommandList;
struct ID3D12Fence;
struct ID3D12Resource;

// A GPU virtual address is a plain 64-bit value in the SDK; a typedef keeps the
// declaration honest without needing the DXGI header's spelling.
using D3D12_GPU_VIRTUAL_ADDRESS = unsigned long long;
struct ID3D12DescriptorHeap;
struct ID3D12RootSignature;
struct ID3D12PipelineState;
struct ID3DBlob;

struct D3D12_ROOT_SIGNATURE_DESC;
struct D3D12_DESCRIPTOR_RANGE;
struct D3D12_ROOT_PARAMETER;
struct D3D12_STATIC_SAMPLER_DESC;
struct D3D12_SHADER_BYTECODE;
struct D3D12_INPUT_LAYOUT_DESC;
struct D3D12_BLEND_DESC;
struct D3D12_RASTERIZER_DESC;
struct D3D12_DEPTH_STENCIL_DESC;
struct D3D12_CACHED_PIPELINE_STATE;

struct D3D12_COMMAND_SIGNATURE_DESC;
struct D3D12_ROOT_SIGNATURE_DESC;
struct D3D12_GRAPHICS_PIPELINE_STATE_DESC;
struct D3D12_COMPUTE_PIPELINE_STATE_DESC;
struct D3D12_RESOURCE_BARRIER;
struct D3D12_RESOURCE_DESC;
struct D3D12_HEAP_PROPERTIES;
struct D3D12_HEAP_DESC;
struct D3D12_RESOURCE_ALLOCATION_INFO;
struct D3D12_DESCRIPTOR_HEAP_DESC;
struct D3D12_CPU_DESCRIPTOR_HANDLE;
struct D3D12_GPU_DESCRIPTOR_HANDLE;
struct D3D12_VERTEX_BUFFER_VIEW;
struct D3D12_INDEX_BUFFER_VIEW;
struct D3D12_INDEX_BUFFER_VIEW;
struct D3D12_VIEWPORT;
struct D3D12_RECT;
struct D3D12_RANGE;
struct D3D12_RENDER_TARGET_VIEW_DESC;
struct D3D12_DEPTH_STENCIL_VIEW_DESC;
struct D3D12_SHADER_RESOURCE_VIEW_DESC;
struct D3D12_UNORDERED_ACCESS_VIEW_DESC;
struct D3D12_CONSTANT_BUFFER_VIEW_DESC;
struct D3D12_SAMPLER_DESC;
struct D3D12_ROOT_PARAMETER;
struct D3D12_STATIC_SAMPLER_DESC;
struct D3D12_CLEAR_VALUE;
struct D3D12_QUERY_HEAP_DESC;

enum D3D_FEATURE_LEVEL : UINT {
    D3D_FEATURE_LEVEL_1_0_CORE = 0x1000,
    D3D_FEATURE_LEVEL_9_1 = 0x9100,
    D3D_FEATURE_LEVEL_9_3 = 0x9300,
    D3D_FEATURE_LEVEL_10_0 = 0xa000,
    D3D_FEATURE_LEVEL_10_1 = 0xa100,
    D3D_FEATURE_LEVEL_11_0 = 0xb000,
    D3D_FEATURE_LEVEL_11_1 = 0xb100,
    D3D_FEATURE_LEVEL_12_0 = 0xc000,
    D3D_FEATURE_LEVEL_12_1 = 0xc100,
    D3D_FEATURE_LEVEL_12_2 = 0xc200,
};

enum D3D12_COMMAND_LIST_TYPE : UINT {
    D3D12_COMMAND_LIST_TYPE_DIRECT = 0,
    D3D12_COMMAND_LIST_TYPE_BUNDLE = 1,
    D3D12_COMMAND_LIST_TYPE_COMPUTE = 2,
    D3D12_COMMAND_LIST_TYPE_COPY = 3,
    D3D12_COMMAND_LIST_TYPE_VIDEO_DECODE = 4,
};

enum D3D12_COMMAND_QUEUE_FLAGS : UINT {
    D3D12_COMMAND_QUEUE_FLAG_NONE = 0,
    D3D12_COMMAND_QUEUE_FLAG_DISABLE_GPU_TIMEOUT = 0x1,
};

enum D3D12_COMMAND_QUEUE_PRIORITY : UINT {
    D3D12_COMMAND_QUEUE_PRIORITY_NORMAL = 0,
    D3D12_COMMAND_QUEUE_PRIORITY_HIGH = 100,
};

enum D3D12_DESCRIPTOR_HEAP_TYPE : UINT {
    D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV = 0,
    D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER = 1,
    D3D12_DESCRIPTOR_HEAP_TYPE_RTV = 2,
    D3D12_DESCRIPTOR_HEAP_TYPE_DSV = 3,
    D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES = 4,
};

enum D3D12_DESCRIPTOR_HEAP_FLAGS : UINT {
    D3D12_DESCRIPTOR_HEAP_FLAG_NONE = 0,
    D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE = 0x1,
};

enum D3D12_HEAP_TYPE : UINT {
    D3D12_HEAP_TYPE_DEFAULT = 1,
    D3D12_HEAP_TYPE_UPLOAD = 2,
    D3D12_HEAP_TYPE_READBACK = 3,
    D3D12_HEAP_TYPE_CUSTOM = 4,
};

enum D3D12_CPU_PAGE_PROPERTY : UINT {
    D3D12_CPU_PAGE_PROPERTY_UNKNOWN = 0,
    D3D12_CPU_PAGE_PROPERTY_NOT_AVAILABLE = 1,
    D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE = 2,
    D3D12_CPU_PAGE_PROPERTY_WRITE_BACK = 3,
};

enum D3D12_MEMORY_POOL : UINT {
    D3D12_MEMORY_POOL_UNKNOWN = 0,
    D3D12_MEMORY_POOL_L0 = 1,
    D3D12_MEMORY_POOL_L1 = 2,
};

enum D3D12_RESOURCE_DIMENSION : UINT {
    D3D12_RESOURCE_DIMENSION_UNKNOWN = 0,
    D3D12_RESOURCE_DIMENSION_BUFFER = 1,
    D3D12_RESOURCE_DIMENSION_TEXTURE1D = 2,
    D3D12_RESOURCE_DIMENSION_TEXTURE2D = 3,
    D3D12_RESOURCE_DIMENSION_TEXTURE3D = 4,
};

enum D3D12_TEXTURE_LAYOUT : UINT {
    D3D12_TEXTURE_LAYOUT_UNKNOWN = 0,
    D3D12_TEXTURE_LAYOUT_ROW_MAJOR = 1,
    D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE = 2,
    D3D12_TEXTURE_LAYOUT_64KB_STANDARD_SWIZZLE = 3,
};

enum D3D12_RESOURCE_FLAGS : UINT {
    D3D12_RESOURCE_FLAG_NONE = 0,
    D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET = 0x1,
    D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL = 0x2,
    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS = 0x4,
    D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE = 0x8,
};

enum D3D12_RESOURCE_BARRIER_TYPE : UINT {
    D3D12_RESOURCE_BARRIER_TYPE_TRANSITION = 0,
    D3D12_RESOURCE_BARRIER_TYPE_ALIASING = 1,
    D3D12_RESOURCE_BARRIER_TYPE_UAV = 2,
};

enum D3D12_RESOURCE_BARRIER_FLAGS : UINT {
    D3D12_RESOURCE_BARRIER_FLAG_NONE = 0,
    D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY = 0x1,
    D3D12_RESOURCE_BARRIER_FLAG_END_ONLY = 0x2,
};

// The SDK's RESOURCE_STATES is a bit mask, not an enum class.
using D3D12_RESOURCE_STATES = UINT;
constexpr D3D12_RESOURCE_STATES D3D12_RESOURCE_STATE_COMMON = 0;
constexpr D3D12_RESOURCE_STATES D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER = 0x1;
constexpr D3D12_RESOURCE_STATES D3D12_RESOURCE_STATE_INDEX_BUFFER = 0x2;
constexpr D3D12_RESOURCE_STATES D3D12_RESOURCE_STATE_RENDER_TARGET = 0x4;
constexpr D3D12_RESOURCE_STATES D3D12_RESOURCE_STATE_UNORDERED_ACCESS = 0x8;
constexpr D3D12_RESOURCE_STATES D3D12_RESOURCE_STATE_DEPTH_WRITE = 0x10;
constexpr D3D12_RESOURCE_STATES D3D12_RESOURCE_STATE_DEPTH_READ = 0x20;
constexpr D3D12_RESOURCE_STATES D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE = 0x40;
constexpr D3D12_RESOURCE_STATES D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE = 0x80;
constexpr D3D12_RESOURCE_STATES D3D12_RESOURCE_STATE_COPY_DEST = 0x400;
constexpr D3D12_RESOURCE_STATES D3D12_RESOURCE_STATE_COPY_SOURCE = 0x800;
constexpr D3D12_RESOURCE_STATES D3D12_RESOURCE_STATE_GENERIC_READ = 0xAC3;
constexpr D3D12_RESOURCE_STATES D3D12_RESOURCE_STATE_PRESENT = 0;
constexpr D3D12_RESOURCE_STATES D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE = 0xC0;

enum D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES : UINT { D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES = 0xFFFFFFFF };

enum D3D12_HEAP_FLAGS : UINT {
    D3D12_HEAP_FLAG_NONE = 0,
    D3D12_HEAP_FLAG_SHARED = 0x1,
    D3D12_HEAP_FLAG_DENY_BUFFERS = 0x4,
    D3D12_HEAP_FLAG_ALLOW_DISPLAY = 0x8,
};

enum D3D12_FENCE_FLAGS : UINT {
    D3D12_FENCE_FLAG_NONE = 0,
    D3D12_FENCE_FLAG_SHARED = 0x1,
};

enum D3D12_MULTISAMPLE_QUALITY_LEVEL_FLAGS : UINT {
    D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE = 0,
};

enum D3D12_BLEND : UINT {
    D3D12_BLEND_ZERO = 1,
    D3D12_BLEND_ONE = 2,
    D3D12_BLEND_SRC_ALPHA = 5,
    D3D12_BLEND_INV_SRC_ALPHA = 6,
    D3D12_BLEND_SRC_COLOR = 3,
    D3D12_BLEND_INV_SRC_COLOR = 4,
    D3D12_BLEND_DEST_ALPHA = 7,
    D3D12_BLEND_INV_DEST_ALPHA = 8,
    D3D12_BLEND_DEST_COLOR = 9,
};

enum D3D12_BLEND_OP : UINT {
    D3D12_BLEND_OP_ADD = 1,
    D3D12_BLEND_OP_SUBTRACT = 2,
    D3D12_BLEND_OP_REV_SUBTRACT = 3,
    D3D12_BLEND_OP_MIN = 4,
    D3D12_BLEND_OP_MAX = 5,
};

enum D3D12_COMPARISON_FUNC : UINT {
    D3D12_COMPARISON_FUNC_NEVER = 1,
    D3D12_COMPARISON_FUNC_LESS = 2,
    D3D12_COMPARISON_FUNC_EQUAL = 3,
    D3D12_COMPARISON_FUNC_LESS_EQUAL = 4,
    D3D12_COMPARISON_FUNC_GREATER = 5,
    D3D12_COMPARISON_FUNC_NOT_EQUAL = 6,
    D3D12_COMPARISON_FUNC_GREATER_EQUAL = 7,
    D3D12_COMPARISON_FUNC_ALWAYS = 8,
};

enum D3D12_DEPTH_WRITE_MASK : UINT {
    D3D12_DEPTH_WRITE_MASK_ZERO = 0,
    D3D12_DEPTH_WRITE_MASK_ALL = 1,
};

enum D3D12_STENCIL_OP : UINT {
    D3D12_STENCIL_OP_KEEP = 1,
    D3D12_STENCIL_OP_ZERO = 2,
    D3D12_STENCIL_OP_REPLACE = 3,
};

enum D3D12_PRIMITIVE_TOPOLOGY_TYPE : UINT {
    D3D12_PRIMITIVE_TOPOLOGY_TYPE_UNDEFINED = 0,
    D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT = 1,
    D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE = 2,
    D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE = 3,
    D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH = 4,
};

enum D3D12_PRIMITIVE_TOPOLOGY : UINT {
    D3D_PRIMITIVE_TOPOLOGY_UNDEFINED = 0,
    D3D_PRIMITIVE_TOPOLOGY_POINTLIST = 1,
    D3D_PRIMITIVE_TOPOLOGY_LINELIST = 2,
    D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST = 4,
    D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP = 5,
};

enum D3D12_INPUT_CLASSIFICATION : UINT {
    D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA = 0,
    D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA = 1,
};

enum D3D12_FILL_MODE : UINT { D3D12_FILL_MODE_WIREFRAME = 2, D3D12_FILL_MODE_SOLID = 3 };
enum D3D12_CULL_MODE : UINT { D3D12_CULL_MODE_NONE = 1, D3D12_CULL_MODE_FRONT = 2, D3D12_CULL_MODE_BACK = 3 };

enum D3D12_SHADER_COMPONENT_MAPPING : UINT {
    D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0 = 0,
    D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_1 = 1,
    D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_2 = 2,
    D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_3 = 3,
};
constexpr UINT D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING = 0x1688;
constexpr UINT D3D12_APPEND_ALIGNED_ELEMENT = 0xFFFFFFFF;

enum D3D12_FILTER : UINT {
    D3D12_FILTER_MIN_MAG_MIP_POINT = 0,
    D3D12_FILTER_MIN_MAG_MIP_LINEAR = 0x15,
    D3D12_FILTER_ANISOTROPIC = 0x55,
    D3D12_FILTER_COMPARISON_MIN_MAG_MIP_POINT = 0x80,
};
enum D3D12_TEXTURE_ADDRESS_MODE : UINT {
    D3D12_TEXTURE_ADDRESS_MODE_WRAP = 1,
    D3D12_TEXTURE_ADDRESS_MODE_MIRROR = 2,
    D3D12_TEXTURE_ADDRESS_MODE_CLAMP = 3,
    D3D12_TEXTURE_ADDRESS_MODE_BORDER = 4,
};
enum D3D12_SHADER_VISIBILITY : UINT {
    D3D12_SHADER_VISIBILITY_ALL = 0,
    D3D12_SHADER_VISIBILITY_VERTEX = 1,
    D3D12_SHADER_VISIBILITY_PIXEL = 2,
};
enum D3D12_ROOT_PARAMETER_TYPE : UINT {
    D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE = 0,
    D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS = 1,
    D3D12_ROOT_PARAMETER_TYPE_CBV = 2,
    D3D12_ROOT_PARAMETER_TYPE_SRV = 3,
    D3D12_ROOT_PARAMETER_TYPE_UAV = 4,
};
enum D3D12_DESCRIPTOR_RANGE_TYPE : UINT {
    D3D12_DESCRIPTOR_RANGE_TYPE_SRV = 0,
    D3D12_DESCRIPTOR_RANGE_TYPE_UAV = 1,
    D3D12_DESCRIPTOR_RANGE_TYPE_CBV = 2,
    D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER = 3,
};
enum D3D12_ROOT_SIGNATURE_FLAGS : UINT {
    D3D12_ROOT_SIGNATURE_FLAG_NONE = 0,
    D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT = 0x1,
    D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS = 0x2,
    D3D12_ROOT_SIGNATURE_FLAG_DENY_PIXEL_SHADER_ROOT_ACCESS = 0x4,
    D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS = 0x8,
    D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS = 0x10,
    D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS = 0x20,
};
// The SDK's name is D3D_ROOT_SIGNATURE_VERSION (shared with D3D12CreateRootSignature-
// Deserializer), not D3D12_ROOT_SIGNATURE_VERSION.
enum D3D_ROOT_SIGNATURE_VERSION : UINT {
    D3D_ROOT_SIGNATURE_VERSION_1 = 0x1,
    D3D_ROOT_SIGNATURE_VERSION_1_0 = 0x1,
    D3D_ROOT_SIGNATURE_VERSION_1_1 = 0x2,
};
enum D3D12_PIPELINE_STATE_FLAGS : UINT {
    D3D12_PIPELINE_STATE_FLAG_NONE = 0,
    D3D12_PIPELINE_STATE_FLAG_TOOL_DEBUG = 0x1,
};

struct D3D12_RESOURCE_TRANSITION_BARRIER {
    void* pResource;
    UINT Subresource;
    D3D12_RESOURCE_STATES StateBefore;
    D3D12_RESOURCE_STATES StateAfter;
};

struct D3D12_RESOURCE_UAV_BARRIER {
    void* pResource;
};

struct D3D12_RESOURCE_ALIASING_BARRIER {
    void* pResourceBefore;
    void* pResourceAfter;
};

// A union in the SDK; a struct with all three members keeps the same field names,
// so every `.Transition.…` / `.Aliasing.…` / `.UAV.…` use type-checks identically.
struct D3D12_RESOURCE_BARRIER {
    D3D12_RESOURCE_BARRIER_TYPE Type;
    D3D12_RESOURCE_BARRIER_FLAGS Flags;
    D3D12_RESOURCE_TRANSITION_BARRIER Transition;
    D3D12_RESOURCE_ALIASING_BARRIER Aliasing;
    D3D12_RESOURCE_UAV_BARRIER UAV;
};

// D3D12_RESOURCE_STATES is a plain UINT bit mask, so the built-in operators are
// the ones the SDK's callers use; no overloads here.

struct D3D12_CPU_DESCRIPTOR_HANDLE {
    SIZE_T ptr = 0;
};

struct D3D12_VERTEX_BUFFER_VIEW {
    D3D12_GPU_VIRTUAL_ADDRESS BufferLocation = 0;
    UINT SizeInBytes = 0;
    UINT StrideInBytes = 0;
};

struct D3D12_INDEX_BUFFER_VIEW {
    D3D12_GPU_VIRTUAL_ADDRESS BufferLocation = 0;
    UINT SizeInBytes = 0;
    DXGI_FORMAT Format = DXGI_FORMAT_UNKNOWN;
};

struct D3D12_GPU_DESCRIPTOR_HANDLE {
    UINT64 ptr = 0;
};

struct D3D12_HEAP_PROPERTIES {
    D3D12_HEAP_TYPE Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_CPU_PAGE_PROPERTY CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    D3D12_MEMORY_POOL MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    UINT CreationNodeMask = 1;
    UINT VisibleNodeMask = 1;
};

struct D3D12_HEAP_DESC {
    UINT64 SizeInBytes = 0;
    D3D12_HEAP_PROPERTIES Properties;
    UINT64 Alignment = 0;
    D3D12_HEAP_FLAGS Flags = D3D12_HEAP_FLAG_NONE;
};

struct D3D12_RESOURCE_ALLOCATION_INFO {
    UINT64 SizeInBytes = 0;
    UINT64 Alignment = 0;
};

struct D3D12_RESOURCE_DESC {
    D3D12_RESOURCE_DIMENSION Dimension = D3D12_RESOURCE_DIMENSION_UNKNOWN;
    UINT64 Alignment = 0;
    UINT64 Width = 0;
    UINT Height = 1;
    UINT16 DepthOrArraySize = 1;
    UINT16 MipLevels = 1;
    DXGI_FORMAT Format = DXGI_FORMAT_UNKNOWN;
    DXGI_SAMPLE_DESC SampleDesc{1, 0};
    D3D12_TEXTURE_LAYOUT Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    D3D12_RESOURCE_FLAGS Flags = D3D12_RESOURCE_FLAG_NONE;
};

struct D3D12_DESCRIPTOR_HEAP_DESC {
    D3D12_DESCRIPTOR_HEAP_TYPE Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    UINT NumDescriptors = 0;
    D3D12_DESCRIPTOR_HEAP_FLAGS Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    UINT NodeMask = 0;
};

struct D3D12_COMMAND_QUEUE_DESC {
    D3D12_COMMAND_LIST_TYPE Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    INT Priority = 0;
    D3D12_COMMAND_QUEUE_FLAGS Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    UINT NodeMask = 0;
};

struct D3D12_VIEWPORT {
    float TopLeftX = 0, TopLeftY = 0, Width = 0, Height = 0, MinDepth = 0, MaxDepth = 1;
};

struct D3D12_RECT {
    LONG left = 0, top = 0, right = 0, bottom = 0;
};

struct D3D12_RANGE {
    SIZE_T Begin = 0;
    SIZE_T End = 0;
};

enum D3D12_RTV_DIMENSION : UINT {
    D3D12_RTV_DIMENSION_UNKNOWN = 0,
    D3D12_RTV_DIMENSION_BUFFER = 1,
    D3D12_RTV_DIMENSION_TEXTURE1D = 2,
    D3D12_RTV_DIMENSION_TEXTURE1DARRAY = 3,
    D3D12_RTV_DIMENSION_TEXTURE2D = 4,
    D3D12_RTV_DIMENSION_TEXTURE2DARRAY = 5,
    D3D12_RTV_DIMENSION_TEXTURE2DMS = 6,
    D3D12_RTV_DIMENSION_TEXTURE3D = 8,
};

struct D3D12_TEX2D_RTV {
    UINT MipSlice = 0;
    UINT PlaneSlice = 0;
};

struct D3D12_RENDER_TARGET_VIEW_DESC {
    DXGI_FORMAT Format = DXGI_FORMAT_UNKNOWN;
    D3D12_RTV_DIMENSION ViewDimension = D3D12_RTV_DIMENSION_UNKNOWN;
    D3D12_TEX2D_RTV Texture2D;
};

// Raw (byte-address) buffers: the format is R32_TYPELESS and this flag says so.
constexpr UINT D3D12_BUFFER_SRV_FLAG_NONE = 0;
constexpr UINT D3D12_BUFFER_SRV_FLAG_RAW = 0x1;
constexpr UINT D3D12_BUFFER_UAV_FLAG_NONE = 0;
constexpr UINT D3D12_BUFFER_UAV_FLAG_RAW = 0x1;

struct D3D12_SHADER_RESOURCE_VIEW_DESC {
    DXGI_FORMAT Format = DXGI_FORMAT_UNKNOWN;
    UINT ViewDimension = 0;
    UINT Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    struct {
        UINT MostDetailedMip = 0;
        UINT MipLevels = 1;
        UINT PlaneSlice = 0;
        UINT ResourceMinLODClamp = 0;
    } Texture2D;
    struct {
        UINT FirstElement = 0;
        UINT NumElements = 0;
        UINT StructureByteStride = 0;
        UINT Flags = D3D12_BUFFER_SRV_FLAG_NONE;
    } Buffer;
};

struct D3D12_SUBRESOURCE_FOOTPRINT {
    DXGI_FORMAT Format = DXGI_FORMAT_UNKNOWN;
    UINT Width = 0;
    UINT Height = 0;
    UINT Depth = 0;
    UINT RowPitch = 0;
};

struct D3D12_PLACED_SUBRESOURCE_FOOTPRINT {
    UINT64 Offset = 0;
    D3D12_SUBRESOURCE_FOOTPRINT Footprint;
};

enum D3D12_TEXTURE_COPY_TYPE : UINT {
    D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX = 0,
    D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT = 1,
};

// One struct for both sides of a copy, exactly as the SDK has it: `Type` says
// which union arm is live, and the stub keeps both arms as plain members because
// nothing here reads the inactive one.
struct D3D12_TEXTURE_COPY_LOCATION {
    ID3D12Resource* pResource = nullptr;
    D3D12_TEXTURE_COPY_TYPE Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT PlacedFootprint;
    UINT SubresourceIndex = 0;
};

// Not an enum in the SDK (it is a plain UINT), so these are constants: a struct
// field typed UINT must accept `= D3D12_SRV_DIMENSION_TEXTURE2D` and g++ is happy.
constexpr UINT D3D12_SRV_DIMENSION_UNKNOWN = 0;
constexpr UINT D3D12_SRV_DIMENSION_BUFFER = 1;
constexpr UINT D3D12_SRV_DIMENSION_TEXTURE1D = 2;
constexpr UINT D3D12_SRV_DIMENSION_TEXTURE2D = 4;
constexpr UINT D3D12_SRV_DIMENSION_TEXTURE2DARRAY = 5;
constexpr UINT D3D12_SRV_DIMENSION_TEXTURE2DMS = 6;
constexpr UINT D3D12_SRV_DIMENSION_TEXTURE3D = 8;
constexpr UINT D3D12_SRV_DIMENSION_TEXTURECUBE = 9;
constexpr UINT D3D12_UAV_DIMENSION_UNKNOWN = 0;
constexpr UINT D3D12_UAV_DIMENSION_BUFFER = 1;
constexpr UINT D3D12_UAV_DIMENSION_TEXTURE1D = 2;
constexpr UINT D3D12_UAV_DIMENSION_TEXTURE2D = 4;
constexpr UINT D3D12_UAV_DIMENSION_TEXTURE2DARRAY = 5;
constexpr UINT D3D12_UAV_DIMENSION_TEXTURE3D = 8;

struct D3D12_UNORDERED_ACCESS_VIEW_DESC {
    DXGI_FORMAT Format = DXGI_FORMAT_UNKNOWN;
    UINT ViewDimension = D3D12_UAV_DIMENSION_UNKNOWN;
    struct {
        UINT FirstElement = 0;
        UINT NumElements = 0;
        UINT StructureByteStride = 0;
        UINT64 CounterOffsetInBytes = 0;
        UINT Flags = 0;
    } Buffer;
};

enum D3D12_INDEX_BUFFER_STRIP_CUT_VALUE : UINT {
    D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED = 0,
    D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF = 1,
    D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF = 2,
};

struct D3D12_SAMPLER_DESC {
    D3D12_FILTER Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    D3D12_TEXTURE_ADDRESS_MODE AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    D3D12_TEXTURE_ADDRESS_MODE AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    D3D12_TEXTURE_ADDRESS_MODE AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    float MipLODBias = 0;
    UINT MaxAnisotropy = 1;
    D3D12_COMPARISON_FUNC ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    float BorderColor[4] = {0, 0, 0, 0};
    float MinLOD = 0;
    float MaxLOD = 3.402823466e+38f;
};

struct DECLSPEC_UUID("10ec4d5b-975a-4689-b9e4-d0aac30fe333") ID3D12Object : IUnknown {
    virtual HRESULT SetName(LPCWSTR name) = 0;
    virtual HRESULT GetName(UINT size, LPWSTR name) = 0;
};

struct DECLSPEC_UUID("905db94b-a00c-4140-9df5-2b64ca9ea357") ID3D12DeviceChild : ID3D12Object {};

struct DECLSPEC_UUID("6102e791-8d0a-4869-a8c8-e7293f60efef") ID3D12Pageable : ID3D12DeviceChild {};

struct DECLSPEC_UUID("189819f1-1db6-4b57-be54-1821339b85f7") ID3D12Device : ID3D12Object {
    virtual UINT GetNodeCount() = 0;
    virtual HRESULT CreateCommandQueue(const D3D12_COMMAND_QUEUE_DESC* desc, REFIID riid,
                                       void** queue) = 0;
    virtual HRESULT CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE type, REFIID riid,
                                           void** allocator) = 0;
    virtual HRESULT CreateGraphicsPipelineState(const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc,
                                               REFIID riid, void** state) = 0;
    virtual HRESULT CreateComputePipelineState(const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc,
                                              REFIID riid, void** state) = 0;
    virtual HRESULT CreateCommandList(UINT node_mask, D3D12_COMMAND_LIST_TYPE type,
                                      ID3D12CommandAllocator* allocator,
                                      const void* initial_state, REFIID riid, void** list) = 0;
    virtual HRESULT CheckFeatureSupport(UINT feature, void* data, UINT data_size) = 0;
    virtual HRESULT CreateDescriptorHeap(const D3D12_DESCRIPTOR_HEAP_DESC* desc, REFIID riid,
                                        void** heap) = 0;
    virtual UINT GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE type) = 0;
    virtual HRESULT CreateRootSignature(UINT node_mask, const void* blob, SIZE_T blob_size,
                                       REFIID riid, void** signature) = 0;
    virtual HRESULT CreateConstantBufferView(const D3D12_CONSTANT_BUFFER_VIEW_DESC* desc,
                                            D3D12_CPU_DESCRIPTOR_HANDLE destination) = 0;
    virtual HRESULT CreateShaderResourceView(void* resource, const D3D12_SHADER_RESOURCE_VIEW_DESC* desc,
                                             D3D12_CPU_DESCRIPTOR_HANDLE destination) = 0;
    virtual HRESULT CreateUnorderedAccessView(void* resource, void* counter_resource,
                                              const D3D12_UNORDERED_ACCESS_VIEW_DESC* desc,
                                              D3D12_CPU_DESCRIPTOR_HANDLE destination) = 0;
    virtual HRESULT CreateRenderTargetView(void* resource, const D3D12_RENDER_TARGET_VIEW_DESC* desc,
                                           D3D12_CPU_DESCRIPTOR_HANDLE destination) = 0;
    virtual HRESULT CreateSampler(const D3D12_SAMPLER_DESC* desc,
                                  D3D12_CPU_DESCRIPTOR_HANDLE destination) = 0;
    virtual HRESULT CreateFence(UINT64 initial_value, D3D12_FENCE_FLAGS flags, REFIID riid,
                                void** fence) = 0;
    virtual HRESULT GetDeviceRemovedReason() = 0;
    virtual HRESULT CreateHeap(const D3D12_HEAP_DESC* desc, REFIID riid, void** heap) = 0;
    virtual HRESULT CreatePlacedResource(void* heap, UINT64 offset, const D3D12_RESOURCE_DESC* desc,
                                        D3D12_RESOURCE_STATES initial_state,
                                        const D3D12_CLEAR_VALUE* clear_value, REFIID riid,
                                        void** resource) = 0;
    virtual HRESULT CreateCommittedResource(const D3D12_HEAP_PROPERTIES* heap_properties,
                                            D3D12_HEAP_FLAGS heap_flags,
                                            const D3D12_RESOURCE_DESC* desc,
                                            D3D12_RESOURCE_STATES initial_state,
                                            const D3D12_CLEAR_VALUE* clear_value, REFIID riid,
                                            void** resource) = 0;
    virtual D3D12_RESOURCE_ALLOCATION_INFO GetResourceAllocationInfo(UINT visible_mask,
                                                                    UINT count,
                                                                    const D3D12_RESOURCE_DESC* descs) = 0;
};

struct DECLSPEC_UUID("5d5b7f8a-2f0b-4a6a-9c6c-8e1f0a3d5b11") ID3D12Device8 : ID3D12Device {
    virtual void CreateCommittedResource1(const D3D12_HEAP_PROPERTIES* heap_properties,
                                          D3D12_HEAP_FLAGS heap_flags,
                                          const D3D12_RESOURCE_DESC* desc,
                                          D3D12_RESOURCE_STATES initial_state,
                                          const D3D12_CLEAR_VALUE* clear_value,
                                          void* protected_session, REFIID riid,
                                          void** resource) = 0;
    virtual HRESULT GetResourceAllocationInfo2(UINT visible_mask, UINT count,
                                               const D3D12_RESOURCE_DESC* descs,
                                               D3D12_RESOURCE_ALLOCATION_INFO* info,
                                               UINT64* sizes) = 0;
};

struct DECLSPEC_UUID("8efb471d-616c-4f49-90f7-127bb763fa51") ID3D12CommandQueue : ID3D12Pageable {
    virtual void UpdateTileMappings(void* resource, UINT region_count, const void* regions,
                                   void* heap, UINT range_count, const void* ranges,
                                   const void* flags, UINT* total) = 0;
    virtual void CopyTileMappings(void* destination, const void* destination_region,
                                  void* source, const void* source_region, const void* flags,
                                  UINT* total) = 0;
    virtual void ExecuteCommandLists(UINT count, ID3D12CommandList* const* lists) = 0;
    virtual void SetMarker(UINT metadata, const void* data, UINT size) = 0;
    virtual void BeginEvent(UINT metadata, const void* data, UINT size) = 0;
    virtual void EndEvent() = 0;
    virtual HRESULT Signal(ID3D12Fence* fence, UINT64 value) = 0;
    virtual HRESULT Wait(ID3D12Fence* fence, UINT64 value) = 0;
    virtual HRESULT GetTimestampFrequency(UINT64* frequency) = 0;
    virtual HRESULT GetClockCalibration(UINT64* gpu_timestamp, UINT64* cpu_timestamp) = 0;
};

struct DECLSPEC_UUID("0a753dcf-c4d8-4b91-adf6-be5a60d95a76") ID3D12Fence : ID3D12Pageable {
    virtual UINT64 GetCompletedValue() = 0;
    virtual HRESULT SetEventOnCompletion(UINT64 value, HANDLE event) = 0;
    virtual HRESULT Signal(UINT64 value) = 0;
};

struct DECLSPEC_UUID("63ee58fb-1268-4835-8608-f55a00c2d305") ID3D12CommandAllocator : ID3D12Pageable {
    virtual HRESULT Reset() = 0;
};

struct DECLSPEC_UUID("6b3b2502-6e51-45b3-90ee-9884265e8df3") ID3D12DescriptorHeap : ID3D12Pageable {
    virtual void GetDesc(D3D12_DESCRIPTOR_HEAP_DESC* desc) = 0;
    virtual D3D12_CPU_DESCRIPTOR_HANDLE GetCPUDescriptorHandleForHeapStart() = 0;
    virtual D3D12_GPU_DESCRIPTOR_HANDLE GetGPUDescriptorHandleForHeapStart() = 0;
};

struct DECLSPEC_UUID("696442be-a72e-4059-bc79-5b5c98040fad") ID3D12Resource : ID3D12Pageable {
    virtual HRESULT Map(UINT subresource, const D3D12_RANGE* read_range, void** data) = 0;
    virtual void Unmap(UINT subresource, const D3D12_RANGE* written_range) = 0;
    virtual HRESULT WriteToSubresource(UINT subresource, const void* region, const void* src,
                                      UINT src_row_pitch, UINT src_depth_pitch) = 0;
    virtual HRESULT ReadFromSubresource(void* dst, UINT dst_row_pitch, UINT dst_depth_pitch,
                                       UINT subresource, const void* region) = 0;
    virtual HRESULT GetHeapProperties(D3D12_HEAP_PROPERTIES* properties, D3D12_HEAP_FLAGS* flags) = 0;
    virtual D3D12_GPU_VIRTUAL_ADDRESS GetGPUVirtualAddress() = 0;
};

struct DECLSPEC_UUID("7116d91c-e7e4-47ce-b8c6-ec8168f437e5") ID3D12CommandList : ID3D12DeviceChild {
    virtual HRESULT Close() = 0;
    virtual HRESULT Reset(ID3D12CommandAllocator* allocator, const void* initial_state) = 0;
    virtual HRESULT GetType() = 0;
};

struct DECLSPEC_UUID("5b160d0f-ac1b-4185-8ba8-b3aec28e8000") ID3D12GraphicsCommandList : ID3D12CommandList {
    virtual void SetDescriptorHeaps(UINT count, ID3D12DescriptorHeap* const* heaps) = 0;
    virtual void SetRootSignature(void* signature) = 0;
    virtual void ExecuteBundle(ID3D12GraphicsCommandList* bundle) = 0;
    virtual void SetPipelineState(void* state) = 0;
    virtual void ResourceBarrier(UINT count, const D3D12_RESOURCE_BARRIER* barriers) = 0;
    virtual void DrawInstanced(UINT vertex_count, UINT instance_count, UINT vertex_offset,
                               UINT instance_offset) = 0;
    virtual void DrawIndexedInstanced(UINT index_count, UINT instance_count, UINT index_offset,
                                      INT base_vertex, UINT instance_offset) = 0;
    virtual void Dispatch(UINT x, UINT y, UINT z) = 0;
    virtual void IASetVertexBuffers(UINT start_slot, UINT count,
                                    const D3D12_VERTEX_BUFFER_VIEW* views) = 0;
    virtual void IASetPrimitiveTopology(D3D12_PRIMITIVE_TOPOLOGY topology) = 0;
    // The events the replay emits for the recorder's markers; a no-op without a
    // debug layer, which is exactly what they are on a retail console.
    virtual void BeginEvent(UINT metadata, const void* data, UINT size) = 0;
    virtual void EndEvent() = 0;
    virtual void CopyBufferRegion(ID3D12Resource* destination, UINT64 destination_offset,
                                  ID3D12Resource* source, UINT64 source_offset, UINT64 bytes) = 0;
    virtual void CopyTextureRegion(const void* destination, UINT destination_x, UINT destination_y,
                                  UINT destination_z, const void* source, const D3D12_RECT* rect) = 0;
    virtual void CopyResource(ID3D12Resource* destination, ID3D12Resource* source) = 0;
    virtual void BeginQuery(void* heap, UINT type, UINT index) = 0;
    virtual void EndQuery(void* heap, UINT type, UINT index) = 0;
    virtual void ResolveQueryData(void* heap, UINT type, UINT start, UINT count,
                                  ID3D12Resource* destination, UINT64 offset) = 0;
    virtual void ClearRenderTargetView(D3D12_CPU_DESCRIPTOR_HANDLE rtv, const float color[4],
                                       UINT rect_count, const D3D12_RECT* rects) = 0;
    virtual void ClearUnorderedAccessViewUint(D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle,
                                              ID3D12Resource* resource, const UINT values[4],
                                              UINT rect_count, const D3D12_RECT* rects) = 0;
    virtual void DiscardResource(ID3D12Resource* resource, const void* region) = 0;
    virtual void RSSetViewports(UINT count, const D3D12_VIEWPORT* viewports) = 0;
    virtual void RSSetScissorRects(UINT count, const D3D12_RECT* rects) = 0;
    virtual void OMSetRenderTargets(UINT count, const D3D12_CPU_DESCRIPTOR_HANDLE* rtvs,
                                   BOOL single_handle, const D3D12_CPU_DESCRIPTOR_HANDLE* dsv) = 0;
    virtual void OMSetBlendFactor(const float blend_factor[4]) = 0;
    virtual void OMSetStencilRef(UINT ref) = 0;
    virtual void SetGraphicsRootSignature(void* signature) = 0;
    virtual void SetComputeRootSignature(void* signature) = 0;
    virtual void SetGraphicsRootDescriptorTable(UINT index, D3D12_GPU_DESCRIPTOR_HANDLE handle) = 0;
    virtual void SetGraphicsRoot32BitConstant(UINT index, UINT value, UINT offset) = 0;
    virtual void SetGraphicsRoot32BitConstants(UINT index, UINT count, const void* values,
                                              UINT offset) = 0;
    virtual void SetComputeRootDescriptorTable(UINT index, D3D12_GPU_DESCRIPTOR_HANDLE handle) = 0;
    virtual void SetComputeRoot32BitConstant(UINT index, UINT value, UINT offset) = 0;
    virtual void SetComputeRoot32BitConstants(UINT index, UINT count, const void* values,
                                              UINT offset) = 0;
};

struct DECLSPEC_UUID("c4fec28f-7966-4e95-9f94-f431cb56c3b8") IDXGIAdapter3;

struct ID3D12Debug;

HRESULT D3D12CreateDevice(IUnknown* adapter, D3D_FEATURE_LEVEL minimum_feature_level,
                          REFIID riid, void** device);
struct DECLSPEC_UUID("344488b7-6846-474b-b989-f027448245e0") ID3D12Debug : IUnknown {
    virtual void EnableDebugLayer() = 0;
};

HRESULT D3D12GetDebugInterface(REFIID riid, void** debug);
HRESULT D3D12SerializeRootSignature(const D3D12_ROOT_SIGNATURE_DESC* desc,
                                   D3D_ROOT_SIGNATURE_VERSION version, ID3DBlob** blob,
                                   ID3DBlob** error_blob);

// Feature-query ids the port asks for.
enum D3D12_FEATURE : UINT {
    D3D12_FEATURE_D3D12_OPTIONS = 0,
    D3D12_FEATURE_ARCHITECTURE = 1,
    D3D12_FEATURE_FEATURE_LEVELS = 2,
    D3D12_FEATURE_FORMAT_SUPPORT = 3,
    D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS = 4,
    D3D12_FEATURE_SHADER_MODEL = 7,
    D3D12_FEATURE_D3D12_OPTIONS1 = 8,
};

struct D3D12_FEATURE_DATA_D3D12_OPTIONS {
    BOOL DoublePrecisionFloatShaderOps = FALSE;
    BOOL OutputMergerLogicOp = FALSE;
    UINT MinPrecisionSupport = 0;
    UINT ResourceBindingTier = 0;
    UINT TiledResourcesTier = 0;
    UINT CrossNodeSharingTier = 0;
    BOOL CrossAdapterRowMajorTextureSupported = FALSE;
    BOOL VPAndRTArrayIndexFromAnyShaderFeedingRasterizerSupported = FALSE;
    UINT ResourceHeapTier = 0;
};

struct D3D12_FEATURE_DATA_ARCHITECTURE {
    UINT NodeIndex = 0;
    BOOL TileBasedRenderer = FALSE;
    BOOL UMA = FALSE;
    BOOL CacheCoherentUMA = FALSE;
};

struct D3D12_FEATURE_DATA_SHADER_MODEL {
    UINT HighestShaderModel = 0;
};

struct D3D12_FEATURE_DATA_D3D12_OPTIONS1 {
    BOOL WaveOps = FALSE;
    UINT WaveLaneCountMin = 0;
    UINT WaveLaneCountMax = 0;
    UINT TotalLaneCount = 0;
    BOOL ExpandedComputeResourceStates = FALSE;
    BOOL Int64ShaderOps = FALSE;
};

constexpr UINT D3D_SHADER_MODEL_6_4 = 0x64;
constexpr UINT D3D12_RESOURCE_BINDING_TIER_3 = 3;
constexpr UINT D3D12_RESOURCE_BINDING_TIER_2 = 2;

// --- root signatures and pipeline state --------------------------------------
enum D3D12_STATIC_BORDER_COLOR : UINT {
    D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK = 0,
    D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK = 1,
    D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE = 2,
};

enum D3D12_CONSERVATIVE_RASTERIZATION_MODE : UINT {
    D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF = 0,
    D3D12_CONSERVATIVE_RASTERIZATION_MODE_ON = 1,
};

struct D3D12_DESCRIPTOR_RANGE {
    D3D12_DESCRIPTOR_RANGE_TYPE RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    UINT NumDescriptors = 1;
    UINT BaseShaderRegister = 0;
    UINT RegisterSpace = 0;
    UINT OffsetInDescriptorsFromTableStart = D3D12_APPEND_ALIGNED_ELEMENT;
};

struct D3D12_ROOT_DESCRIPTOR_TABLE {
    UINT NumDescriptorRanges = 0;
    const D3D12_DESCRIPTOR_RANGE* pDescriptorRanges = nullptr;
};

struct D3D12_ROOT_CONSTANTS {
    UINT ShaderRegister = 0;
    UINT RegisterSpace = 0;
    UINT Num32BitValues = 0;
};

struct D3D12_ROOT_PARAMETER {
    D3D12_ROOT_PARAMETER_TYPE ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    D3D12_ROOT_DESCRIPTOR_TABLE DescriptorTable;
    D3D12_ROOT_CONSTANTS Constants;
    D3D12_SHADER_VISIBILITY ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
};

struct D3D12_STATIC_SAMPLER_DESC {
    D3D12_FILTER Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    D3D12_TEXTURE_ADDRESS_MODE AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    D3D12_TEXTURE_ADDRESS_MODE AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    D3D12_TEXTURE_ADDRESS_MODE AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    float MipLODBias = 0;
    UINT MaxAnisotropy = 1;
    D3D12_COMPARISON_FUNC ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    D3D12_STATIC_BORDER_COLOR BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    float MinLOD = 0;
    float MaxLOD = 3.402823466e+38f;
    UINT ShaderRegister = 0;
    UINT RegisterSpace = 0;
    D3D12_SHADER_VISIBILITY ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
};

struct D3D12_ROOT_SIGNATURE_DESC {
    UINT NumParameters = 0;
    const D3D12_ROOT_PARAMETER* pParameters = nullptr;
    UINT NumStaticSamplers = 0;
    const D3D12_STATIC_SAMPLER_DESC* pStaticSamplers = nullptr;
    D3D12_ROOT_SIGNATURE_FLAGS Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
};

struct D3D12_SHADER_BYTECODE {
    const void* pShaderBytecode = nullptr;
    SIZE_T BytecodeLength = 0;
};

struct D3D12_INPUT_ELEMENT_DESC {
    LPCSTR SemanticName = nullptr;
    UINT SemanticIndex = 0;
    DXGI_FORMAT Format = DXGI_FORMAT_UNKNOWN;
    UINT InputSlot = 0;
    UINT AlignedByteOffset = 0;
    D3D12_INPUT_CLASSIFICATION InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
    UINT InstanceDataStepRate = 0;
};

struct D3D12_INPUT_LAYOUT_DESC {
    const D3D12_INPUT_ELEMENT_DESC* pInputElementDescs = nullptr;
    UINT NumElements = 0;
};

struct D3D12_RENDER_TARGET_BLEND_DESC {
    BOOL BlendEnable = FALSE;
    BOOL LogicOpEnable = FALSE;
    D3D12_BLEND SrcBlend = D3D12_BLEND_ONE;
    D3D12_BLEND DestBlend = D3D12_BLEND_ZERO;
    D3D12_BLEND_OP BlendOp = D3D12_BLEND_OP_ADD;
    D3D12_BLEND SrcBlendAlpha = D3D12_BLEND_ONE;
    D3D12_BLEND DestBlendAlpha = D3D12_BLEND_ZERO;
    D3D12_BLEND_OP BlendOpAlpha = D3D12_BLEND_OP_ADD;
    UINT8 LogicOp = 0;
    UINT8 RenderTargetWriteMask = 0x0F;
};

struct D3D12_BLEND_DESC {
    BOOL AlphaToCoverageEnable = FALSE;
    BOOL IndependentBlendEnable = FALSE;
    D3D12_RENDER_TARGET_BLEND_DESC RenderTarget[8];
};

struct D3D12_RASTERIZER_DESC {
    D3D12_FILL_MODE FillMode = D3D12_FILL_MODE_SOLID;
    D3D12_CULL_MODE CullMode = D3D12_CULL_MODE_BACK;
    BOOL FrontCounterClockwise = FALSE;
    INT DepthBias = 0;
    float DepthBiasClamp = 0;
    float SlopeScaledDepthBias = 0;
    BOOL DepthClipEnable = TRUE;
    BOOL MultisampleEnable = FALSE;
    BOOL AntialiasedLineEnable = FALSE;
    UINT ForcedSampleCount = 0;
    D3D12_CONSERVATIVE_RASTERIZATION_MODE ConservativeRaster =
        D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
};

struct D3D12_DEPTH_STENCILOP_DESC {
    D3D12_STENCIL_OP StencilFailOp = D3D12_STENCIL_OP_KEEP;
    D3D12_STENCIL_OP StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
    D3D12_STENCIL_OP StencilPassOp = D3D12_STENCIL_OP_KEEP;
    D3D12_COMPARISON_FUNC StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
};

struct D3D12_DEPTH_STENCIL_DESC {
    BOOL DepthEnable = TRUE;
    D3D12_DEPTH_WRITE_MASK DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    D3D12_COMPARISON_FUNC DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    BOOL StencilEnable = FALSE;
    UINT8 StencilReadMask = 0xFF;
    UINT8 StencilWriteMask = 0xFF;
    D3D12_DEPTH_STENCILOP_DESC FrontFace;
    D3D12_DEPTH_STENCILOP_DESC BackFace;
};

struct D3D12_CACHED_PIPELINE_STATE {
    const void* pCachedBlob = nullptr;
    SIZE_T CachedBlobSizeInBytes = 0;
};

struct D3D12_STREAM_OUTPUT_DESC {
    const void* pSODeclaration = nullptr;
    UINT NumEntries = 0;
    const UINT* pBufferStrides = nullptr;
    UINT NumStrides = 0;
    UINT RasterizedStream = 0;
};

struct D3D12_GRAPHICS_PIPELINE_STATE_DESC {
    ID3D12RootSignature* pRootSignature = nullptr;
    D3D12_SHADER_BYTECODE VS;
    D3D12_SHADER_BYTECODE PS;
    D3D12_SHADER_BYTECODE DS;
    D3D12_SHADER_BYTECODE HS;
    D3D12_SHADER_BYTECODE GS;
    D3D12_STREAM_OUTPUT_DESC StreamOutput;
    D3D12_BLEND_DESC BlendState;
    UINT SampleMask = 0xFFFFFFFF;
    D3D12_RASTERIZER_DESC RasterizerState;
    D3D12_DEPTH_STENCIL_DESC DepthStencilState;
    DXGI_FORMAT DSVFormat = DXGI_FORMAT_UNKNOWN;
    D3D12_INPUT_LAYOUT_DESC InputLayout;
    // Typed as the SDK types it (an enum, not UINT): an `= 0` would compile here
    // and fail on the real header, which is the bug class this stub exists for.
    D3D12_INDEX_BUFFER_STRIP_CUT_VALUE IBStripCutValue =
        D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
    D3D12_PRIMITIVE_TOPOLOGY_TYPE PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    UINT NumRenderTargets = 1;
    DXGI_FORMAT RTVFormats[8] = {DXGI_FORMAT_UNKNOWN};
    DXGI_SAMPLE_DESC SampleDesc{1, 0};
    UINT NodeMask = 0;
    D3D12_CACHED_PIPELINE_STATE CachedPSO;
    D3D12_PIPELINE_STATE_FLAGS Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
};

struct D3D12_COMPUTE_PIPELINE_STATE_DESC {
    ID3D12RootSignature* pRootSignature = nullptr;
    D3D12_SHADER_BYTECODE CS;
    UINT NodeMask = 0;
    D3D12_CACHED_PIPELINE_STATE CachedPSO;
    D3D12_PIPELINE_STATE_FLAGS Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
};

struct ID3DBlob : IUnknown {
    virtual const void* GetBufferPointer() = 0;
    virtual SIZE_T GetBufferSize() = 0;
};

struct DECLSPEC_UUID("c079a9e4-0e4a-4d8b-9f0f-1fd2f6b8a3a1") ID3D12RootSignature : ID3D12DeviceChild {};
struct DECLSPEC_UUID("765a30f3-f624-4c6f-a828-ace948622445") ID3D12PipelineState : ID3D12Pageable {};
