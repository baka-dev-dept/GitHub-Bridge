#pragma once

#include "Types/RpcMessage.h"

#include <cstdint>
#include <string>

namespace ghb {

// GitHub API 错误分类。区分「令牌问题」「权限不足」「限流」「网络」四类，
// 因为它们的补救方式完全不同，把 HTTP 码直接抛给 Agent 会误导重试。
enum class GitHubErrorKind : int {
    None = 0,
    Transport = 1,      // WinHTTP 层失败：DNS、TLS、超时、连接中断
    Unauthorized = 2,   // 401：令牌缺失或失效
    Forbidden = 3,      // 403：权限或 scope 不足
    NotFound = 4,       // 404
    RateLimited = 5,    // 403/429 且 X-RateLimit-Remaining 为 0
    Validation = 6,     // 422：参数或状态校验失败
    Server = 7,         // 5xx
    Redirect = 8,       // 3xx：本项目禁止自动跟随重定向后仍需处理的情况
    Decode = 9,         // JSON 解析或 gzip 解压失败
};

struct RateLimitInfo {
    int limit = 0;
    int remaining = 0;
    std::string reset_at;
};

struct GitHubHeaders {
    // fine-grained token 不返回此头，此时 scopes 为空。
    std::string oauth_scopes;
    std::string token_expires_at;
    std::string request_id;
    RateLimitInfo rate_limit;
};

// GitHub Client 对上层唯一暴露的返回类型。
// 禁止在此结构中存放任何请求头原值，Authorization 一经发出即丢弃。
struct GitHubResponse {
    int status = 0;
    bool ok = false;
    GitHubErrorKind error_kind = GitHubErrorKind::None;
    std::string error_message;
    // 成功时是响应正文；失败时是 GitHub 的 message 字段，便于 Agent 理解。
    std::string body;
    GitHubHeaders headers;

    static GitHubResponse MakeTransportError(const std::string& message) {
        GitHubResponse r;
        r.error_kind = GitHubErrorKind::Transport;
        r.error_message = message;
        return r;
    }
};

} // namespace ghb
