// SPDX-License-Identifier: GPL-3.0-or-later
// The PS4 fake-SELF unwrapper, on fixtures built here rather than on a dumped eboot.
//
// Every case builds its own bytes: a minimal but valid 64-bit x86-64 ELF, wrapped
// in a fSELF with and without compression, and the four ways a dump goes wrong
// (truncated table, no ELF inside, retail-signed, corrupt stream). A real dump is
// 90 MB of somebody's game; these are the same bytes at 1/100000 the size, and they
// pin the parser's rules rather than one file's luck.
#include "harness.h"

#include "src/host/core/bbx_self.h"

#include <cstring>
#include <string>
#include <vector>

#if defined(BBX_HAVE_ZLIB)
#include <zlib.h>
#endif

using bbx::core::FselfError;
using bbx::core::FselfResult;
using bbx::core::UnwrapOrbisImage;

namespace {

constexpr std::size_t kElfHeaderSize = 64;
constexpr std::size_t kPhdrSize = 56;
constexpr std::size_t kSegmentOneOffset = 0x200;
constexpr std::size_t kSegmentOneSize = 0x100;
constexpr std::size_t kSegmentTwoOffset = 0x400;
constexpr std::size_t kSegmentTwoSize = 0x80;
constexpr std::size_t kElfSize = 0x600;

void Put16(std::vector<std::uint8_t>& out, std::size_t at, std::uint16_t value) {
    out[at] = static_cast<std::uint8_t>(value & 0xFF);
    out[at + 1] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
}

void Put32(std::vector<std::uint8_t>& out, std::size_t at, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) { out[at + i] = static_cast<std::uint8_t>((value >> (8 * i)) & 0xFF); }
}

void Put64(std::vector<std::uint8_t>& out, std::size_t at, std::uint64_t value) {
    for (int i = 0; i < 8; ++i) { out[at + i] = static_cast<std::uint8_t>((value >> (8 * i)) & 0xFF); }
}

// A flat ELF: two PT_LOADs, an empty PT_DYNAMIC, and two recognisable payloads
// ("SEGMENT-ONE"/"SEGMENT-TWO" at their starts) so a placement bug shows up as a
// wrong string rather than as arithmetic that happens to agree.
std::vector<std::uint8_t> MakeElf() {
    std::vector<std::uint8_t> elf(kElfSize, 0);
    elf[0] = 0x7F;
    elf[1] = 'E';
    elf[2] = 'L';
    elf[3] = 'F';
    elf[4] = 2;  // ELFCLASS64
    elf[5] = 1;  // ELFDATA2LSB
    elf[6] = 1;  // EV_CURRENT
    Put16(elf, 0x10, 2);   // e_type: ET_EXEC
    Put16(elf, 0x12, 62);  // e_machine: x86-64
    Put32(elf, 0x14, 1);   // e_version
    Put64(elf, 0x18, 0x400100);  // e_entry
    Put64(elf, 0x20, kElfHeaderSize);  // e_phoff
    Put16(elf, 0x34, kElfHeaderSize);  // e_ehsize
    Put16(elf, 0x36, kPhdrSize);       // e_phentsize
    Put16(elf, 0x38, 3);               // e_phnum

    auto phdr = [&elf](std::size_t index, std::uint32_t type, std::uint64_t offset,
                       std::uint64_t vaddr, std::uint64_t filesz, std::uint64_t memsz,
                       std::uint32_t flags) {
        const std::size_t at = kElfHeaderSize + index * kPhdrSize;
        Put32(elf, at + 0x00, type);
        Put32(elf, at + 0x04, flags);
        Put64(elf, at + 0x08, offset);
        Put64(elf, at + 0x10, vaddr);
        Put64(elf, at + 0x18, vaddr);
        Put64(elf, at + 0x20, filesz);
        Put64(elf, at + 0x28, memsz);
        Put64(elf, at + 0x30, 0x1000);
    };
    phdr(0, 1, kSegmentOneOffset, 0x400000, kSegmentOneSize, kSegmentOneSize, 5);
    phdr(1, 1, kSegmentTwoOffset, 0x500000, kSegmentTwoSize, kSegmentTwoSize, 6);
    phdr(2, 2, 0x580, 0x500580, 0x20, 0x20, 6);

    std::memcpy(elf.data() + kSegmentOneOffset, "SEGMENT-ONE", 11);
    std::memcpy(elf.data() + kSegmentTwoOffset, "SEGMENT-TWO", 11);
    return elf;
}

