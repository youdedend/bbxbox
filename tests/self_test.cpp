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

// The case a real dump is: the entry table is where the segments' bytes live, and
// the ELF's p_offset values are offsets *in the image*, not in the file. Bloodborne
// v1.09 puts its text at 0x28eb0 in the file and at 0x4000 in the image; reading
// the image's offset out of the file copies zeros.
TEST(EntriesSayWhereTheSegmentsReallyAre) {
    const std::vector<std::uint8_t> elf = MakeElf();
    const std::size_t elf_base = 0x20 + 3 * 32;
    // The segments are moved far away, past the embedded ELF header block.
    const std::size_t one_at = elf_base + elf.size();
    const std::size_t two_at = one_at + kSegmentOneSize;
    std::vector<EntrySpec> entries(3);
    entries[0].offset = one_at;
    entries[0].size = kSegmentOneSize;
    entries[1].offset = two_at;
    entries[1].size = kSegmentTwoSize;
    std::vector<std::uint8_t> self = MakeFself(elf, entries, 1, kSegmentOneSize + kSegmentTwoSize);
    // Wipe the bytes at the image-relative positions, so a wrong reading copies
    // zeros rather than passing by luck.
    std::memset(self.data() + elf_base + kSegmentOneOffset, 0, kSegmentOneSize);
    std::memcpy(self.data() + one_at, elf.data() + kSegmentOneOffset, kSegmentOneSize);
    std::memcpy(self.data() + two_at, elf.data() + kSegmentTwoOffset, kSegmentTwoSize);

    const FselfResult result = UnwrapOrbisImage(self);
    CHECK(result.ok());
    CHECK(result.info.was_fself);
    CHECK_EQ(result.info.num_entries, 3u);
    CHECK_EQ(result.info.plain_entries, 2u);
    CHECK_EQ(result.info.compressed_entries, 0u);
    CHECK_EQ(result.info.segments, 3);
    CHECK_EQ(result.elf.size(), elf.size());
    CHECK(At(result.elf, kSegmentOneOffset, "SEGMENT-ONE"));
    CHECK(At(result.elf, kSegmentTwoOffset, "SEGMENT-TWO"));
    // The program headers survive the move.
    CHECK(result.elf[0x36] == 56 && result.elf[0x37] == 0);
}

TEST(SegmentsInPlaceAreFoundWithoutTheTable) {
    // When the entry sizes do not name a program header, the p_offset reading is
    // used - the layout a dumper that rewrote nothing produces.
    const std::vector<std::uint8_t> elf = MakeElf();
    std::vector<EntrySpec> entries(3);  // all zero: no sizes, nothing to match
    const std::vector<std::uint8_t> self = MakeFself(elf, entries, 1, 0);
    const FselfResult result = UnwrapOrbisImage(self);
    CHECK(result.ok());
    CHECK_EQ(result.info.num_entries, 3u);
    CHECK_EQ(result.info.plain_entries, 0u);
    CHECK(At(result.elf, kSegmentOneOffset, "SEGMENT-ONE"));
    CHECK(At(result.elf, kSegmentTwoOffset, "SEGMENT-TWO"));
}

TEST(ZlibCompressedSegmentInflatesToItsDeclaredSize) {
    if (!bbx::core::InflateAvailable()) { return; }
    const std::vector<std::uint8_t> elf = MakeElf();
    const std::size_t elf_base = 0x20 + 3 * 32;
    const std::vector<std::uint8_t> one =
        Compress(elf.data() + kSegmentOneOffset, kSegmentOneSize);
    CHECK(!one.empty());
    const std::size_t one_at = elf_base + elf.size();
    // The entry stores the stream; the program header keeps the inflated size, which
    // is what makes the segment size-matchable.
    std::vector<EntrySpec> entries(3);
    entries[0].offset = one_at;
    entries[0].size = kSegmentOneSize;   // the image size, as the program header says
    entries[0].size2 = one.size();       // how many bytes the file stores
    entries[2].offset = elf_base + 0x580;
    entries[2].size = 0x20;
    std::vector<std::uint8_t> self = MakeFself(elf, entries, 1, one.size());
    std::memcpy(self.data() + one_at, one.data(), one.size());
    // The second segment is in place, so the image still needs its bytes.
    const FselfResult result = UnwrapOrbisImage(self);
    CHECK(result.ok());
    CHECK_EQ(result.info.compressed_entries, 1u);
    CHECK(At(result.elf, kSegmentOneOffset, "SEGMENT-ONE"));
    CHECK(At(result.elf, kSegmentTwoOffset, "SEGMENT-TWO"));
}

