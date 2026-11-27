#include "Security/TokenManager.h"

#include "Platform/Platform.h"
#include "Utils/Logger.h"

#include <nlohmann/json.hpp>


#include <chrono>
#include <algorithm>
#include <utility>

namespace ghb {

namespace {

// 令牌格式与长度上下限。上限远小于任何真实令牌，只用于拦截误传整段文件。
constexpr size_t kMinTokenLength = 20;
constexpr size_t kMaxTokenLength = 1024;

// 磁盘格式版本。v1 是裸令牌字符串，v2 是带 profiles 的 JSON。
constexpr int kStoreVersion = 2;

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
    platform::Get()->SecureZero(&(*text)[0], text->size());
    text->clear();
}

int64_t NowUnixSeconds() {
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

} // namespace

std::string TokenManager::ValidateProfileName(const std::string& name) {
    if (name.empty()) {
        return "profile 名不能为空";
    }
    if (name.size() > 32) {
        return "profile 名过长，最多 32 个字符";
    }
    for (const char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) {
            return "profile 名含非法字符，只允许字母、数字、下划线和连字符";
        }
    }
    return {};
}

void TokenManager::StoreInMemory(const std::string& plaintext) {
    EraseFromMemory();
    cached_token_ = plaintext;
    cached_ = true;
}

void TokenManager::EraseFromMemory() {
    for (auto& entry : profiles_) {
        SecureErase(&entry.second.token);
    }
    profiles_.clear();
    active_profile_.clear();
    SecureErase(&cached_token_);
    cached_ = false;
}

bool TokenManager::MigrateLegacyIfNeeded(const std::string& plaintext) {
    // v1 存的是裸令牌，不是 JSON。解析成功就说明已经是新格式。
    const nlohmann::json parsed = nlohmann::json::parse(plaintext, nullptr, false);
    if (parsed.is_object() && parsed.contains("profiles")) {
        return false;
    }

    // 迁移：把整段明文当作 default profile 的令牌。
    profiles_.clear();
    StoredProfile legacy;
    legacy.token = plaintext;
    legacy.saved_at_unix = NowUnixSeconds();
    profiles_.emplace_back(kDefaultProfile, legacy);
    active_profile_ = kDefaultProfile;
    Logger::Get()->Info("检测到旧版单令牌文件，已迁移为 profile: default");
    return true;
}

bool TokenManager::PersistProfiles(std::string* error_message) {
    nlohmann::json root;
    root["version"] = kStoreVersion;
    root["active"] = active_profile_;

    nlohmann::json profiles = nlohmann::json::object();
    for (const auto& entry : profiles_) {
        profiles[entry.first] = {
            {"token", entry.second.token},
            {"saved_at", entry.second.saved_at_unix},
        };
    }
    root["profiles"] = profiles;

    const std::string serialized = root.dump();
    const bool saved = TokenStorage::Get()->Save(serialized, error_message);
    if (saved) {
        // 落盘的是完整 JSON，内存里的 active 令牌也要跟着刷新。
        const auto it = std::find_if(profiles_.begin(), profiles_.end(), [this](const auto& e) { return e.first == active_profile_; });
        if (it != profiles_.end()) {
            cached_token_ = it->second.token;
        } else {
            SecureErase(&cached_token_);
        }
    }
    return saved;
}

bool TokenManager::EnsureLoaded() {
    if (cached_) {
        return true;
    }
    return Reload();
}

bool TokenManager::Reload() {
    std::string plaintext;
    const TokenLoadStatus status = TokenStorage::Get()->Load(&plaintext);
    last_load_status_ = status;

    if (status != TokenLoadStatus::Ok) {
        return false;
    }

    if (MigrateLegacyIfNeeded(plaintext)) {
        SecureErase(&plaintext);
        cached_ = true;
        std::string error;
        if (!PersistProfiles(&error)) {
            // 迁移回写失败不影响本次使用，只是下次还要再迁移一遍。
            Logger::Get()->Warn("旧格式回写失败: " + error);
        }
        return true;
    }

    const nlohmann::json root = nlohmann::json::parse(plaintext, nullptr, false);
    SecureErase(&plaintext);
    if (!root.is_object() || !root.contains("profiles") || !root["profiles"].is_object()) {
        last_load_status_ = TokenLoadStatus::Corrupted;
        Logger::Get()->Error("令牌文件格式无法识别");
        return false;
    }

    profiles_.clear();
    for (auto it = root["profiles"].begin(); it != root["profiles"].end(); ++it) {
        if (!it.value().is_object() || !it.value().contains("token") || !it.value()["token"].is_string()) {
            continue;
        }
        StoredProfile entry;
        entry.token = it.value()["token"].get<std::string>();
        entry.saved_at_unix = it.value().value("saved_at", static_cast<int64_t>(0));
        profiles_.emplace_back(it.key(), entry);
    }

    if (profiles_.empty()) {
        last_load_status_ = TokenLoadStatus::Corrupted;
        return false;
    }

    active_profile_ = root.value("active", std::string());
    // active 指向不存在的 profile 时回退到第一个，避免整个服务不可用。
    const auto active_it = std::find_if(profiles_.begin(), profiles_.end(), [this](const auto& e) { return e.first == active_profile_; });
    if (active_it == profiles_.end()) {
        active_profile_ = profiles_.front().first;
        Logger::Get()->Warn("active profile 不存在，回退到: " + active_profile_);
    }

    cached_token_ = active_it == profiles_.end() ? profiles_.front().second.token : active_it->second.token;
    cached_ = true;
    return true;
}

