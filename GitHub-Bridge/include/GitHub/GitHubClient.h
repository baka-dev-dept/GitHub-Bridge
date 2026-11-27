#pragma once

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
    GitHubResponse Request(const std::string& method, const std::string& path,
                           const std::string& body, const std::string& authorization);

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

private:
    GitHubClient() = default;
    ~GitHubClient() = default;

    static GitHubResponse Classify(int status, const std::string& body,
                                   const std::map<std::string, std::string>& headers);
};

} // namespace ghb
