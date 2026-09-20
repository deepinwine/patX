// patX test runner. Minimal framework: TEST(name) registers a function,
// main() runs everything and reports failures to CTest via exit code.
#include "test_registry.hpp"

#include <cstdio>
#include <filesystem>
#include <string>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace testutil {

int getpid_valid() {
#ifdef _WIN32
    return static_cast<int>(::GetCurrentProcessId());
#else
    return static_cast<int>(::getpid());
#endif
}

std::string FixturePath(const std::string& relative) {
    // Fixtures are copied next to the test binary at build time; also resolve
    // from the source tree so running the binary from the repo root works.
    auto exe_dir = std::filesystem::current_path();
    for (const auto& candidate : {exe_dir / "fixtures" / relative,
                                  exe_dir / "build-macos" / "fixtures" / relative,
                                  exe_dir / "tests" / "fixtures" / relative}) {
        if (std::filesystem::exists(candidate)) return candidate.string();
    }
    return (exe_dir / "fixtures" / relative).string();
}

std::string TempDbPath(const char* name) {
    static int counter = 0;
    auto dir = std::filesystem::temp_directory_path();
    std::string unique = std::string(name) + "_" + std::to_string(++counter) + "_" +
                         std::to_string(getpid_valid()) + ".db";
    return (dir / unique).string();
}

} // namespace testutil

int main() {
    int failed = 0;
    for (const auto& test : testutil::AllTests()) {
        std::printf("[ RUN  ] %s\n", test.name);
        try {
            test.fn();
            std::printf("[  OK  ] %s\n", test.name);
        } catch (const std::exception& e) {
            std::printf("[ FAIL ] %s: %s\n", test.name, e.what());
            failed++;
        } catch (...) {
            std::printf("[ FAIL ] %s: unknown exception\n", test.name);
            failed++;
        }
    }
    std::printf("\n%zu test(s), %d failure(s)\n",
                testutil::AllTests().size(), failed);
    return failed == 0 ? 0 : 1;
}
