#include "Security/TokenManager.h"

#include "Utils/Logger.h"

#include <windows.h>

#include <utility>

namespace ghb {

namespace {

// 令牌格式与长度上下限。上限远小于任何真实令牌，只用于拦截误传整段文件。
constexpr size_t kMinTokenLength = 20;
constexpr size_t kMaxTokenLength = 1024;

bool HasPrefix(const std::string& text, const char* prefix) {
    const size_t len = std::char_traits<char>::length(prefix);
    return text.size() >= len && text.compare(0, len, prefix) == 0;
}

bool LooksLikeGitHubToken(const std::string& token) {
    static constexpr const char* kPrefixes[] = {"ghp_", "gho_", "ghu_", "ghs_", "ghr_", "github_pat_"};
    for (const char* prefix : kPrefixes) {
        if (HasPrefix(token, prefix)) {
            return true;
        }
    }
    return false;
}

bool IsValidTokenCharacters(const std::string& token) {
    for (const char c : token) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        if (!ok) {
            return false;
        }
    }
    return true;
}

// memset 会被编译器当作死存储优化掉，必须用平台 API 保证真的写零。
void SecureErase(std::string* text) {
    if (text == nullptr || text->empty()) {
        return;
    }
    SecureZeroMemory(text->data(), text->size());
    text->clear();
}

} // namespace

void TokenManager::StoreInMemory(const std::string& plaintext) {
    EraseFromMemory();
    cached_token_ = plaintext;
    cached_ = true;
}

void TokenManager::EraseFromMemory() {
    SecureErase(&cached_token_);
    cached_ = false;
}

bool TokenManager::EnsureLoaded() {
    if (cached_) {
        return true;
    }

    std::string plaintext;
    const TokenLoadStatus status = TokenStorage::Get()->Load(&plaintext);
    last_load_status_ = status;

    if (status != TokenLoadStatus::Ok) {
        return false;
    }
    StoreInMemory(plaintext);
    SecureErase(&plaintext);
    return true;
}

TokenImportResult TokenManager::Import(const std::string& plaintext) {
    TokenImportResult result;

    std::string token = plaintext;
    // 客户端可能带换行或空白，先裁掉首尾空白再校验。
    const size_t first = token.find_first_not_of(" \t\r\n");
    const size_t last = token.find_last_not_of(" \t\r\n");
    if (first == std::string::npos) {
        result.message = "令牌为空";
        return result;
    }
    token = token.substr(first, last - first + 1);

    if (token.size() < kMinTokenLength) {
        SecureErase(&token);
        result.message = "令牌过短，至少 " + std::to_string(kMinTokenLength) + " 个字符";
        return result;
    }
    if (token.size() > kMaxTokenLength) {
        SecureErase(&token);
        result.message = "令牌过长，最多 " + std::to_string(kMaxTokenLength) + " 个字符";
        return result;
    }
    if (!LooksLikeGitHubToken(token)) {
        SecureErase(&token);
        result.message = "令牌前缀不合法，应为 ghp_ / gho_ / ghu_ / ghs_ / ghr_ / github_pat_ 之一";
        return result;
    }
    if (!IsValidTokenCharacters(token)) {
        SecureErase(&token);
        result.message = "令牌包含非法字符，只允许字母、数字和下划线";
        return result;
    }

    std::string error;
    const bool saved = TokenStorage::Get()->Save(token, &error);
    if (saved) {
        StoreInMemory(token);
        last_load_status_ = TokenLoadStatus::Ok;
        result.ok = true;
        result.message = "令牌已加密保存";
    } else {
        result.message = error.empty() ? "保存令牌失败" : error;
    }

    SecureErase(&token);
    return result;
}

bool TokenManager::HasToken() {
    return EnsureLoaded();
}

TokenHandle TokenManager::AcquireHandle() {
    TokenHandle handle;
    handle.configured = EnsureLoaded();
    if (!handle.configured) {
        return handle;
    }

    // GitHub 现行推荐 Bearer；老式 token 前缀仍被接受。
    std::string header = "Bearer ";
    header += cached_token_;
    handle.authorization_header = std::move(header);

    // Phase 2 未做真实校验，valid 保持 false，由上层如实说明。
    handle.valid = false;
    return handle;
}

TokenStatusSnapshot TokenManager::Snapshot() {
    TokenStatusSnapshot snapshot;
    snapshot.configured = EnsureLoaded();

    if (!snapshot.configured) {
        switch (last_load_status_) {
            case TokenLoadStatus::NotFound:
                snapshot.token_type = "";
                snapshot.login = "";
                break;
            case TokenLoadStatus::Corrupted:
                snapshot.token_type = "corrupted";
                break;
            case TokenLoadStatus::EntropyMismatch:
                snapshot.token_type = "entropy_mismatch";
                break;
            case TokenLoadStatus::IoError:
                snapshot.token_type = "io_error";
                break;
            default:
                break;
        }
        return snapshot;
    }

    // fine-grained 令牌带 github_pat_ 前缀，经典令牌是 gh?_ 前缀。
    snapshot.token_type = HasPrefix(cached_token_, "github_pat_") ? "fine_grained" : "classic";
    // valid 必须是真实 API 调用的结果，Phase 2 没有接，所以恒 false。
    snapshot.valid = false;
    return snapshot;
}

bool TokenManager::Clear() {
    EraseFromMemory();
    std::string error;
    const bool erased = TokenStorage::Get()->Erase(&error);
    last_load_status_ = TokenLoadStatus::NotFound;
    if (!erased) {
        Logger::Get()->Error("清除令牌失败: " + error);
    }
    return erased;
}

} // namespace ghb
