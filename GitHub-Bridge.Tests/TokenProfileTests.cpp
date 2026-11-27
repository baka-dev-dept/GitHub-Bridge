#include "TestFramework.h"

#include "Security/TokenManager.h"

using namespace ghb;

// 这些用例只测纯逻辑（命名校验、结构约束），不碰 DPAPI，
// 所以不需要真实凭据，也不会读写磁盘。

TEST(TokenProfile, ValidNames) {
    CHECK_EQ(TokenManager::ValidateProfileName("default"), std::string(""));
    CHECK_EQ(TokenManager::ValidateProfileName("work"), std::string(""));
    CHECK_EQ(TokenManager::ValidateProfileName("personal-2"), std::string(""));
    CHECK_EQ(TokenManager::ValidateProfileName("A_b-9"), std::string(""));
}

TEST(TokenProfile, EmptyNameRejected) {
    CHECK(!TokenManager::ValidateProfileName("").empty());
}

TEST(TokenProfile, TooLongRejected) {
    CHECK_EQ(TokenManager::ValidateProfileName(std::string(32, 'a')), std::string(""));
    CHECK(!TokenManager::ValidateProfileName(std::string(33, 'a')).empty());
}

TEST(TokenProfile, IllegalCharactersRejected) {
    // 空格、路径分隔符、点号都不允许，避免与文件系统或 JSON 键语义混淆。
    CHECK(!TokenManager::ValidateProfileName("has space").empty());
    CHECK(!TokenManager::ValidateProfileName("has/slash").empty());
    CHECK(!TokenManager::ValidateProfileName("has\\slash").empty());
    CHECK(!TokenManager::ValidateProfileName("has.dot").empty());
    CHECK(!TokenManager::ValidateProfileName("has:colon").empty());
}

TEST(TokenProfile, CaseIsSignificant) {
    // Work 与 work 是两个不同 profile，命名校验都要通过。
    CHECK_EQ(TokenManager::ValidateProfileName("Work"), std::string(""));
    CHECK_EQ(TokenManager::ValidateProfileName("work"), std::string(""));
}

TEST(TokenProfile, DefaultProfileConstant) {
    CHECK_EQ(std::string(TokenManager::kDefaultProfile), std::string("default"));
}
