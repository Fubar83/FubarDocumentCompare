// Minimal testram utan beroenden: TEST(namn) { CHECK(...); CHECK_EQ(a, b); CHECK_NEAR(a, b, tol); }
#pragma once

#include <cmath>
#include <cstdio>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace pctest {

struct Case {
    const char* name;
    std::function<void()> fn;
};
inline std::vector<Case>& registry() {
    static std::vector<Case> r;
    return r;
}
inline int& failures() {
    static int n = 0;
    return n;
}
struct Register {
    Register(const char* name, std::function<void()> fn) { registry().push_back({name, std::move(fn)}); }
};
template <class T>
std::string show(const T& v) {
    std::ostringstream s;
    s << v;
    return s.str();
}
inline void fail(const char* file, int line, const std::string& what) {
    ++failures();
    std::fprintf(stderr, "  %s:%d: %s\n", file, line, what.c_str());
}

}  // namespace pctest

#define PC_CAT2(a, b) a##b
#define PC_CAT(a, b) PC_CAT2(a, b)
#define TEST(name)                                                              \
    static void name();                                                         \
    static pctest::Register PC_CAT(reg_, name)(#name, name);                    \
    static void name()
#define CHECK(c) \
    do { if (!(c)) pctest::fail(__FILE__, __LINE__, "CHECK(" #c ")"); } while (0)
#define CHECK_EQ(a, b)                                                                                     \
    do {                                                                                                   \
        auto _a = (a);                                                                                     \
        auto _b = (b);                                                                                     \
        if (!(_a == _b)) pctest::fail(__FILE__, __LINE__, #a " == " #b ": " + pctest::show(_a) + " != " + pctest::show(_b)); \
    } while (0)
#define CHECK_NEAR(a, b, tol)                                                                              \
    do {                                                                                                   \
        double _a = (a), _b = (b);                                                                         \
        if (!(std::fabs(_a - _b) <= (tol))) pctest::fail(__FILE__, __LINE__, #a " ≈ " #b ": " + pctest::show(_a) + " vs " + pctest::show(_b)); \
    } while (0)
