#include "GitHub/GitHubClient.h"

#include "GitHub/RateLimiter.h"
#include "GitHub/WinHttpClient.h"
#include "Utils/Logger.h"

#include <nlohmann/json.hpp>

#include <windows.h>

#include <chrono>

namespace ghb {

namespace {

int64_t NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

void SleepMs(DWORD ms) {
    Sleep(ms);
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

GitHubResponse GitHubClient::Request(const std::string& method, const std::string& path, const std::string& body, const std::string& authorization) {
    // 只有 GET 是幂等的，才能在限流后安全重发；写操作重发可能重复创建或重复提交。
    const bool idempotent = (method == "GET");

    // 发送前先看已知状态：如果正处于限流窗口且剩余等待在可接受范围内，就先等。
    // 这样可以避免明知会失败还去撞一次配额。
    if (idempotent) {
        const int64_t pending = RateLimiter::Get()->WaitBeforeMs(NowMs());
        if (pending > 0 && pending <= RateLimiter::kMaxAutoWaitMs) {
            Logger::Get()->Info("限流窗口内，等待 " + std::to_string(pending) + "ms 后重试");
            SleepMs(static_cast<DWORD>(pending));
        }
    }

    const HttpResponse http = SendOnce(method, path, body, authorization);
    if (http.TransportFailed()) {
        Logger::Get()->Warn(method + " " + path + " 传输失败: " + http.transport_error);
        return GitHubResponse::MakeTransportError(http.transport_error);
    }

    Logger::Get()->Debug(method + " " + path + " -> " + std::to_string(http.status));

    GitHubResponse response = Classify(http.status, http.body, http.headers);
    RateLimiter::Get()->Observe(response.headers, NowMs());

    // 限流且是幂等请求且能接受等待时，等一次再重发。只重试一次，
    // 不循环——再失败说明不是单纯限流，继续等只会拖住调用方。
    if (response.error_kind == GitHubErrorKind::RateLimited && idempotent) {
        const int64_t wait_ms = RateLimiter::Get()->WaitBeforeMs(NowMs());
        if (wait_ms > 0 && wait_ms <= RateLimiter::kMaxAutoWaitMs) {
            Logger::Get()->Info("触发限流，等待 " + std::to_string(wait_ms) + "ms 后重试一次");
            SleepMs(static_cast<DWORD>(wait_ms));

            const HttpResponse retry_http = SendOnce(method, path, body, authorization);
            if (!retry_http.TransportFailed()) {
                response = Classify(retry_http.status, retry_http.body, retry_http.headers);
                RateLimiter::Get()->Observe(response.headers, NowMs());
                Logger::Get()->Debug("重试 " + path + " -> " + std::to_string(retry_http.status));
            }
            // 传输失败的保留第一次的响应，让调用方看到原始的限流原因。
        }
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

HttpResponse GitHubClient::SendOnce(const std::string& method, const std::string& path, const std::string& body, const std::string& authorization) {
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
    if (!authorization.empty()) {
        request.headers.push_back({"Authorization", authorization});
    }

    return WinHttpClient::Get()->Send(request);
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
