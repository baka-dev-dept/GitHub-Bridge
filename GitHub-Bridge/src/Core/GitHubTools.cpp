#include "Core/GitHubTools.h"

#include "Core/ToolRegistry.h"
#include "GitHub/GitHubClient.h"
#include "GitHub/GraphQLGuard.h"
#include "GitHub/RateLimiter.h"
#include "Security/TokenManager.h"
#include "Types/GitHubResponse.h"
#include "Types/McpTool.h"

#include <nlohmann/json.hpp>

#include <chrono>

namespace ghb {

namespace {

int64_t NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

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
    nlohmann::json out = {
        {"ok", false},
        {"error_kind", ErrorKindName(response.error_kind)},
        {"status", response.status},
        {"message", response.error_message},
        {"hint", FailureHint(response.error_kind)},
    };

    // 限流时把「还要等多久」算成具体秒数给出去，Agent 可以直接据此等待重试，
    // 而不是拿一句「请稍后再试」无从下手。
    if (response.error_kind == GitHubErrorKind::RateLimited) {
        out["rate_limit"] = {
            {"limit", response.headers.rate_limit.limit},
            {"remaining", response.headers.rate_limit.remaining},
            {"reset_at", response.headers.rate_limit.reset_at},
        };
        const int64_t wait_ms = RateLimiter::Get()->WaitBeforeMs(NowMs());
        if (wait_ms > 0) {
            const int64_t wait_sec = (wait_ms + 999) / 1000;
            out["retry_after_seconds"] = wait_sec;
            out["hint"] = "额度已耗尽（上限 " + std::to_string(response.headers.rate_limit.limit) +
                          "），请在约 " + std::to_string(wait_sec) + " 秒后重试";
        } else if (wait_ms == 0) {
            // 重置时间已过但服务端仍返回限流，说明是别的限制（例如次级限流）。
            out["hint"] = "服务端仍返回限流，但本地判断重置时间已过，可能是次级限流，请稍后重试";
        } else {
            out["hint"] = "额度已耗尽，且无法从响应头确定重置时间，请稍后重试";
        }
    }
    return out;
}

// 需要令牌的工具用这个拿句柄；未配置时直接返回「失败 JSON」并置 ok=false。
// 所有 GitHub 业务接口都要求认证，无令牌发出去必然 401，白耗一次配额。
bool RequireToken(TokenHandle* handle, std::string* failure) {
    *handle = TokenManager::Get()->AcquireHandle();
    if (handle->configured) {
        return true;
    }
    *failure = nlohmann::json{
        {"ok", false},
        {"error_kind", "not_configured"},
        {"message", "尚未配置令牌，请用 github_token_import 导入"},
    }.dump();
    return false;
}

// 解析响应正文为对象；失败时给出统一的解码错误。
bool ParseObject(const GitHubResponse& response, nlohmann::json* out, std::string* failure) {
    const nlohmann::json body = nlohmann::json::parse(response.body, nullptr, false);
    if (body.is_discarded() || !body.is_object()) {
        *failure = nlohmann::json{{"ok", false}, {"error_kind", "decode"}, {"message", "响应正文不是 JSON 对象"}}.dump();
        return false;
    }
    *out = body;
    return true;
}

std::string Base64Encode(const std::string& raw) {
    static constexpr char kTable[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((raw.size() + 2) / 3) * 4);
    size_t i = 0;
    while (i + 3 <= raw.size()) {
        const unsigned int n = (static_cast<unsigned char>(raw[i]) << 16) | (static_cast<unsigned char>(raw[i + 1]) << 8) | static_cast<unsigned char>(raw[i + 2]);
        out.push_back(kTable[(n >> 18) & 0x3F]);
        out.push_back(kTable[(n >> 12) & 0x3F]);
        out.push_back(kTable[(n >> 6) & 0x3F]);
        out.push_back(kTable[n & 0x3F]);
        i += 3;
    }
    const size_t remain = raw.size() - i;
    if (remain == 1) {
        const unsigned int n = static_cast<unsigned char>(raw[i]) << 16;
        out.push_back(kTable[(n >> 18) & 0x3F]);
        out.push_back(kTable[(n >> 12) & 0x3F]);
        out.push_back('=');
        out.push_back('=');
    } else if (remain == 2) {
        const unsigned int n = (static_cast<unsigned char>(raw[i]) << 16) | (static_cast<unsigned char>(raw[i + 1]) << 8);
        out.push_back(kTable[(n >> 18) & 0x3F]);
        out.push_back(kTable[(n >> 12) & 0x3F]);
        out.push_back(kTable[(n >> 6) & 0x3F]);
        out.push_back('=');
    }
    return out;
}

// GitHub 的 contents 接口返回带换行的 base64，解码前要丢掉所有空白。
bool Base64Decode(const std::string& encoded, std::string* out) {
    auto value_of = [](const char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };

    out->clear();
    unsigned int buffer = 0;
    int bits = 0;
    for (const char c : encoded) {
        if (c == '=' || c == '\n' || c == '\r' || c == ' ' || c == '\t') {
            continue;
        }
        const int value = value_of(c);
        if (value < 0) {
            return false;
        }
        buffer = (buffer << 6) | static_cast<unsigned int>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out->push_back(static_cast<char>((buffer >> bits) & 0xFF));
        }
    }
    return true;
}

// repo_full_name 统一用 "owner/repo"，直接拼进路径。
std::string RepoPath(const std::string& owner, const std::string& repo) {
    return "/repos/" + owner + "/" + repo;
}

// 从参数里取必填字符串字段；缺失或类型不对返回 false。
bool GetString(const nlohmann::json& args, const char* key, std::string* out) {
    const auto it = args.find(key);
    if (it == args.end() || !it->is_string() || it->get<std::string>().empty()) {
        return false;
    }
    *out = it->get<std::string>();
    return true;
}

std::string MissingParam(const char* key) {
    return nlohmann::json{{"ok", false}, {"error_kind", "invalid_params"}, {"message", std::string("缺少必填参数: ") + key}}.dump();
}

// 可选整数字段：存在且是整数才写入，否则保持默认。
bool GetOptionalInt(const nlohmann::json& args, const char* key, int* out) {
    const auto it = args.find(key);
    if (it == args.end() || !it->is_number_integer()) {
        return false;
    }
    *out = it->get<int>();
    return true;
}