// The fSELF wrapper. `entries` describe where each segment's bytes really are.
struct EntrySpec {
    std::uint64_t props = 0;
    std::uint64_t offset = 0;
    std::uint64_t size = 0;
    std::uint64_t size2 = 0;
};

std::vector<std::uint8_t> MakeFself(const std::vector<std::uint8_t>& elf,
                                    const std::vector<EntrySpec>& entries, std::uint16_t header_type,
                                    std::size_t trailer) {
    const std::size_t elf_base = 0x20 + entries.size() * 32;
    std::vector<std::uint8_t> self(elf_base + elf.size() + trailer, 0);
    Put32(self, 0x00, 0x1D3D154Fu);
    Put32(self, 0x04, 3);
    Put16(self, 0x08, 0x8000);
    Put16(self, 0x0A, header_type);
    Put32(self, 0x0C, 0x20);
    Put32(self, 0x10, 0x20);
    Put32(self, 0x14, static_cast<std::uint32_t>(elf.size()));
    Put32(self, 0x18, static_cast<std::uint32_t>(entries.size()));
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const std::size_t at = 0x20 + i * 32;
        Put64(self, at + 0x00, entries[i].props);
        Put64(self, at + 0x08, entries[i].offset);
        Put64(self, at + 0x10, entries[i].size);
        Put64(self, at + 0x18, entries[i].size2);
    }
    std::memcpy(self.data() + elf_base, elf.data(), elf.size());
    return self;
}

std::vector<std::uint8_t> Compress(const std::uint8_t* data, std::size_t size) {
#if defined(BBX_HAVE_ZLIB)
    uLongf bound = ::compressBound(static_cast<uLong>(size));
    std::vector<std::uint8_t> out(bound);
    const int rc = ::compress(out.data(), &bound, data, static_cast<uLong>(size));
    if (rc != Z_OK) { return {}; }
    out.resize(bound);
    return out;
#else
    (void)data;
    (void)size;
    return {};
#endif
}

// Is `needle` present at `at` in `image`?
bool At(const std::vector<std::uint8_t>& image, std::size_t at, const char* needle) {
    const std::size_t length = std::strlen(needle);
    return at + length <= image.size() && std::memcmp(image.data() + at, needle, length) == 0;
}

}  // namespace

TEST(PlainElfIsPassedThroughUnchanged) {
    const std::vector<std::uint8_t> elf = MakeElf();
    const FselfResult result = UnwrapOrbisImage(elf);
    CHECK(result.ok());
    CHECK(!result.info.was_fself);
    CHECK_EQ(result.info.elf_size, elf.size());
    // Byte-identical: the sha256 that gates the address-based patches must not
    // change for a file that was already flat.
    CHECK(result.elf == elf);
    CHECK(At(result.elf, kSegmentOneOffset, "SEGMENT-ONE"));
}

