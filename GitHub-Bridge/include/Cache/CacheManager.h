#pragma once

#include "Types/GitHubResponse.h"

#include <cstddef>
#include <list>
#include <string>
#include <unordered_map>

namespace ghb {

// GET 响应的进程内缓存，靠 ETag 做条件请求。
//
// 重要：实测确认 304 响应**照常消耗** GitHub 配额（与 200 一样各扣 1），
// 所以这里的收益是带宽和往返延迟，不是配额。别把它当成省额度的工具。
//
// 只有收到 304 才会用缓存的正文；其余情况一律以服务端返回为准。
class CacheManager {
public:
    static CacheManager* Get() {
        static CacheManager i;
        return &i;
    }

    static constexpr size_t kMaxEntries = 64;
    static constexpr size_t kMaxBodyBytes = 256 * 1024;

    // 关掉缓存后 Lookup 一律未命中、Store 一律忽略。
    static bool Enabled();

    // 缓存键。auth_scope 由调用方传入授权头的哈希前缀——
    // 放散列值而不是令牌原文，换账号不会串用，内存里也不留凭据。
    static std::string MakeKey(const std::string& method, const std::string& path, const std::string& auth_scope);

    // 命中返回 true 并拷出条目；同时把该项移到 LRU 队首。
    bool Lookup(const std::string& key, CacheEntry* out);

    // 写入或更新。正文超过上限或 ETag 为空时不写。
    void Store(const std::string& key, const std::string& etag, const GitHubResponse& response);

    void Remove(const std::string& key);
    void Clear();
    size_t Size() const { return entries_.size(); }

private:
    CacheManager() = default;
    ~CacheManager() = default;

    // 队首是最近访问，队尾是最久未用。
    std::list<std::string> order_;
    std::unordered_map<std::string, std::pair<CacheEntry, std::list<std::string>::iterator>> entries_;
};

} // namespace ghb
