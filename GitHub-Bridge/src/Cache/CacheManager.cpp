#include "Cache/CacheManager.h"

#include "Utils/Logger.h"

#include <windows.h>

namespace ghb {

bool CacheManager::Enabled() {
    // 只认 "0"，其余（含未设置）都按开启处理：缓存对调用方透明，
    // 默认关掉反而不符合预期。
    char buffer[8] = {};
    const DWORD written = GetEnvironmentVariableA("GHB_CACHE", buffer, sizeof(buffer));
    if (written == 0 || written >= sizeof(buffer)) {
        return true;
    }
    return !(buffer[0] == '0' && buffer[1] == '\0');
}

std::string CacheManager::MakeKey(const std::string& method, const std::string& path, const std::string& auth_scope) {
    return method + " " + path + "|" + auth_scope;
}

bool CacheManager::Lookup(const std::string& key, CacheEntry* out) {
    if (!Enabled() || out == nullptr) {
        return false;
    }
    const auto it = entries_.find(key);
    if (it == entries_.end()) {
        return false;
    }

    // 命中即提升到队首，维持 LRU 顺序。
    order_.splice(order_.begin(), order_, it->second.second);
    *out = it->second.first;
    return true;
}

void CacheManager::Store(const std::string& key, const std::string& etag, const GitHubResponse& response) {
    if (!Enabled()) {
        return;
    }
    // 没有 ETag 就没法做条件请求，存下来也用不上。
    if (etag.empty()) {
        return;
    }
    if (response.body.size() > kMaxBodyBytes) {
        // 大响应（如文件正文）缓存收益低、占内存高，直接跳过。
        Logger::Get()->Debug("响应超过缓存上限，不缓存: " + std::to_string(response.body.size()) + " 字节");
        return;
    }

    const auto existing = entries_.find(key);
    if (existing != entries_.end()) {
        existing->second.first = CacheEntry{etag, response};
        order_.splice(order_.begin(), order_, existing->second.second);
        return;
    }

    order_.push_front(key);
    entries_[key] = {CacheEntry{etag, response}, order_.begin()};

    // 超出容量时淘汰队尾。
    while (entries_.size() > kMaxEntries && !order_.empty()) {
        const std::string victim = order_.back();
        order_.pop_back();
        entries_.erase(victim);
    }
}

void CacheManager::Remove(const std::string& key) {
    const auto it = entries_.find(key);
    if (it == entries_.end()) {
        return;
    }
    order_.erase(it->second.second);
    entries_.erase(it);
}

void CacheManager::Clear() {
    order_.clear();
    entries_.clear();
}

} // namespace ghb