TEST(UncompressedFselfUnwrapsToTheSameElf) {
    const std::vector<std::uint8_t> elf = MakeElf();
    const std::size_t elf_base = 0x20 + 3 * 32;
    // The entry table points at the segments where they already are, which is what
    // an uncompressed fSELF from a dumper looks like.
    std::vector<EntrySpec> entries(3);
    entries[0].offset = elf_base + kSegmentOneOffset;
    entries[0].size = kSegmentOneSize;
    entries[1].offset = elf_base + kSegmentTwoOffset;
    entries[1].size = kSegmentTwoSize;
    entries[2].offset = elf_base + 0x580;
    entries[2].size = 0x20;
    const std::vector<std::uint8_t> self = MakeFself(elf, entries, 1, 0);

    const FselfResult result = UnwrapOrbisImage(self);
    CHECK(result.ok());
    CHECK(result.info.was_fself);
    CHECK_EQ(result.info.header_type, 1);
    CHECK_EQ(result.info.num_entries, 3u);
    CHECK_EQ(result.info.compressed_entries, 0u);
    CHECK_EQ(result.info.plain_entries, 3u);
    CHECK_EQ(result.info.elf_base, elf_base);
    CHECK_EQ(result.info.segments, 3);
    // The reconstructed image is the ELF itself: same size, same bytes where the
    // segments and the headers live.
    CHECK_EQ(result.elf.size(), elf.size());
    CHECK(At(result.elf, kSegmentOneOffset, "SEGMENT-ONE"));
    CHECK(At(result.elf, kSegmentTwoOffset, "SEGMENT-TWO"));
    CHECK(At(result.elf, 0, "\x7f" "ELF"));
    // The program headers survive the move, so a loader that walks them finds the
    // same offsets it would have found in the flat file.
    CHECK(result.elf[0x36] == 56 && result.elf[0x37] == 0);
}

TEST(ZlibCompressedFselfInflatesEverySegment) {
    if (!bbx::core::InflateAvailable()) {
        // A build without zlib must still be honest about it; the case below pins
        // that, so this one is allowed to be skipped here.
        return;
    }
    const std::vector<std::uint8_t> elf = MakeElf();
    const std::size_t elf_base = 0x20 + 3 * 32;
    const std::vector<std::uint8_t> one =
        Compress(elf.data() + kSegmentOneOffset, kSegmentOneSize);
    const std::vector<std::uint8_t> two =
        Compress(elf.data() + kSegmentTwoOffset, kSegmentTwoSize);
    CHECK(!one.empty());
    CHECK(!two.empty());

    // The compressed streams are appended after the embedded ELF, and the entries
    // point at them with the "compressed" property bit set.
    const std::size_t one_at = elf_base + elf.size();
    const std::size_t two_at = one_at + one.size();
    std::vector<std::uint8_t> self =
        MakeFself(elf, std::vector<EntrySpec>(3), 1, one.size() + two.size());
    std::memcpy(self.data() + one_at, one.data(), one.size());
    std::memcpy(self.data() + two_at, two.data(), two.size());
    Put64(self, 0x20 + 0 * 32 + 0x00, std::uint64_t{1} << 4);
    Put64(self, 0x20 + 0 * 32 + 0x08, one_at);
    Put64(self, 0x20 + 0 * 32 + 0x10, one.size());
    Put64(self, 0x20 + 1 * 32 + 0x00, std::uint64_t{1} << 4);
    Put64(self, 0x20 + 1 * 32 + 0x08, two_at);
    Put64(self, 0x20 + 1 * 32 + 0x10, two.size());
    Put64(self, 0x20 + 2 * 32 + 0x00, 0);
    Put64(self, 0x20 + 2 * 32 + 0x08, elf_base + 0x580);
    Put64(self, 0x20 + 2 * 32 + 0x10, 0x20);

    const FselfResult result = UnwrapOrbisImage(self);
    CHECK(result.ok());
    CHECK_EQ(result.info.compressed_entries, 2u);
    CHECK_EQ(result.info.plain_entries, 1u);
    CHECK_EQ(result.elf.size(), elf.size());
    CHECK(At(result.elf, kSegmentOneOffset, "SEGMENT-ONE"));
    CHECK(At(result.elf, kSegmentTwoOffset, "SEGMENT-TWO"));
}

