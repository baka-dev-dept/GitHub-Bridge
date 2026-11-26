#include "Core/GitHubTools.h"

#include "Core/ToolRegistry.h"
#include "GitHub/GitHubClient.h"
#include "Security/TokenManager.h"
#include "Types/GitHubResponse.h"
#include "Types/McpTool.h"

#include <nlohmann/json.hpp>

namespace ghb {

namespace {

std::string ErrorKindName(GitHubErrorKind kind) {
    switch (kind) {
        case GitHubErrorKind::None: return "none";
        case GitHubErrorKind::Transport: return "transport";
        case GitHubErrorKind::Unauthorized: return "unauthorized";
        case GitHubErrorKind::Forbidden: return "forbidden";
        case GitHubErrorKind::NotFound: return "not_found";
        case GitHubErrorKind::RateLimited: return "rate_limited";
        case GitHubErrorKind::Validation: return "validation";
        case GitHubErrorKind::Server: return "server";
        case GitHubErrorKind::Redirect: return "redirect";
        case GitHubErrorKind::Decode: return "decode";
    }
    return "unknown";
}

// 把失败翻译成对 Agent 有用的下一步，而不是把 HTTP 码原样丢回去。
std::string FailureHint(GitHubErrorKind kind) {
    switch (kind) {
        case GitHubErrorKind::Unauthorized:
            return "令牌无效或已过期，请用 github_token_import 重新导入";
        case GitHubErrorKind::Forbidden:
            return "令牌权限或 scope 不足";
        case GitHubErrorKind::RateLimited:
            return "已触发速率限制，请等 X-RateLimit-Reset 时间之后再试";
        case GitHubErrorKind::Transport:
            return "网络不可达或 TLS 失败，这不是令牌问题";
        case GitHubErrorKind::Decode:
            return "响应无法解析，可能是代理或网络中间层改写了内容";
        case GitHubErrorKind::Redirect:
            return "服务端要求重定向，出于安全考虑不会自动跟随";
        default:
            return {};
    }
}

nlohmann::json FailureJson(const GitHubResponse& response) {
    return {
        {"ok", false},
        {"error_kind", ErrorKindName(response.error_kind)},
        {"status", response.status},
        {"message", response.error_message},
        {"hint", FailureHint(response.error_kind)},
    };
}

std::string HandleGetUser(const std::string&) {
    TokenHandle handle = TokenManager::Get()->AcquireHandle();
    if (!handle.configured) {
        // 没有令牌就不发请求：/user 匿名必然 401，白耗一次调用没有意义。
        return nlohmann::json{
            {"ok", false},
            {"error_kind", "not_configured"},
            {"message", "尚未配置令牌，请用 github_token_import 导入"},
        }.dump();
    }

    const GitHubResponse response = GitHubClient::Get()->GetUser(handle.authorization_header);
    if (!response.ok) {
        return FailureJson(response).dump();
    }

    const nlohmann::json body = nlohmann::json::parse(response.body, nullptr, false);
    if (body.is_discarded() || !body.is_object()) {
        return nlohmann::json{{"ok", false}, {"error_kind", "decode"}, {"message", "响应正文不是对象"}}.dump();
    }

    return nlohmann::json{
        {"ok", true},
        {"login", body.value("login", "")},
        {"id", body.value("id", static_cast<long long>(0))},
        {"name", body.value("name", "")},
        {"type", body.value("type", "")},
        {"scopes", response.headers.oauth_scopes},
        {"request_id", response.headers.request_id},
    }.dump();
}

std::string HandleRateLimit(const std::string&) {
    // 匿名即可查询；有令牌时额度更高，因此有就带上。
    TokenHandle handle = TokenManager::Get()->AcquireHandle();
    const GitHubResponse response = handle.configured
                                        ? GitHubClient::Get()->AuthorizedGet("/rate_limit", handle.authorization_header)
                                        : GitHubClient::Get()->GetRateLimit();
    if (!response.ok) {
        return FailureJson(response).dump();
    }

    const nlohmann::json body = nlohmann::json::parse(response.body, nullptr, false);
    if (body.is_discarded() || !body.is_object()) {
        return nlohmann::json{{"ok", false}, {"error_kind", "decode"}, {"message", "响应正文不是对象"}}.dump();
    }

    nlohmann::json resources = nlohmann::json::object();
    const auto it = body.find("resources");
    if (it != body.end() && it->is_object()) {
        for (auto entry = it->begin(); entry != it->end(); ++entry) {
            if (!entry.value().is_object()) {
                continue;
            }
            resources[entry.key()] = {
                {"limit", entry.value().value("limit", 0)},
                {"remaining", entry.value().value("remaining", 0)},
                {"reset", entry.value().value("reset", static_cast<long long>(0))},
            };
        }
    }

    const nlohmann::json core = resources.contains("core") ? resources["core"] : nlohmann::json::object();
    return nlohmann::json{
        {"ok", true},
        {"authenticated", handle.configured},
        {"limit", core.value("limit", 0)},
        {"remaining", core.value("remaining", 0)},
        {"reset_at", core.value("reset", static_cast<long long>(0))},
        {"resources", resources},
    }.dump();
}

} // namespace

void RegisterGitHubTools() {
    ToolRegistry* registry = ToolRegistry::Get();

    ToolRegistration user;
    user.tool.name = "github_get_user";
    user.tool.title = "Get current user";
    user.tool.description = "调用 GitHub /user 返回当前令牌对应的账号信息。未导入令牌时直接报错，不发请求。";
    user.tool.input_schema_json = R"({"type":"object","properties":{},"additionalProperties":false})";
    user.tool.annotations.read_only_hint = true;
    user.tool.annotations.idempotent_hint = true;
    user.tool.annotations.open_world_hint = true;
    user.tool.safety.kind = OperationKind::Read;
    user.handler = &HandleGetUser;
    registry->Register(user);

    ToolRegistration rate;
    rate.tool.name = "github_rate_limit";
    rate.tool.title = "Rate limit";
    rate.tool.description = "查询 GitHub API 速率限制。未导入令牌时以匿名身份查询，同样能验证网络链路。";
    rate.tool.input_schema_json = R"({"type":"object","properties":{},"additionalProperties":false})";
    rate.tool.annotations.read_only_hint = true;
    rate.tool.annotations.idempotent_hint = true;
    rate.tool.annotations.open_world_hint = true;
    rate.tool.safety.kind = OperationKind::Read;
    rate.handler = &HandleRateLimit;
    registry->Register(rate);
}

} // namespace ghb
