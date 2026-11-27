#include "GitHub/GitHubClient.h"

#include "Cache/CacheManager.h"
#include "Platform/HttpTransport.h"
#include "Platform/Platform.h"
#include "GitHub/RateLimiter.h"
#include "Utils/Logger.h"

#include <nlohmann/json.hpp>


#include <chrono>
#include <cstdio>

namespace ghb {

namespace {

// 把授权头折成一个定长散列，只用来做缓存键的账号隔离。
// 不是安全边界：目标是别把令牌原文留在内存的键里，也避免换账号串用缓存。
std::string HmacLikeDigest(const std::string& text) {
    uint64_t h = 1469598103934665603ULL;
    for (const char c : text) {
        h ^= static_cast<uint64_t>(static_cast<unsigned char>(c));
        h *= 1099511628211ULL;
    }
    char buf[17] = {};
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(h));
    return std::string(buf);
}

int64_t NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

void SleepMs(int ms) {
    platform::Get()->SleepMs(ms);
}

std::string HeaderValue(const std::map<std::string, std::string>& headers, const char* name) {
    const auto it = headers.find(name);
    return it == headers.end() ? std::string() : it->second;
}

int HeaderInt(const std::map<std::string, std::string>& headers, const char* name) {
    const std::string text = HeaderValue(headers, name);
    if (text.empty()) {
        return 0;
    }
    try {
        return std::stoi(text);
    } catch (...) {
        return 0;
    }
}

// GitHub 的错误正文是 {"message": "..."}，把它提出来给 Agent 看，
// 比直接丢一段 JSON 更有用。
std::string ExtractMessage(const std::string& body) {
    const nlohmann::json parsed = nlohmann::json::parse(body, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        return {};
    }
    const auto it = parsed.find("message");
    return (it != parsed.end() && it->is_string()) ? it->get<std::string>() : std::string();
}

GitHubHeaders ExtractHeaders(const std::map<std::string, std::string>& headers) {
    GitHubHeaders out;
    out.oauth_scopes = HeaderValue(headers, "x-oauth-scopes");
    out.token_expires_at = HeaderValue(headers, "x-github-authentication-token-expiration");
    out.request_id = HeaderValue(headers, "x-github-request-id");
    out.rate_limit.limit = HeaderInt(headers, "x-ratelimit-limit");
    out.rate_limit.remaining = HeaderInt(headers, "x-ratelimit-remaining");
    out.rate_limit.reset_at = HeaderValue(headers, "x-ratelimit-reset");
    out.etag = HeaderValue(headers, "etag");
    return out;
}

} // namespace

GitHubResponse GitHubClient::Classify(int status, const std::string& body, const std::map<std::string, std::string>& headers) {
    GitHubResponse response;
    response.status = status;
    response.body = body;
    response.headers = ExtractHeaders(headers);
    response.ok = status >= 200 && status < 300;

    if (response.ok) {
        return response;
    }

    response.error_message = ExtractMessage(body);
    if (response.error_message.empty()) {
        response.error_message = "HTTP " + std::to_string(status);
    }

    if (status >= 300 && status < 400) {
        // 客户端禁止自动跟随重定向，能走到这里说明服务端要求跳转。
        // 不跟随是刻意的：跟随便可能把 Authorization 带到别的主机。
        response.error_kind = GitHubErrorKind::Redirect;
    } else if (status == 401) {
        response.error_kind = GitHubErrorKind::Unauthorized;
    } else if (status == 403 || status == 429) {
        // 限流和权限不足都可能是 403，靠 X-RateLimit-Remaining 区分：
        // 该头存在且为 0 才是额度耗尽，否则是权限或 scope 不足。
        const std::string remaining_header = HeaderValue(headers, "x-ratelimit-remaining");
        const bool exhausted = !remaining_header.empty() && remaining_header == "0";
        response.error_kind = (status == 429 || exhausted) ? GitHubErrorKind::RateLimited : GitHubErrorKind::Forbidden;
    } else if (status == 404) {
        response.error_kind = GitHubErrorKind::NotFound;
    } else if (status == 422) {
        response.error_kind = GitHubErrorKind::Validation;
    } else if (status >= 500) {
        response.error_kind = GitHubErrorKind::Server;
    } else {
        response.error_kind = GitHubErrorKind::Forbidden;
    }
    return response;
}

