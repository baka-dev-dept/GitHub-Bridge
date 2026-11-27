#include "Platform/HttpTransport.h"

#include "Utils/Logger.h"

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <mutex>
#include <string>

namespace ghb {

namespace {

std::string ToLower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

// curl 全局初始化只做一次。libcurl 不是线程安全的初始化方，用 call_once 兜住。
void EnsureCurlGlobal() {
    static std::once_flag once;
    std::call_once(once, []() { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

size_t OnWriteBody(char* data, size_t size, size_t count, void* user) {
    auto* out = static_cast<HttpResponse*>(user);
    const size_t total = size * count;
    // 超过上限就截断并停止接收，避免异常响应耗尽内存。
    if (out->body.size() + total > IHttpTransport::kMaxBodyBytes) {
        const size_t room = IHttpTransport::kMaxBodyBytes - out->body.size();
        out->body.append(data, room);
        return 0;  // 返回 0 让 curl 中止传输
    }
    out->body.append(data, total);
    return total;
}

size_t OnHeaderLine(char* data, size_t size, size_t count, void* user) {
    auto* out = static_cast<HttpResponse*>(user);
    const size_t total = size * count;
    std::string line(data, total);

    // 响应头块以空行结束
    if (line == "\r\n" || line == "\n") {
        return total;
    }
    const size_t colon = line.find(':');
    if (colon == std::string::npos) {
        return total;  // 状态行等，跳过
    }
    std::string name = ToLower(line.substr(0, colon));
    std::string value = line.substr(colon + 1);
    const size_t first = value.find_first_not_of(" \t");
    const size_t last = value.find_last_not_of(" \t\r\n");
    value = (first == std::string::npos) ? std::string() : value.substr(first, last - first + 1);
    out->headers[name] = value;
    return total;
}

class CurlTransport : public IHttpTransport {
public:
    HttpResponse Send(const HttpRequest& request) const override;
};

HttpResponse CurlTransport::Send(const HttpRequest& request) const {
    EnsureCurlGlobal();

    HttpResponse response;
    CURL* curl = curl_easy_init();
    if (curl == nullptr) {
        response.transport_error = "curl_easy_init 失败";
        return response;
    }

    const std::string url = std::string("https://") + kHost + request.path;

    // 坑 4：必须设超时，否则网络异常时进程会一直挂着，stdio 服务等于死掉。
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);

    // 坑 2：显式要求 TLS 1.2 及以上，不依赖系统默认协商结果。
    curl_easy_setopt(curl, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);

    // 坑 3：禁止自动重定向。默认跟随会把 Authorization 头带到重定向目标主机，
    // 这是凭据泄露，不是可用性问题。
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);

    // 坑 1：WinHTTP 需要 identity，而 curl 自己会解压。这里传空串让它启用
    // 全部支持的编码并自动解码，比手写 identity 更省带宽。行为与 Windows 端
    // 的「拿到的是明文正文」这一契约一致。
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, OnWriteBody);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, OnHeaderLine);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response);

    curl_slist* header_list = nullptr;
    for (const auto& [name, value] : request.headers) {
        header_list = curl_slist_append(header_list, (name + ": " + value).c_str());
    }
    if (header_list != nullptr) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
    }

    if (request.method == "POST") {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request.body.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(request.body.size()));
    } else if (request.method == "PUT") {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request.body.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(request.body.size()));
    } else if (request.method == "PATCH") {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PATCH");
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request.body.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(request.body.size()));
    } else if (request.method == "DELETE") {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
        if (!request.body.empty()) {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request.body.c_str());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(request.body.size()));
        }
    } else {
        curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    }

    const CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        response.transport_error = std::string("curl 请求失败: ") + curl_easy_strerror(rc);
        curl_easy_cleanup(curl);
        if (header_list != nullptr) {
            curl_slist_free_all(header_list);
        }
        return response;
    }

    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    response.status = static_cast<int>(status);

    if (response.body.size() >= kMaxBodyBytes) {
        Logger::Get()->Warn("响应体超过 10MiB 上限，已截断: " + request.path);
    }

    curl_easy_cleanup(curl);
    if (header_list != nullptr) {
        curl_slist_free_all(header_list);
    }
    return response;
}

} // namespace

IHttpTransport* IHttpTransport::Create() {
    return new CurlTransport();
}

} // namespace ghb