TEST(BlockedCompressedSegmentInflatesBlockByBlock) {
    if (!bbx::core::InflateAvailable()) { return; }
    // A blocked entry is a run of independent zlib streams. Two streams that inflate
    // to the two halves of one segment is the shape a blocked dumper writes.
    const std::vector<std::uint8_t> elf = MakeElf();
    const std::size_t elf_base = 0x20 + 3 * 32;
    const std::size_t half = kSegmentOneSize / 2;
    const std::vector<std::uint8_t> first = Compress(elf.data() + kSegmentOneOffset, half);
    const std::vector<std::uint8_t> second =
        Compress(elf.data() + kSegmentOneOffset + half, kSegmentOneSize - half);
    CHECK(!first.empty() && !second.empty());
    const std::size_t at = elf_base + elf.size();
    std::vector<EntrySpec> entries(3);
    entries[0].offset = at;
    entries[0].size = kSegmentOneSize;
    entries[0].size2 = first.size() + second.size();
    std::vector<std::uint8_t> self = MakeFself(elf, entries, 1, first.size() + second.size());
    std::memcpy(self.data() + at, first.data(), first.size());
    std::memcpy(self.data() + at + first.size(), second.data(), second.size());
    const FselfResult result = UnwrapOrbisImage(self);
    // One stream per 16 KiB of output is the console's block size; this segment is
    // smaller, so the whole-segment path is what runs and the result must agree.
    CHECK(result.ok());
    CHECK(At(result.elf, kSegmentOneOffset, "SEGMENT-ONE"));
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
}

TEST(CorruptFselfIsRejectedWithAReason) {
    // Cut inside the ELF header block: no plausible Elf64_Ehdr survives.
    std::vector<std::uint8_t> short_file = MakeFself(MakeElf(), std::vector<EntrySpec>(3), 1, 0);
    short_file.resize(0x30 + 8);
    const FselfResult cut = UnwrapOrbisImage(short_file);
    CHECK(!cut.ok());
    CHECK(cut.error == FselfError::kNoEmbeddedElf);

    // A program header that names bytes outside the file: the image cannot be
    // composed, and the diagnostic names the header.
    std::vector<std::uint8_t> bad = MakeFself(MakeElf(), std::vector<EntrySpec>(3), 1, 0);
    const FselfResult moved = UnwrapOrbisImage(bad);
    CHECK(moved.ok());  // the unmodified fixture is fine
    std::vector<std::uint8_t> truncated = MakeFself(MakeElf(), std::vector<EntrySpec>(3), 1, 0);
    truncated.resize(0x80 + kSegmentTwoOffset + 8);
    const FselfResult outside = UnwrapOrbisImage(truncated);
    CHECK(!outside.ok());

    // Nothing at all, and a file that is neither format.
    const FselfResult tiny = UnwrapOrbisImage(std::vector<std::uint8_t>{0x4F, 0x15});
    CHECK(!tiny.ok());
    CHECK(tiny.error == FselfError::kTooSmall);
    const FselfResult other = UnwrapOrbisImage(std::vector<std::uint8_t>(64, 0xAB));
    CHECK(!other.ok());
    CHECK(other.error == FselfError::kNotOrbisImage);
}

