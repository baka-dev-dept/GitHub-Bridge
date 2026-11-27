#include "TestFramework.h"

#include "Security/PermissionManager.h"

using namespace ghb;

// 白名单匹配与裁决都是纯逻辑，不需要网络或磁盘。

TEST(Whitelist, MatchExact) {
    CHECK(PermissionManager::MatchRepositoryPattern("octocat/hello-world", "octocat/hello-world"));
    CHECK(!PermissionManager::MatchRepositoryPattern("octocat/hello-world", "octocat/other"));
}

TEST(Whitelist, MatchIsCaseInsensitive) {
    // GitHub 的 owner/repo 本身不区分大小写，配置里写大写不该失效。
    CHECK(PermissionManager::MatchRepositoryPattern("OctoCat/Hello-World", "octocat/hello-world"));
    CHECK(PermissionManager::MatchRepositoryPattern("octocat/hello-world", "OCTOCAT/HELLO-WORLD"));
}

TEST(Whitelist, MatchStarInRepo) {
    CHECK(PermissionManager::MatchRepositoryPattern("baka-dev-dept/ghb-test-*", "baka-dev-dept/ghb-test-20261127"));
    CHECK(!PermissionManager::MatchRepositoryPattern("baka-dev-dept/ghb-test-*", "baka-dev-dept/production"));
}

TEST(Whitelist, MatchStarInOwner) {
    CHECK(PermissionManager::MatchRepositoryPattern("*/playground", "someone/playground"));
    CHECK(!PermissionManager::MatchRepositoryPattern("*/playground", "someone/other"));
}

TEST(Whitelist, MatchBareStar) {
    // 单个 '*' 是「全部允许」，等价于不限制。
    CHECK(PermissionManager::MatchRepositoryPattern("*", "any/thing"));
}

TEST(Whitelist, StarDoesNotCrossSlashRequirement) {
    // '*' 可以吞掉斜杠：org/* 匹配 org 下任意仓库。
    CHECK(PermissionManager::MatchRepositoryPattern("org/*", "org/a"));
    CHECK(PermissionManager::MatchRepositoryPattern("org/*", "org/a/b"));
    CHECK(!PermissionManager::MatchRepositoryPattern("org/*", "other/a"));
}

TEST(Whitelist, EmptyPatternMatchesNothing) {
    CHECK(!PermissionManager::MatchRepositoryPattern("", "octocat/hello-world"));
}

TEST(Whitelist, NoWhitelistMeansUnrestricted) {
    // 未配置白名单时行为与升级前一致。
    PermissionConfig config;
    config.write = true;
    PermissionManager::Get()->ApplyConfig(config);
    const PermissionOutcome outcome = PermissionManager::Get()->Check("github_delete_file", false, "write", "fp", "", R"({"owner":"any","repo":"thing","path":"a"})");
    CHECK(outcome.verdict == PermissionVerdict::Allow);
}

TEST(Whitelist, ListedRepositoryAllowed) {
    PermissionConfig config;
    config.write = true;
    config.allowed_repositories = {"baka-dev-dept/*"};
    PermissionManager::Get()->ApplyConfig(config);
    const PermissionOutcome outcome = PermissionManager::Get()->Check("github_delete_file", false, "write", "fp", "", R"({"owner":"baka-dev-dept","repo":"ghb-test","path":"a"})");
    CHECK(outcome.verdict == PermissionVerdict::Allow);
}

TEST(Whitelist, UnlistedRepositoryDenied) {
    PermissionConfig config;
    config.write = true;
    config.allowed_repositories = {"baka-dev-dept/*"};
    PermissionManager::Get()->ApplyConfig(config);
    const PermissionOutcome outcome = PermissionManager::Get()->Check("github_delete_file", false, "write", "fp", "", R"({"owner":"production-org","repo":"critical","path":"a"})");
    CHECK(outcome.verdict == PermissionVerdict::Denied);
    CHECK(outcome.reason.find("production-org/critical") != std::string::npos);
}

TEST(Whitelist, ToolsWithoutRepoUnaffected) {
    // search / get_user 这类没有明确单仓库目标的工具不受白名单约束。
    PermissionConfig config;
    config.read = true;
    config.allowed_repositories = {"only/this"};
    PermissionManager::Get()->ApplyConfig(config);
    const PermissionOutcome outcome = PermissionManager::Get()->Check("github_get_user", false, "read", "fp", "", R"({})");
    CHECK(outcome.verdict == PermissionVerdict::Allow);
}

TEST(Whitelist, WhitelistCheckedBeforeConfirmation) {
    // 白名单在确认票据之前：不该为注定被拒的操作下发票据。
    PermissionConfig config;
    config.delete_repository = true;
    config.allowed_repositories = {"safe/*"};
    PermissionManager::Get()->ApplyConfig(config);
    const PermissionOutcome outcome = PermissionManager::Get()->Check("github_delete_repository", true, "delete_repository", "fp", "", R"({"owner":"danger","repo":"repo"})");
    CHECK(outcome.verdict == PermissionVerdict::Denied);
    CHECK(outcome.confirm_token.empty());
}

TEST(Whitelist, SwitchStillCheckedFirst) {
    // 开关关闭时，即使仓库在白名单里也拒绝。
    PermissionConfig config;
    config.write = false;
    config.allowed_repositories = {"*"};
    PermissionManager::Get()->ApplyConfig(config);
    const PermissionOutcome outcome = PermissionManager::Get()->Check("github_delete_file", false, "write", "fp", "", R"({"owner":"any","repo":"thing","path":"a"})");
    CHECK(outcome.verdict == PermissionVerdict::Denied);
    CHECK(outcome.reason.find("write") != std::string::npos);
}