TokenImportResult TokenManager::Import(const std::string& plaintext, const std::string& profile) {
    TokenImportResult result;

    std::string profile_name = profile.empty() ? std::string(kDefaultProfile) : profile;
    const std::string name_error = ValidateProfileName(profile_name);
    if (!name_error.empty()) {
        result.message = name_error;
        return result;
    }

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

    // 先确保已加载现有 profile，否则会把别的 profile 冲掉。
    if (!cached_) {
        Reload();
    }

    StoredProfile entry;
    entry.token = token;
    entry.saved_at_unix = NowUnixSeconds();

    bool replaced = false;
    for (auto& existing : profiles_) {
        if (existing.first == profile_name) {
            SecureErase(&existing.second.token);
            existing.second = entry;
            replaced = true;
            break;
        }
    }
    if (!replaced) {
        profiles_.emplace_back(profile_name, entry);
        // 第一个导入的 profile 自动成为 active。
        if (active_profile_.empty()) {
            active_profile_ = profile_name;
        }
    }

    std::string error;
    if (PersistProfiles(&error)) {
        cached_ = true;
        last_load_status_ = TokenLoadStatus::Ok;
        result.ok = true;
        result.message = replaced ? ("profile 已更新: " + profile_name) : ("profile 已保存: " + profile_name);
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

    // valid 保持 false，真实有效性由调用方发请求判定。
    handle.valid = false;
    return handle;
}

TokenImportResult TokenManager::Use(const std::string& profile) {
    TokenImportResult result;
    const std::string name_error = ValidateProfileName(profile);
    if (!name_error.empty()) {
        result.message = name_error;
        return result;
    }

    if (!cached_) {
        Reload();
    }
    if (profiles_.empty()) {
        result.message = "尚未导入任何令牌";
        return result;
    }

    const auto it = std::find_if(profiles_.begin(), profiles_.end(), [&profile](const auto& e) { return e.first == profile; });
    if (it == profiles_.end()) {
        result.message = "profile 不存在: " + profile;
        return result;
    }

    const std::string previous = active_profile_;
    active_profile_ = profile;
    std::string error;
    if (!PersistProfiles(&error)) {
        active_profile_ = previous;
        result.message = error.empty() ? "切换 profile 失败" : error;
        return result;
    }

    result.ok = true;
    result.message = "已切换到 profile: " + profile;
    return result;
}

TokenImportResult TokenManager::ClearProfile(const std::string& profile) {
    TokenImportResult result;

    if (!cached_) {
        Reload();
    }
    if (profiles_.empty()) {
        result.message = "尚未导入任何令牌";
        return result;
    }

    const std::string target = profile.empty() ? active_profile_ : profile;
    auto it = std::find_if(profiles_.begin(), profiles_.end(), [&target](const auto& e) { return e.first == target; });
    if (it == profiles_.end()) {
        result.message = "profile 不存在: " + target;
        return result;
    }

    SecureErase(&it->second.token);
    profiles_.erase(it);

    if (profiles_.empty()) {
        // 删光了就把文件也删掉，避免留下一个空壳。
        EraseFromMemory();
        TokenStorage::Get()->Erase(nullptr);
        last_load_status_ = TokenLoadStatus::NotFound;
        result.ok = true;
        result.message = "已删除最后一个 profile，令牌文件已清除";
        return result;
    }

    if (target == active_profile_) {
        active_profile_ = profiles_.front().first;
    }

    std::string error;
    if (!PersistProfiles(&error)) {
        result.message = error.empty() ? "删除 profile 失败" : error;
        return result;
    }

    result.ok = true;
    result.message = "已删除 profile: " + target + "，当前 active: " + active_profile_;
    return result;
}

std::vector<TokenProfileInfo> TokenManager::ListProfiles() {
    std::vector<TokenProfileInfo> out;
    if (!EnsureLoaded()) {
        return out;
    }
    for (const auto& entry : profiles_) {
        TokenProfileInfo info;
        info.name = entry.first;
        info.active = (entry.first == active_profile_);
        info.saved_at_unix = entry.second.saved_at_unix;
        out.push_back(std::move(info));
    }
    return out;
}

std::string TokenManager::ActiveProfile() {
    if (!EnsureLoaded()) {
        return {};
    }
    return active_profile_;
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
    // valid 必须是真实 API 调用的结果，这里不校验，恒 false。
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
