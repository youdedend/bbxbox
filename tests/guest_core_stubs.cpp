// SPDX-License-Identifier: GPL-3.0-or-later
// Test-only link shims for the vendored guest core.
//
// `src/core/imports.cpp` and `src/hle/runtime.cpp` reference the HLE registry and
// the VM's thread entry points. The tests in guest_core_test.cpp exercise the
// loader's pure pieces - SHA-256, the NID table, the TLS rewrite, the paced file -
// none of which call into HLE. Linking the whole of `src/hle` to satisfy three
// symbols would drag the guest VM, the GCN runtime and the audio stack into a
// 200 kB test binary, so the symbols the linker wants are defined here instead.
//
// This file is compiled into `bbx_tests` only. It is not part of bbxbox, and the
// port's rule - no stand-in implementation of the guest core - is intact: the
// stand-ins below are for symbols the tests never reach.
#include <cstdint>
#include <string>

void register_hle() {}
std::uint64_t hle_make_stub(const std::string&, const std::string&, std::uint64_t) { return 0; }
void* hle_wrap_fn(void*) { return nullptr; }
void hle_call_counts_take() {}
void main_wait_sites_report() {}
