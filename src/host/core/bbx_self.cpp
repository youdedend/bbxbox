// SPDX-License-Identifier: GPL-3.0-or-later
// The PS4 fake-SELF unwrapper (see bbx_self.h).
#include "src/host/core/bbx_self.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>

#if defined(BBX_HAVE_ZLIB)
#include <zlib.h>
#endif

namespace bbx::core {
namespace {

constexpr std::uint32_t kSelfMagic = 0x1D3D154Fu;  // "4F 15 3D 1D" little-endian
constexpr std::uint32_t kElfMagic = 0x464C457Fu;   // "7F 45 4C 46"
constexpr std::size_t kSelfHeaderSize = 0x20;
constexpr std::size_t kSelfEntrySize = 32;
constexpr std::uint16_t kHeaderTypeFself = 1;
constexpr std::uint16_t kHeaderTypeSigned = 2;
// Where the fields this parser uses live. Named so the struct below reads like
// the table of contents it is.
constexpr std::size_t kOffVersion = 0x04;
constexpr std::size_t kOffSdkType = 0x08;
constexpr std::size_t kOffHeaderType = 0x0A;
constexpr std::size_t kOffHeaderLen = 0x10;
constexpr std::size_t kOffElfFileSize = 0x14;
constexpr std::size_t kOffNumEntries = 0x18;

constexpr std::size_t kEntryOffProps = 0x00;
constexpr std::size_t kEntryOffOffset = 0x08;
constexpr std::size_t kEntryOffSize = 0x10;
constexpr std::size_t kEntryOffSize2 = 0x18;

constexpr std::uint32_t kEntryEncryptedShift = 0;
constexpr std::uint32_t kEntryCompressedShift = 4;
constexpr std::uint32_t kEntryBlocksShift = 8;

constexpr std::uint32_t PT_LOAD = 1;
constexpr std::uint32_t PT_DYNAMIC = 2;
constexpr std::uint16_t kElfMachineX86_64 = 62;
constexpr std::uint16_t kElfPhdrSize = 56;

// PS4 compressed segments are written in 16 KiB blocks, each its own zlib stream.
constexpr std::size_t kBlockSize = 0x4000;
// How far into the file the embedded ELF header is looked for. A SELF that hides
// it further than this is not a layout any dumper produces, and scanning a 90 MB
// file for a magic that may repeat inside shader data would find the wrong one.
constexpr std::size_t kElfSearchLimit = 1u << 20;

bool g_inflate_available =
#if defined(BBX_HAVE_ZLIB)
    true;
#else
    false;
#endif

std::uint16_t ReadU16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0] | (static_cast<std::uint16_t>(p[1]) << 8));
}

std::uint32_t ReadU32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

std::uint64_t ReadU64(const std::uint8_t* p) {
    return static_cast<std::uint64_t>(ReadU32(p)) |
           (static_cast<std::uint64_t>(ReadU32(p + 4)) << 32);
}

// The fields of Elf64_Ehdr this module needs, read at an arbitrary base.
struct ElfHeader {
    std::uint16_t machine = 0;
    std::uint64_t phoff = 0;
    std::uint16_t phentsize = 0;
    std::uint16_t phnum = 0;
};

struct ElfProgramHeader {
    std::uint32_t type = 0;
    std::uint64_t offset = 0;
    std::uint64_t filesz = 0;
    std::uint64_t memsz = 0;
};

ElfHeader ReadElfHeader(std::span<const std::uint8_t> data, std::size_t base) {
    ElfHeader header{};
    if (base + 64 > data.size()) { return header; }
    const std::uint8_t* p = data.data() + base;
    header.machine = ReadU16(p + 0x12);
    header.phoff = ReadU64(p + 0x20);
    header.phentsize = ReadU16(p + 0x36);
    header.phnum = ReadU16(p + 0x38);
    return header;
}

// A candidate is only accepted when the fields a loader would trust are sane:
// 64-bit LE x86-64, a program header table that fits the file, and no absurd
// count. That is what keeps a shader-blob 'txf.' that happens to contain
// 7F 45 4C 46 from being mistaken for the image.
bool ElfHeaderPlausible(std::span<const std::uint8_t> data, std::size_t base) {
    if (base + 64 > data.size()) { return false; }
    const std::uint8_t* p = data.data() + base;
    if (p[4] != 2 || p[5] != 1) { return false; }               // ELFCLASS64, ELFDATA2LSB
    const ElfHeader header = ReadElfHeader(data, base);
    if (header.machine != kElfMachineX86_64) { return false; }
    if (header.phentsize != kElfPhdrSize) { return false; }
    if (header.phnum == 0 || header.phnum > 64) { return false; }
    if (header.phoff == 0 || header.phoff > (1u << 20)) { return false; }
    const std::uint64_t table_end =
        static_cast<std::uint64_t>(base) + header.phoff + header.phnum * header.phentsize;
    return table_end <= data.size();
}

