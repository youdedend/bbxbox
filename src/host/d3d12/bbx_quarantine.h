// SPDX-License-Identifier: GPL-3.0-or-later
// The precompile breadcrumb and the quarantine.
//
// A shader or a PSO that crashes DXC or the driver must cost one title boot,
// not every boot. The protocol is write-ahead: the entry is on disk (flushed)
// before the dangerous call, so a `__fastfail` inside the CRT -- which no
// exception handler sees -- still leaves a record. The next start turns that
// record into a quarantine entry and skips the shader.
//
// Text, not binary: the file is the only evidence after a console crash, so it
// has to be readable in Device Portal's file browser.
#pragma once

#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace bbx::d3d12 {

enum class ShaderKind : std::uint8_t { kVertex = 0, kPixel, kCompute, kGeometry, kHull, kDomain };

[[nodiscard]] const char* ShaderKindName(ShaderKind kind);

// One unit of work: a stage on its own, or a link (a VS and a PS that must
// compile together). `partner` is 0 for a single stage.
struct Unit {
    std::uint64_t hash = 0;
    std::uint64_t partner = 0;
    ShaderKind kind = ShaderKind::kVertex;
    std::string bundle;       // "GXShader.shaderbnd.dcx"
    std::string entry;        // "GXFlver_Gbuf_[A].vso"
    std::string partner_entry;

    [[nodiscard]] bool is_pair() const { return partner != 0; }
};

// LocalState\precompile_inflight.txt, as parsed.
struct Inflight {
    bool present = false;
    Unit unit;
    std::uint32_t index = 0;   // item N of the current pass
    std::uint32_t total = 0;
};

// `1\nkind <hash> [partner] <stage> <index> <total> <bundle> <entry> [partner-entry]\n`
[[nodiscard]] std::string SerializeInflight(const Unit& unit, std::uint32_t index,
                                            std::uint32_t total);
// Never throws and never crashes: a corrupt breadcrumb is worse than useless,
// so an unparsable file yields `present = false` and the caller logs it.
[[nodiscard]] Inflight ParseInflight(std::string_view text);

// The quarantine file: one 16-hex-digit hash per line, sorted and unique.
class Quarantine {
public:
    // Parses `text`; tolerates CRLF, blank lines, a missing trailing newline
    // and (by ignoring it) a garbage line, so one corrupted write does not
    // discard the whole quarantine.
    static Quarantine Parse(std::string_view text);

    [[nodiscard]] bool Contains(std::uint64_t hash) const;
    [[nodiscard]] bool Contains(const Unit& unit) const;
    void Add(std::uint64_t hash);
    void Add(const Unit& unit);  // both hashes

    [[nodiscard]] const std::set<std::uint64_t>& hashes() const { return hashes_; }
    [[nodiscard]] std::size_t size() const { return hashes_.size(); }
    [[nodiscard]] std::string Serialize() const;
    // Entries a human should see in a bug report, capped.
    [[nodiscard]] std::string Summary(std::size_t max_entries = 8) const;

private:
    std::set<std::uint64_t> hashes_;
};

// What the start-up pass decided about a breadcrumb.
struct CrashRecovery {
    bool found = false;
    bool promoted = false;
    Unit unit;
    std::string message;  // the log line, ready to print
};

// The whole rule in one call: an inflight file means the last run died inside
// that unit, so it joins the quarantine. The caller is responsible for writing
// the new quarantine back and clearing the inflight file.
[[nodiscard]] CrashRecovery RecoverFromInflight(std::string_view inflight_text,
                                                Quarantine& quarantine);

// Hash of a stage's key material, as the quarantine and the cache name it.
// The port hashes the container bytes plus the translator identity; the
// precompiler fills in what it has and `program_hash` is what a log prints.
[[nodiscard]] std::uint64_t StageHash(std::uint64_t program_hash, ShaderKind kind,
                                      std::string_view translator_stamp);

}  // namespace bbx::d3d12
