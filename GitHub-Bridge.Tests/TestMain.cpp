#include "TestFramework.h"

#include <cstdio>
#include <iostream>

namespace ghbt {

std::vector<TestCase>& Registry() {
    static std::vector<TestCase> cases;
    return cases;
}

int& CurrentFailures() {
    static int failures = 0;
    return failures;
}

void Fail(const char* file, int line, const std::string& message) {
    ++CurrentFailures();
    std::printf("[FAIL] %s:%d\n        %s\n", file, line, message.c_str());
}

} // namespace ghbt

int main() {
    auto& cases = ghbt::Registry();

    // 按注册顺序跑，不排序：同文件内顺序稳定，便于定位。
    int total_failures = 0;
    int failed_cases = 0;

    for (const auto& test : cases) {
        ghbt::CurrentFailures() = 0;
        test.fn();
        if (ghbt::CurrentFailures() > 0) {
            ++failed_cases;
            total_failures += ghbt::CurrentFailures();
            std::printf("[FAILED] %s.%s (%d assertion%s)\n", test.suite, test.name, ghbt::CurrentFailures(), ghbt::CurrentFailures() == 1 ? "" : "s");
        } else {
            std::printf("[PASS] %s.%s\n", test.suite, test.name);
        }
    }

    std::printf("\n%d case(s), %d failed case(s), %d failed assertion(s)\n", static_cast<int>(cases.size()), failed_cases, total_failures);
    if (total_failures > 0) {
        std::printf("RESULT: FAILED\n");
        return 1;
    }
    std::printf("RESULT: PASSED\n");
    return 0;
}