std::size_t FindElfBase(std::span<const std::uint8_t> data) {
    const std::size_t limit = std::min(data.size(), kElfSearchLimit);
    for (std::size_t at = 0; at + 64 <= limit; ++at) {
        // A whole-word compare, and no memchr dance: this runs once per boot.
        if (ReadU32(data.data() + at) != kElfMagic) { continue; }
        if (ElfHeaderPlausible(data, at)) { return at; }
    }
    return std::string::npos;
}

struct SelfEntry {
    std::uint64_t props = 0;
    std::uint64_t offset = 0;
    std::uint64_t size = 0;
    std::uint64_t size2 = 0;

    [[nodiscard]] bool encrypted() const {
        return ((props >> kEntryEncryptedShift) & 0xFu) != 0;
    }
    [[nodiscard]] bool compressed() const {
        return ((props >> kEntryCompressedShift) & 0xFu) != 0;
    }
    [[nodiscard]] bool blocked() const { return ((props >> kEntryBlocksShift) & 0xFu) != 0; }
};

// Reads the entry table, when the header's count and the file agree about it.
// `table_ok` false means the file has no usable table, which is legal: a flat
// fSELF with no compressed segments needs none.
bool ReadEntryTable(std::span<const std::uint8_t> data, std::uint32_t declared_entries,
                    std::size_t elf_base, std::vector<SelfEntry>* entries, std::string* why) {
    entries->clear();
    if (declared_entries == 0 || declared_entries > 256) {
        // No table, or a count no eboot has. Not fatal: the segments may all be
        // plaintext at their p_offset, which the caller then tries.
        return false;
    }
    const std::uint64_t table_end =
        static_cast<std::uint64_t>(kSelfHeaderSize) + declared_entries * kSelfEntrySize;
    if (table_end > data.size() || table_end > elf_base + kSelfEntrySize) {
        *why = "the entry table does not fit the file";
        return false;
    }
    for (std::uint32_t index = 0; index < declared_entries; ++index) {
        const std::uint8_t* p = data.data() + kSelfHeaderSize + index * kSelfEntrySize;
        SelfEntry entry{};
        entry.props = ReadU64(p + kEntryOffProps);
        entry.offset = ReadU64(p + kEntryOffOffset);
        entry.size = ReadU64(p + kEntryOffSize);
        entry.size2 = ReadU64(p + kEntryOffSize2);
        entries->push_back(entry);
    }
    return true;
}

#if defined(BBX_HAVE_ZLIB)
bool InflateWhole(const std::uint8_t* src, std::size_t src_size, std::uint8_t* dst,
                  std::size_t dst_size) {
    uLongf out = static_cast<uLongf>(dst_size);
    const int rc = ::uncompress(dst, &out, src, static_cast<uLong>(src_size));
    return rc == Z_OK && out == dst_size;
}

bool InflateBlocks(const std::uint8_t* src, std::size_t src_size, std::uint8_t* dst,
                   std::size_t dst_size) {
    // Blocked: each 16 KiB of output is its own zlib stream, back to back.
    std::size_t in = 0;
    std::size_t out = 0;
    while (out < dst_size) {
        if (in + 2 > src_size) { return false; }
        const std::size_t want = std::min(kBlockSize, dst_size - out);
        uLongf produced = static_cast<uLongf>(want);
        const int rc = ::uncompress(dst + out, &produced, src + in,
                                    static_cast<uLong>(src_size - in));
        if (rc != Z_OK || produced == 0) { return false; }
        out += produced;
        // zlib's uncompress does not report how much input it consumed; a blocked
        // stream's block is self-delimiting, so the consumed length is found by
        // inflating again with a running total.
        std::size_t consumed = src_size - in;
        {
            // Re-inflate against a shrinking window to find the block's end.
            std::size_t lo = 0;
            std::size_t hi = src_size - in;
            while (lo < hi) {
                const std::size_t mid = (lo + hi) / 2;
                uLongf size = static_cast<uLongf>(want);
                std::vector<std::uint8_t> scratch(want);
                const int probe = ::uncompress(scratch.data(), &size, src + in,
                                               static_cast<uLong>(mid));
                if (probe == Z_OK && size == produced) {
                    hi = mid;
                } else {
                    lo = mid + 1;
                }
            }
            consumed = lo;
        }
        if (consumed == 0) { return false; }
        in += consumed;
        if (produced < want) { break; }
    }
    return out == dst_size;
}
#endif

}  // namespace

