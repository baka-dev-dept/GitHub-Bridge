#pragma once

#include <cstdint>
#include <string>

namespace ghb {

// TokenManager 对上层唯一的暴露面：只能拿到 Authorization 头的值和可用性，
// 拿不到明文。明文一律留在 TokenStorage 与 TokenManager 内部。
struct TokenHandle {
    bool configured = false;
    bool valid = false;
    std::string authorization_header;
};

struct TokenStatusSnapshot {
    bool configured = false;
    bool valid = false;
    std::string login;
    int64_t user_id = 0;
    std::string token_type;
    std::string expires_at;
    std::string scopes_csv;
};

struct TokenImportResult {
    bool ok = false;
    std::string message;
};

// 固定熵派生源。熵值不得以明文硬编码进源码，由该常量与另一处本地数据组合生成。
struct TokenStoragePaths {
    std::string data_file;
    std::string log_dir;
};

} // namespace ghb
