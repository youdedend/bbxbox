// SPDX-License-Identifier: GPL-3.0-or-later
#include "harness.h"

#include <cstring>

namespace bbx::test {
namespace {
const char* g_current = "";
}

std::vector<Case>& Registry() {
    static std::vector<Case> registry;
    return registry;
}

int& Failures() {
    static int failures = 0;
    return failures;
}

const char* CurrentCase() { return g_current; }

}  // namespace bbx::test

int main(int argc, char** argv) {
    using namespace bbx::test;
    const char* filter = argc > 1 ? argv[1] : nullptr;
    int ran = 0;
    for (const Case& c : Registry()) {
        if (filter && std::strstr(c.name, filter) == nullptr) continue;
        g_current = c.name;
        const int before = Failures();
        c.fn();
        ++ran;
        if (Failures() == before) {
            std::printf("  ok   %s\n", c.name);
        }
    }
    std::printf("%d case(s), %d failure(s)\n", ran, Failures());
    return Failures() == 0 ? 0 : 1;
}
