#include "Security/TokenStorage.h"

#include "Platform/Platform.h"
#include "Platform/TokenVault.h"
#include "Utils/Logger.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

namespace ghb {

namespace {

constexpr char kAppDirName[] = "GitHub-Bridge";
constexpr char kDataFileName[] = "tokens.dat";
// 令牌文件不该超过这个大小；超了说明不是我们写的，按损坏处理。
constexpr std::streamoff kMaxCipherBytes = 1024 * 1024;

} // namespace

TokenStoragePaths TokenStorage::ResolvePaths() const {
    TokenStoragePaths paths;
    paths.data_file = DataFilePath();
    const std::string dir = AppDirPath();
    if (!dir.empty()) {
        paths.log_dir = dir + "/logs";
    }
    return paths;
}

std::string TokenStorage::AppDirPath() const {
    return platform::Get()->AppDataDir();
}

std::string TokenStorage::DataFilePath() const {
    const std::string dir = AppDirPath();
    if (dir.empty()) {
        return {};
    }
    return dir + "/" + kDataFileName;
}

bool TokenStorage::EnsureAppDir(std::string* error_message) const {
    const std::string dir = AppDirPath();
    if (dir.empty()) {
        if (error_message) {
            *error_message = "无法解析应用数据目录";
        }
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec && !std::filesystem::exists(dir)) {
        if (error_message) {
            *error_message = "创建数据目录失败: " + ec.message();
        }
        return false;
    }

    // 目录也收紧：Linux 上是 0700，避免同机其他用户列目录。
    std::string perm_error;
    if (!platform::Get()->RestrictDirToCurrentUser(dir, &perm_error)) {
        Logger::Get()->Warn("收紧数据目录权限失败: " + perm_error);
    }
    return true;
}

bool TokenStorage::Save(const std::string& plaintext, std::string* error_message) {
    if (!EnsureAppDir(error_message)) {
        return false;
    }

    std::string cipher;
    std::string vault_error;
    if (!vault::Get()->Encrypt(plaintext, &cipher, &vault_error)) {
        if (error_message) {
            *error_message = vault_error.empty() ? "加密失败" : vault_error;
        }
        return false;
    }

    const std::string final_path = DataFilePath();
    const std::string tmp_path = final_path + ".tmp";

    {
        std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
        if (!out) {
            if (error_message) {
                *error_message = "创建临时文件失败: " + tmp_path;
            }
            return false;
        }
        out.write(cipher.data(), static_cast<std::streamsize>(cipher.size()));
        out.flush();
        if (!out) {
            if (error_message) {
                *error_message = "写入临时文件失败";
            }
            out.close();
            std::remove(tmp_path.c_str());
            return false;
        }
    }

    // 先落盘再改名，避免断电留下半截密文。
    std::error_code ec;
    std::filesystem::rename(tmp_path, final_path, ec);
    if (ec) {
        std::remove(tmp_path.c_str());
        if (error_message) {
            *error_message = "替换令牌文件失败: " + ec.message();
        }
        return false;
    }

    // 权限收紧失败不该让保存整体失败：密文已落盘，只是没到最严。
    std::string perm_error;
    if (!platform::Get()->RestrictFileToCurrentUser(final_path, &perm_error)) {
        Logger::Get()->Warn("收紧令牌文件权限失败: " + perm_error);
    }
    return true;
}

TokenLoadStatus TokenStorage::Load(std::string* plaintext) {
    if (plaintext == nullptr) {
        return TokenLoadStatus::IoError;
    }
    plaintext->clear();

    const std::string path = DataFilePath();
    if (path.empty()) {
        return TokenLoadStatus::IoError;
    }

    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return TokenLoadStatus::NotFound;
    }

    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size <= 0 || static_cast<std::streamoff>(size) > kMaxCipherBytes) {
        return TokenLoadStatus::Corrupted;
    }

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return TokenLoadStatus::IoError;
    }
    std::string cipher(static_cast<size_t>(size), '\0');
    in.read(cipher.data(), static_cast<std::streamsize>(size));
    if (!in) {
        return TokenLoadStatus::IoError;
    }

    std::string vault_error;
    if (!vault::Get()->Decrypt(cipher, plaintext, &vault_error)) {
        // 解不开有两种原因，补救方式完全不同：换了机器/用户，或文件真的坏了。
        // vault 无法分辨时会给出通用错误，这里按「是否像我们的密文」粗略区分。
        if (cipher.size() < 32) {
            return TokenLoadStatus::Corrupted;
        }
        Logger::Get()->Warn("令牌解密失败: " + vault_error);
        return TokenLoadStatus::EntropyMismatch;
    }
    return TokenLoadStatus::Ok;
}

bool TokenStorage::Erase(std::string* error_message) {
    const std::string path = DataFilePath();
    if (path.empty()) {
        if (error_message) {
            *error_message = "无法解析令牌文件路径";
        }
        return false;
    }

    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return true;
    }
    std::filesystem::remove(path, ec);
    if (ec) {
        if (error_message) {
            *error_message = "删除令牌文件失败: " + ec.message();
        }
        return false;
    }
    return true;
}

bool TokenStorage::Exists() const {
    const std::string path = DataFilePath();
    if (path.empty()) {
        return false;
    }
    std::error_code ec;
    return std::filesystem::exists(path, ec);
}

} // namespace ghb
