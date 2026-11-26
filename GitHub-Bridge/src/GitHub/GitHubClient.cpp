#include "GitHub/GitHubClient.h"

#include "GitHub/WinHttpClient.h"
#include "Utils/Logger.h"

#include <nlohmann/json.hpp>

namespace ghb {

namespace {

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

GitHubResponse GitHubClient::Classify(int status, const std::string& body,
                                      const std::map<std::string, std::string>& headers) {
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

GitHubResponse GitHubClient::Perform(const std::string& path, const std::string& authorization) {
    HttpRequest request;
    request.method = "GET";
    request.path = path;
    request.headers.push_back({"Accept", "application/vnd.github+json"});
    request.headers.push_back({"X-GitHub-Api-Version", kApiVersion});
    // GitHub 拒绝没有 User-Agent 的请求。
    request.headers.push_back({"User-Agent", kUserAgent});
    if (!authorization.empty()) {
        request.headers.push_back({"Authorization", authorization});
    }

    const HttpResponse http = WinHttpClient::Get()->Send(request);
    if (http.TransportFailed()) {
        Logger::Get()->Warn("GitHub 请求失败（传输层）: " + path + " -> " + http.transport_error);
        return GitHubResponse::MakeTransportError(http.transport_error);
    }

    GitHubResponse response = Classify(http.status, http.body, http.headers);

    // 成功状态但正文不是 JSON，说明链路或服务端返回了非预期内容。
    if (response.ok && !http.body.empty()) {
        const nlohmann::json parsed = nlohmann::json::parse(http.body, nullptr, false);
        if (parsed.is_discarded()) {
            response.ok = false;
            response.error_kind = GitHubErrorKind::Decode;
            response.error_message = "响应正文不是合法 JSON";
        }
    }
    return response;
}

GitHubResponse GitHubClient::AnonymousGet(const std::string& path) {
    return Perform(path, std::string());
}

GitHubResponse GitHubClient::AuthorizedGet(const std::string& path, const std::string& authorization) {
    return Perform(path, authorization);
}

GitHubResponse GitHubClient::GetUser(const std::string& authorization) {
    return AuthorizedGet("/user", authorization);
}

GitHubResponse GitHubClient::GetRateLimit() {
    return AnonymousGet("/rate_limit");
}

} // namespace ghb