// 分支名可能含 "/"，直接拼进 URL 会被当成路径分隔符，必须做百分号编码。
std::string UrlEncodePathSegment(const std::string& text) {
    static constexpr char kHexUpper[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(text.size() * 3);
    for (const char c : text) {
        const bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
        if (safe) {
            out.push_back(c);
        } else {
            out.push_back('%');
            out.push_back(kHexUpper[(static_cast<unsigned char>(c) >> 4) & 0x0F]);
            out.push_back(kHexUpper[static_cast<unsigned char>(c) & 0x0F]);
        }
    }
    return out;
}

// GitHub 对可选文本字段常返回显式 null（issue 无正文、仓库无描述等）。
// nlohmann 的 value() 只在字段缺失时用默认值，字段存在但为 null 会抛异常，
// 所以必须显式判断 is_string，否则工具会以 -32603 崩溃。
std::string SafeString(const nlohmann::json& object, const char* key) {
    if (!object.is_object()) {
        return {};
    }
    const auto it = object.find(key);
    if (it == object.end() || !it->is_string()) {
        return {};
    }
    return it->get<std::string>();
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
        {"login", SafeString(body, "login")},
        {"id", body.value("id", static_cast<long long>(0))},
        {"name", SafeString(body, "name")},
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

// ===== Repository =====

std::string HandleSearchRepositories(const std::string& arguments_json) {
    const nlohmann::json args = nlohmann::json::parse(arguments_json, nullptr, false);
    std::string query;
    if (!args.is_object() || !GetString(args, "query", &query)) {
        return MissingParam("query");
    }

    TokenHandle handle;
    std::string failure;
    if (!RequireToken(&handle, &failure)) {
        return failure;
    }

    // 搜索接口不能直接把空格拼进 URL，需要百分号编码。
    std::string encoded;
    for (const char c : query) {
        const bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~' || c == ':';
        if (safe) {
            encoded.push_back(c);
        } else {
            encoded.push_back('%');
            static constexpr char kHexUpper[] = "0123456789ABCDEF";
            encoded.push_back(kHexUpper[(static_cast<unsigned char>(c) >> 4) & 0x0F]);
            encoded.push_back(kHexUpper[static_cast<unsigned char>(c) & 0x0F]);
        }
    }

    const GitHubResponse response = GitHubClient::Get()->AuthorizedGet("/search/repositories?q=" + encoded, handle.authorization_header);
    if (!response.ok) {
        return FailureJson(response).dump();
    }

    nlohmann::json body;
    if (!ParseObject(response, &body, &failure)) {
        return failure;
    }

    nlohmann::json items = nlohmann::json::array();
    const auto found = body.find("items");
    if (found != body.end() && found->is_array()) {
        for (const auto& item : *found) {
            if (!item.is_object()) {
                continue;
            }
            items.push_back({
                {"full_name", item.value("full_name", "")},
                {"description", SafeString(item, "description")},
                {"private", item.value("private", false)},
                {"html_url", item.value("html_url", "")},
                {"default_branch", item.value("default_branch", "")},
                {"stars", item.value("stargazers_count", 0)},
            });
        }
    }

    return nlohmann::json{
        {"ok", true},
        {"total_count", body.value("total_count", 0)},
        {"items", items},
    }.dump();
}

std::string HandleGetRepository(const std::string& arguments_json) {
    const nlohmann::json args = nlohmann::json::parse(arguments_json, nullptr, false);
    std::string owner;
    std::string repo;
    if (!args.is_object() || !GetString(args, "owner", &owner)) {
        return MissingParam("owner");
    }
    if (!GetString(args, "repo", &repo)) {
        return MissingParam("repo");
    }

    TokenHandle handle;
    std::string failure;
    if (!RequireToken(&handle, &failure)) {
        return failure;
    }

    const GitHubResponse response = GitHubClient::Get()->AuthorizedGet(RepoPath(owner, repo), handle.authorization_header);
    if (!response.ok) {
        return FailureJson(response).dump();
    }

    nlohmann::json body;
    if (!ParseObject(response, &body, &failure)) {
        return failure;
    }

    return nlohmann::json{
        {"ok", true},
        {"full_name", body.value("full_name", "")},
        {"description", SafeString(body, "description")},
        {"private", body.value("private", false)},
        {"html_url", body.value("html_url", "")},
        {"default_branch", body.value("default_branch", "")},
        {"stars", body.value("stargazers_count", 0)},
        {"forks", body.value("forks_count", 0)},
        {"open_issues", body.value("open_issues_count", 0)},
    }.dump();
}

std::string HandleCreateRepository(const std::string& arguments_json) {
    const nlohmann::json args = nlohmann::json::parse(arguments_json, nullptr, false);
    std::string name;
    if (!args.is_object() || !GetString(args, "name", &name)) {
        return MissingParam("name");
    }

    TokenHandle handle;
    std::string failure;
    if (!RequireToken(&handle, &failure)) {
        return failure;
    }

    nlohmann::json payload = {{"name", name}};
    if (args.contains("description") && args["description"].is_string()) {
        payload["description"] = args["description"];
    }
    if (args.contains("private") && args["private"].is_boolean()) {
        payload["private"] = args["private"];
    }
    if (args.contains("auto_init") && args["auto_init"].is_boolean()) {
        payload["auto_init"] = args["auto_init"];
    }

    const GitHubResponse response = GitHubClient::Get()->AuthorizedPost("/user/repos", payload.dump(), handle.authorization_header);
    if (!response.ok) {
        return FailureJson(response).dump();
    }

    nlohmann::json body;
    if (!ParseObject(response, &body, &failure)) {
        return failure;
    }
    return nlohmann::json{
        {"ok", true},
        {"full_name", body.value("full_name", "")},
        {"html_url", body.value("html_url", "")},
        {"private", body.value("private", false)},
        {"default_branch", body.value("default_branch", "")},
    }.dump();
}

std::string HandleUpdateRepository(const std::string& arguments_json) {
    const nlohmann::json args = nlohmann::json::parse(arguments_json, nullptr, false);
    std::string owner;
    std::string repo;
    if (!args.is_object() || !GetString(args, "owner", &owner)) {
        return MissingParam("owner");
    }
    if (!GetString(args, "repo", &repo)) {
        return MissingParam("repo");
    }

    TokenHandle handle;
    std::string failure;
    if (!RequireToken(&handle, &failure)) {
        return failure;
    }

    // 只透传调用方显式给出的字段，避免 PATCH 把未提及的字段清空。
    nlohmann::json payload = nlohmann::json::object();
    if (args.contains("description") && args["description"].is_string()) {
        payload["description"] = args["description"];
    }
    if (args.contains("private") && args["private"].is_boolean()) {
        payload["private"] = args["private"];
    }
    if (args.contains("name") && args["name"].is_string()) {
        payload["name"] = args["name"];
    }
    if (args.contains("default_branch") && args["default_branch"].is_string()) {
        payload["default_branch"] = args["default_branch"];
    }
    if (payload.empty()) {
        return nlohmann::json{{"ok", false}, {"error_kind", "invalid_params"}, {"message", "没有可更新的字段"}}.dump();
    }

    const GitHubResponse response = GitHubClient::Get()->AuthorizedPatch(RepoPath(owner, repo), payload.dump(), handle.authorization_header);
    if (!response.ok) {
        return FailureJson(response).dump();
    }

    nlohmann::json body;
    if (!ParseObject(response, &body, &failure)) {
        return failure;
    }
    return nlohmann::json{
        {"ok", true},
        {"full_name", body.value("full_name", "")},
        {"description", SafeString(body, "description")},
        {"private", body.value("private", false)},
    }.dump();
}

std::string HandleDeleteRepository(const std::string& arguments_json) {
    const nlohmann::json args = nlohmann::json::parse(arguments_json, nullptr, false);
    std::string owner;
    std::string repo;
    if (!args.is_object() || !GetString(args, "owner", &owner)) {
        return MissingParam("owner");
    }
    if (!GetString(args, "repo", &repo)) {
        return MissingParam("repo");
    }

    TokenHandle handle;
    std::string failure;
    if (!RequireToken(&handle, &failure)) {
        return failure;
    }

    const GitHubResponse response = GitHubClient::Get()->AuthorizedDelete(RepoPath(owner, repo), handle.authorization_header);
    // 删除成功返回 204，没有正文。
    if (!response.ok) {
        return FailureJson(response).dump();
    }
    return nlohmann::json{{"ok", true}, {"deleted", owner + "/" + repo}}.dump();
}

// ===== Files =====

std::string HandleGetFile(const std::string& arguments_json) {
    const nlohmann::json args = nlohmann::json::parse(arguments_json, nullptr, false);
    std::string owner;
    std::string repo;
    if (!args.is_object() || !GetString(args, "owner", &owner)) {
        return MissingParam("owner");
    }
    if (!GetString(args, "repo", &repo)) {
        return MissingParam("repo");
    }
    std::string path;
    if (!GetString(args, "path", &path)) {
        return MissingParam("path");
    }

    TokenHandle handle;
    std::string failure;
    if (!RequireToken(&handle, &failure)) {
        return failure;
    }

    std::string url = RepoPath(owner, repo) + "/contents/" + path;
    if (args.contains("ref") && args["ref"].is_string()) {
        url += "?ref=" + args["ref"].get<std::string>();
    }

    const GitHubResponse response = GitHubClient::Get()->AuthorizedGet(url, handle.authorization_header);
    if (!response.ok) {
        return FailureJson(response).dump();
    }

    nlohmann::json body;
    if (!ParseObject(response, &body, &failure)) {
        return failure;
    }

    // 目录也走同一个接口，此时返回数组形态，这里明确区分。
    if (body.contains("type") && body["type"] == "dir") {
        return nlohmann::json{{"ok", false}, {"error_kind", "is_directory"}, {"message", "该路径是目录，不是文件"}}.dump();
    }

    // 元数据与内容分开：默认只回元数据，正文由 github_get_file_content 取，避免大文件撑爆返回。
    return nlohmann::json{
        {"ok", true},
        {"path", body.value("path", "")},
        {"sha", body.value("sha", "")},
        {"size", body.value("size", 0)},
        {"encoding", body.value("encoding", "")},
        {"download_url", SafeString(body, "download_url")},
    }.dump();
}

std::string HandleGetFileContent(const std::string& arguments_json) {
    const nlohmann::json args = nlohmann::json::parse(arguments_json, nullptr, false);
    std::string owner;
    std::string repo;
    if (!args.is_object() || !GetString(args, "owner", &owner)) {
        return MissingParam("owner");
    }
    if (!GetString(args, "repo", &repo)) {
        return MissingParam("repo");
    }
    std::string path;
    if (!GetString(args, "path", &path)) {
        return MissingParam("path");
    }

    TokenHandle handle;
    std::string failure;
    if (!RequireToken(&handle, &failure)) {
        return failure;
    }

    std::string url = RepoPath(owner, repo) + "/contents/" + path;
    if (args.contains("ref") && args["ref"].is_string()) {
        url += "?ref=" + args["ref"].get<std::string>();
    }

    const GitHubResponse response = GitHubClient::Get()->AuthorizedGet(url, handle.authorization_header);
    if (!response.ok) {
        return FailureJson(response).dump();
    }

    nlohmann::json body;
    if (!ParseObject(response, &body, &failure)) {
        return failure;
    }

    const auto content_it = body.find("content");
    if (content_it == body.end() || !content_it->is_string()) {
        return nlohmann::json{{"ok", false}, {"error_kind", "no_content"}, {"message", "响应中没有 content 字段"}}.dump();
    }

    std::string decoded;
    if (!Base64Decode(content_it->get<std::string>(), &decoded)) {
        return nlohmann::json{{"ok", false}, {"error_kind", "decode"}, {"message", "base64 解码失败"}}.dump();
    }

    return nlohmann::json{
        {"ok", true},
        {"path", body.value("path", "")},
        {"sha", body.value("sha", "")},
        {"size", body.value("size", 0)},
        {"content", decoded},
    }.dump();
}

std::string HandleCreateOrUpdateFile(const std::string& arguments_json) {
    const nlohmann::json args = nlohmann::json::parse(arguments_json, nullptr, false);
    std::string owner;
    std::string repo;
    if (!args.is_object() || !GetString(args, "owner", &owner)) {
        return MissingParam("owner");
    }
    if (!GetString(args, "repo", &repo)) {
        return MissingParam("repo");
    }
    std::string path;
    if (!GetString(args, "path", &path)) {
        return MissingParam("path");
    }
    std::string content;
    if (!GetString(args, "content", &content)) {
        return MissingParam("content");
    }

    TokenHandle handle;
    std::string failure;
    if (!RequireToken(&handle, &failure)) {
        return failure;
    }

    nlohmann::json payload = {
        {"message", args.contains("message") && args["message"].is_string() ? args["message"].get<std::string>() : "Update " + path},
        {"content", Base64Encode(content)},
    };
    if (args.contains("branch") && args["branch"].is_string()) {
        payload["branch"] = args["branch"];
    }
    // 更新已有文件必须带 sha，否则 GitHub 返回 422；创建新文件则不能带。
    if (args.contains("sha") && args["sha"].is_string()) {
        payload["sha"] = args["sha"];
    }

    const GitHubResponse response = GitHubClient::Get()->AuthorizedPut(RepoPath(owner, repo) + "/contents/" + path, payload.dump(), handle.authorization_header);
    if (!response.ok) {
        return FailureJson(response).dump();
    }

    nlohmann::json body;
    if (!ParseObject(response, &body, &failure)) {
        return failure;
    }
    const nlohmann::json commit = body.contains("commit") && body["commit"].is_object() ? body["commit"] : nlohmann::json::object();
    return nlohmann::json{
        {"ok", true},
        {"path", body.contains("content") && body["content"].is_object() ? body["content"].value("path", "") : path},
        {"commit_sha", commit.value("sha", "")},
    }.dump();
}

std::string HandleDeleteFile(const std::string& arguments_json) {
    const nlohmann::json args = nlohmann::json::parse(arguments_json, nullptr, false);
    std::string owner;
    std::string repo;
    if (!args.is_object() || !GetString(args, "owner", &owner)) {
        return MissingParam("owner");
    }
    if (!GetString(args, "repo", &repo)) {
        return MissingParam("repo");
    }
    std::string path;
    if (!GetString(args, "path", &path)) {
        return MissingParam("path");
    }
    // 删除必须带 sha：GitHub 用它做并发保护，避免删掉别人刚推的新版本。
    std::string sha;
    if (!GetString(args, "sha", &sha)) {
        return MissingParam("sha");
    }

    TokenHandle handle;
    std::string failure;
    if (!RequireToken(&handle, &failure)) {
        return failure;
    }

    // contents 接口的删除是 DELETE + 请求体，WinHttpClient 已支持带正文。
    nlohmann::json payload = {
        {"message", args.contains("message") && args["message"].is_string() ? args["message"].get<std::string>() : "Delete " + path},
        {"sha", sha},
    };
    if (args.contains("branch") && args["branch"].is_string()) {
        payload["branch"] = args["branch"];
    }

    const GitHubResponse response = GitHubClient::Get()->Request("DELETE", RepoPath(owner, repo) + "/contents/" + path, payload.dump(), handle.authorization_header);
    if (!response.ok) {
        return FailureJson(response).dump();
    }
    return nlohmann::json{{"ok", true}, {"deleted", path}}.dump();
}

// ===== Issues =====

nlohmann::json ProjectIssue(const nlohmann::json& item) {
    // 带 pull_request 字段说明它是 PR 不是 Issue：GitHub 的 issues 接口两者混返。
    return {
        {"number", item.value("number", 0)},
        {"title", SafeString(item, "title")},
        {"state", item.value("state", "")},
        {"html_url", item.value("html_url", "")},
        {"is_pull_request", item.contains("pull_request")},
        {"comments", item.value("comments", 0)},
    };
}

std::string HandleListIssues(const std::string& arguments_json) {
    const nlohmann::json args = nlohmann::json::parse(arguments_json, nullptr, false);
    std::string owner;
    std::string repo;
    if (!args.is_object() || !GetString(args, "owner", &owner)) {
        return MissingParam("owner");
    }
    if (!GetString(args, "repo", &repo)) {
        return MissingParam("repo");
    }

    TokenHandle handle;
    std::string failure;
    if (!RequireToken(&handle, &failure)) {
        return failure;
    }

    std::string url = RepoPath(owner, repo) + "/issues";
    std::string state;
    if (GetString(args, "state", &state)) {
        url += "?state=" + state;
    }

    const GitHubResponse response = GitHubClient::Get()->AuthorizedGet(url, handle.authorization_header);
    if (!response.ok) {
        return FailureJson(response).dump();
    }

    const nlohmann::json body = nlohmann::json::parse(response.body, nullptr, false);
    if (body.is_discarded() || !body.is_array()) {
        return nlohmann::json{{"ok", false}, {"error_kind", "decode"}, {"message", "响应正文不是数组"}}.dump();
    }

    nlohmann::json items = nlohmann::json::array();
    for (const auto& item : body) {
        if (item.is_object()) {
            items.push_back(ProjectIssue(item));
        }
    }
    return nlohmann::json{{"ok", true}, {"count", items.size()}, {"items", items}}.dump();
}

std::string HandleGetIssue(const std::string& arguments_json) {
    const nlohmann::json args = nlohmann::json::parse(arguments_json, nullptr, false);
    std::string owner;
    std::string repo;
    if (!args.is_object() || !GetString(args, "owner", &owner)) {
        return MissingParam("owner");
    }
    if (!GetString(args, "repo", &repo)) {
        return MissingParam("repo");
    }
    int number = 0;
    if (!GetOptionalInt(args, "number", &number) || number <= 0) {
        return MissingParam("number");
    }

    TokenHandle handle;
    std::string failure;
    if (!RequireToken(&handle, &failure)) {
        return failure;
    }

    const GitHubResponse response = GitHubClient::Get()->AuthorizedGet(RepoPath(owner, repo) + "/issues/" + std::to_string(number), handle.authorization_header);
    if (!response.ok) {
        return FailureJson(response).dump();
    }

    nlohmann::json body;
    if (!ParseObject(response, &body, &failure)) {
        return failure;
    }

    return nlohmann::json{
        {"ok", true},
        {"number", body.value("number", 0)},
        {"title", SafeString(body, "title")},
        {"state", body.value("state", "")},
        {"body", SafeString(body, "body")},
        {"html_url", body.value("html_url", "")},
        {"is_pull_request", body.contains("pull_request")},
    }.dump();
}

std::string HandleCreateIssue(const std::string& arguments_json) {
    const nlohmann::json args = nlohmann::json::parse(arguments_json, nullptr, false);
    std::string owner;
    std::string repo;
    if (!args.is_object() || !GetString(args, "owner", &owner)) {
        return MissingParam("owner");
    }
    if (!GetString(args, "repo", &repo)) {
        return MissingParam("repo");
    }
    std::string title;
    if (!GetString(args, "title", &title)) {
        return MissingParam("title");
    }

    TokenHandle handle;
    std::string failure;
    if (!RequireToken(&handle, &failure)) {
        return failure;
    }

    nlohmann::json payload = {{"title", title}};
    if (args.contains("body") && args["body"].is_string()) {
        payload["body"] = args["body"];
    }
    if (args.contains("labels") && args["labels"].is_array()) {
        payload["labels"] = args["labels"];
    }
    if (args.contains("assignees") && args["assignees"].is_array()) {
        payload["assignees"] = args["assignees"];
    }

    const GitHubResponse response = GitHubClient::Get()->AuthorizedPost(RepoPath(owner, repo) + "/issues", payload.dump(), handle.authorization_header);
    if (!response.ok) {
        return FailureJson(response).dump();
    }

    nlohmann::json body;
    if (!ParseObject(response, &body, &failure)) {
        return failure;
    }
    return nlohmann::json{{"ok", true}, {"number", body.value("number", 0)}, {"html_url", body.value("html_url", "")}}.dump();
}

std::string HandleUpdateIssue(const std::string& arguments_json) {
    const nlohmann::json args = nlohmann::json::parse(arguments_json, nullptr, false);
    std::string owner;
    std::string repo;
    if (!args.is_object() || !GetString(args, "owner", &owner)) {
        return MissingParam("owner");
    }
    if (!GetString(args, "repo", &repo)) {
        return MissingParam("repo");
    }
    int number = 0;
    if (!GetOptionalInt(args, "number", &number) || number <= 0) {
        return MissingParam("number");
    }

    TokenHandle handle;
    std::string failure;
    if (!RequireToken(&handle, &failure)) {
        return failure;
    }

    // 只透传显式给出的字段，避免 PATCH 把未提及的字段清空。
    nlohmann::json payload = nlohmann::json::object();
    if (args.contains("title") && args["title"].is_string()) {
        payload["title"] = args["title"];
    }
    if (args.contains("body") && args["body"].is_string()) {
        payload["body"] = args["body"];
    }
    if (args.contains("state") && args["state"].is_string()) {
        payload["state"] = args["state"];
    }
    if (payload.empty()) {
        return nlohmann::json{{"ok", false}, {"error_kind", "invalid_params"}, {"message", "没有可更新的字段"}}.dump();
    }

    const GitHubResponse response = GitHubClient::Get()->AuthorizedPatch(RepoPath(owner, repo) + "/issues/" + std::to_string(number), payload.dump(), handle.authorization_header);
    if (!response.ok) {
        return FailureJson(response).dump();
    }

    nlohmann::json body;
    if (!ParseObject(response, &body, &failure)) {
        return failure;
    }
    return nlohmann::json{{"ok", true}, {"number", body.value("number", 0)}, {"state", body.value("state", "")}}.dump();
}

std::string HandleCloseIssue(const std::string& arguments_json) {
    const nlohmann::json args = nlohmann::json::parse(arguments_json, nullptr, false);
    std::string owner;
    std::string repo;
    if (!args.is_object() || !GetString(args, "owner", &owner)) {
        return MissingParam("owner");
    }
    if (!GetString(args, "repo", &repo)) {
        return MissingParam("repo");
    }
    int number = 0;
    if (!GetOptionalInt(args, "number", &number) || number <= 0) {
        return MissingParam("number");
    }

    TokenHandle handle;
    std::string failure;
    if (!RequireToken(&handle, &failure)) {
        return failure;
    }

    // 关闭就是 PATCH state=closed；单独成工具是为了让意图更明确。
    nlohmann::json payload = {{"state", "closed"}};
    if (args.contains("state_reason") && args["state_reason"].is_string()) {
        payload["state_reason"] = args["state_reason"];
    }

    const GitHubResponse response = GitHubClient::Get()->AuthorizedPatch(RepoPath(owner, repo) + "/issues/" + std::to_string(number), payload.dump(), handle.authorization_header);
    if (!response.ok) {
        return FailureJson(response).dump();
    }

    nlohmann::json body;
    if (!ParseObject(response, &body, &failure)) {
        return failure;
    }
    return nlohmann::json{{"ok", true}, {"number", body.value("number", 0)}, {"state", body.value("state", "")}}.dump();
}

// ===== Branches =====

std::string HandleListBranches(const std::string& arguments_json) {
    const nlohmann::json args = nlohmann::json::parse(arguments_json, nullptr, false);
    std::string owner;
    std::string repo;
    if (!args.is_object() || !GetString(args, "owner", &owner)) {
        return MissingParam("owner");
    }
    if (!GetString(args, "repo", &repo)) {
        return MissingParam("repo");
    }

    TokenHandle handle;
    std::string failure;
    if (!RequireToken(&handle, &failure)) {
        return failure;
    }

    const GitHubResponse response = GitHubClient::Get()->AuthorizedGet(RepoPath(owner, repo) + "/branches", handle.authorization_header);
    if (!response.ok) {
        return FailureJson(response).dump();
    }

    const nlohmann::json body = nlohmann::json::parse(response.body, nullptr, false);
    if (body.is_discarded() || !body.is_array()) {
        return nlohmann::json{{"ok", false}, {"error_kind", "decode"}, {"message", "响应正文不是数组"}}.dump();
    }

    nlohmann::json items = nlohmann::json::array();
    for (const auto& item : body) {
        if (!item.is_object()) {
            continue;
        }
        const nlohmann::json commit = item.contains("commit") && item["commit"].is_object() ? item["commit"] : nlohmann::json::object();
        items.push_back({{"name", SafeString(item, "name")}, {"sha", commit.value("sha", "")}, {"protected", item.value("protected", false)}});
    }
    return nlohmann::json{{"ok", true}, {"count", items.size()}, {"items", items}}.dump();
}

std::string HandleGetBranch(const std::string& arguments_json) {
    const nlohmann::json args = nlohmann::json::parse(arguments_json, nullptr, false);
    std::string owner;
    std::string repo;
    if (!args.is_object() || !GetString(args, "owner", &owner)) {
        return MissingParam("owner");
    }
    if (!GetString(args, "repo", &repo)) {
        return MissingParam("repo");
    }
    std::string branch;
    if (!GetString(args, "branch", &branch)) {
        return MissingParam("branch");
    }

    TokenHandle handle;
    std::string failure;
    if (!RequireToken(&handle, &failure)) {
        return failure;
    }

    const GitHubResponse response = GitHubClient::Get()->AuthorizedGet(RepoPath(owner, repo) + "/branches/" + UrlEncodePathSegment(branch), handle.authorization_header);
    if (!response.ok) {
        return FailureJson(response).dump();
    }

    nlohmann::json body;
    if (!ParseObject(response, &body, &failure)) {
        return failure;
    }
    const nlohmann::json commit = body.contains("commit") && body["commit"].is_object() ? body["commit"] : nlohmann::json::object();
    return nlohmann::json{
        {"ok", true},
        {"name", SafeString(body, "name")},
        {"sha", commit.value("sha", "")},
        {"protected", body.value("protected", false)},
    }.dump();
}

std::string HandleCreateBranch(const std::string& arguments_json) {
    const nlohmann::json args = nlohmann::json::parse(arguments_json, nullptr, false);
    std::string owner;
    std::string repo;
    if (!args.is_object() || !GetString(args, "owner", &owner)) {
        return MissingParam("owner");
    }
    if (!GetString(args, "repo", &repo)) {
        return MissingParam("repo");
    }
    std::string branch;
    if (!GetString(args, "branch", &branch)) {
        return MissingParam("branch");
    }
    // 建分支本质是建一个指向某提交的 ref，起点必须由调用方明确给出。
    std::string from_sha;
    if (!GetString(args, "from_sha", &from_sha)) {
        return MissingParam("from_sha");
    }

    TokenHandle handle;
    std::string failure;
    if (!RequireToken(&handle, &failure)) {
        return failure;
    }

    nlohmann::json payload = {{"ref", "refs/heads/" + branch}, {"sha", from_sha}};
    const GitHubResponse response = GitHubClient::Get()->AuthorizedPost(RepoPath(owner, repo) + "/git/refs", payload.dump(), handle.authorization_header);
    if (!response.ok) {
        return FailureJson(response).dump();
    }

    nlohmann::json body;
    if (!ParseObject(response, &body, &failure)) {
        return failure;
    }
    const nlohmann::json object = body.contains("object") && body["object"].is_object() ? body["object"] : nlohmann::json::object();
    return nlohmann::json{{"ok", true}, {"ref", body.value("ref", "")}, {"sha", object.value("sha", "")}}.dump();
}

std::string HandleDeleteBranch(const std::string& arguments_json) {
    const nlohmann::json args = nlohmann::json::parse(arguments_json, nullptr, false);
    std::string owner;
    std::string repo;
    if (!args.is_object() || !GetString(args, "owner", &owner)) {
        return MissingParam("owner");
    }
    if (!GetString(args, "repo", &repo)) {
        return MissingParam("repo");
    }
    std::string branch;
    if (!GetString(args, "branch", &branch)) {
        return MissingParam("branch");
    }

    TokenHandle handle;
    std::string failure;
    if (!RequireToken(&handle, &failure)) {
        return failure;
    }

    const GitHubResponse response = GitHubClient::Get()->AuthorizedDelete(RepoPath(owner, repo) + "/git/refs/heads/" + UrlEncodePathSegment(branch), handle.authorization_header);
    if (!response.ok) {
        return FailureJson(response).dump();
    }
    return nlohmann::json{{"ok", true}, {"deleted", branch}}.dump();
}

// ===== Pull Requests =====

nlohmann::json ProjectPullRequest(const nlohmann::json& item) {
    const nlohmann::json head = item.contains("head") && item["head"].is_object() ? item["head"] : nlohmann::json::object();
    const nlohmann::json base = item.contains("base") && item["base"].is_object() ? item["base"] : nlohmann::json::object();
    return {
        {"number", item.value("number", 0)},
        {"title", SafeString(item, "title")},
        {"state", item.value("state", "")},
        {"merged", item.value("merged", false)},
        {"head", head.value("ref", "")},
        {"base", base.value("ref", "")},
        {"html_url", item.value("html_url", "")},
    };
}

std::string HandleListPullRequests(const std::string& arguments_json) {
    const nlohmann::json args = nlohmann::json::parse(arguments_json, nullptr, false);
    std::string owner;
    std::string repo;
    if (!args.is_object() || !GetString(args, "owner", &owner)) {
        return MissingParam("owner");
    }
    if (!GetString(args, "repo", &repo)) {
        return MissingParam("repo");
    }

    TokenHandle handle;
    std::string failure;
    if (!RequireToken(&handle, &failure)) {
        return failure;
    }

    std::string url = RepoPath(owner, repo) + "/pulls";
    std::string state;
    if (GetString(args, "state", &state)) {
        url += "?state=" + state;
    }

    const GitHubResponse response = GitHubClient::Get()->AuthorizedGet(url, handle.authorization_header);
    if (!response.ok) {
        return FailureJson(response).dump();
    }

    const nlohmann::json body = nlohmann::json::parse(response.body, nullptr, false);
    if (body.is_discarded() || !body.is_array()) {
        return nlohmann::json{{"ok", false}, {"error_kind", "decode"}, {"message", "响应正文不是数组"}}.dump();
    }

    nlohmann::json items = nlohmann::json::array();
    for (const auto& item : body) {
        if (item.is_object()) {
            items.push_back(ProjectPullRequest(item));
        }
    }
    return nlohmann::json{{"ok", true}, {"count", items.size()}, {"items", items}}.dump();
}

std::string HandleGetPullRequest(const std::string& arguments_json) {
    const nlohmann::json args = nlohmann::json::parse(arguments_json, nullptr, false);
    std::string owner;
    std::string repo;
    if (!args.is_object() || !GetString(args, "owner", &owner)) {
        return MissingParam("owner");
    }
    if (!GetString(args, "repo", &repo)) {
        return MissingParam("repo");
    }
    int number = 0;
    if (!GetOptionalInt(args, "number", &number) || number <= 0) {
        return MissingParam("number");
    }

    TokenHandle handle;
    std::string failure;
    if (!RequireToken(&handle, &failure)) {
        return failure;
    }

    const GitHubResponse response = GitHubClient::Get()->AuthorizedGet(RepoPath(owner, repo) + "/pulls/" + std::to_string(number), handle.authorization_header);
    if (!response.ok) {
        return FailureJson(response).dump();
    }

    nlohmann::json body;
    if (!ParseObject(response, &body, &failure)) {
        return failure;
    }

    nlohmann::json result = ProjectPullRequest(body);
    result["ok"] = true;
    result["body"] = SafeString(body, "body");
    result["mergeable"] = body.value("mergeable", false);
    result["merged_at"] = body.contains("merged_at") && body["merged_at"].is_string() ? body["merged_at"] : "";
    return result.dump();
}

std::string HandleCreatePullRequest(const std::string& arguments_json) {
    const nlohmann::json args = nlohmann::json::parse(arguments_json, nullptr, false);
    std::string owner;
    std::string repo;
    if (!args.is_object() || !GetString(args, "owner", &owner)) {
        return MissingParam("owner");
    }
    if (!GetString(args, "repo", &repo)) {
        return MissingParam("repo");
    }
    std::string title;
    if (!GetString(args, "title", &title)) {
        return MissingParam("title");
    }
    std::string head;
    if (!GetString(args, "head", &head)) {
        return MissingParam("head");
    }
    std::string base;
    if (!GetString(args, "base", &base)) {
        return MissingParam("base");
    }

    TokenHandle handle;
    std::string failure;
    if (!RequireToken(&handle, &failure)) {
        return failure;
    }

    nlohmann::json payload = {{"title", title}, {"head", head}, {"base", base}};
    if (args.contains("body") && args["body"].is_string()) {
        payload["body"] = args["body"];
    }
    if (args.contains("draft") && args["draft"].is_boolean()) {
        payload["draft"] = args["draft"];
    }

    const GitHubResponse response = GitHubClient::Get()->AuthorizedPost(RepoPath(owner, repo) + "/pulls", payload.dump(), handle.authorization_header);
    if (!response.ok) {
        return FailureJson(response).dump();
    }

    nlohmann::json body;
    if (!ParseObject(response, &body, &failure)) {
        return failure;
    }
    return nlohmann::json{{"ok", true}, {"number", body.value("number", 0)}, {"html_url", body.value("html_url", "")}}.dump();
}

std::string HandleUpdatePullRequest(const std::string& arguments_json) {
    const nlohmann::json args = nlohmann::json::parse(arguments_json, nullptr, false);
    std::string owner;
    std::string repo;
    if (!args.is_object() || !GetString(args, "owner", &owner)) {
        return MissingParam("owner");
    }
    if (!GetString(args, "repo", &repo)) {
        return MissingParam("repo");
    }
    int number = 0;
    if (!GetOptionalInt(args, "number", &number) || number <= 0) {
        return MissingParam("number");
    }

    TokenHandle handle;
    std::string failure;
    if (!RequireToken(&handle, &failure)) {
        return failure;
    }

    nlohmann::json payload = nlohmann::json::object();
    if (args.contains("title") && args["title"].is_string()) {
        payload["title"] = args["title"];
    }
    if (args.contains("body") && args["body"].is_string()) {
        payload["body"] = args["body"];
    }
    if (args.contains("state") && args["state"].is_string()) {
        payload["state"] = args["state"];
    }
    if (args.contains("base") && args["base"].is_string()) {
        payload["base"] = args["base"];
    }
    if (payload.empty()) {
        return nlohmann::json{{"ok", false}, {"error_kind", "invalid_params"}, {"message", "没有可更新的字段"}}.dump();
    }

    const GitHubResponse response = GitHubClient::Get()->AuthorizedPatch(RepoPath(owner, repo) + "/pulls/" + std::to_string(number), payload.dump(), handle.authorization_header);
    if (!response.ok) {
        return FailureJson(response).dump();
    }

    nlohmann::json body;
    if (!ParseObject(response, &body, &failure)) {
        return failure;
    }
    return nlohmann::json{{"ok", true}, {"number", body.value("number", 0)}, {"state", body.value("state", "")}}.dump();
}

std::string HandleMergePullRequest(const std::string& arguments_json) {
    const nlohmann::json args = nlohmann::json::parse(arguments_json, nullptr, false);
    std::string owner;
    std::string repo;
    if (!args.is_object() || !GetString(args, "owner", &owner)) {
        return MissingParam("owner");
    }
    if (!GetString(args, "repo", &repo)) {
        return MissingParam("repo");
    }
    int number = 0;
    if (!GetOptionalInt(args, "number", &number) || number <= 0) {
        return MissingParam("number");
    }

    TokenHandle handle;
    std::string failure;
    if (!RequireToken(&handle, &failure)) {
        return failure;
    }

    // merge_method 决定历史形态；不传时 GitHub 用仓库默认值。
    nlohmann::json payload = nlohmann::json::object();
    if (args.contains("commit_title") && args["commit_title"].is_string()) {
        payload["commit_title"] = args["commit_title"];
    }
    if (args.contains("merge_method") && args["merge_method"].is_string()) {
        payload["merge_method"] = args["merge_method"];
    }

    const GitHubResponse response = GitHubClient::Get()->AuthorizedPut(RepoPath(owner, repo) + "/pulls/" + std::to_string(number) + "/merge", payload.dump(), handle.authorization_header);
    if (!response.ok) {
        return FailureJson(response).dump();
    }

    nlohmann::json body;
    if (!ParseObject(response, &body, &failure)) {
        return failure;
    }
    return nlohmann::json{
        {"ok", true},
        {"merged", body.value("merged", false)},
        {"message", SafeString(body, "message")},
        {"sha", body.value("sha", "")},
    }.dump();
}

// ===== Releases =====

nlohmann::json ProjectRelease(const nlohmann::json& item) {
    return {
        {"id", item.value("id", static_cast<long long>(0))},
        {"tag_name", SafeString(item, "tag_name")},
        {"name", SafeString(item, "name")},
        {"draft", item.value("draft", false)},
        {"prerelease", item.value("prerelease", false)},
        {"html_url", item.value("html_url", "")},
    };
}

std::string HandleListReleases(const std::string& arguments_json) {
    const nlohmann::json args = nlohmann::json::parse(arguments_json, nullptr, false);
    std::string owner;
    std::string repo;
    if (!args.is_object() || !GetString(args, "owner", &owner)) {
        return MissingParam("owner");
    }
    if (!GetString(args, "repo", &repo)) {
        return MissingParam("repo");
    }

    TokenHandle handle;
    std::string failure;
    if (!RequireToken(&handle, &failure)) {
        return failure;
    }

    const GitHubResponse response = GitHubClient::Get()->AuthorizedGet(RepoPath(owner, repo) + "/releases", handle.authorization_header);
    if (!response.ok) {
        return FailureJson(response).dump();
    }

    const nlohmann::json body = nlohmann::json::parse(response.body, nullptr, false);
    if (body.is_discarded() || !body.is_array()) {
        return nlohmann::json{{"ok", false}, {"error_kind", "decode"}, {"message", "响应正文不是数组"}}.dump();
    }

    nlohmann::json items = nlohmann::json::array();
    for (const auto& item : body) {
        if (item.is_object()) {
            items.push_back(ProjectRelease(item));
        }
    }
    return nlohmann::json{{"ok", true}, {"count", items.size()}, {"items", items}}.dump();
}

std::string HandleGetRelease(const std::string& arguments_json) {
    const nlohmann::json args = nlohmann::json::parse(arguments_json, nullptr, false);
    std::string owner;
    std::string repo;
    if (!args.is_object() || !GetString(args, "owner", &owner)) {
        return MissingParam("owner");
    }
    if (!GetString(args, "repo", &repo)) {
        return MissingParam("repo");
    }
    int id = 0;
    if (!GetOptionalInt(args, "id", &id) || id <= 0) {
        return MissingParam("id");
    }

    TokenHandle handle;
    std::string failure;
    if (!RequireToken(&handle, &failure)) {
        return failure;
    }

    const GitHubResponse response = GitHubClient::Get()->AuthorizedGet(RepoPath(owner, repo) + "/releases/" + std::to_string(id), handle.authorization_header);
    if (!response.ok) {
        return FailureJson(response).dump();
    }

    nlohmann::json body;
    if (!ParseObject(response, &body, &failure)) {
        return failure;
    }
    nlohmann::json result = ProjectRelease(body);
    result["ok"] = true;
    result["body"] = SafeString(body, "body");
    result["target_commitish"] = SafeString(body, "target_commitish");
    return result.dump();
}

std::string HandleCreateRelease(const std::string& arguments_json) {
    const nlohmann::json args = nlohmann::json::parse(arguments_json, nullptr, false);
    std::string owner;
    std::string repo;
    if (!args.is_object() || !GetString(args, "owner", &owner)) {
        return MissingParam("owner");
    }
    if (!GetString(args, "repo", &repo)) {
        return MissingParam("repo");
    }
    std::string tag_name;
    if (!GetString(args, "tag_name", &tag_name)) {
        return MissingParam("tag_name");
    }

    TokenHandle handle;
    std::string failure;
    if (!RequireToken(&handle, &failure)) {
        return failure;
    }

    nlohmann::json payload = {{"tag_name", tag_name}};
    if (args.contains("name") && args["name"].is_string()) {
        payload["name"] = args["name"];
    }
    if (args.contains("body") && args["body"].is_string()) {
        payload["body"] = args["body"];
    }
    if (args.contains("draft") && args["draft"].is_boolean()) {
        payload["draft"] = args["draft"];
    }
    if (args.contains("prerelease") && args["prerelease"].is_boolean()) {
        payload["prerelease"] = args["prerelease"];
    }
    if (args.contains("target_commitish") && args["target_commitish"].is_string()) {
        payload["target_commitish"] = args["target_commitish"];
    }

    const GitHubResponse response = GitHubClient::Get()->AuthorizedPost(RepoPath(owner, repo) + "/releases", payload.dump(), handle.authorization_header);
    if (!response.ok) {
        return FailureJson(response).dump();
    }

    nlohmann::json body;
    if (!ParseObject(response, &body, &failure)) {
        return failure;
    }
    return nlohmann::json{{"ok", true}, {"id", body.value("id", static_cast<long long>(0))}, {"html_url", body.value("html_url", "")}}.dump();
}

std::string HandleUpdateRelease(const std::string& arguments_json) {
    const nlohmann::json args = nlohmann::json::parse(arguments_json, nullptr, false);
    std::string owner;
    std::string repo;
    if (!args.is_object() || !GetString(args, "owner", &owner)) {
        return MissingParam("owner");
    }
    if (!GetString(args, "repo", &repo)) {
        return MissingParam("repo");
    }
    int id = 0;
    if (!GetOptionalInt(args, "id", &id) || id <= 0) {
        return MissingParam("id");
    }

    TokenHandle handle;
    std::string failure;
    if (!RequireToken(&handle, &failure)) {
        return failure;
    }

    nlohmann::json payload = nlohmann::json::object();
    if (args.contains("tag_name") && args["tag_name"].is_string()) {
        payload["tag_name"] = args["tag_name"];
    }
    if (args.contains("name") && args["name"].is_string()) {
        payload["name"] = args["name"];
    }
    if (args.contains("body") && args["body"].is_string()) {
        payload["body"] = args["body"];
    }
    if (args.contains("draft") && args["draft"].is_boolean()) {
        payload["draft"] = args["draft"];
    }
    if (args.contains("prerelease") && args["prerelease"].is_boolean()) {
        payload["prerelease"] = args["prerelease"];
    }
    if (payload.empty()) {
        return nlohmann::json{{"ok", false}, {"error_kind", "invalid_params"}, {"message", "没有可更新的字段"}}.dump();
    }

    const GitHubResponse response = GitHubClient::Get()->AuthorizedPatch(RepoPath(owner, repo) + "/releases/" + std::to_string(id), payload.dump(), handle.authorization_header);
    if (!response.ok) {
        return FailureJson(response).dump();
    }

    nlohmann::json body;
    if (!ParseObject(response, &body, &failure)) {
        return failure;
    }
    return nlohmann::json{{"ok", true}, {"id", body.value("id", static_cast<long long>(0))}, {"tag_name", SafeString(body, "tag_name")}}.dump();
}

// ===== GraphQL =====

std::string HandleGraphQL(const std::string& arguments_json) {
    const nlohmann::json args = nlohmann::json::parse(arguments_json, nullptr, false);
    std::string query;
    if (!args.is_object() || !GetString(args, "query", &query)) {
        return MissingParam("query");
    }

    TokenHandle handle;
    std::string failure;
    if (!RequireToken(&handle, &failure)) {
        return failure;
    }

    nlohmann::json payload = {{"query", query}};
    if (args.contains("variables") && args["variables"].is_object()) {
        payload["variables"] = args["variables"];
    }

    const GitHubResponse response = GitHubClient::Get()->AuthorizedPost("/graphql", payload.dump(), handle.authorization_header);

    // GraphQL 出错时常返回 200，错误放在响应体的 errors 里，必须单独判断，
    // 否则会把查询失败当成成功返回给调用方。
    const nlohmann::json body = nlohmann::json::parse(response.body, nullptr, false);
    const bool has_errors = body.is_object() && body.contains("errors") && body["errors"].is_array() && !body["errors"].empty();

    if (!response.ok) {
        nlohmann::json failure_json = FailureJson(response);
        if (body.is_object() && body.contains("errors")) {
            failure_json["graphql_errors"] = body["errors"];
        }
        return failure_json.dump();
    }
    if (has_errors) {
        return nlohmann::json{
            {"ok", false},
            {"error_kind", "graphql_error"},
            {"graphql_errors", body["errors"]},
            {"data", body.contains("data") ? body["data"] : nlohmann::json()},
        }.dump();
    }

    return nlohmann::json{{"ok", true}, {"data", body.contains("data") ? body["data"] : nlohmann::json()}}.dump();
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

    // ===== Repository =====
    const char* repoSchema = R"({"type":"object","properties":{"owner":{"type":"string"},"repo":{"type":"string"}},"required":["owner","repo"],"additionalProperties":false})";

    ToolRegistration search;
    search.tool.name = "github_search_repositories";
    search.tool.title = "Search repositories";
    search.tool.description = "按关键词搜索 GitHub 仓库。返回仓库全名、描述、可见性、星标数。";
    search.tool.input_schema_json = R"({"type":"object","properties":{"query":{"type":"string","description":"GitHub 搜索语法，如 \"bridge language:c++\""}},"required":["query"],"additionalProperties":false})";
    search.tool.annotations.read_only_hint = true;
    search.tool.annotations.idempotent_hint = true;
    search.tool.annotations.open_world_hint = true;
    search.tool.safety.kind = OperationKind::Read;
    search.tool.safety.permission_key = "read";
    search.handler = &HandleSearchRepositories;
    registry->Register(search);

    ToolRegistration getRepo;
    getRepo.tool.name = "github_get_repository";
    getRepo.tool.title = "Get repository";
    getRepo.tool.description = "查询单个仓库的详情，包括默认分支、星标、issue 数。";
    getRepo.tool.input_schema_json = repoSchema;
    getRepo.tool.annotations.read_only_hint = true;
    getRepo.tool.annotations.idempotent_hint = true;
    getRepo.tool.annotations.open_world_hint = true;
    getRepo.tool.safety.kind = OperationKind::Read;
    getRepo.tool.safety.permission_key = "read";
    getRepo.handler = &HandleGetRepository;
    registry->Register(getRepo);

    ToolRegistration createRepo;
    createRepo.tool.name = "github_create_repository";
    createRepo.tool.title = "Create repository";
    createRepo.tool.description = "在当前账号下创建仓库。默认不初始化 README，需要仓库有初始提交时传 auto_init。";
    createRepo.tool.input_schema_json = R"({"type":"object","properties":{"name":{"type":"string"},"description":{"type":"string"},"private":{"type":"boolean"},"auto_init":{"type":"boolean"}},"required":["name"],"additionalProperties":false})";
    createRepo.tool.annotations.read_only_hint = false;
    createRepo.tool.annotations.destructive_hint = false;
    createRepo.tool.annotations.idempotent_hint = false;
    createRepo.tool.annotations.open_world_hint = true;
    createRepo.tool.safety.kind = OperationKind::Create;
    createRepo.tool.safety.permission_key = "create";
    createRepo.handler = &HandleCreateRepository;
    registry->Register(createRepo);

    ToolRegistration updateRepo;
    updateRepo.tool.name = "github_update_repository";
    updateRepo.tool.title = "Update repository";
    updateRepo.tool.description = "修改仓库的描述、可见性、名称或默认分支。只更新显式给出的字段。";
    updateRepo.tool.input_schema_json = R"({"type":"object","properties":{"owner":{"type":"string"},"repo":{"type":"string"},"description":{"type":"string"},"private":{"type":"boolean"},"name":{"type":"string"},"default_branch":{"type":"string"}},"required":["owner","repo"],"additionalProperties":false})";
    updateRepo.tool.annotations.read_only_hint = false;
    updateRepo.tool.annotations.destructive_hint = false;
    updateRepo.tool.annotations.idempotent_hint = true;
    updateRepo.tool.annotations.open_world_hint = true;
    updateRepo.tool.safety.kind = OperationKind::Write;
    updateRepo.tool.safety.permission_key = "write";
    updateRepo.handler = &HandleUpdateRepository;
    registry->Register(updateRepo);

    ToolRegistration deleteRepo;
    deleteRepo.tool.name = "github_delete_repository";
    deleteRepo.tool.title = "Delete repository";
    deleteRepo.tool.description = "永久删除仓库，不可恢复。默认被权限配置禁止，且需要运行时二次确认。";
    deleteRepo.tool.input_schema_json = repoSchema;
    deleteRepo.tool.annotations.read_only_hint = false;
    deleteRepo.tool.annotations.destructive_hint = true;
    deleteRepo.tool.annotations.idempotent_hint = false;
    deleteRepo.tool.annotations.open_world_hint = true;
    deleteRepo.tool.safety.kind = OperationKind::Delete;
    deleteRepo.tool.safety.requires_confirmation = true;
    deleteRepo.tool.safety.permission_key = "delete_repository";
    deleteRepo.handler = &HandleDeleteRepository;
    registry->Register(deleteRepo);

    // ===== Files =====
    ToolRegistration getFile;
    getFile.tool.name = "github_get_file";
    getFile.tool.title = "Get file metadata";
    getFile.tool.description = "查询仓库中某个文件的元数据（sha、大小）。不返回正文，正文用 github_get_file_content。";
    getFile.tool.input_schema_json = R"({"type":"object","properties":{"owner":{"type":"string"},"repo":{"type":"string"},"path":{"type":"string"},"ref":{"type":"string","description":"分支、标签或提交 SHA"}},"required":["owner","repo","path"],"additionalProperties":false})";
    getFile.tool.annotations.read_only_hint = true;
    getFile.tool.annotations.idempotent_hint = true;
    getFile.tool.annotations.open_world_hint = true;
    getFile.tool.safety.kind = OperationKind::Read;
    getFile.tool.safety.permission_key = "read";
    getFile.handler = &HandleGetFile;
    registry->Register(getFile);

    ToolRegistration getContent;
    getContent.tool.name = "github_get_file_content";
    getContent.tool.title = "Get file content";
    getContent.tool.description = "读取仓库文件正文（自动解码 base64）。同时返回 sha，更新或删除文件时要用。";
    getContent.tool.input_schema_json = R"({"type":"object","properties":{"owner":{"type":"string"},"repo":{"type":"string"},"path":{"type":"string"},"ref":{"type":"string"}},"required":["owner","repo","path"],"additionalProperties":false})";
    getContent.tool.annotations.read_only_hint = true;
    getContent.tool.annotations.idempotent_hint = true;
    getContent.tool.annotations.open_world_hint = true;
    getContent.tool.safety.kind = OperationKind::Read;
    getContent.tool.safety.permission_key = "read";
    getContent.handler = &HandleGetFileContent;
    registry->Register(getContent);

    ToolRegistration putFile;
    putFile.tool.name = "github_create_or_update_file";
    putFile.tool.title = "Create or update file";
    putFile.tool.description = "创建或更新仓库文件。更新已有文件时必须提供该文件当前的 sha，否则 GitHub 会拒绝。";
    putFile.tool.input_schema_json = R"({"type":"object","properties":{"owner":{"type":"string"},"repo":{"type":"string"},"path":{"type":"string"},"content":{"type":"string"},"message":{"type":"string"},"branch":{"type":"string"},"sha":{"type":"string","description":"更新已有文件时必填"}},"required":["owner","repo","path","content"],"additionalProperties":false})";
    putFile.tool.annotations.read_only_hint = false;
    putFile.tool.annotations.destructive_hint = false;
    putFile.tool.annotations.idempotent_hint = false;
    putFile.tool.annotations.open_world_hint = true;
    putFile.tool.safety.kind = OperationKind::Write;
    putFile.tool.safety.permission_key = "write";
    putFile.handler = &HandleCreateOrUpdateFile;
    registry->Register(putFile);

    ToolRegistration deleteFile;
    deleteFile.tool.name = "github_delete_file";
    deleteFile.tool.title = "Delete file";
    deleteFile.tool.description = "删除仓库文件。必须提供文件当前 sha 作为并发保护。不可恢复，需要运行时二次确认。";
    deleteFile.tool.input_schema_json = R"({"type":"object","properties":{"owner":{"type":"string"},"repo":{"type":"string"},"path":{"type":"string"},"sha":{"type":"string"},"message":{"type":"string"},"branch":{"type":"string"}},"required":["owner","repo","path","sha"],"additionalProperties":false})";
    deleteFile.tool.annotations.read_only_hint = false;
    deleteFile.tool.annotations.destructive_hint = true;
    deleteFile.tool.annotations.idempotent_hint = false;
    deleteFile.tool.annotations.open_world_hint = true;
    deleteFile.tool.safety.kind = OperationKind::Delete;
    deleteFile.tool.safety.requires_confirmation = true;
    deleteFile.tool.safety.permission_key = "write";
    deleteFile.handler = &HandleDeleteFile;
    registry->Register(deleteFile);
    // ===== Issues =====
    const char* issueSchema = R"({"type":"object","properties":{"owner":{"type":"string"},"repo":{"type":"string"},"number":{"type":"integer"}},"required":["owner","repo","number"],"additionalProperties":false})";

    ToolRegistration listIssues;
    listIssues.tool.name = "github_list_issues";
    listIssues.tool.title = "List issues";
    listIssues.tool.description = "列出仓库 Issue。GitHub 此接口也会返回 PR，用 is_pull_request 区分。";
    listIssues.tool.input_schema_json = R"({"type":"object","properties":{"owner":{"type":"string"},"repo":{"type":"string"},"state":{"type":"string","enum":["open","closed","all"]}},"required":["owner","repo"],"additionalProperties":false})";
    listIssues.tool.annotations.read_only_hint = true;
    listIssues.tool.annotations.idempotent_hint = true;
    listIssues.tool.annotations.open_world_hint = true;
    listIssues.tool.safety.kind = OperationKind::Read;
    listIssues.tool.safety.permission_key = "read";
    listIssues.handler = &HandleListIssues;
    registry->Register(listIssues);

    ToolRegistration getIssue;
    getIssue.tool.name = "github_get_issue";
    getIssue.tool.title = "Get issue";
    getIssue.tool.description = "查询单个 Issue 的详情与正文。";
    getIssue.tool.input_schema_json = issueSchema;
    getIssue.tool.annotations.read_only_hint = true;
    getIssue.tool.annotations.idempotent_hint = true;
    getIssue.tool.annotations.open_world_hint = true;
    getIssue.tool.safety.kind = OperationKind::Read;
    getIssue.tool.safety.permission_key = "read";
    getIssue.handler = &HandleGetIssue;
    registry->Register(getIssue);

    ToolRegistration createIssue;
    createIssue.tool.name = "github_create_issue";
    createIssue.tool.title = "Create issue";
    createIssue.tool.description = "创建 Issue，可选正文、标签与指派人。";
    createIssue.tool.input_schema_json = R"({"type":"object","properties":{"owner":{"type":"string"},"repo":{"type":"string"},"title":{"type":"string"},"body":{"type":"string"},"labels":{"type":"array","items":{"type":"string"}},"assignees":{"type":"array","items":{"type":"string"}}},"required":["owner","repo","title"],"additionalProperties":false})";
    createIssue.tool.annotations.read_only_hint = false;
    createIssue.tool.annotations.destructive_hint = false;
    createIssue.tool.annotations.idempotent_hint = false;
    createIssue.tool.annotations.open_world_hint = true;
    createIssue.tool.safety.kind = OperationKind::Create;
    createIssue.tool.safety.permission_key = "create";
    createIssue.handler = &HandleCreateIssue;
    registry->Register(createIssue);

    ToolRegistration updateIssue;
    updateIssue.tool.name = "github_update_issue";
    updateIssue.tool.title = "Update issue";
    updateIssue.tool.description = "修改 Issue 的标题、正文或状态。只更新显式给出的字段。";
    updateIssue.tool.input_schema_json = R"({"type":"object","properties":{"owner":{"type":"string"},"repo":{"type":"string"},"number":{"type":"integer"},"title":{"type":"string"},"body":{"type":"string"},"state":{"type":"string","enum":["open","closed"]}},"required":["owner","repo","number"],"additionalProperties":false})";
    updateIssue.tool.annotations.read_only_hint = false;
    updateIssue.tool.annotations.destructive_hint = false;
    updateIssue.tool.annotations.idempotent_hint = true;
    updateIssue.tool.annotations.open_world_hint = true;
    updateIssue.tool.safety.kind = OperationKind::Write;
    updateIssue.tool.safety.permission_key = "write";
    updateIssue.handler = &HandleUpdateIssue;
    registry->Register(updateIssue);

    ToolRegistration closeIssue;
    closeIssue.tool.name = "github_close_issue";
    closeIssue.tool.title = "Close issue";
    closeIssue.tool.description = "关闭 Issue，可附带 state_reason。";
    closeIssue.tool.input_schema_json = R"({"type":"object","properties":{"owner":{"type":"string"},"repo":{"type":"string"},"number":{"type":"integer"},"state_reason":{"type":"string","enum":["completed","not_planned","reopened"]}},"required":["owner","repo","number"],"additionalProperties":false})";
    closeIssue.tool.annotations.read_only_hint = false;
    closeIssue.tool.annotations.destructive_hint = false;
    closeIssue.tool.annotations.idempotent_hint = true;
    closeIssue.tool.annotations.open_world_hint = true;
    closeIssue.tool.safety.kind = OperationKind::Write;
    closeIssue.tool.safety.permission_key = "write";
    closeIssue.handler = &HandleCloseIssue;
    registry->Register(closeIssue);

    // ===== Branches =====
    ToolRegistration listBranches;
    listBranches.tool.name = "github_list_branches";
    listBranches.tool.title = "List branches";
    listBranches.tool.description = "列出仓库所有分支及其最新提交 SHA。";
    listBranches.tool.input_schema_json = repoSchema;
    listBranches.tool.annotations.read_only_hint = true;
    listBranches.tool.annotations.idempotent_hint = true;
    listBranches.tool.annotations.open_world_hint = true;
    listBranches.tool.safety.kind = OperationKind::Read;
    listBranches.tool.safety.permission_key = "read";
    listBranches.handler = &HandleListBranches;
    registry->Register(listBranches);

    ToolRegistration getBranch;
    getBranch.tool.name = "github_get_branch";
    getBranch.tool.title = "Get branch";
    getBranch.tool.description = "查询单个分支的提交 SHA 与保护状态。";
    getBranch.tool.input_schema_json = R"({"type":"object","properties":{"owner":{"type":"string"},"repo":{"type":"string"},"branch":{"type":"string"}},"required":["owner","repo","branch"],"additionalProperties":false})";
    getBranch.tool.annotations.read_only_hint = true;
    getBranch.tool.annotations.idempotent_hint = true;
    getBranch.tool.annotations.open_world_hint = true;
    getBranch.tool.safety.kind = OperationKind::Read;
    getBranch.tool.safety.permission_key = "read";
    getBranch.handler = &HandleGetBranch;
    registry->Register(getBranch);

    ToolRegistration createBranch;
    createBranch.tool.name = "github_create_branch";
    createBranch.tool.title = "Create branch";
    createBranch.tool.description = "从指定提交创建分支。from_sha 必须明确给出，可先用 github_get_branch 取。";
    createBranch.tool.input_schema_json = R"({"type":"object","properties":{"owner":{"type":"string"},"repo":{"type":"string"},"branch":{"type":"string"},"from_sha":{"type":"string"}},"required":["owner","repo","branch","from_sha"],"additionalProperties":false})";
    createBranch.tool.annotations.read_only_hint = false;
    createBranch.tool.annotations.destructive_hint = false;
    createBranch.tool.annotations.idempotent_hint = false;
    createBranch.tool.annotations.open_world_hint = true;
    createBranch.tool.safety.kind = OperationKind::Create;
    createBranch.tool.safety.permission_key = "create";
    createBranch.handler = &HandleCreateBranch;
    registry->Register(createBranch);

    ToolRegistration deleteBranch;
    deleteBranch.tool.name = "github_delete_branch";
    deleteBranch.tool.title = "Delete branch";
    deleteBranch.tool.description = "删除分支，不可恢复。默认被权限配置禁止，且需要运行时二次确认。";
    deleteBranch.tool.input_schema_json = R"({"type":"object","properties":{"owner":{"type":"string"},"repo":{"type":"string"},"branch":{"type":"string"}},"required":["owner","repo","branch"],"additionalProperties":false})";
    deleteBranch.tool.annotations.read_only_hint = false;
    deleteBranch.tool.annotations.destructive_hint = true;
    deleteBranch.tool.annotations.idempotent_hint = false;
    deleteBranch.tool.annotations.open_world_hint = true;
    deleteBranch.tool.safety.kind = OperationKind::Delete;
    deleteBranch.tool.safety.requires_confirmation = true;
    deleteBranch.tool.safety.permission_key = "delete_branch";
    deleteBranch.handler = &HandleDeleteBranch;
    registry->Register(deleteBranch);

    // ===== Pull Requests =====
    ToolRegistration listPulls;
    listPulls.tool.name = "github_list_pull_requests";
    listPulls.tool.title = "List pull requests";
    listPulls.tool.description = "列出仓库的 Pull Request。";
    listPulls.tool.input_schema_json = R"({"type":"object","properties":{"owner":{"type":"string"},"repo":{"type":"string"},"state":{"type":"string","enum":["open","closed","all"]}},"required":["owner","repo"],"additionalProperties":false})";
    listPulls.tool.annotations.read_only_hint = true;
    listPulls.tool.annotations.idempotent_hint = true;
    listPulls.tool.annotations.open_world_hint = true;
    listPulls.tool.safety.kind = OperationKind::Read;
    listPulls.tool.safety.permission_key = "read";
    listPulls.handler = &HandleListPullRequests;
    registry->Register(listPulls);

    ToolRegistration getPull;
    getPull.tool.name = "github_get_pull_request";
    getPull.tool.title = "Get pull request";
    getPull.tool.description = "查询单个 PR 的详情，含可合并状态。";
    getPull.tool.input_schema_json = issueSchema;
    getPull.tool.annotations.read_only_hint = true;
    getPull.tool.annotations.idempotent_hint = true;
    getPull.tool.annotations.open_world_hint = true;
    getPull.tool.safety.kind = OperationKind::Read;
    getPull.tool.safety.permission_key = "read";
    getPull.handler = &HandleGetPullRequest;
    registry->Register(getPull);

    ToolRegistration createPull;
    createPull.tool.name = "github_create_pull_request";
    createPull.tool.title = "Create pull request";
    createPull.tool.description = "创建 PR，需要给出源分支 head 与目标分支 base。";
    createPull.tool.input_schema_json = R"({"type":"object","properties":{"owner":{"type":"string"},"repo":{"type":"string"},"title":{"type":"string"},"head":{"type":"string"},"base":{"type":"string"},"body":{"type":"string"},"draft":{"type":"boolean"}},"required":["owner","repo","title","head","base"],"additionalProperties":false})";
    createPull.tool.annotations.read_only_hint = false;
    createPull.tool.annotations.destructive_hint = false;
    createPull.tool.annotations.idempotent_hint = false;
    createPull.tool.annotations.open_world_hint = true;
    createPull.tool.safety.kind = OperationKind::Create;
    createPull.tool.safety.permission_key = "create";
    createPull.handler = &HandleCreatePullRequest;
    registry->Register(createPull);

    ToolRegistration updatePull;
    updatePull.tool.name = "github_update_pull_request";
    updatePull.tool.title = "Update pull request";
    updatePull.tool.description = "修改 PR 的标题、正文、状态或目标分支。";
    updatePull.tool.input_schema_json = R"({"type":"object","properties":{"owner":{"type":"string"},"repo":{"type":"string"},"number":{"type":"integer"},"title":{"type":"string"},"body":{"type":"string"},"state":{"type":"string","enum":["open","closed"]},"base":{"type":"string"}},"required":["owner","repo","number"],"additionalProperties":false})";
    updatePull.tool.annotations.read_only_hint = false;
    updatePull.tool.annotations.destructive_hint = false;
    updatePull.tool.annotations.idempotent_hint = true;
    updatePull.tool.annotations.open_world_hint = true;
    updatePull.tool.safety.kind = OperationKind::Write;
    updatePull.tool.safety.permission_key = "write";
    updatePull.handler = &HandleUpdatePullRequest;
    registry->Register(updatePull);

    ToolRegistration mergePull;
    mergePull.tool.name = "github_merge_pull_request";
    mergePull.tool.title = "Merge pull request";
    mergePull.tool.description = "合并 PR。merge_method 可选 merge/squash/rebase。难以回退，需要运行时二次确认。";
    mergePull.tool.input_schema_json = R"({"type":"object","properties":{"owner":{"type":"string"},"repo":{"type":"string"},"number":{"type":"integer"},"commit_title":{"type":"string"},"merge_method":{"type":"string","enum":["merge","squash","rebase"]}},"required":["owner","repo","number"],"additionalProperties":false})";
    mergePull.tool.annotations.read_only_hint = false;
    mergePull.tool.annotations.destructive_hint = true;
    mergePull.tool.annotations.idempotent_hint = false;
    mergePull.tool.annotations.open_world_hint = true;
    mergePull.tool.safety.kind = OperationKind::Write;
    mergePull.tool.safety.requires_confirmation = true;
    mergePull.tool.safety.permission_key = "merge_pull_request";
    mergePull.handler = &HandleMergePullRequest;
    registry->Register(mergePull);

    // ===== Releases =====
    ToolRegistration listReleases;
    listReleases.tool.name = "github_list_releases";
    listReleases.tool.title = "List releases";
    listReleases.tool.description = "列出仓库的 Release。";
    listReleases.tool.input_schema_json = repoSchema;
    listReleases.tool.annotations.read_only_hint = true;
    listReleases.tool.annotations.idempotent_hint = true;
    listReleases.tool.annotations.open_world_hint = true;
    listReleases.tool.safety.kind = OperationKind::Read;
    listReleases.tool.safety.permission_key = "read";
    listReleases.handler = &HandleListReleases;
    registry->Register(listReleases);

    ToolRegistration getRelease;
    getRelease.tool.name = "github_get_release";
    getRelease.tool.title = "Get release";
    getRelease.tool.description = "查询单个 Release 的详情与说明正文。";
    getRelease.tool.input_schema_json = R"({"type":"object","properties":{"owner":{"type":"string"},"repo":{"type":"string"},"id":{"type":"integer"}},"required":["owner","repo","id"],"additionalProperties":false})";
    getRelease.tool.annotations.read_only_hint = true;
    getRelease.tool.annotations.idempotent_hint = true;
    getRelease.tool.annotations.open_world_hint = true;
    getRelease.tool.safety.kind = OperationKind::Read;
    getRelease.tool.safety.permission_key = "read";
    getRelease.handler = &HandleGetRelease;
    registry->Register(getRelease);

    ToolRegistration createRelease;
    createRelease.tool.name = "github_create_release";
    createRelease.tool.title = "Create release";
    createRelease.tool.description = "创建 Release，tag_name 必填；draft 为 true 时先存为草稿。";
    createRelease.tool.input_schema_json = R"({"type":"object","properties":{"owner":{"type":"string"},"repo":{"type":"string"},"tag_name":{"type":"string"},"name":{"type":"string"},"body":{"type":"string"},"draft":{"type":"boolean"},"prerelease":{"type":"boolean"},"target_commitish":{"type":"string"}},"required":["owner","repo","tag_name"],"additionalProperties":false})";
    createRelease.tool.annotations.read_only_hint = false;
    createRelease.tool.annotations.destructive_hint = false;
    createRelease.tool.annotations.idempotent_hint = false;
    createRelease.tool.annotations.open_world_hint = true;
    createRelease.tool.safety.kind = OperationKind::Create;
    createRelease.tool.safety.permission_key = "create";
    createRelease.handler = &HandleCreateRelease;
    registry->Register(createRelease);

    ToolRegistration updateRelease;
    updateRelease.tool.name = "github_update_release";
    updateRelease.tool.title = "Update release";
    updateRelease.tool.description = "修改 Release。只更新显式给出的字段。";
    updateRelease.tool.input_schema_json = R"({"type":"object","properties":{"owner":{"type":"string"},"repo":{"type":"string"},"id":{"type":"integer"},"tag_name":{"type":"string"},"name":{"type":"string"},"body":{"type":"string"},"draft":{"type":"boolean"},"prerelease":{"type":"boolean"}},"required":["owner","repo","id"],"additionalProperties":false})";
    updateRelease.tool.annotations.read_only_hint = false;
    updateRelease.tool.annotations.destructive_hint = false;
    updateRelease.tool.annotations.idempotent_hint = true;
    updateRelease.tool.annotations.open_world_hint = true;
    updateRelease.tool.safety.kind = OperationKind::Write;
    updateRelease.tool.safety.permission_key = "write";
    updateRelease.handler = &HandleUpdateRelease;
    registry->Register(updateRelease);

    // ===== GraphQL =====
    ToolRegistration graphql;
    graphql.tool.name = "github_graphql";
    graphql.tool.title = "GraphQL query";
    graphql.tool.description = "执行 GraphQL 查询。注意 GitHub 出错时也可能返回 200，错误在响应的 errors 字段里。";
    graphql.tool.input_schema_json = R"({"type":"object","properties":{"query":{"type":"string"},"variables":{"type":"object"}},"required":["query"],"additionalProperties":false})";
    graphql.tool.annotations.read_only_hint = false;
    graphql.tool.annotations.destructive_hint = false;
    graphql.tool.annotations.idempotent_hint = false;
    graphql.tool.annotations.open_world_hint = true;
    graphql.tool.safety.kind = OperationKind::Read;
    graphql.tool.safety.permission_key = "read";
    // mutation 是写操作，必须按 query 内容改判成 write，否则能绕过写开关。
    graphql.tool.safety.resolve_permission_key = &ResolveGraphQLPermissionKey;
    graphql.handler = &HandleGraphQL;
    registry->Register(graphql);
}

} // namespace ghb
