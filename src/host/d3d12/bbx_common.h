// SPDX-License-Identifier: GPL-3.0-or-later
// Small things every part of the port needs: a hash that is stable across
// runs and compilers, alignment, and checked arithmetic for address maths.
#pragma once

#include <string>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>

namespace bbx {

// FNV-1a, 64-bit. Stable across compilers and platforms, which is the only
// property that matters for cache keys: std::hash is not.
[[nodiscard]] constexpr std::uint64_t Fnv1a64(std::span<const std::uint8_t> bytes,
                                              std::uint64_t seed = 0xcbf29ce484222325ull) {
    std::uint64_t h = seed;
    for (const std::uint8_t b : bytes) {
        h ^= b;
        h *= 0x100000001b3ull;
    }
    return h;
}

// Not constexpr: reinterpret_cast in a constant expression is C++23 at best,
// and no caller needs a compile-time string hash.
[[nodiscard]] inline std::uint64_t Fnv1a64(std::string_view text,
                                           std::uint64_t seed = 0xcbf29ce484222325ull) {
    std::uint64_t h = seed;
    for (const char c : text) {
        h ^= static_cast<std::uint8_t>(c);
        h *= 0x100000001b3ull;
    }
    return h;
}

template <class T>
    requires std::is_trivially_copyable_v<T>
[[nodiscard]] inline std::uint64_t Fnv1a64(const T& value) {
    return Fnv1a64(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(&value), sizeof(T)});
}

// A second, independent mix used to widen a 64-bit hash to 128 bits where a
// collision would corrupt a cache rather than cost a rebuild.
[[nodiscard]] constexpr std::uint64_t Mix64(std::uint64_t x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ull;
    x ^= x >> 33;
    return x;
}

[[nodiscard]] constexpr std::uint64_t AlignUp(std::uint64_t value, std::uint64_t alignment) {
    return alignment == 0 ? value : (value + alignment - 1) / alignment * alignment;
}

[[nodiscard]] constexpr bool IsPow2(std::uint64_t value) {
    return value != 0 && (value & (value - 1)) == 0;
}

[[nodiscard]] constexpr std::uint64_t MiB(std::uint64_t mib) { return mib << 20; }

// Saturating add for byte counters, so a runaway counter cannot wrap into a
// "fits" verdict.
[[nodiscard]] constexpr std::uint64_t SatAdd(std::uint64_t a, std::uint64_t b) {
    return a > UINT64_MAX - b ? UINT64_MAX : a + b;
}

[[nodiscard]] constexpr std::size_t RoundUpDiv(std::uint64_t value, std::uint64_t divisor) {
    return static_cast<std::size_t>(divisor == 0 ? 0 : (value + divisor - 1) / divisor);
}

}  // namespace bbx