const char* FselfErrorName(FselfError error) {
    switch (error) {
        case FselfError::kOk: return "ok";
        case FselfError::kTooSmall: return "file too small (or unreadable)";
        case FselfError::kNotOrbisImage: return "not an ELF and not a SELF";
        case FselfError::kRetailEncrypted: return "retail (signed/encrypted) SELF";
        case FselfError::kBadEntryTable: return "bad SELF entry table";
        case FselfError::kNoEmbeddedElf: return "no embedded 64-bit x86-64 ELF header";
        case FselfError::kBadElfHeader: return "bad ELF header";
        case FselfError::kSegmentOutOfBounds: return "segment outside the file";
        case FselfError::kDecompressFailed: return "compressed segment did not inflate";
        case FselfError::kDecompressUnavailable: return "compressed segment, no zlib in this build";
        case FselfError::kSizeMismatch: return "reconstructed image does not cover its headers";
    }
    return "unknown";
}

std::string FselfInfo::Describe() const {
    char text[256];
    if (!was_fself) {
        std::snprintf(text, sizeof(text), "plain ELF, %llu bytes",
                      static_cast<unsigned long long>(elf_size));
        return text;
    }
    std::snprintf(text, sizeof(text),
                  "fSELF v%u (header_type %u): %u/%u segment(s) compressed, ELF at 0x%llx, "
                  "%llu -> %llu bytes",
                  version, header_type, compressed_entries, num_entries,
                  static_cast<unsigned long long>(elf_base),
                  static_cast<unsigned long long>(input_size),
                  static_cast<unsigned long long>(elf_size));
    return text;
}

bool LooksLikeElf(std::span<const std::uint8_t> data) {
    return data.size() >= 4 && ReadU32(data.data()) == kElfMagic;
}

bool LooksLikeFself(std::span<const std::uint8_t> data) {
    return data.size() >= 4 && ReadU32(data.data()) == kSelfMagic;
}

bool InflateAvailable() { return g_inflate_available; }

void SetInflateAvailableForTesting(bool available) { g_inflate_available = available; }