TEST(BlockedCompressedFselfInflatesBlockByBlock) {
    if (!bbx::core::InflateAvailable()) { return; }
    // A blocked entry is a run of independent zlib streams, one per 16 KiB of
    // output. Force two blocks by giving the segment a second stream.
    const std::vector<std::uint8_t> elf = MakeElf();
    const std::size_t elf_base = 0x20 + 3 * 32;
    const std::vector<std::uint8_t> block =
        Compress(elf.data() + kSegmentOneOffset, kSegmentOneSize);
    CHECK(!block.empty());
    std::vector<std::uint8_t> self = MakeFself(elf, std::vector<EntrySpec>(3), 1, block.size());
    Put64(self, 0x20 + 0x00, (std::uint64_t{1} << 4) | (std::uint64_t{1} << 8));
    Put64(self, 0x20 + 0x08, elf_base + elf.size());
    Put64(self, 0x20 + 0x10, block.size());
    std::memcpy(self.data() + elf_base + elf.size(), block.data(), block.size());
    Put64(self, 0x20 + 32 + 0x08, elf_base + kSegmentTwoOffset);
    Put64(self, 0x20 + 32 + 0x10, kSegmentTwoSize);

    const FselfResult result = UnwrapOrbisImage(self);
    CHECK(result.ok());
    CHECK(At(result.elf, kSegmentOneOffset, "SEGMENT-ONE"));
}

TEST(TruncatedFselfIsRejectedWithAReason) {
    // A table that runs into the embedded ELF, and a count no eboot has. The first
    // is a corrupt file and is refused; the second is a corrupt header, and the
    // segments are still plaintext where the ELF says they are.
    std::vector<std::uint8_t> overlapping = MakeFself(MakeElf(), std::vector<EntrySpec>(3), 1, 0);
    Put32(overlapping, 0x18, 5);  // 0x20 + 5*32 = 0xC0 > elf at 0x80 + 32
    const FselfResult bad_table = UnwrapOrbisImage(overlapping);
    CHECK(!bad_table.ok());
    CHECK(bad_table.error == FselfError::kBadEntryTable);
    CHECK(!bad_table.why.empty());

    std::vector<std::uint8_t> absurd = MakeFself(MakeElf(), std::vector<EntrySpec>(3), 1, 0);
    Put32(absurd, 0x18, 4096);
    const FselfResult ignored = UnwrapOrbisImage(absurd);
    CHECK(ignored.ok());
    CHECK(!ignored.info.warning.empty());
    CHECK(At(ignored.elf, kSegmentOneOffset, "SEGMENT-ONE"));

    // Cut inside the ELF header block: no plausible Elf64_Ehdr survives.
    std::vector<std::uint8_t> short_file = MakeFself(MakeElf(), std::vector<EntrySpec>(3), 1, 0);
    short_file.resize(0x30 + 8);
    const FselfResult cut = UnwrapOrbisImage(short_file);
    CHECK(!cut.ok());
    CHECK(cut.error == FselfError::kNoEmbeddedElf);

    // Nothing at all, and a file that is neither format.
    const FselfResult tiny = UnwrapOrbisImage(std::vector<std::uint8_t>{0x4F, 0x15});
    CHECK(!tiny.ok());
    CHECK(tiny.error == FselfError::kTooSmall);
    const FselfResult other = UnwrapOrbisImage(std::vector<std::uint8_t>(64, 0xAB));
    CHECK(!other.ok());
    CHECK(other.error == FselfError::kNotOrbisImage);
}

