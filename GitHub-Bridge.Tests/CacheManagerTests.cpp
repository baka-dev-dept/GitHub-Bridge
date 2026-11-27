#include "TestFramework.h"

#include "Cache/CacheManager.h"

using namespace ghb;

namespace {

GitHubResponse MakeResponse(const std::string& body) {
    GitHubResponse r;
    r.status = 200;
    r.ok = true;
    r.body = body;
    return r;
}

std::string Key(const std::string& path) {
    return CacheManager::MakeKey("GET", path, "scope");
}

} // namespace

TEST(Cache, MissOnEmpty) {
    CacheManager::Get()->Clear();
    CacheEntry entry;
    CHECK(!CacheManager::Get()->Lookup(Key("/repos/a/b"), &entry));
}

TEST(Cache, StoreThenHit) {
    CacheManager::Get()->Clear();
    CacheManager::Get()->Store(Key("/repos/a/b"), "W/\"abc\"", MakeResponse("{\"id\":1}"));
    CacheEntry entry;
    CHECK(CacheManager::Get()->Lookup(Key("/repos/a/b"), &entry));
    CHECK_EQ(entry.etag, std::string("W/\"abc\""));
    CHECK_EQ(entry.response.body, std::string("{\"id\":1}"));
}

TEST(Cache, EmptyEtagNotStored) {
    // 没有 ETag 就无法做条件请求，存下来也没用。
    CacheManager::Get()->Clear();
    CacheManager::Get()->Store(Key("/repos/a/b"), "", MakeResponse("{}"));
    CacheEntry entry;
    CHECK(!CacheManager::Get()->Lookup(Key("/repos/a/b"), &entry));
    CHECK_EQ(static_cast<int>(CacheManager::Get()->Size()), 0);
}

TEST(Cache, OversizedBodyNotStored) {
    CacheManager::Get()->Clear();
    const std::string huge(CacheManager::kMaxBodyBytes + 1, 'x');
    CacheManager::Get()->Store(Key("/big"), "W/\"big\"", MakeResponse(huge));
    CacheEntry entry;
    CHECK(!CacheManager::Get()->Lookup(Key("/big"), &entry));
}

TEST(Cache, DifferentPathsIsolated) {
    CacheManager::Get()->Clear();
    CacheManager::Get()->Store(Key("/repos/a/b"), "W/\"1\"", MakeResponse("one"));
    CacheManager::Get()->Store(Key("/repos/c/d"), "W/\"2\"", MakeResponse("two"));
    CacheEntry e1;
    CacheEntry e2;
    CHECK(CacheManager::Get()->Lookup(Key("/repos/a/b"), &e1));
    CHECK(CacheManager::Get()->Lookup(Key("/repos/c/d"), &e2));
    CHECK_EQ(e1.response.body, std::string("one"));
    CHECK_EQ(e2.response.body, std::string("two"));
}

TEST(Cache, DifferentAuthScopesIsolated) {
    // 换令牌不能拿到上一个令牌的缓存。
    CacheManager::Get()->Clear();
    const std::string k1 = CacheManager::MakeKey("GET", "/user", "scope_a");
    const std::string k2 = CacheManager::MakeKey("GET", "/user", "scope_b");
    CacheManager::Get()->Store(k1, "W/\"a\"", MakeResponse("user_a"));
    CacheEntry entry;
    CHECK(!CacheManager::Get()->Lookup(k2, &entry));
}

TEST(Cache, LruEvictsOldest) {
    CacheManager::Get()->Clear();
    // 填满到上限，再插入一条，最早的那条应被淘汰。
    for (size_t i = 0; i < CacheManager::kMaxEntries; ++i) {
        CacheManager::Get()->Store(Key("/p/" + std::to_string(i)), "W/\"" + std::to_string(i) + "\"", MakeResponse("b"));
    }
    CHECK_EQ(static_cast<int>(CacheManager::Get()->Size()), static_cast<int>(CacheManager::kMaxEntries));

    CacheManager::Get()->Store(Key("/p/extra"), "W/\"extra\"", MakeResponse("b"));
    CHECK_EQ(static_cast<int>(CacheManager::Get()->Size()), static_cast<int>(CacheManager::kMaxEntries));

    CacheEntry entry;
    CHECK(!CacheManager::Get()->Lookup(Key("/p/0"), &entry));
    CHECK(CacheManager::Get()->Lookup(Key("/p/extra"), &entry));
}

TEST(Cache, AccessPromotesEntry) {
    // 访问会把条目移到队首，之后淘汰的是真正最久未用的那条。
    CacheManager::Get()->Clear();
    for (size_t i = 0; i < CacheManager::kMaxEntries; ++i) {
        CacheManager::Get()->Store(Key("/q/" + std::to_string(i)), "W/\"" + std::to_string(i) + "\"", MakeResponse("b"));
    }
    CacheEntry entry;
    CHECK(CacheManager::Get()->Lookup(Key("/q/0"), &entry));

    CacheManager::Get()->Store(Key("/q/extra"), "W/\"e\"", MakeResponse("b"));
    // 0 刚被访问过，应存活；1 成了最久未用。
    CHECK(CacheManager::Get()->Lookup(Key("/q/0"), &entry));
    CHECK(!CacheManager::Get()->Lookup(Key("/q/1"), &entry));
}

TEST(Cache, RemoveDropsEntry) {
    CacheManager::Get()->Clear();
    CacheManager::Get()->Store(Key("/r"), "W/\"r\"", MakeResponse("b"));
    CacheManager::Get()->Remove(Key("/r"));
    CacheEntry entry;
    CHECK(!CacheManager::Get()->Lookup(Key("/r"), &entry));
}

TEST(Cache, StoreUpdatesExisting) {
    CacheManager::Get()->Clear();
    CacheManager::Get()->Store(Key("/u"), "W/\"v1\"", MakeResponse("v1"));
    CacheManager::Get()->Store(Key("/u"), "W/\"v2\"", MakeResponse("v2"));
    CacheEntry entry;
    CHECK(CacheManager::Get()->Lookup(Key("/u"), &entry));
    CHECK_EQ(entry.etag, std::string("W/\"v2\""));
    CHECK_EQ(entry.response.body, std::string("v2"));
    CHECK_EQ(static_cast<int>(CacheManager::Get()->Size()), 1);
}

TEST(Cache, KeyIncludesMethodAndPath) {
    const std::string a = CacheManager::MakeKey("GET", "/repos/a/b", "s");
    const std::string b = CacheManager::MakeKey("POST", "/repos/a/b", "s");
    const std::string c = CacheManager::MakeKey("GET", "/repos/a/c", "s");
    CHECK(a != b);
    CHECK(a != c);
}