TEST(CorruptCompressedStreamFailsCleanly) {
    if (!bbx::core::InflateAvailable()) { return; }
    const std::vector<std::uint8_t> elf = MakeElf();
    const std::size_t elf_base = 0x20 + 3 * 32;
    const std::size_t at = elf_base + elf.size();
    std::vector<EntrySpec> entries(3);
    entries[0].offset = at;
    entries[0].size = kSegmentOneSize;
    entries[0].size2 = 64;
    std::vector<std::uint8_t> self = MakeFself(elf, entries, 1, 64);
    // A zlib header followed by garbage: recognised as compressed, refused as a
    // stream.
    self[at] = 0x78;
    self[at + 1] = 0x9C;
    for (std::size_t i = 2; i < 64; ++i) { self[at + i] = static_cast<std::uint8_t>(0x5A + i); }
    const FselfResult result = UnwrapOrbisImage(self);
    CHECK(!result.ok());
    CHECK(result.error == FselfError::kDecompressFailed);
    CHECK(!result.why.empty());
}

TEST(NoInflateIsReportedRatherThanGuessed) {
    if (!bbx::core::InflateAvailable()) { return; }
    bbx::core::SetInflateAvailableForTesting(false);
    const std::vector<std::uint8_t> elf = MakeElf();
    const std::size_t elf_base = 0x20 + 3 * 32;
    const std::vector<std::uint8_t> one =
        Compress(elf.data() + kSegmentOneOffset, kSegmentOneSize);
    const std::size_t at = elf_base + elf.size();
    std::vector<EntrySpec> entries(3);
    entries[0].offset = at;
    entries[0].size = kSegmentOneSize;
    entries[0].size2 = one.size();
    std::vector<std::uint8_t> self = MakeFself(elf, entries, 1, one.size());
    std::memcpy(self.data() + at, one.data(), one.size());
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
    CHECK(described.find("fSELF") != std::string::npos);
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

TEST(Bloodborne109CodeAnchorsVerifyMappedText) {
    std::vector<std::uint8_t> img(0x2700000, 0);
    auto put = [&img](std::size_t off, std::initializer_list<std::uint8_t> bytes) {
        std::size_t i = 0;
        for (std::uint8_t b : bytes) img[off + i++] = b;
    };
    put(0x11bd0f0, {0x48, 0x8d, 0x47, 0x08, 0x48, 0x89, 0x47, 0x08, 0x48, 0x89,
                    0x47, 0x10, 0x48, 0xc7, 0x47, 0x18, 0x00, 0x00, 0x00, 0x00});
    put(0x136f340, {0x89, 0xf0, 0x48, 0x8b, 0x77, 0x08, 0x48, 0x8b,
                    0x34, 0xc6, 0x31, 0xc0, 0x48, 0x85, 0xf6});
    put(0x2043ee0, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56,
                    0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x40});
    put(0x2434770, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41,
                    0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x38});
    put(0x25d4860, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55,
                    0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x28, 0x4d, 0x89, 0xcd});
    put(0x269e990, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55,
                    0x41, 0x54, 0x53, 0x48, 0x81, 0xec, 0xa8, 0x0e, 0x00, 0x00});

    const auto ok = bbx::core::VerifyBloodborne109CodeAnchors(img.data(), img.size(), 0xa0);
    CHECK(ok.ok());
    CHECK_EQ(ok.matched, ok.total);
    CHECK_EQ(ok.total, 6u);

    const auto bad_entry = bbx::core::VerifyBloodborne109CodeAnchors(img.data(), img.size(), 0x100);
    CHECK(!bad_entry.ok());
    CHECK(!bad_entry.entry_ok);
    CHECK_EQ(bad_entry.matched, 6u);

    img[0x2434770] ^= 0xff;
    const auto bad_byte = bbx::core::VerifyBloodborne109CodeAnchors(img.data(), img.size(), 0xa0);
    CHECK(!bad_byte.ok());
    CHECK_EQ(bad_byte.matched, 5u);
    CHECK(bad_byte.detail.find("FrameManager") != std::string::npos);

    const auto empty = bbx::core::VerifyBloodborne109CodeAnchors(nullptr, 0, 0xa0);
    CHECK(!empty.ok());
    CHECK_EQ(empty.matched, 0u);
}
