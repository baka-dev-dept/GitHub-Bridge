#include "Security/PermissionManager.h"

#include "Utils/Logger.h"

#include <windows.h>
#include <bcrypt.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>

namespace ghb {

namespace {

int64_t NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

// 票据 token 必须不可预测：用系统 CSPRNG，而不是 rand() 或时间戳。
// 可预测的 token 等于没有二次确认——Agent 可以自己猜出来。
std::string RandomToken() {
    BYTE buffer[16] = {};
    // BCryptGenRandom 返回 NTSTATUS，成功是 0；写成 !调用 会把成功当失败。
    const NTSTATUS status = BCryptGenRandom(nullptr, buffer, sizeof(buffer), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (status != 0) {
        // 取不到随机数时不能退化成弱随机：宁可让本次确认失败，也不发一个可猜的票据。
        Logger::Get()->Error("生成确认票据失败：CSPRNG 不可用，NTSTATUS=0x" + std::to_string(static_cast<unsigned long>(status)));
        return {};
    }
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(sizeof(buffer) * 2);
    for (const BYTE byte : buffer) {
        out.push_back(kHex[byte >> 4]);
        out.push_back(kHex[byte & 0x0F]);
    }
    return out;
}

std::string ConfigFilePath() {
    char expanded[MAX_PATH] = {};
    if (ExpandEnvironmentStringsA("%APPDATA%", expanded, MAX_PATH) == 0) {
        return {};
    }
    return std::string(expanded) + "\\GitHub-Bridge\\permissions.json";
}

} // namespace

void PermissionManager::LoadConfig() {
    const std::string path = ConfigFilePath();
    if (path.empty()) {
        Logger::Get()->Warn("权限配置：无法解析 %APPDATA%，使用默认值");
        return;
    }

    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        Logger::Get()->Info("权限配置不存在，使用默认值: " + path);
        return;
    }

    std::FILE* file = nullptr;
    if (fopen_s(&file, path.c_str(), "rb") != 0 || file == nullptr) {
        Logger::Get()->Warn("权限配置无法读取，使用默认值: " + path);
        return;
    }
    std::string content;
    char chunk[4096] = {};
    size_t read = 0;
    while ((read = std::fread(chunk, 1, sizeof(chunk), file)) > 0) {
        content.append(chunk, read);
    }
    std::fclose(file);

    const nlohmann::json parsed = nlohmann::json::parse(content, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        Logger::Get()->Warn("权限配置不是合法 JSON 对象，使用默认值");
        return;
    }

    const auto it = parsed.find("permissions");
    if (it == parsed.end() || !it->is_object()) {
        Logger::Get()->Warn("权限配置缺少 permissions 对象，使用默认值");
        return;
    }

    // 未知字段直接忽略（前向兼容）；类型不对的字段也忽略，不让一个坏字段拖垮整份配置。
    auto pick = [&it](const char* key, bool fallback) {
        const auto found = it->find(key);
        return (found != it->end() && found->is_boolean()) ? found->get<bool>() : fallback;
    };

    PermissionConfig loaded;
    loaded.read = pick("read", loaded.read);
    loaded.write = pick("write", loaded.write);
    loaded.create = pick("create", loaded.create);
    loaded.delete_repository = pick("delete_repository", loaded.delete_repository);
    loaded.delete_branch = pick("delete_branch", loaded.delete_branch);
    loaded.force_push = pick("force_push", loaded.force_push);
    loaded.merge_pull_request = pick("merge_pull_request", loaded.merge_pull_request);
    config_ = loaded;

