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
                {"description", item.value("description", "")},
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
        {"description", body.value("description", "")},
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
        {"description", body.value("description", "")},
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
        {"download_url", body.contains("download_url") && body["download_url"].is_string() ? body["download_url"] : ""},
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
}

} // namespace ghb
