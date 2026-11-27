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

// HTTP 传输层接口。只负责把一次请求发出去并收回响应，不理解 GitHub 语义。
//
// 四个已知坑在做具体实现时都要处理，缺一不可：
//   1. 压缩   —— 不自动解压的客户端（WinHTTP）要显式要求 identity
//   2. TLS    —— 显式指定最低版本，不依赖系统默认
//   3. 重定向 —— 禁止自动跟随，否则 Authorization 会被带到别的主机
//   4. 超时   —— 必须设置，否则网络异常会导致进程无限挂起
class IHttpTransport {
public:
    virtual ~IHttpTransport() = default;

    virtual HttpResponse Send(const HttpRequest& request) const = 0;

    // 按平台返回实现：Windows 是 WinHTTP，Linux 是 libcurl。
    // 返回的指针归调用方所有；调用方会把它放进单例长期持有。
    static IHttpTransport* Create();

    // 响应体上限。超出部分截断并记日志，避免异常响应撑爆内存。
    static constexpr size_t kMaxBodyBytes = 10u * 1024u * 1024u;

    // 目标主机。固定在实现里，调用方不能改，避免被构造出任意主机请求。
    static constexpr const char* kHost = "api.github.com";
};

} // namespace ghb
