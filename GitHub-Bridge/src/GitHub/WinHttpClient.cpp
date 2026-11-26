#include "GitHub/WinHttpClient.h"

#include "Utils/Logger.h"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cctype>

namespace ghb {

namespace {

std::wstring ToWide(const std::string& utf8) {
    if (utf8.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (size <= 0) {
        return {};
    }
    std::wstring out(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, out.data(), size);
    out.resize(static_cast<size_t>(size) - 1);
    return out;
}

std::string ToUtf8(const std::wstring& wide) {
    if (wide.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return {};
    }
    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, out.data(), size, nullptr, nullptr);
    out.resize(static_cast<size_t>(size) - 1);
    return out;
}

std::string ToLower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

// WinHTTP 句柄必须每个都关，漏一个就是句柄泄漏。用 RAII 兜住全部退出路径。
class InternetHandle {
public:
    InternetHandle() = default;
    explicit InternetHandle(HINTERNET handle) : handle_(handle) {}
    ~InternetHandle() { Close(); }

    InternetHandle(const InternetHandle&) = delete;
    InternetHandle& operator=(const InternetHandle&) = delete;

    InternetHandle(InternetHandle&& other) noexcept : handle_(other.handle_) { other.handle_ = nullptr; }
    InternetHandle& operator=(InternetHandle&& other) noexcept {
        if (this != &other) {
            Close();
            handle_ = other.handle_;
            other.handle_ = nullptr;
        }
        return *this;
    }

    HINTERNET Get() const { return handle_; }
    explicit operator bool() const { return handle_ != nullptr; }

    void Close() {
        if (handle_ != nullptr) {
            WinHttpCloseHandle(handle_);
            handle_ = nullptr;
        }
    }

private:
    HINTERNET handle_ = nullptr;
};

} // namespace

HttpResponse WinHttpClient::Send(const HttpRequest& request) const {
    HttpResponse response;

    InternetHandle session(WinHttpOpen(L"GitHub-Bridge/0.1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                       WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) {
        response.transport_error = "WinHttpOpen 失败，错误码 " + std::to_string(GetLastError());
        return response;
    }

    // 坑 4：必须设超时，否则网络异常时进程会一直挂着，stdio 服务等于死掉。
    WinHttpSetTimeouts(session.Get(), 10000, 10000, 30000, 60000);

    const std::wstring host = ToWide(kHost);
    InternetHandle connection(WinHttpConnect(session.Get(), host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0));
    if (!connection) {
        response.transport_error = "WinHttpConnect 失败，错误码 " + std::to_string(GetLastError());
        return response;
    }

    const std::wstring path = ToWide(request.path);
    const std::wstring method = ToWide(request.method);
    InternetHandle handle(WinHttpOpenRequest(connection.Get(), method.c_str(), path.c_str(), nullptr,
                                             WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                             WINHTTP_FLAG_SECURE));
    if (!handle) {
        response.transport_error = "WinHttpOpenRequest 失败，错误码 " + std::to_string(GetLastError());
        return response;
    }

    // 坑 2：显式指定 TLS 1.2/1.3，不依赖系统默认协商结果。
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
    WinHttpSetOption(handle.Get(), WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));

    // 坑 3：禁止自动重定向。默认跟随会把 Authorization 头带到重定向目标主机，
    // 这是凭据泄露，不是可用性问题。
    DWORD redirect_policy = WINHTTP_DISABLE_REDIRECTS;
    WinHttpSetOption(handle.Get(), WINHTTP_OPTION_DISABLE_FEATURE, &redirect_policy, sizeof(redirect_policy));

    // 坑 1：WinHTTP 不会自动解压 gzip。GitHub 默认返回 gzip，
    // 不处理会导致 JSON 解析失败，因此明确要求服务器返回未压缩内容。
    std::wstring header_block = L"Accept-Encoding: identity\r\n";
    for (const auto& [name, value] : request.headers) {
        header_block += ToWide(name) + L": " + ToWide(value) + L"\r\n";
    }

    void* body = request.body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(request.body.data());
    const DWORD body_size = static_cast<DWORD>(request.body.size());

    if (!WinHttpSendRequest(handle.Get(), header_block.c_str(), static_cast<DWORD>(-1L), body, body_size, body_size, 0)) {
        response.transport_error = "WinHttpSendRequest 失败，错误码 " + std::to_string(GetLastError());
        return response;
    }
    if (!WinHttpReceiveResponse(handle.Get(), nullptr)) {
        response.transport_error = "WinHttpReceiveResponse 失败，错误码 " + std::to_string(GetLastError());
        return response;
    }

    DWORD status_code = 0;
    DWORD status_size = sizeof(status_code);
    if (!WinHttpQueryHeaders(handle.Get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status_code, &status_size, WINHTTP_NO_HEADER_INDEX)) {
        response.transport_error = "读取状态码失败，错误码 " + std::to_string(GetLastError());
        return response;
    }
    response.status = static_cast<int>(status_code);

    // 一次遍历把所有响应头收进来，后续按名查询走内存，避免重复 IPC。
    DWORD header_size = 0;
    WinHttpQueryHeaders(handle.Get(), WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX,
                        WINHTTP_NO_OUTPUT_BUFFER, &header_size, WINHTTP_NO_HEADER_INDEX);
    if (header_size > 0) {
        std::wstring raw(header_size / sizeof(wchar_t) + 1, L'\0');
        if (WinHttpQueryHeaders(handle.Get(), WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX,
                                raw.data(), &header_size, WINHTTP_NO_HEADER_INDEX)) {
            raw.resize(header_size / sizeof(wchar_t));
            while (!raw.empty() && raw.back() == L'\0') {
                raw.pop_back();
            }
            const std::string text = ToUtf8(raw);
            size_t pos = 0;
            while (pos < text.size()) {
                const size_t eol = text.find("\r\n", pos);
                const std::string line = text.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
                pos = (eol == std::string::npos) ? text.size() : eol + 2;
                const size_t colon = line.find(':');
                if (colon == std::string::npos) {
                    continue;
                }
                std::string name = ToLower(line.substr(0, colon));
                std::string value = line.substr(colon + 1);
                const size_t first = value.find_first_not_of(" \t");
                value = (first == std::string::npos) ? std::string() : value.substr(first);
                response.headers[name] = value;
            }
        }
    }

    // 逐块读取响应体，超过上限就停止，防止异常响应耗尽内存。
    bool truncated = false;
    while (true) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(handle.Get(), &available) || available == 0) {
            break;
        }
        std::string chunk(available, '\0');
        DWORD read = 0;
        if (!WinHttpReadData(handle.Get(), chunk.data(), available, &read) || read == 0) {
            break;
        }
        chunk.resize(read);
        if (response.body.size() + chunk.size() > kMaxBodyBytes) {
            const size_t room = kMaxBodyBytes - response.body.size();
            response.body.append(chunk, 0, room);
            truncated = true;
            break;
        }
        response.body += chunk;
    }

    if (truncated) {
        Logger::Get()->Warn("响应体超过 10MiB 上限，已截断: " + request.path);
    }
    return response;
}

} // namespace ghb