bool GitHubClient::IsRetryable(GitHubErrorKind kind) {
    // 只有「重发一次可能就好了」的错误才重试。
    // Unauthorized / Forbidden / NotFound / Validation / Redirect / Decode 重发结果不变。
    return kind == GitHubErrorKind::Transport || kind == GitHubErrorKind::Server;
}

GitHubResponse GitHubClient::Request(const std::string& method, const std::string& path, const std::string& body, const std::string& authorization) {
    // 只有 GET 是幂等的，才能在失败后安全重发；写操作重发可能重复创建或重复提交。
    // PUT/DELETE 虽然本身幂等，但网络失败时无法确定服务端是否已执行，同样不重试。
    const bool idempotent = (method == "GET");

    // 缓存只对 GET 生效。授权头用哈希入键，换账号不串用，也不在内存里留令牌原文。
    const std::string cache_scope = authorization.empty() ? "anon" : HmacLikeDigest(authorization);
    const std::string cache_key = CacheManager::MakeKey(method, path, cache_scope);
    CacheEntry cached;
    const bool cache_hit = idempotent && CacheManager::Get()->Lookup(cache_key, &cached);

    // 发送前先看已知状态：如果正处于限流窗口且剩余等待在可接受范围内，就先等。
    // 这样可以避免明知会失败还去撞一次配额。
    if (idempotent) {
        const int64_t pending = RateLimiter::Get()->WaitBeforeMs(NowMs());
        if (pending > 0 && pending <= RateLimiter::kMaxAutoWaitMs) {
            Logger::Get()->Info("限流窗口内，等待 " + std::to_string(pending) + "ms 后重试");
            SleepMs(pending);
        }
    }

    GitHubResponse response;
    int transient_attempt = 0;
    bool rate_limit_retried = false;
    bool cache_validated = false;

    while (true) {
        const std::string if_none_match = cache_hit ? cached.etag : std::string();
        const HttpResponse http = SendOnce(method, path, body, authorization, if_none_match);
        if (http.TransportFailed()) {
            response = GitHubResponse::MakeTransportError(http.transport_error);
        } else {
            Logger::Get()->Debug(method + " " + path + " -> " + std::to_string(http.status));
            response = Classify(http.status, http.body, http.headers);
            // 304 也带 X-RateLimit-*，而且实测同样计配额，必须照常记录。
            RateLimiter::Get()->Observe(response.headers, NowMs());
        }

        if (!idempotent) {
            break;
        }

        // 304 说明内容没变，直接用缓存副本。这是唯一允许回放缓存的分支，
        // 其他状态一律以服务端返回为准，避免返回过期数据。
        if (response.status == 304 && cache_hit) {
            const GitHubResponse restored = cached.response;
            if (if_none_match == cached.etag) {
                cache_validated = true;
                response = restored;
                response.from_cache = true;
                Logger::Get()->Debug("缓存命中: " + path);
                break;
            }
        }

        // 限流单独处理：等待时长由服务端 reset 决定，与退避序列无关，且只做一次。
        if (response.error_kind == GitHubErrorKind::RateLimited && !rate_limit_retried) {
            const int64_t wait_ms = RateLimiter::Get()->WaitBeforeMs(NowMs());
            if (wait_ms > 0 && wait_ms <= RateLimiter::kMaxAutoWaitMs) {
                rate_limit_retried = true;
                Logger::Get()->Info("触发限流，等待 " + std::to_string(wait_ms) + "ms 后重试一次");
                SleepMs(wait_ms);
                continue;
            }
            break;
        }

        // 暂时性故障按固定退避重试，次数有上限。
        if (!IsRetryable(response.error_kind)) {
            break;
        }
        if (transient_attempt >= kMaxTransientRetries) {
            break;
        }

        const int backoff = kBackoffMs[transient_attempt];
        ++transient_attempt;
        Logger::Get()->Warn(method + " " + path + " 失败（" + (response.error_kind == GitHubErrorKind::Server ? "Server" : "Transport") +
                            "），" + std::to_string(backoff) + "ms 后重试（" + std::to_string(transient_attempt) + "/" + std::to_string(kMaxTransientRetries) + "）");
        SleepMs(backoff);
    }

    // 拿到新的 200 就刷新缓存；失败响应则清掉旧条目，避免下次拿到已失效内容。
    if (idempotent) {
        if (response.ok && !response.from_cache) {
            CacheManager::Get()->Store(cache_key, response.headers.etag, response);
        } else if (!cache_validated && !response.ok) {
            CacheManager::Get()->Remove(cache_key);
        }
    }

    if (idempotent && (transient_attempt > 0 || rate_limit_retried) && response.ok) {
        Logger::Get()->Info("重试后成功: " + method + " " + path);
    }

    // 成功状态但正文不是 JSON，说明链路或服务端返回了非预期内容。
    // 204 No Content 是正常结果（删除类接口），没有正文不算解码失败。
    if (response.ok && !response.body.empty()) {
        const nlohmann::json parsed = nlohmann::json::parse(response.body, nullptr, false);
        if (parsed.is_discarded()) {
            response.ok = false;
            response.error_kind = GitHubErrorKind::Decode;
            response.error_message = "响应正文不是合法 JSON";
        }
    }
    return response;
}

