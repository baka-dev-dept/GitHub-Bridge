#include "GitHub/RateLimiter.h"

#include "Utils/Logger.h"

#include <string>

namespace ghb {

namespace {

// reset 头是 Unix 秒的十进制字符串。解析失败返回 0，由调用方当作「不可用」处理。
int64_t ParseResetSeconds(const std::string& text) {
    if (text.empty()) {
        return 0;
    }
    try {
        return std::stoll(text);
    } catch (...) {
        return 0;
    }
}

} // namespace

void RateLimiter::Observe(const GitHubHeaders& headers, int64_t now_ms) {
    // 缺少 reset 头说明这组数据不足以判断窗口，直接丢弃，
    // 保留上一次的已知状态比写入半份数据更安全。
    if (headers.rate_limit.reset_at.empty()) {
        return;
    }
    const int64_t reset_at = ParseResetSeconds(headers.rate_limit.reset_at);
    if (reset_at <= 0) {
        return;
    }

    state_.limit = headers.rate_limit.limit;
    state_.remaining = headers.rate_limit.remaining;
    state_.reset_at_unix = reset_at;
    state_.observed_at_ms = now_ms;
    state_.known = true;
}

int64_t RateLimiter::WaitBeforeMs(int64_t now_ms) const {
    if (!state_.known) {
        return 0;
    }
    // 还有额度就不需要等待，哪怕 reset 还没到。
    if (state_.remaining > 0) {
        return 0;
    }

    const int64_t now_sec = now_ms / 1000;
    const int64_t wait_sec = state_.reset_at_unix - now_sec;

    // 已过重置时间：GitHub 的时间戳有秒级粒度，给一点余量后视为恢复。
    if (wait_sec <= 0) {
        return 0;
    }
    // 距离重置超过一小时，说明头信息不可信（或时间戳被篡改），不做估算。
    if (wait_sec > kMaxPlausibleWindowSec) {
        Logger::Get()->Warn("限流重置时间异常，无法估算等待时长");
        return -1;
    }
    return wait_sec * 1000;
}

void RateLimiter::Reset() {
    state_ = RateState{};
}

} // namespace ghb
