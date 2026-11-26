#pragma once

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace ghb {

struct HttpRequest {
    std::string method = "GET";
    // 以 '/' 开头的路径，例如 "/user"。
    std::string path;
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;
};

struct HttpResponse {
    int status = 0;
    std::string body;
    // 头名统一小写存放，查询时不必关心服务端大小写。
    std::map<std::string, std::string> headers;
    // 非空表示传输层失败，此时 status 为 0，body 无意义。
    std::string transport_error;

    bool TransportFailed() const { return !transport_error.empty(); }
};

// WinHTTP 传输层。只负责把一次请求发出去并收回响应，不理解 GitHub 语义。
//
// 四个已知坑都在这里处理，缺一不可：
//   1. gzip     —— WinHTTP 不自动解压，请求时显式要求 identity
//   2. TLS      —— 显式指定 1.2/1.3，不依赖系统默认
//   3. 重定向   —— 禁止自动跟随，否则 Authorization 会被带到别的主机
//   4. 超时     —— 必须设置，否则网络异常会导致进程无限挂起
class WinHttpClient {
public:
    static WinHttpClient* Get() {
        static WinHttpClient i;
        return &i;
    }

    HttpResponse Send(const HttpRequest& request) const;

    // 响应体上限。超出部分截断并记日志，避免异常响应撑爆内存。
    static constexpr size_t kMaxBodyBytes = 10u * 1024u * 1024u;

    // 目标主机。固定在客户端里，调用方不能改，避免被构造出任意主机请求。
    static constexpr const char* kHost = "api.github.com";

private:
    WinHttpClient() = default;
    ~WinHttpClient() = default;
};

} // namespace ghb
