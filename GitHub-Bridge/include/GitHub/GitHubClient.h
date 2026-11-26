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

    // 不带令牌。用于 /rate_limit 这类匿名可用的端点。
    GitHubResponse AnonymousGet(const std::string& path);

    // 带令牌。令牌缺失时由调用方预先判断，这里要求传入已组装好的头值。
    GitHubResponse AuthorizedGet(const std::string& path, const std::string& authorization);

    // /user 必须带令牌，否则 GitHub 一律返回 401。
    GitHubResponse GetUser(const std::string& authorization);
    // /rate_limit 匿名也可用；有令牌时额度更高，无令牌时也能验证链路是否通。
    GitHubResponse GetRateLimit();

    static constexpr const char* kApiVersion = "2022-11-28";
    static constexpr const char* kUserAgent = "GitHub-Bridge/0.1.0";

private:
    GitHubClient() = default;
    ~GitHubClient() = default;

    GitHubResponse Perform(const std::string& path, const std::string& authorization);
    static GitHubResponse Classify(int status, const std::string& body,
                                   const std::map<std::string, std::string>& headers);
};

} // namespace ghb
