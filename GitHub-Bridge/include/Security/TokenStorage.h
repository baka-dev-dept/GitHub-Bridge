#pragma once

#include "Types/Security.h"

#include <string>

namespace ghb {

// DPAPI 密文的读取结果。三类失败必须分开，因为补救方式完全不同：
// NotFound 是首次运行，Corrupted 要重导，EntropyMismatch 是换了机器或用户。
enum class TokenLoadStatus : int {
    Ok = 0,
    NotFound = 1,
    Corrupted = 2,
    EntropyMismatch = 3,
    IoError = 4,
};

// 纯持久化层：只做 DPAPI 加解密、文件读写、ACL 收紧。
// 不校验令牌有效性，不理解 GitHub API——那是 TokenManager 与 GitHubClient 的事。
class TokenStorage {
public:
    static TokenStorage* Get() {
        static TokenStorage i;
        return &i;
    }

    // 返回 %APPDATA%\GitHub-Bridge 下的文件与目录路径。
    TokenStoragePaths ResolvePaths() const;

    // 加密并原子写入。成功后收紧 ACL。
    bool Save(const std::string& plaintext, std::string* error_message);

    TokenLoadStatus Load(std::string* plaintext);

    // 删除密文。文件本就不存在时返回 true（幂等）。
    bool Erase(std::string* error_message);

    bool Exists() const;

    // 由编译期常量与机器级数据派生的 DPAPI 第二因子。
    // 若读不到 MachineGuid，降级为仅用编译期常量并记 Warn，不硬失败。
    static std::string DeriveEntropy();

private:
    TokenStorage() = default;
    ~TokenStorage() = default;

    std::string DataFilePath() const;
    std::string AppDirPath() const;
    bool EnsureAppDir(std::string* error_message) const;
    bool RestrictAcl(const std::string& path, std::string* error_message) const;
};

} // namespace ghb
