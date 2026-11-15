// SPDX-License-Identifier: GPL-3.0-or-later
// A test harness small enough to read: no framework, no dependency, exit code
// 1 on failure. Every test is a function in a namespace; a translation unit
// registers its cases with TEST().
#pragma once

#include <cstdio>
#include <optional>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

namespace bbx::test {

struct Case {
    const char* name;
    void (*fn)();
};

std::vector<Case>& Registry();
int& Failures();
const char* CurrentCase();

struct Registrar {
    Registrar(const char* name, void (*fn)()) { Registry().push_back(Case{name, fn}); }
};

// Values that are not arithmetic (enums, flags) print their numeric value.
template <class T>
std::string Show(const std::optional<T>& value);

template <class T>
std::string Show(const T& value) {
    if constexpr (std::is_arithmetic_v<T>) {
        return std::to_string(value);
    } else if constexpr (std::is_enum_v<T>) {
        return std::to_string(static_cast<long long>(value));
    } else {
        std::ostringstream out;
        out << value;
        return out.str();
    }
}

template <class T>
std::string Show(const std::optional<T>& value) {
    return value.has_value() ? Show(*value) : std::string("nullopt");
}

inline void Report(const char* file, int line, const std::string& message) {
    std::printf("  FAIL %s:%d [%s] %s\n", file, line, CurrentCase(), message.c_str());
    ++Failures();
}

}  // namespace bbx::test

#define TEST(name)                                                     \
    static void name();                                                \
    static ::bbx::test::Registrar registrar_##name(#name, &name);       \
    static void name()

#define CHECK(condition)                                                          \
    do {                                                                          \
        if (!(condition)) ::bbx::test::Report(__FILE__, __LINE__, #condition);     \
    } while (0)

#define CHECK_EQ(a, b)                                                                        \
    do {                                                                                      \
        const auto va = (a);                                                                  \
        const auto vb = (b);                                                                  \
        if (!(va == vb)) {                                                                    \
            ::bbx::test::Report(__FILE__, __LINE__,                                           \
                                std::string(#a " == " #b " (") + ::bbx::test::Show(va) +       \
                                    " vs " + ::bbx::test::Show(vb) + ")");                     \
        }                                                                                     \
    } while (0)

#define CHECK_MSG(condition, message) \
    do {                              \
        if (!(condition)) ::bbx::test::Report(__FILE__, __LINE__, (message)); \
    } while (0)
