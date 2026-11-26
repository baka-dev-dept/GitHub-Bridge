#pragma once

#include "Security/TokenStorage.h"
#include "Types/Security.h"

#include <string>

namespace ghb {

// 业务层：加载/保存令牌、内存缓存、组装 Authorization 头、状态快照。
// 不直接做文件 IO——全部经由 TokenStorage。
//
// 铁律：任何公开方法都不得返回令牌明文。上层只能拿到 Authorization 头值，
// 而且那个值也只能交给 GitHubClient，不得写日志、不得回给 MCP 客户端。
class TokenManager {
public:
    static TokenManager* Get() {
        static TokenManager i;
        return &i;
    }

    // 校验格式后交给 TokenStorage 加密落盘。
    TokenImportResult Import(const std::string& plaintext);

    bool HasToken();
    TokenHandle AcquireHandle();

    // Phase 2 中 valid 恒为 false：还没有接 /user 校验，不能谎报有效。
    TokenStatusSnapshot Snapshot();

    bool Clear();

    // 上一次加载失败的原因，供 status 工具如实回报。
    TokenLoadStatus LastLoadStatus() const { return last_load_status_; }

private:
    TokenManager() = default;
    ~TokenManager() = default;

    bool EnsureLoaded();
    void StoreInMemory(const std::string& plaintext);
    void EraseFromMemory();

    // 令牌明文只在这个类里存在，且只在需要构造头部时短暂出现。
    std::string cached_token_;
    bool cached_ = false;
    TokenLoadStatus last_load_status_ = TokenLoadStatus::NotFound;
};

} // namespace ghb