    Logger::Get()->Info("已加载权限配置: read=" + std::to_string(loaded.read) +
                        " write=" + std::to_string(loaded.write) +
                        " create=" + std::to_string(loaded.create) +
                        " delete_repository=" + std::to_string(loaded.delete_repository) +
                        " delete_branch=" + std::to_string(loaded.delete_branch) +
                        " merge_pull_request=" + std::to_string(loaded.merge_pull_request));
}

bool PermissionManager::IsSwitchEnabled(const std::string& permission_key) const {
    if (permission_key == "read") return config_.read;
    if (permission_key == "write") return config_.write;
    if (permission_key == "create") return config_.create;
    if (permission_key == "delete_repository") return config_.delete_repository;
    if (permission_key == "delete_branch") return config_.delete_branch;
    if (permission_key == "force_push") return config_.force_push;
    if (permission_key == "merge_pull_request") return config_.merge_pull_request;
    // 未知键视为关闭：宁可多拦一次，也不能因为漏登记开关而放行。
    Logger::Get()->Warn("未知权限键，按拒绝处理: " + permission_key);
    return false;
}

void PermissionManager::PurgeExpired() {
    const int64_t now = NowMs();
    tickets_.erase(std::remove_if(tickets_.begin(), tickets_.end(), [now](const ConfirmTicket& t) { return t.expires_at_unix_ms <= now || t.used; }), tickets_.end());
}

std::string PermissionManager::Fingerprint(const std::string& tool_name, const std::string& arguments_json) {
    // 去掉 confirm_token 后再算指纹：否则同一目标带着不同票据会被当成不同目标，
    // 校验永远无法通过。这是本机制最容易写错的地方。
    nlohmann::json args = nlohmann::json::parse(arguments_json, nullptr, false);
    if (args.is_object()) {
        args.erase("confirm_token");
    } else {
        args = nlohmann::json::object();
    }

    // 用有序键序列化，保证同一组参数无论字段顺序如何都得到同一指纹。
    std::string material = tool_name;
    material += "|";
    material += args.dump();

    // 简单稳定哈希（FNV-1a 64 位）即可：指纹只用于「同不同」判断，不承担密码学职责。
    uint64_t hash = 1469598103934665603ULL;
    for (const char c : material) {
        hash ^= static_cast<uint64_t>(static_cast<unsigned char>(c));
        hash *= 1099511628211ULL;
    }
    char hex[17] = {};
    std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(hash));
    return hex;
}

PermissionOutcome PermissionManager::Check(const std::string& tool_name, bool requires_confirmation, const std::string& permission_key, const std::string& target_fingerprint, const std::string& provided_token) {
    PermissionOutcome outcome;

    // 第一道：配置开关。没有权限键表示不受开关约束。
    if (!permission_key.empty() && !IsSwitchEnabled(permission_key)) {
        outcome.verdict = PermissionVerdict::Denied;
        outcome.reason = "已被权限配置禁止: " + permission_key;
        Logger::Get()->Warn("权限拒绝 " + tool_name + "：" + outcome.reason);
        return outcome;
    }

    // 非不可逆操作到此即可执行。
    if (!requires_confirmation) {
        outcome.verdict = PermissionVerdict::Allow;
        return outcome;
    }

    // 第二道：运行时二次确认。
    PurgeExpired();

    if (provided_token.empty()) {
        // 首次调用：下发一次性票据。
        const std::string token = RandomToken();
        if (token.empty()) {
            outcome.verdict = PermissionVerdict::Denied;
            outcome.reason = "无法生成确认票据，已拒绝执行";
            return outcome;
        }
        ConfirmTicket ticket;
        ticket.token = token;
        ticket.tool_name = tool_name;
        ticket.target_fingerprint = target_fingerprint;
        ticket.expires_at_unix_ms = NowMs() + kTicketTtlMs;
        ticket.used = false;
        tickets_.push_back(ticket);

        outcome.verdict = PermissionVerdict::NeedConfirm;
        outcome.confirm_token = token;
        outcome.expires_at_unix_ms = ticket.expires_at_unix_ms;
        Logger::Get()->Info("下发确认票据: " + tool_name + " 目标 " + target_fingerprint);
        return outcome;
    }

    // 携带票据重试：必须同时匹配 token、工具名、目标指纹，且未过期未使用。
    for (auto& ticket : tickets_) {
        if (ticket.token != provided_token) {
            continue;
        }
        if (ticket.tool_name != tool_name || ticket.target_fingerprint != target_fingerprint) {
            outcome.verdict = PermissionVerdict::BadConfirmToken;
            outcome.reason = "确认票据与本次目标不匹配，目标已变更则需重新发起";
            Logger::Get()->Warn("确认票据目标不匹配: " + tool_name);
            return outcome;
        }
        // 一次性：先用掉再执行，防止同一张票据被重放两次。
        ticket.used = true;
        outcome.verdict = PermissionVerdict::Allow;
        Logger::Get()->Info("确认通过: " + tool_name);
        return outcome;
    }

    outcome.verdict = PermissionVerdict::BadConfirmToken;
    outcome.reason = "确认票据无效、已过期或已使用，请重新发起调用获取新票据";
    Logger::Get()->Warn("确认票据无效: " + tool_name);
    return outcome;
}

} // namespace ghb
