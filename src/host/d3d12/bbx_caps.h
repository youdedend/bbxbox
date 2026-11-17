// SPDX-License-Identifier: GPL-3.0-or-later
// What the console's GPU actually is, and what the port therefore may emit.
//
// Everything here comes from hardware work on the Series X (KytyPS5's
// `docs/d3d12-backend.md` and `XBOX_CHANGES.md`, and Cosmo's `988ceea`), not
// from a spec sheet: a UWP game on the console gets feature level 11_0, shader
// model **6.4**, resource binding tier 3, 64-lane waves, typed UAV loads, and
// nothing newer. Shader model 6.6's `ResourceDescriptorHeap[...]`, 6.5's mesh
// shaders, barycentrics and native 16-bit arithmetic are all absent.
//
// This header is portable on purpose: the DXIL compiler flags, the PSO cache and
// the tests must agree on one description of the device, and the tests run on a
// build machine with no console.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

namespace bbx::d3d12 {

// --- shader model, in D3D_SHADER_MODEL's units --------------------------------
//
// D3D_SHADER_MODEL encodes the version in two nibbles: 6.4 is 0x64, which is one
// hundred as a decimal and *not* sixty-four. The port once stored 64 here while
// the device answered 0x64, so a device that D3D12CreateDevice had already
// created at feature level 11_0 was refused by the caps check ("device reports
// SM 100 but the shaders are compiled for 64") and nothing ever reached the
// swapchain. Every shader-model value below is this encoding and nothing else;
// device.cpp static_asserts these against the SDK's own enumerators.
inline constexpr std::uint32_t kShaderModel64 = 0x64u;  // D3D_SHADER_MODEL_6_4
inline constexpr std::uint32_t kShaderModel68 = 0x68u;  // D3D_SHADER_MODEL_6_8

// 0x64 -> 6.
[[nodiscard]] constexpr std::uint32_t ShaderModelMajor(std::uint32_t shader_model) {
    return shader_model >> 4;
}

// 0x64 -> 4: the digit in the profile's `_6_4`.
[[nodiscard]] constexpr std::uint32_t ShaderModelMinor(std::uint32_t shader_model) {
    return shader_model & 0xFu;
}

// "6.4": what a person reads.
[[nodiscard]] inline std::string ShaderModelText(std::uint32_t shader_model) {
    return std::to_string(ShaderModelMajor(shader_model)) + "." +
           std::to_string(ShaderModelMinor(shader_model));
}

// "100 (0x64)": the decimal and the hex together, so a units mistake can never
// be read as a version again.
[[nodiscard]] inline std::string ShaderModelValues(std::uint32_t shader_model) {
    char buffer[32] = {0};
    std::snprintf(buffer, sizeof(buffer), "%u (0x%02X)", shader_model, shader_model);
    return std::string(buffer);
}

// The one line the device logs when it compares what the hardware reports with
// what the shaders were compiled for: the device's value, the expected value,
// and both of them in hex on the same line.
[[nodiscard]] inline std::string FormatShaderModelLine(std::uint32_t device_shader_model,
                                                       std::uint32_t expected_shader_model) {
    std::string line = "shader model: device=SM " + ShaderModelText(device_shader_model) + " " +
                       ShaderModelValues(device_shader_model) + ", expected=SM " +
                       ShaderModelText(expected_shader_model) + " " +
                       ShaderModelValues(expected_shader_model) + ", adequate=";
    line += device_shader_model >= expected_shader_model ? "1" : "0";
    return line;
}

// The two devices the port can meet.
enum class GpuClass : std::uint8_t {
    kXboxSeries = 0,  // SraKmd_arden, the only target that ships
    kDesktop,         // the Windows build, for development and CI
};

struct Caps {
    GpuClass gpu_class = GpuClass::kXboxSeries;
    std::uint32_t feature_level_major = 11;
    std::uint32_t feature_level_minor = 0;
    // What the shaders may target: the console's ceiling, 6.4 = 0x64.
    std::uint32_t shader_model = kShaderModel64;
    // What the device itself answered, before the ceiling was applied. This is
    // the value the log quotes and the value the check reads.
    std::uint32_t device_shader_model = kShaderModel64;
    std::uint32_t binding_tier = 3;    // descriptor tables, unbounded arrays
    std::uint32_t wave_lanes = 64;
    std::uint32_t max_group_shared_bytes = 32u << 10;  // G15
    bool native_16_bit = false;        // no int16/float16 ALU: lower to 32-bit
    bool descriptor_heap_indexing = false;  // SM 6.6 ResourceDescriptorHeap
    bool mesh_shaders = false;         // SM 6.5
    bool barycentrics = false;
    bool pipeline_library = false;     // CreatePipelineLibrary removes the device
    bool hardware_tessellator = false; // emits zero domain points on Xbox
    bool typed_uav_loads = true;
    bool constant_alpha_blend = false; // G4: the device fails such a PSO
    std::uint32_t max_swapchain_width = 3840;   // G8
    std::uint32_t max_swapchain_height = 2160;
};

[[nodiscard]] inline Caps XboxSeriesCaps() { return Caps{}; }

[[nodiscard]] inline Caps DesktopCaps() {
    Caps caps;
    caps.gpu_class = GpuClass::kDesktop;
    caps.feature_level_major = 12;
    caps.feature_level_minor = 0;
    caps.shader_model = kShaderModel68;
    caps.device_shader_model = kShaderModel68;
    caps.native_16_bit = true;
    caps.descriptor_heap_indexing = true;
    caps.mesh_shaders = true;
    caps.barycentrics = true;
    caps.pipeline_library = true;
    caps.hardware_tessellator = true;
    caps.constant_alpha_blend = true;
    return caps;
}

// `ps_6_4` / `cs_6_4` / ... for the given caps. DXC's `-T` argument.
[[nodiscard]] inline std::string ShaderTarget(const char* stage, const Caps& caps) {
    return std::string(stage) + "_" + std::to_string(ShaderModelMajor(caps.shader_model)) + "_" +
           std::to_string(ShaderModelMinor(caps.shader_model));
}

// What the compiler is allowed to use. `-enable-16bit-types` must never appear
// on the console: the device has no native 16-bit operations, and a shader that
// needs them fails to create its pipeline at best, hangs the driver at worst.
struct CompileFlags {
    bool enable_16bit_types = false;
    bool descriptor_heap_indexing = false;
    bool mesh_shaders = false;
    bool barycentrics = false;
    bool lower_16bit_ops = true;   // 16-bit arithmetic is emitted as 32-bit
    bool loop_attribute = true;    // G6: `break` inside a loop needs [loop]
    std::uint32_t max_group_shared_bytes = 32u << 10;
};

[[nodiscard]] inline CompileFlags FlagsFor(const Caps& caps) {
    CompileFlags flags;
    flags.enable_16bit_types = caps.native_16_bit;
    flags.descriptor_heap_indexing = caps.descriptor_heap_indexing;
    flags.mesh_shaders = caps.mesh_shaders;
    flags.barycentrics = caps.barycentrics;
    flags.lower_16bit_ops = !caps.native_16_bit;
    flags.max_group_shared_bytes = caps.max_group_shared_bytes;
    return flags;
}

// True when a shader that declares `group_shared_bytes` can run: D3D12 allows at
// most 32 KiB, and the guest's LDS is clamped to it (Kyty warns once).
[[nodiscard]] inline bool GroupSharedFits(const Caps& caps, std::uint32_t group_shared_bytes) {
    return group_shared_bytes <= caps.max_group_shared_bytes;
}

// A one-line description for the log; the same line goes into every bug report.
[[nodiscard]] inline std::string DescribeCaps(const Caps& caps) {
    std::string text = "GPU: ";
    text += caps.gpu_class == GpuClass::kXboxSeries ? "Xbox Series (SraKmd_arden)" : "desktop";
    text += "; FL " + std::to_string(caps.feature_level_major) + "_" +
            std::to_string(caps.feature_level_minor);
    text += "; SM " + ShaderModelText(caps.shader_model);
    text += "; binding tier " + std::to_string(caps.binding_tier);
    text += "; wave " + std::to_string(caps.wave_lanes);
    text += caps.native_16_bit ? "; 16-bit ALU" : "; no 16-bit ALU (lowered)";
    text += caps.descriptor_heap_indexing ? "; heap indexing" : "; descriptor tables only";
    text += caps.pipeline_library ? "; pipeline library" : "; no pipeline library";
    text += caps.hardware_tessellator ? "; HW tessellator" : "; tessellator emulated";
    return text;
}

// The device-level checks the backend runs once at creation, so a wrong
// assumption is a log line instead of a mystery later.
struct CapsCheck {
    bool ok = true;
    std::string problem;
};

[[nodiscard]] inline CapsCheck Validate(const Caps& expected, const Caps& actual) {
    CapsCheck check;
    // Both sides are D3D_SHADER_MODEL units (6.4 is 0x64). A device that reports
    // newer than the shaders need is fine; one that reports older is a line in
    // the log, not a refused device - device.cpp's QueryAndValidate says why.
    if (actual.device_shader_model < expected.shader_model) {
        check.ok = false;
        check.problem = "device reports SM " + ShaderModelText(actual.device_shader_model) + " (" +
                        ShaderModelValues(actual.device_shader_model) +
                        ") but the shaders are compiled for SM " +
                        ShaderModelText(expected.shader_model) + " (" +
                        ShaderModelValues(expected.shader_model) + ")";
    }
    if (expected.gpu_class == GpuClass::kXboxSeries && actual.pipeline_library) {
        check.ok = false;
        check.problem = "pipeline library reported on a console build; it removes the device";
    }
    return check;
}

}  // namespace bbx::d3d12
