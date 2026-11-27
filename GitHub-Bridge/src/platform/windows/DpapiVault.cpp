#include "Platform/Platform.h"
#include "Platform/TokenVault.h"

#include "Utils/Logger.h"

#include <windows.h>
#include <wincrypt.h>

#include <string>

namespace ghb {

namespace {

// 编译期常量，源码里不出现完整熵值。
constexpr char kEntropySalt[] = "GitHub-Bridge/DPAPI/SecondFactor/v1";

// 第二因子 = SHA-256(编译期盐 | MachineGuid)。
// 必须与 MachineGuid 组合，换机即失效，这正是「解密失败要重配」的正确语义。
std::string DeriveEntropy(const std::string& machine_id) {
    std::string material = kEntropySalt;
    material += "|";
    material += machine_id;

    HCRYPTPROV provider = 0;
    HCRYPTHASH hash = 0;
    std::string digest;
    if (!CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) {
        return {};
    }
    if (CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash)) {
        if (CryptHashData(hash, reinterpret_cast<const BYTE*>(material.data()), static_cast<DWORD>(material.size()), 0)) {
            BYTE buffer[32] = {};
            DWORD size = sizeof(buffer);
            if (CryptGetHashParam(hash, HP_HASHVAL, buffer, &size, 0)) {
                digest.assign(reinterpret_cast<const char*>(buffer), size);
            }
            SecureZeroMemory(buffer, sizeof(buffer));
        }
        CryptDestroyHash(hash);
    }
    CryptReleaseContext(provider, 0);
    return digest;
}

class DpapiVault : public ITokenVault {
public:
    bool Encrypt(const std::string& plaintext, std::string* ciphertext, std::string* error_message) override;
    bool Decrypt(const std::string& ciphertext, std::string* plaintext, std::string* error_message) override;
    const char* BackendName() const override { return "windows-dpapi"; }
};

bool DpapiVault::Encrypt(const std::string& plaintext, std::string* ciphertext, std::string* error_message) {
    if (ciphertext == nullptr) {
        if (error_message) {
            *error_message = "输出缓冲为空";
        }
        return false;
    }

    const std::string machine_id = platform::Get()->MachineId();
    if (machine_id.empty()) {
        Logger::Get()->Warn("读取 MachineGuid 失败，DPAPI 第二因子降级为仅使用编译期常量");
    }
    const std::string entropy = DeriveEntropy(machine_id);
    if (entropy.empty()) {
        if (error_message) {
            *error_message = "派生第二因子失败";
        }
        return false;
    }

    DATA_BLOB input{};
    input.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(plaintext.data()));
    input.cbData = static_cast<DWORD>(plaintext.size());

    DATA_BLOB entropy_blob{};
    entropy_blob.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(entropy.data()));
    entropy_blob.cbData = static_cast<DWORD>(entropy.size());

    DATA_BLOB output{};
    // UI_FORBIDDEN：解密永远不能弹窗，否则会卡死 stdio 服务。
    // 不加 CRYPTPROTECT_LOCAL_MACHINE，保持绑定当前用户。
    if (!CryptProtectData(&input, L"GitHub-Bridge token", &entropy_blob, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output)) {
        if (error_message) {
            *error_message = "DPAPI 加密失败，错误码 " + std::to_string(GetLastError());
        }
        return false;
    }

    ciphertext->assign(reinterpret_cast<const char*>(output.pbData), output.cbData);
    SecureZeroMemory(output.pbData, output.cbData);
    LocalFree(output.pbData);
    return true;
}

bool DpapiVault::Decrypt(const std::string& ciphertext, std::string* plaintext, std::string* error_message) {
    if (plaintext == nullptr) {
        if (error_message) {
            *error_message = "输出缓冲为空";
        }
        return false;
    }
    plaintext->clear();

    const std::string machine_id = platform::Get()->MachineId();
    const std::string entropy = DeriveEntropy(machine_id);
    if (entropy.empty()) {
        if (error_message) {
            *error_message = "派生第二因子失败";
        }
        return false;
    }

    DATA_BLOB input{};
    input.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(ciphertext.data()));
    input.cbData = static_cast<DWORD>(ciphertext.size());

    DATA_BLOB entropy_blob{};
    entropy_blob.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(entropy.data()));
    entropy_blob.cbData = static_cast<DWORD>(entropy.size());

    DATA_BLOB output{};
    if (!CryptUnprotectData(&input, nullptr, &entropy_blob, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output)) {
        if (error_message) {
            *error_message = "DPAPI 解密失败，错误码 " + std::to_string(GetLastError());
        }
        return false;
    }

    plaintext->assign(reinterpret_cast<const char*>(output.pbData), output.cbData);
    SecureZeroMemory(output.pbData, output.cbData);
    LocalFree(output.pbData);
    return true;
}

} // namespace

ITokenVault* ITokenVault::Create() {
    return new DpapiVault();
}

} // namespace ghb