TEST(RetailSignedSelfIsRejectedByName) {
    const std::vector<std::uint8_t> elf = MakeElf();
    std::vector<EntrySpec> entries(3);
    entries[0].offset = 0x80 + kSegmentOneOffset;
    entries[0].size = kSegmentOneSize;
    const std::vector<std::uint8_t> signed_self = MakeFself(elf, entries, 2, 0);
    const FselfResult result = UnwrapOrbisImage(signed_self);
    CHECK(!result.ok());
    CHECK(result.error == FselfError::kRetailEncrypted);
    // The message has to send the user to the right fix.
    CHECK(result.why.find("signed retail SELF") != std::string::npos);

    // The other half of the same rule: a fSELF-shaped file whose entry table marks
    // a segment encrypted is a retail SELF in disguise.
    std::vector<std::uint8_t> disguised = MakeFself(elf, entries, 1, 0);
    Put64(disguised, 0x20 + 0x00, 1);  // props: encrypted
    const FselfResult marked = UnwrapOrbisImage(disguised);
    CHECK(!marked.ok());
    CHECK(marked.error == FselfError::kRetailEncrypted);
}

TEST(CorruptCompressedStreamFailsCleanly) {
    if (!bbx::core::InflateAvailable()) { return; }
    const std::vector<std::uint8_t> elf = MakeElf();
    const std::size_t elf_base = 0x20 + 3 * 32;
    std::vector<std::uint8_t> self = MakeFself(elf, std::vector<EntrySpec>(3), 1, 64);
    Put64(self, 0x20 + 0x00, std::uint64_t{1} << 4);
    Put64(self, 0x20 + 0x08, elf_base + elf.size());
    Put64(self, 0x20 + 0x10, 64);
    // Garbage where a zlib stream should be.
    for (std::size_t i = 0; i < 64; ++i) {
        self[elf_base + elf.size() + i] = static_cast<std::uint8_t>(0x5A + i);
    }
    const FselfResult result = UnwrapOrbisImage(self);
    CHECK(!result.ok());
    CHECK(result.error == FselfError::kDecompressFailed);
    CHECK(!result.why.empty());
}

TEST(NoInflateIsReportedRatherThanGuessed) {
    // A build without zlib must say so, not silently copy compressed bytes into the
    // image and let the guest fault later with no breadcrumb.
    if (!bbx::core::InflateAvailable()) { return; }
    bbx::core::SetInflateAvailableForTesting(false);
    const std::vector<std::uint8_t> elf = MakeElf();
    const std::size_t elf_base = 0x20 + 3 * 32;
    std::vector<std::uint8_t> self = MakeFself(elf, std::vector<EntrySpec>(3), 1, 32);
    Put64(self, 0x20 + 0x00, std::uint64_t{1} << 4);
    Put64(self, 0x20 + 0x08, elf_base + elf.size());
    Put64(self, 0x20 + 0x10, 32);
    const FselfResult result = UnwrapOrbisImage(self);
    bbx::core::SetInflateAvailableForTesting(true);
    CHECK(!result.ok());
    CHECK(result.error == FselfError::kDecompressUnavailable);
}

TEST(FselfInfoDescribesWhatItDid) {
    const std::vector<std::uint8_t> elf = MakeElf();
    std::vector<EntrySpec> entries(3);
    entries[0].offset = 0x80 + kSegmentOneOffset;
    entries[0].size = kSegmentOneSize;
    const FselfResult result = UnwrapOrbisImage(MakeFself(elf, entries, 1, 0));
    CHECK(result.ok());
    const std::string described = result.info.Describe();
    CHECK(described.find("fSELF v3") != std::string::npos);
    CHECK(described.find("0x80") != std::string::npos);

    const std::string plain = UnwrapOrbisImage(elf).info.Describe();
    CHECK(plain.find("plain ELF") != std::string::npos);
}

TEST(UnreadableFileSaysWhichFile) {
    const FselfResult missing = bbx::core::UnwrapOrbisFile("/nonexistent/eboot.bin");
    CHECK(!missing.ok());
    CHECK(missing.error == FselfError::kTooSmall);
    CHECK(missing.why.find("/nonexistent/eboot.bin") != std::string::npos);
    const FselfResult none = bbx::core::UnwrapOrbisFile(nullptr);
    CHECK(!none.ok());
}
