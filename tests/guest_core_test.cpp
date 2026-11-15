// SPDX-License-Identifier: GPL-3.0-or-later
// Guest-core invariants that can be checked without a console, a VM or a game
// image: the parts of bbhost's `src/core` that the boot path depends on and whose
// failures are silent.
//
// What is deliberately not here: `load_orbis_elf`. It is the next test (a
// synthetic ELF64 with one PT_LOAD and a PT_DYNAMIC), but it needs the arena
// plumbing the console supplies, so it belongs with the P1 loader work rather
// than with these pure functions.
#include <cstdio>
#include <cstring>
#include <string>

#include "harness.h"

#include "core/elf.h"
#include "core/imports.h"
#include "core/paced_file.h"
#include "core/sha256.h"
#include "core/tls_rewrite.h"

namespace {

std::string Hex(const unsigned char* data, std::size_t size) { return sha256_hex(data, size); }

}  // namespace

TEST(Sha256MatchesTheStandardVectors) {
    // FIPS 180-2 / NIST's own examples. The digest gates bbhost's
    // address-based patches, so a wrong digest silently selects the wrong build.
    CHECK(Hex(reinterpret_cast<const unsigned char*>(""), 0) ==
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(Hex(reinterpret_cast<const unsigned char*>("abc"), 3) ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(Hex(reinterpret_cast<const unsigned char*>("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
              56) == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST(NidLookupResolvesTheTableAndRefusesTheRest) {
    // Orbis NIDs are 11-character base64 strings; the table is generated from the
    // firmware's own export lists. An import that resolves to the wrong name
    // becomes the wrong HLE function, which is the worst kind of bug.
    CHECK(lookup_nid_name("cfAXurvfl5o") == "__cxa_allocate_exception");
    CHECK(lookup_nid_name("tsvEmnenz48") == "__cxa_atexit");
    CHECK(lookup_nid_name("not-a-nid") == "");
    CHECK(lookup_nid_name("") == "");
}

TEST(TlsGsSlotStaysInsideTheTeb) {
    // The guest's `mov rax, fs:[0]` sites are rewritten to read the TCB from a
    // Windows TEB TLS slot. A slot at or past 64 would read past TlsSlots into
    // whatever Windows keeps next, and the guest would run with a garbage TCB.
    const std::uint32_t slot = tls_gs_slot();
    const std::uint32_t disp = tls_gs_disp();
    CHECK(slot < kTebTlsSlotCount);
    CHECK_EQ(disp, kTebTlsSlots + 8u * slot);
    // TlsSlots is 64 pointers at TEB+0x1480, so the last valid displacement is
    // 0x1480 + 8 * 63; the rewrite writes a 4-byte displacement and must stay
    // inside.
    CHECK(disp + 8u <= kTebTlsSlots + 8u * kTebTlsSlotCount);
    CHECK(disp + 4u <= kTebStandInSize || !tls_gs_mode());
}

TEST(TlsRewriteTurnsExactlyTheFsSelfPointerIntoAGsRead) {
    // 17,127 sites in the 1.09 eboot, all `64 48 8b 04 25 00 00 00 00`. The
    // rewrite is in place and must be exactly the same length: the surrounding
    // code's relative offsets were computed by the linker.
    unsigned char text[64];
    std::memset(text, 0x90, sizeof(text));
    const unsigned char kFrom[9] = {0x64, 0x48, 0x8b, 0x04, 0x25, 0, 0, 0, 0};
    std::memcpy(text + 4, kFrom, sizeof(kFrom));
    std::memcpy(text + 40, kFrom, sizeof(kFrom));
    // Not the FS self pointer: a `mov rax, fs:[0x28]` (stack cookie) must survive
    // untouched, and so must the same bytes with a different offset.
    unsigned char other[9] = {0x64, 0x48, 0x8b, 0x04, 0x25, 0x28, 0, 0, 0};
    std::memcpy(text + 20, other, sizeof(other));

    // The real entry point: a segment of an image, addressed by guest VA. The
    // arena here is the test's own buffer, slide 0, so va == offset.
    ElfImage image;
    image.mem.base = text;
    image.mem.size = sizeof(text);
    image.mem.slide = 0;
    TlsRewriteStats stats;
    tls_rewrite_to_gs(&image, 0, sizeof(text), &stats);

    CHECK_EQ(stats.rewritten, 2u);
    const std::uint32_t disp = tls_gs_disp();
    for (std::size_t offset : {std::size_t{4}, std::size_t{40}}) {
        CHECK_EQ(static_cast<int>(text[offset]), 0x65);            // gs, not fs
        CHECK_EQ(static_cast<int>(text[offset + 1]), 0x48);
        CHECK_EQ(static_cast<int>(text[offset + 2]), 0x8b);
        CHECK_EQ(static_cast<int>(text[offset + 3]), 0x04);
        CHECK_EQ(static_cast<int>(text[offset + 4]), 0x25);
        std::uint32_t written = 0;
        std::memcpy(&written, text + offset + 5, sizeof(written));
        CHECK_EQ(written, disp);
    }
    // The stack-cookie read is left alone, and so is everything else.
    CHECK(std::memcmp(text + 20, other, sizeof(other)) == 0);
    CHECK_EQ(static_cast<int>(text[0]), 0x90);
}

TEST(HostPacedFileCommitsByRenameAndKeepsTheOldFileOnFailure) {
    // The shader caches are written through this: a crash (or a full disk) mid
    // write must leave the previous cache intact rather than truncated, and the
    // directory must never hold a half-written file with the real name.
    const std::string path = "/tmp/bbx_paced_file_test.bin";
    const std::string tmp = path + ".tmp";
    std::remove(path.c_str());
    std::remove(tmp.c_str());

    {
        HostPacedFile writer(path, tmp, 4);
        CHECK(writer.write("abcd", 4));
        CHECK(writer.write("efgh", 4));
        CHECK(writer.commit());
    }
    FILE* file = std::fopen(path.c_str(), "rb");
    CHECK(file != nullptr);
    if (file) {
        char buffer[16] = {};
        const std::size_t read = std::fread(buffer, 1, sizeof(buffer), file);
        std::fclose(file);
        CHECK_EQ(read, 8u);
        CHECK(std::memcmp(buffer, "abcdefgh", 8) == 0);
    }
    CHECK(std::fopen(tmp.c_str(), "rb") == nullptr);  // renamed, not copied

    // Not committed: the temporary goes, the committed file stays as it was.
    {
        HostPacedFile writer(path, tmp, 4);
        CHECK(writer.write("zzzz", 4));
    }
    file = std::fopen(path.c_str(), "rb");
    CHECK(file != nullptr);
    if (file) {
        char buffer[16] = {};
        const std::size_t read = std::fread(buffer, 1, sizeof(buffer), file);
        std::fclose(file);
        CHECK_EQ(read, 8u);
        CHECK(std::memcmp(buffer, "abcdefgh", 8) == 0);
    }
    CHECK(std::fopen(tmp.c_str(), "rb") == nullptr);
    std::remove(path.c_str());
}