HttpResponse GitHubClient::SendOnce(const std::string& method, const std::string& path, const std::string& body, const std::string& authorization, const std::string& if_none_match) {
    HttpRequest request;
    request.method = method;
    request.path = path;
    request.body = body;
    request.headers.push_back({"Accept", "application/vnd.github+json"});
    request.headers.push_back({"X-GitHub-Api-Version", kApiVersion});
    // GitHub 拒绝没有 User-Agent 的请求。
    request.headers.push_back({"User-Agent", kUserAgent});
    // 带正文的写操作必须声明 JSON，否则 GitHub 会把 body 当成表单而报 400。
    if (!body.empty()) {
        request.headers.push_back({"Content-Type", "application/json"});
    }
    // 条件请求：内容未变时 GitHub 回 304 且无正文。注意 304 照样计配额。
    if (!if_none_match.empty()) {
        request.headers.push_back({"If-None-Match", if_none_match});
    }
    if (!authorization.empty()) {
        request.headers.push_back({"Authorization", authorization});
    }

    static IHttpTransport* const transport = IHttpTransport::Create();
    return transport->Send(request);
}

GitHubResponse GitHubClient::AnonymousGet(const std::string& path) {
    return Request("GET", path, std::string(), std::string());
}

GitHubResponse GitHubClient::AuthorizedGet(const std::string& path, const std::string& authorization) {
    return Request("GET", path, std::string(), authorization);
}

GitHubResponse GitHubClient::AuthorizedPost(const std::string& path, const std::string& body, const std::string& authorization) {
    return Request("POST", path, body, authorization);
}

GitHubResponse GitHubClient::AuthorizedPut(const std::string& path, const std::string& body, const std::string& authorization) {
    return Request("PUT", path, body, authorization);
}

GitHubResponse GitHubClient::AuthorizedPatch(const std::string& path, const std::string& body, const std::string& authorization) {
    return Request("PATCH", path, body, authorization);
}

GitHubResponse GitHubClient::AuthorizedDelete(const std::string& path, const std::string& authorization) {
    // 删除接口没有请求体，成功时返回 204，Classify 会正确置 ok。
    return Request("DELETE", path, std::string(), authorization);
}

GitHubResponse GitHubClient::GetUser(const std::string& authorization) {
    return AuthorizedGet("/user", authorization);
}

GitHubResponse GitHubClient::GetRateLimit() {
    return AnonymousGet("/rate_limit");
}

} // namespace ghb
