#pragma once

#include "GitHub/WinHttpClient.h"
#include "Types/GitHubResponse.h"

#include <map>
#include <string>

namespace ghb {

// GitHub REST API 语义层。负责拼标准请求头、把 HTTP 状态翻译成
// GitHubErrorKind、抽取响应头，并把正文透传给调用方。
//
// 令牌只在这里以 Authorization 头的形式出现一次，不缓存、不记录。
class GitHubClient {
public:
    static GitHubClient* Get() {
        static GitHubClient i;
        return &i;
    }

    // 通用请求。method 取 GET/POST/PUT/PATCH/DELETE；
    // body 为空时不发请求体，非空时按 JSON 发送。
    // authorization 为空表示匿名请求。
    GitHubResponse Request(const std::string& method, const std::string& path, const std::string& body, const std::string& authorization);

    // 便捷包装：GET 两个变体保留，避免已有调用点全部改写。
    GitHubResponse AnonymousGet(const std::string& path);
    GitHubResponse AuthorizedGet(const std::string& path, const std::string& authorization);

    // 带正文的写操作。DELETE 不带正文时用上面的 Request。
    GitHubResponse AuthorizedPost(const std::string& path, const std::string& body, const std::string& authorization);
    GitHubResponse AuthorizedPut(const std::string& path, const std::string& body, const std::string& authorization);
    GitHubResponse AuthorizedPatch(const std::string& path, const std::string& body, const std::string& authorization);
    GitHubResponse AuthorizedDelete(const std::string& path, const std::string& authorization);

    // /user 必须带令牌，否则 GitHub 一律返回 401。
    GitHubResponse GetUser(const std::string& authorization);
    // /rate_limit 匿名也可用；有令牌时额度更高，无令牌时也能验证链路是否通。
    GitHubResponse GetRateLimit();

    static constexpr const char* kApiVersion = "2022-11-28";
    static constexpr const char* kUserAgent = "GitHub-Bridge/0.1.0";

    // 暂时性故障的重试上限。MCP 调用是同步阻塞的，重试太多会把一次调用
    // 拖到几十秒，比直接失败还难受，所以只给两次。
    static constexpr int kMaxTransientRetries = 2;

    // 退避间隔：第一次 500ms，第二次 1500ms。
    static constexpr int kBackoffMs[] = {500, 1500};

    // 这个错误值不值得重发：只有传输层失败与服务端 5xx 是暂时性的。
    // 凭据、权限、参数类错误重发也不会变。纯函数，公开以便单元测试。
    static bool IsRetryable(GitHubErrorKind kind);

    // HTTP 状态到 error_kind 的翻译。纯函数，公开以便单元测试。
    static GitHubResponse Classify(int status, const std::string& body, const std::map<std::string, std::string>& headers);

private:
    GitHubClient() = default;
    ~GitHubClient() = default;

    // 发一次请求，不含限流等待与重试。重试逻辑在 Request 里，避免递归。
    // if_none_match 非空时作为条件请求发送。
    HttpResponse SendOnce(const std::string& method, const std::string& path, const std::string& body, const std::string& authorization, const std::string& if_none_match);
};

} // namespace ghb