FselfResult UnwrapOrbisImage(std::span<const std::uint8_t> data) {
    FselfResult result{};
    result.info.input_size = data.size();

    if (LooksLikeElf(data)) {
        // Already a flat ELF: copied unchanged, so the sha256 that gates the
        // address-based patches is the same digest it always was.
        result.info.was_fself = false;
        result.info.elf_size = data.size();
        result.elf.assign(data.begin(), data.end());
        return result;
    }
    if (data.size() < kSelfHeaderSize) {
        result.error = FselfError::kTooSmall;
        result.why = "shorter than a SELF header";
        return result;
    }
    if (!LooksLikeFself(data)) {
        result.error = FselfError::kNotOrbisImage;
        char text[96];
        std::snprintf(text, sizeof(text), "magic %02X %02X %02X %02X", data[0], data[1], data[2],
                      data[3]);
        result.why = text;
        return result;
    }

    const std::uint8_t* header = data.data();
    result.info.was_fself = true;
    result.info.version = ReadU32(header + kOffVersion);
    result.info.sdk_type = ReadU16(header + kOffSdkType);
    result.info.header_type = ReadU16(header + kOffHeaderType);
    const std::uint32_t declared_elf_size = ReadU32(header + kOffElfFileSize);
    const std::uint32_t declared_entries = ReadU32(header + kOffNumEntries);
    result.info.num_entries = declared_entries;

    // A signed SELF is the retail file. Saying so is the whole point: a player who
    // dumped the payload instead of the fake-signed image gets one clear line, not
    // a stack of segment errors.
    if (result.info.header_type == kHeaderTypeSigned) {
        result.error = FselfError::kRetailEncrypted;
        result.why =
            "this is a signed retail SELF (header_type 2), not a fake SELF; decrypt/repack it "
            "with the dumper that produced the rest of the dump";
        return result;
    }

    const std::size_t elf_base = FindElfBase(data);
    if (elf_base == std::string::npos) {
        result.error = FselfError::kNoEmbeddedElf;
        result.why = "no 64-bit x86-64 Elf64_Ehdr in the first MiB";
        return result;
    }
    result.info.elf_base = elf_base;

    const ElfHeader elf = ReadElfHeader(data, elf_base);
    if (elf.phnum == 0) {
        result.error = FselfError::kBadElfHeader;
        result.why = "e_phnum is zero";
        return result;
    }
    result.info.segments = elf.phnum;

    std::vector<SelfEntry> entries;
    std::string table_why;
    const bool have_table = ReadEntryTable(data, declared_entries, elf_base, &entries, &table_why);
    if (!have_table && !table_why.empty()) {
        // A count that fits the header but whose table runs into the embedded ELF
        // is not a table this parser can trust: the entries would describe the ELF's
        // own bytes. That is a corrupt file, and it is named as one.
        result.error = FselfError::kBadEntryTable;
        result.why = table_why;
        return result;
    }
    if (!have_table && declared_entries > 256) {
        // An absurd count is a corrupt header, not a corrupt image: the segments are
        // still plaintext at their p_offset, so the unwrap proceeds and says what it
        // ignored.
        char text[128];
        std::snprintf(text, sizeof(text), "header claims %u entry table entries; ignored it",
                      declared_entries);
        result.info.warning = text;
    }
    // The entry table describes the same segments the ELF's program headers do.
    // When the counts disagree the table is not about these segments, so it is not
    // used at all — a wrong entry would place a segment's bytes at the wrong
    // offset, which is far worse than falling back to plaintext.
    const bool entries_match = have_table && entries.size() == elf.phnum;

    struct Placement {
        std::size_t phdr_index = 0;
        ElfProgramHeader phdr{};
        std::size_t src = 0;
        std::size_t stored = 0;
        bool compressed = false;
        bool blocked = false;
    };
    std::vector<Placement> placements;

    for (std::uint16_t index = 0; index < elf.phnum; ++index) {
        const std::size_t phdr_at = elf_base + static_cast<std::size_t>(elf.phoff) +
                                    static_cast<std::size_t>(index) * elf.phentsize;
        if (phdr_at + sizeof(ElfProgramHeader) > data.size()) {
            result.error = FselfError::kBadElfHeader;
            result.why = "the program header table runs past the file";
            return result;
        }
        const std::uint8_t* p = data.data() + phdr_at;
        ElfProgramHeader phdr{};
        phdr.type = ReadU32(p);
        phdr.offset = ReadU64(p + 0x08);
        phdr.filesz = ReadU64(p + 0x20);
        phdr.memsz = ReadU64(p + 0x28);
        if (phdr.filesz == 0) { continue; }

        Placement placement{};
        placement.phdr_index = index;
        placement.phdr = phdr;

        // Order of preference, each validated against the file:
        //  1. the entry table's offset (that is what the dumper wrote the table for),
        //  2. p_offset relative to the embedded ELF (the ELF's own convention, and the
        //     only one that holds for a 90 MB eboot where both readings fit),
        //  3. p_offset as a file offset (some dumpers rewrite it).
        bool placed = false;
        if (entries_match) {
            const SelfEntry& entry = entries[index];
            if (entry.encrypted()) {
                result.error = FselfError::kRetailEncrypted;
                result.why = "a segment is marked encrypted: this SELF is not fake-signed";
                return result;
            }
            if (entry.offset != 0 && entry.size != 0 && entry.offset + entry.size <= data.size()) {
                placement.src = static_cast<std::size_t>(entry.offset);
                placement.stored = static_cast<std::size_t>(entry.size);
                placement.compressed = entry.compressed();
                placement.blocked = entry.blocked();
                // A "compressed" entry whose stored bytes are already the payload is
                // what an unblocked dumper writes; inflating it would fail, so the
                // sizes decide.
                if (placement.compressed && placement.stored >= phdr.filesz) {
                    placement.compressed = false;
                }
                placed = true;
            }
        }
        if (!placed) {
            const std::uint64_t absolute = phdr.offset;
            const std::uint64_t relative = static_cast<std::uint64_t>(elf_base) + phdr.offset;
            if (relative + phdr.filesz <= data.size()) {
                placement.src = static_cast<std::size_t>(relative);
                placement.stored = static_cast<std::size_t>(phdr.filesz);
                placed = true;
            } else if (absolute + phdr.filesz <= data.size()) {
                placement.src = static_cast<std::size_t>(absolute);
                placement.stored = static_cast<std::size_t>(phdr.filesz);
                placed = true;
            }
        }
        if (!placed) {
            result.error = FselfError::kSegmentOutOfBounds;
            char text[192];
            std::snprintf(text, sizeof(text),
                          "PT_LOAD %u: p_offset 0x%llx, p_filesz 0x%llx, file is 0x%llx bytes",
                          index, static_cast<unsigned long long>(phdr.offset),
                          static_cast<unsigned long long>(phdr.filesz),
                          static_cast<unsigned long long>(data.size()));
            result.why = text;
            return result;
        }
        if (placement.compressed) {
            if (!g_inflate_available) {
                result.error = FselfError::kDecompressUnavailable;
                result.why = "a segment is zlib-compressed and this build has no zlib";
                return result;
            }
            ++result.info.compressed_entries;
        } else {
            ++result.info.plain_entries;
        }
        placements.push_back(placement);
    }

    // The image's size: its own headers, plus every segment at its ELF-relative
    // p_offset. Nothing is placed past this, and a segment that would be is a
    // layout this module does not understand.
    std::uint64_t image_size =
        static_cast<std::uint64_t>(elf.phoff) + elf.phnum * static_cast<std::uint64_t>(elf.phentsize);
    for (const Placement& placement : placements) {
        image_size = std::max(image_size, placement.phdr.offset + placement.phdr.filesz);
    }
    if (image_size > data.size() * 4ull || image_size > (2ull << 30)) {
        result.error = FselfError::kSizeMismatch;
        result.why = "the reconstructed image would be implausibly large";
        return result;
    }
    // The header's own size hint is used when it is at least what the segments
    // need: a flat ELF's trailing padding is part of the file a dumper wrote, and
    // reproducing it keeps the reconstruction's size equal to the original's.
    if (declared_elf_size >= image_size &&
        declared_elf_size <= std::max<std::uint64_t>(image_size + (16ull << 20), data.size() * 2ull)) {
        image_size = declared_elf_size;
    }

    std::vector<std::uint8_t> image(static_cast<std::size_t>(image_size), 0);
    // The header block: ident, Ehdr, Phdrs, and whatever else the file keeps
    // between them. Copied verbatim from the embedded ELF, so every offset in it
    // still means what it said.
    const std::size_t header_bytes = std::min<std::size_t>(
        static_cast<std::size_t>(elf.phoff) + elf.phnum * elf.phentsize, image.size());
    std::memcpy(image.data(), data.data() + elf_base, header_bytes);

    for (const Placement& placement : placements) {
        std::uint8_t* dst = image.data() + placement.phdr.offset;
        const std::uint8_t* src = data.data() + placement.src;
        const std::size_t want = static_cast<std::size_t>(placement.phdr.filesz);
        if (!placement.compressed) {
            std::memcpy(dst, src, std::min<std::size_t>(want, placement.stored));
            continue;
        }
#if defined(BBX_HAVE_ZLIB)
        const bool ok = placement.blocked
                            ? InflateBlocks(src, placement.stored, dst, want)
                            : InflateWhole(src, placement.stored, dst, want);
        if (!ok) {
            result.error = FselfError::kDecompressFailed;
            char text[192];
            std::snprintf(text, sizeof(text),
                          "segment %zu: 0x%zx stored bytes did not inflate to 0x%zx%s",
                          placement.phdr_index, placement.stored, want,
                          placement.blocked ? " (blocked)" : "");
            result.why = text;
            return result;
        }
#else
        result.error = FselfError::kDecompressUnavailable;
        result.why = "compressed segment, no zlib";
        return result;
#endif
    }

    result.info.elf_size = image.size();
    result.elf = std::move(image);
    return result;
}

FselfResult UnwrapOrbisFile(const char* path) {
    FselfResult result{};
    if (path == nullptr || *path == '\0') {
        result.error = FselfError::kTooSmall;
        result.why = "no path";
        return result;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        result.error = FselfError::kTooSmall;
        result.why = std::string("cannot open ") + path;
        return result;
    }
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    if (size <= 0) {
        result.error = FselfError::kTooSmall;
        result.why = std::string("empty or unreadable: ") + path;
        return result;
    }
    in.seekg(0);
    std::vector<std::uint8_t> data(static_cast<std::size_t>(size));
    in.read(reinterpret_cast<char*>(data.data()), size);
    if (!in) {
        result.error = FselfError::kTooSmall;
        result.why = std::string("read failed: ") + path;
        return result;
    }
    result = UnwrapOrbisImage(data);
    if (!result.ok()) {
        result.why = std::string(path) + ": " + result.why;
    }
    return result;
}

}  // namespace bbx::core
