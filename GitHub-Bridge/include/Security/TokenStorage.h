#pragma once

#include "Types/Security.h"

#include <string>

namespace ghb {

// 密文的读取结果。三类失败必须分开，因为补救方式完全不同：
// NotFound 是首次运行，Corrupted 要重导，EntropyMismatch 是换了机器或用户。
enum class TokenLoadStatus : int {
    Ok = 0,
    NotFound = 1,
    Corrupted = 2,
    EntropyMismatch = 3,
    IoError = 4,
};

// 纯持久化层：只做加解密调度、文件读写、权限收紧。
// 不校验令牌有效性，不理解 GitHub API——那是 TokenManager 与 GitHubClient 的事。
//
// 加解密本身交给 ITokenVault（Windows 是 DPAPI，Linux 是 libsodium），
// 本类只管「密文放在哪、怎么安全落盘」。
class TokenStorage {
public:
    static TokenStorage* Get() {
        static TokenStorage i;
        return &i;
    }

    // 返回数据目录与文件路径。
    TokenStoragePaths ResolvePaths() const;

    // 加密并原子写入。成功后收紧权限。
    bool Save(const std::string& plaintext, std::string* error_message);

    TokenLoadStatus Load(std::string* plaintext);

    // 删除密文。文件本就不存在时返回 true（幂等）。
    bool Erase(std::string* error_message);

    bool Exists() const;

private:
    TokenStorage() = default;
    ~TokenStorage() = default;

    std::string DataFilePath() const;
    std::string AppDirPath() const;
    bool EnsureAppDir(std::string* error_message) const;
};

} // namespace ghb
