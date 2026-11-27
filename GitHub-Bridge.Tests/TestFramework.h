#pragma once

// 极简断言框架。项目至今零第三方运行时依赖，不为了测试引入 gtest。
// 每个测试文件用 TEST(...) 注册用例，TestMain.cpp 负责跑完并汇总。

#include <string>
#include <vector>

namespace ghbt {

struct TestCase {
    const char* suite;
    const char* name;
    void (*fn)();
};

// 用例在静态初始化期注册，避免手写清单漏加。
std::vector<TestCase>& Registry();

struct Registrar {
    Registrar(const char* suite, const char* name, void (*fn)()) {
        Registry().push_back(TestCase{suite, name, fn});
    }
};

// 当前用例的失败计数，由运行器在每次调用前重置。
int& CurrentFailures();

void Fail(const char* file, int line, const std::string& message);

template <typename T>
std::string ToText(const T& value) {
    return std::to_string(value);
}

inline std::string ToText(const std::string& value) {
    return value;
}

inline std::string ToText(const char* value) {
    return value ? std::string(value) : std::string("(null)");
}

inline std::string ToText(bool value) {
    return value ? "true" : "false";
}

} // namespace ghbt

#define TEST(suite_name, case_name) \
    static void suite_name##_##case_name##_body(); \
    static ::ghbt::Registrar suite_name##_##case_name##_registrar(#suite_name, #case_name, &suite_name##_##case_name##_body); \
    static void suite_name##_##case_name##_body()

#define CHECK(condition) \
    do { if (!(condition)) { ::ghbt::Fail(__FILE__, __LINE__, std::string("CHECK failed: ") + #condition); } } while (0)

#define CHECK_EQ(actual, expected) \
    do { \
        const auto ghbt_a = (actual); \
        const auto ghbt_e = (expected); \
        if (!(ghbt_a == ghbt_e)) { \
            ::ghbt::Fail(__FILE__, __LINE__, std::string("CHECK_EQ failed: ") + #actual " == " #expected " | actual=" + ::ghbt::ToText(ghbt_a) + " expected=" + ::ghbt::ToText(ghbt_e)); \
        } \
    } while (0)
