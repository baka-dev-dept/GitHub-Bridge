#pragma once

#include "Security/TokenStorage.h"
#include "Types/Security.h"

#include <string>
#include <vector>

namespace ghb {

// 业务层：加载/保存令牌、内存缓存、组装 Authorization 头、状态快照。
// 不直接做文件 IO——全部经由 TokenStorage。
//
// 支持多个命名 profile（如 work / personal），同一时刻只有一个 active。
// 磁盘上是结构化 JSON，整体交给 DPAPI 加密；DPAPI 层不感知 profile 概念。
//
// 铁律：任何公开方法都不得返回令牌明文。上层只能拿到 Authorization 头值，
// 而且那个值也只能交给 GitHubClient，不得写日志、不得回给 MCP 客户端。
class TokenManager {
public:
    static TokenManager* Get() {
        static TokenManager i;
        return &i;
    }

    static constexpr const char* kDefaultProfile = "default";

    // profile 名只允许 [A-Za-z0-9_-]，长度 1..32。合法返回空串，否则返回原因。
    static std::string ValidateProfileName(const std::string& name);

    // 校验格式后交给 TokenStorage 加密落盘。profile 为空时用 default。
    TokenImportResult Import(const std::string& plaintext, const std::string& profile);

    bool HasToken();
    TokenHandle AcquireHandle();

    // 切换 active profile。只做本地操作，不联网校验令牌有效性。
    TokenImportResult Use(const std::string& profile);

    // 删除指定 profile；profile 为空时删当前 active。
    TokenImportResult ClearProfile(const std::string& profile);

    // 列出全部 profile 的公开信息（不含令牌）。
    std::vector<TokenProfileInfo> ListProfiles();

    std::string ActiveProfile();

    TokenStatusSnapshot Snapshot();

    bool Clear();

    // 上一次加载失败的原因，供 status 工具如实回报。
    TokenLoadStatus LastLoadStatus() const { return last_load_status_; }

private:
    TokenManager() = default;
    ~TokenManager() = default;

    bool EnsureLoaded();
    bool Reload();
    void StoreInMemory(const std::string& plaintext);
    void EraseFromMemory();

    // 把内存里的 profiles 序列化后落盘。失败时内存状态回滚由调用方保证。
    bool PersistProfiles(std::string* error_message);

    // 旧版单令牌文件解密出来是裸令牌字符串，迁移成 default profile。
    // 返回 true 表示发生了迁移（需要回写）。
    bool MigrateLegacyIfNeeded(const std::string& plaintext);

    // 令牌明文只在这个类里存在，且只在需要构造头部时短暂出现。
    // 多 profile 下按名字存，active_profile_ 指向当前使用的那一个。
    struct StoredProfile {
        std::string token;
        int64_t saved_at_unix = 0;
    };

    std::string cached_token_;
    std::string active_profile_;
    std::vector<std::pair<std::string, StoredProfile>> profiles_;
    bool cached_ = false;
    TokenLoadStatus last_load_status_ = TokenLoadStatus::NotFound;
};

} // namespace ghb
