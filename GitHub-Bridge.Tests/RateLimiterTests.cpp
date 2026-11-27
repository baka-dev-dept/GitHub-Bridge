#include "TestFramework.h"

#include "GitHub/RateLimiter.h"

using namespace ghb;

namespace {

constexpr int64_t kNowMs = 1900000000LL * 1000;
constexpr int64_t kNowSec = 1900000000LL;

GitHubHeaders MakeHeaders(int limit, int remaining, const std::string& reset) {
    GitHubHeaders h;
    h.rate_limit.limit = limit;
    h.rate_limit.remaining = remaining;
    h.rate_limit.reset_at = reset;
    return h;
}

} // namespace

TEST(RateLimiter, UnknownStateDoesNotWait) {
    RateLimiter::Get()->Reset();
    CHECK_EQ(RateLimiter::Get()->WaitBeforeMs(kNowMs), 0);
}

TEST(RateLimiter, RemainingAboveZeroDoesNotWait) {
    RateLimiter::Get()->Reset();
    RateLimiter::Get()->Observe(MakeHeaders(60, 5, std::to_string(kNowSec + 3600)), kNowMs);
    CHECK_EQ(RateLimiter::Get()->WaitBeforeMs(kNowMs), 0);
}

TEST(RateLimiter, ExhaustedWithNearResetReturnsWait) {
    RateLimiter::Get()->Reset();
    RateLimiter::Get()->Observe(MakeHeaders(60, 0, std::to_string(kNowSec + 30)), kNowMs);
    CHECK_EQ(RateLimiter::Get()->WaitBeforeMs(kNowMs), 30000);
}

TEST(RateLimiter, PastResetDoesNotWait) {
    RateLimiter::Get()->Reset();
    RateLimiter::Get()->Observe(MakeHeaders(60, 0, std::to_string(kNowSec - 10)), kNowMs);
    CHECK_EQ(RateLimiter::Get()->WaitBeforeMs(kNowMs), 0);
}

TEST(RateLimiter, MissingResetIgnored) {
    RateLimiter::Get()->Reset();
    RateLimiter::Get()->Observe(MakeHeaders(60, 0, ""), kNowMs);
    CHECK_EQ(RateLimiter::Get()->WaitBeforeMs(kNowMs), 0);
}

TEST(RateLimiter, GarbageResetIgnored) {
    RateLimiter::Get()->Reset();
    RateLimiter::Get()->Observe(MakeHeaders(60, 0, "not-a-number"), kNowMs);
    CHECK_EQ(RateLimiter::Get()->WaitBeforeMs(kNowMs), 0);
}

TEST(RateLimiter, ImplausibleWindowReturnsNegative) {
    // 窗口超过 1 小时说明时钟或头部异常，不能拿它算等待时长。
    RateLimiter::Get()->Reset();
    RateLimiter::Get()->Observe(MakeHeaders(60, 0, std::to_string(kNowSec + 7200)), kNowMs);
    CHECK_EQ(RateLimiter::Get()->WaitBeforeMs(kNowMs), -1);
}

TEST(RateLimiter, RecoveryClearsWait) {
    RateLimiter::Get()->Reset();
    RateLimiter::Get()->Observe(MakeHeaders(60, 0, std::to_string(kNowSec + 30)), kNowMs);
    CHECK_EQ(RateLimiter::Get()->WaitBeforeMs(kNowMs), 30000);
    RateLimiter::Get()->Observe(MakeHeaders(60, 10, std::to_string(kNowSec + 30)), kNowMs);
    CHECK_EQ(RateLimiter::Get()->WaitBeforeMs(kNowMs), 0);
}

TEST(RateLimiter, AutoWaitCapIsTenSeconds) {
    CHECK_EQ(RateLimiter::kMaxAutoWaitMs, 10 * 1000);
}

TEST(RateLimiter, StateExposesObservedValues) {
    RateLimiter::Get()->Reset();
    RateLimiter::Get()->Observe(MakeHeaders(5000, 4999, std::to_string(kNowSec + 60)), kNowMs);
    CHECK(RateLimiter::Get()->State().known);
    CHECK_EQ(static_cast<int>(RateLimiter::Get()->State().limit), 5000);
    CHECK_EQ(static_cast<int>(RateLimiter::Get()->State().remaining), 4999);
}
