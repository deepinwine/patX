// Tiny test registry: TEST(name) { assertions } registers into a global list
// executed by main(). Throw std::runtime_error on failure.
#pragma once

#include <exception>
#include <stdexcept>
#include <functional>
#include <string>
#include <vector>

namespace testutil {

struct TestCase {
    const char* name;
    void (*fn)();
};

inline std::vector<TestCase>& AllTests() {
    static std::vector<TestCase> tests;
    return tests;
}

struct Registrar {
    Registrar(const char* name, void (*fn)()) { AllTests().push_back({name, fn}); }
};

[[noreturn]] inline void Fail(const std::string& message) {
    throw std::runtime_error(message);
}

std::string FixturePath(const std::string& relative);
std::string TempDbPath(const char* name);
int getpid_valid();

} // namespace testutil

#define PATX_TEST_CAT_IMPL(a, b) a##b
#define PATX_TEST_CAT(a, b) PATX_TEST_CAT_IMPL(a, b)
#define TEST(name)                                                        \
    static void PATX_TEST_CAT(test_fn_, __LINE__)();                      \
    static ::testutil::Registrar PATX_TEST_CAT(test_reg_, __LINE__)(      \
        #name, &PATX_TEST_CAT(test_fn_, __LINE__));                       \
    static void PATX_TEST_CAT(test_fn_, __LINE__)()

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond))                                                       \
            ::testutil::Fail(std::string(__FILE__) + ":" +                 \
                             std::to_string(__LINE__) + "  CHECK failed: " #cond); \
    } while (0)

#define CHECK_EQ(a, b)                                                     \
    do {                                                                   \
        auto va_ = (a);                                                    \
        auto vb_ = (b);                                                    \
        if (!(va_ == vb_))                                                 \
            ::testutil::Fail(std::string(__FILE__) + ":" +                 \
                             std::to_string(__LINE__) + "  CHECK_EQ failed: " \
                             #a " != " #b);                                \
    } while (0)

#define CHECK_STR_EQ(a, b)                                                 \
    do {                                                                   \
        std::string va_ = (a);                                             \
        std::string vb_ = (b);                                             \
        if (va_ != vb_)                                                    \
            ::testutil::Fail(std::string(__FILE__) + ":" +                 \
                             std::to_string(__LINE__) + "  expected \"" +  \
                             vb_ + "\" but got \"" + va_ + "\"");          \
    } while (0)
