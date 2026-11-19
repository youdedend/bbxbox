// SPDX-License-Identifier: GPL-3.0-or-later
// Portable validation, container inspection, and HUD progress formatting for
// baking Bloodborne's GCN shaders into signed DXIL SM 6.4 blobs.
//
// Shared by the on-console first-boot baker (xbox/platform/shader_baker.cpp)
// and tested on every host precheck without Windows or DXC.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bbx::d3d12 {

// DXIL container feature flags (SFI0 chunk, first dword).
// Bit 1 (0x02): Int64ShaderOps; bit 7 (0x80): Native16BitOps.
// Neither is available on Xbox Series X|S UWP SM 6.4 without lowering.
inline constexpr std::uint32_t kDxilFeatureInt64 = 0x02u;
inline constexpr std::uint32_t kDxilFeatureNative16Bit = 0x80u;
inline constexpr std::uint32_t kDxilForbiddenConsoleFeatures =
    kDxilFeatureInt64 | kDxilFeatureNative16Bit;

struct DxilFeatures {
    bool present = false;
    std::uint32_t flags = 0;
};

// True iff `object` starts with the "DXBC" magic and has a non-zero 16-byte
// validation digest at bytes 4..20 (a signed DXIL container).
[[nodiscard]] bool IsSignedDxbc(std::span<const std::uint8_t> object, std::string& why);

// True iff `hlsl` references a 64-bit integer or double type forbidden by the
// console's SM 6.4 dialect (no Int64ShaderOps).
[[nodiscard]] bool Has64BitInteger(std::string_view hlsl, std::string& what);

// Reads the SFI0 feature flags chunk from a DXBC/DXIL container, if present.
[[nodiscard]] DxilFeatures DxilFeatureFlags(std::span<const std::uint8_t> object);

// True iff the parsed SFI0 flags request a feature the console driver refuses.
[[nodiscard]] inline bool HasForbiddenConsoleFeature(const DxilFeatures& features) noexcept {
    return !features.present || (features.flags & kDxilForbiddenConsoleFeatures) != 0;
}

// Extracts the 8-hex-digit hash from a PS4 shader container's `OrbShdr` footer,
// or empty string when no footer is present.
[[nodiscard]] std::string ContainerName(std::span<const std::uint8_t> container);

// Extracts the stage type nibble from a PS4 shader container's `Shdr` header
// (1 = vertex, 2 = pixel, 4 = compute), or -1 when missing.
[[nodiscard]] int ContainerType(std::span<const std::uint8_t> container);

// Case-insensitive check for Bloodborne's `.shaderbnd.dcx` and
// `.shaderbnddebug.dcx` archives.
[[nodiscard]] bool IsShaderBundleFilename(std::string_view name);

struct EmbeddedContainerSpan {
    std::size_t offset = 0;
    std::size_t size = 0;
    std::uint8_t type = 0;
    std::string name;
};

// Scans an unwrapped Orbis ELF image for embedded `Shdr`..`OrbShdr` containers.
[[nodiscard]] std::vector<EmbeddedContainerSpan> ScanEbootForContainers(
    std::span<const std::uint8_t> elf_bytes);

enum class BakePhase : std::uint8_t {
    kIdle = 0,
    kScanning,
    kAnalysing,
    kCompiling,
    kWriting,
    kDone,
    kFailed,
};

[[nodiscard]] const char* BakePhaseName(BakePhase phase) noexcept;

struct BakeProgressSnapshot {
    BakePhase phase = BakePhase::kIdle;
    std::uint32_t done = 0;
    std::uint32_t total = 0;
    std::uint32_t signed_count = 0;
    std::uint32_t refused_count = 0;
    std::uint32_t quarantined_count = 0;
    std::uint32_t elapsed_seconds = 0;
    std::string current_name;
    std::string detail;
};

// Formats a single uppercase TV-HUD status line for the active bake progress.
[[nodiscard]] std::string FormatBakeHudLine(const BakeProgressSnapshot& snap);

}  // namespace bbx::d3d12
