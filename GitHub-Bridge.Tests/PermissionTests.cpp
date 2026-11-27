#include "TestFramework.h"

#include "Security/PermissionManager.h"

using namespace ghb;

TEST(Permission, DefaultsAreSafe) {
    // 这三个默认关闭：删仓库、删分支、强推。
    PermissionConfig config;
    CHECK(config.read);
    CHECK(config.write);
    CHECK(config.create);
    CHECK(!config.delete_repository);
    CHECK(!config.delete_branch);
    CHECK(!config.force_push);
}

TEST(Permission, FingerprintIsStable) {
    const std::string a = PermissionManager::Fingerprint("github_delete_repository", R"({"owner":"a","repo":"b"})");
    const std::string b = PermissionManager::Fingerprint("github_delete_repository", R"({"owner":"a","repo":"b"})");
    CHECK_EQ(a, b);
    CHECK(!a.empty());
}

TEST(Permission, FingerprintChangesWithArguments) {
    // 换目标就换票据，这是「改目标即失效」的基础。
    const std::string a = PermissionManager::Fingerprint("github_delete_repository", R"({"owner":"a","repo":"b"})");
    const std::string b = PermissionManager::Fingerprint("github_delete_repository", R"({"owner":"a","repo":"c"})");
    CHECK(a != b);
}

TEST(Permission, FingerprintChangesWithToolName) {
    const std::string a = PermissionManager::Fingerprint("github_delete_repository", R"({"owner":"a","repo":"b"})");
    const std::string b = PermissionManager::Fingerprint("github_delete_branch", R"({"owner":"a","repo":"b"})");
    CHECK(a != b);
}

TEST(Permission, TicketTtlIsSixtySeconds) {
    CHECK_EQ(PermissionManager::kTicketTtlMs, 60 * 1000);
}

TEST(Permission, UnknownPermissionKeyDenied) {
    // 新增工具忘记登记开关时，不能被静默放行。
    PermissionConfig config;
    config.read = true;
    config.write = true;
    PermissionManager::Get()->ApplyConfig(config);
    const PermissionOutcome outcome = PermissionManager::Get()->Check("some_new_tool", false, "not_a_real_key", "fp", "");
    CHECK(outcome.verdict == PermissionVerdict::Denied);
}

TEST(Permission, DisabledReadDeniesReadTool) {
    PermissionConfig config;
    config.read = false;
    PermissionManager::Get()->ApplyConfig(config);
    const PermissionOutcome outcome = PermissionManager::Get()->Check("github_get_user", false, "read", "fp", "");
    CHECK(outcome.verdict == PermissionVerdict::Denied);
}

TEST(Permission, EnabledReadAllows) {
    PermissionConfig config;
    config.read = true;
    PermissionManager::Get()->ApplyConfig(config);
    const PermissionOutcome outcome = PermissionManager::Get()->Check("github_get_user", false, "read", "fp", "");
    CHECK(outcome.verdict == PermissionVerdict::Allow);
}

TEST(Permission, DestructiveToolRequiresConfirmation) {
    // 开关打开只是必要条件，不可逆操作还要票据。
    PermissionConfig config;
    config.delete_repository = true;
    PermissionManager::Get()->ApplyConfig(config);
    const PermissionOutcome outcome = PermissionManager::Get()->Check("github_delete_repository", true, "delete_repository", "fp123", "");
    CHECK(outcome.verdict == PermissionVerdict::NeedConfirm);
    CHECK(!outcome.confirm_token.empty());
}

TEST(Permission, WrongTicketRejected) {
    PermissionConfig config;
    config.delete_repository = true;
    PermissionManager::Get()->ApplyConfig(config);
    const PermissionOutcome outcome = PermissionManager::Get()->Check("github_delete_repository", true, "delete_repository", "fp456", "deadbeef");
    CHECK(outcome.verdict == PermissionVerdict::BadConfirmToken);
}
