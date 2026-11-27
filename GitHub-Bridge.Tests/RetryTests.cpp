#include "TestFramework.h"

#include "GitHub/GitHubClient.h"

using namespace ghb;

TEST(Retry, TransportIsRetryable) {
    CHECK(GitHubClient::IsRetryable(GitHubErrorKind::Transport));
}

TEST(Retry, ServerIsRetryable) {
    CHECK(GitHubClient::IsRetryable(GitHubErrorKind::Server));
}

TEST(Retry, ClientErrorsAreNotRetryable) {
    // 这些错误重发结果不变，重试只是浪费一次往返。
    CHECK(!GitHubClient::IsRetryable(GitHubErrorKind::None));
    CHECK(!GitHubClient::IsRetryable(GitHubErrorKind::Unauthorized));
    CHECK(!GitHubClient::IsRetryable(GitHubErrorKind::Forbidden));
    CHECK(!GitHubClient::IsRetryable(GitHubErrorKind::NotFound));
    CHECK(!GitHubClient::IsRetryable(GitHubErrorKind::Validation));
    CHECK(!GitHubClient::IsRetryable(GitHubErrorKind::Redirect));
    CHECK(!GitHubClient::IsRetryable(GitHubErrorKind::Decode));
}

TEST(Retry, RateLimitedHasSeparatePath) {
    // 限流由 reset 时间决定等待，不走固定退避序列。
    CHECK(!GitHubClient::IsRetryable(GitHubErrorKind::RateLimited));
}

TEST(Retry, Constants) {
    CHECK_EQ(GitHubClient::kMaxTransientRetries, 2);
    CHECK_EQ(GitHubClient::kBackoffMs[0], 500);
    CHECK_EQ(GitHubClient::kBackoffMs[1], 1500);
}

TEST(Retry, ClassifySuccess) {
    const GitHubResponse r = GitHubClient::Classify(200, R"({"ok":true})", {});
    CHECK_EQ(r.status, 200);
    CHECK(r.ok);
    CHECK(r.error_kind == GitHubErrorKind::None);
}

TEST(Retry, ClassifyUnauthorized) {
    const GitHubResponse r = GitHubClient::Classify(401, R"({"message":"Bad credentials"})", {});
    CHECK(!r.ok);
    CHECK(r.error_kind == GitHubErrorKind::Unauthorized);
}

TEST(Retry, ClassifyNotFound) {
    const GitHubResponse r = GitHubClient::Classify(404, R"({"message":"Not Found"})", {});
    CHECK(!r.ok);
    CHECK(r.error_kind == GitHubErrorKind::NotFound);
}

TEST(Retry, ClassifyServerError) {
    const GitHubResponse r = GitHubClient::Classify(503, R"({"message":"unavailable"})", {});
    CHECK(!r.ok);
    CHECK(r.error_kind == GitHubErrorKind::Server);
}

TEST(Retry, ClassifyRateLimitedByHeaders) {
    // 403 + remaining:0 才是限流；单纯 403 是权限问题。
    std::map<std::string, std::string> headers;
    headers["x-ratelimit-remaining"] = "0";
    headers["x-ratelimit-limit"] = "5000";
    headers["x-ratelimit-reset"] = "2000000000";
    const GitHubResponse r = GitHubClient::Classify(403, R"({"message":"API rate limit exceeded"})", headers);
    CHECK(!r.ok);
    CHECK(r.error_kind == GitHubErrorKind::RateLimited);
    CHECK_EQ(r.headers.rate_limit.limit, 5000);
    CHECK_EQ(r.headers.rate_limit.remaining, 0);
}

TEST(Retry, ClassifyForbiddenWithoutExhaustion) {
    std::map<std::string, std::string> headers;
    headers["x-ratelimit-remaining"] = "4999";
    const GitHubResponse r = GitHubClient::Classify(403, R"({"message":"Resource not accessible"})", headers);
    CHECK(!r.ok);
    CHECK(r.error_kind == GitHubErrorKind::Forbidden);
}
