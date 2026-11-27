#pragma once

#include "Types/GitHubResponse.h"

#include <cstdint>

namespace ghb {

// 最近一次观察到的限流状态。GitHub 在每个响应的 X-RateLimit-* 头里给出，
// 因此成功响应也能用来更新，不必专门去查 /rate_limit。
struct RateState {
    int limit = 0;
    int remaining = 0;
    // GitHub 给的是 Unix 秒时间戳，不是剩余秒数。
    int64_t reset_at_unix = 0;
    // 本地观察到该状态的时间，用于判定这组数字是否已经过时。
    int64_t observed_at_ms = 0;
    bool known = false;
};

// 限流状态跟踪与等待决策。
//
// 只做「记录 + 判断该不该等」，不负责发请求，也不负责重试策略本身——
// 那些在 GitHubClient 里，因为只有它知道请求方法与幂等性。
class RateLimiter {
public:
    static RateLimiter* Get() {
        static RateLimiter i;
        return &i;
    }

    // 用一次响应更新状态。只有同时拿到 remaining 与 reset 才认为状态可信，
    // 否则忽略——某些响应（如 404）不带额度头，不能当成「额度为 0」。
    void Observe(const GitHubHeaders& headers, int64_t now_ms);

    // 距离额度恢复还有多少毫秒。
    //   0    —— 当前不受限，可以立即发请求
    //   > 0  —— 处于限流窗口，需等待这么久
    //   -1   —— 已知额度耗尽但重置时间不可用或异常，无法估算
    int64_t WaitBeforeMs(int64_t now_ms) const;

    const RateState& State() const { return state_; }
    void Reset();

    // 自动等待上限。超过这个值就不再阻塞调用方，改为立即返回并告知剩余时间。
    static constexpr int64_t kMaxAutoWaitMs = 10 * 1000;

    // 重置时间距今超过这个值就认为头信息异常（GitHub 窗口最长 1 小时）。
    static constexpr int64_t kMaxPlausibleWindowSec = 60 * 60;

private:
    RateLimiter() = default;
    ~RateLimiter() = default;

    RateState state_;
};

} // namespace ghb
