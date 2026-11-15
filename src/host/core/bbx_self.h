// SPDX-License-Identifier: GPL-3.0-or-later
// PS4 fake-SELF: the wrapper a dumped `eboot.bin` actually arrives in.
//
// A retail Orbis eboot is a signed, encrypted SELF. Every dumper this port is
// meant to work with produces a *fake* SELF instead: the signature and the
// encryption are gone, the segments are plaintext (sometimes zlib-compressed),
// and the ELF headers are still inside. So the file starts with `4F 15 3D 1D`,
// not with `7F 45 4C 46`, and `load_orbis_elf` rejected it with
// "is not an ELF (decrypt SELF first)" — which is exactly wrong: nothing needs
// decrypting, only unwrapping.
//
// Layout this module understands (offsets are from the start of the file):
//
//   0x00  u32 magic          0x1D3D154F ("4F 15 3D 1D" little-endian)
//   0x04  u32 version        usually 3
//   0x08  u16 sdk_type
//   0x0A  u16 header_type    1 = fSELF (segments plaintext), 2 = signed/encrypted
//   0x0C  u32 metadata_offset
//   0x10  u32 header_len     usually 0x20: where the entry table starts
//   0x14  u32 elf_filesize   the size of the ELF this file wraps (a hint, not a rule)
//   0x18  u32 num_entries
//   0x1C  u32 flags
//   0x20  SelfEntry[num_entries], 32 bytes each:
//           0x00  u64 props   bits 0-3 encrypted, 4-7 compressed, 8-11 blocks
//           0x08  u64 offset  where this entry's bytes start in the file
//           0x10  u64 size    how many bytes they are (before inflation)
//           0x18  u64 size2   the inflated/blocked size
//
// Field layouts in the wild differ in the tail of the header (some dumpers write
// a wider, 0x90-byte header with 64-bit offsets), so this parser does not trust
// the header for anything it can check against the ELF it finds. It locates the
// embedded `Elf64_Ehdr`, validates it, and then decides per segment which
// interpretation of `p_offset` and which entry describes its bytes — and it says
// no, with a named reason, rather than composing an image that would fault
// somewhere later without a breadcrumb.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace bbx::core {

enum class FselfError : std::uint8_t {
    kOk = 0,
    kTooSmall,            // shorter than a SELF header, or the file could not be read
    kNotOrbisImage,       // neither a plain ELF nor a SELF
    kRetailEncrypted,     // header_type 2, or a segment marked encrypted: not a fake SELF
    kBadEntryTable,       // num_entries does not fit the file
    kNoEmbeddedElf,       // no valid 64-bit x86-64 Elf64_Ehdr anywhere in the file
    kBadElfHeader,        // e_phoff / e_phnum / e_phentsize do not survive validation
    kSegmentOutOfBounds,  // a PT_LOAD points outside the file, in every interpretation
    kDecompressFailed,    // a compressed segment did not inflate to its declared size
    kDecompressUnavailable,  // compressed, and this build has no zlib
    kSizeMismatch,        // the reconstructed image does not cover its own headers
};

[[nodiscard]] const char* FselfErrorName(FselfError error);

// What the unwrapper did, in the terms the log line and the launcher need.
struct FselfInfo {
    bool was_fself = false;             // false: the input was a plain ELF, copied as is
    std::uint32_t version = 0;
    std::uint16_t sdk_type = 0;
    std::uint16_t header_type = 0;
    std::uint32_t num_entries = 0;
    std::uint32_t compressed_entries = 0;
    std::uint32_t plain_entries = 0;
    std::uint64_t elf_base = 0;         // where the embedded Elf64_Ehdr sits in the input
    std::uint64_t input_size = 0;
    std::uint64_t elf_size = 0;         // the reconstructed image's size
    // Set when the header was ignored in a way the log should mention (an absurd
    // entry count, say), empty otherwise.
    std::string warning;
    std::uint16_t segments = 0;         // PT_LOAD count

    [[nodiscard]] std::string Describe() const;
};

struct FselfResult {
    FselfError error = FselfError::kOk;
    FselfInfo info;
    // The flat ELF image: byte-identical to the input for a plain ELF, so the
    // sha256 that gates the address-based patches does not change for it.
    std::vector<std::uint8_t> elf;
    // The reason, in words, when `error` is not kOk. Always set on failure.
    std::string why;

    [[nodiscard]] bool ok() const { return error == FselfError::kOk; }
};

// Plain ELF: copied unchanged. fSELF: unwrapped. Anything else: kNotOrbisImage.
[[nodiscard]] FselfResult UnwrapOrbisImage(std::span<const std::uint8_t> data);

// The same, from a path. A file that cannot be opened reports kTooSmall with the
// reason naming the path, because "the dump is not where the ini says" is the
// most common failure of all and it must not look like a corrupt eboot.
[[nodiscard]] FselfResult UnwrapOrbisFile(const char* path);

[[nodiscard]] bool LooksLikeElf(std::span<const std::uint8_t> data);
[[nodiscard]] bool LooksLikeFself(std::span<const std::uint8_t> data);

// zlib is what a compressed fSELF segment is deflated with. The host build and the
// console build both link the vendored copy; this flag exists so a build without it
// fails with kDecompressUnavailable rather than a link error, and so the tests can
// prove that path.
[[nodiscard]] bool InflateAvailable();
void SetInflateAvailableForTesting(bool available);

}  // namespace bbx::core
