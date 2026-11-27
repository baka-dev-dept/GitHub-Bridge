#include "Platform/Platform.h"
#include "Platform/TokenVault.h"

#include "Utils/Logger.h"

#include <sodium.h>
#include <sys/stat.h>

#include <cerrno>
#include <cstring>
#include <fstream>
#include <string>
#include <unistd.h>

namespace ghb {

namespace {

// 编译期常量，源码里不出现完整密钥材料。
constexpr char kKeySalt[] = "GitHub-Bridge/libsodium/SecretBox/v1";

// magic + salt 的固定前缀，用来说明「这确实是我们写的密文」。
// 不解密时也能据此把「文件坏了」与「换了机器」区分开。
constexpr char kBlobMagic[4] = {'G', 'H', 'B', '1'};
constexpr size_t kNonceBytes = crypto_secretbox_NONCEBYTES;
constexpr size_t kKeyBytes = crypto_secretbox_KEYBYTES;

// 从机器标识与编译期常量派生密钥。
//
// salt 用编译期常量而不是随机值：密钥必须能由同一台机器重新算出来，
// 否则重启后就解不开了。代价是：同机同用户的其他进程也能算出同一个密钥——
// 这是 Linux 上没有 DPAPI 时无法回避的降级，README 里已如实写明。
bool DeriveKey(const std::string& machine_id, unsigned char* key) {
    if (sodium_init() < 0) {
        return false;
    }
    const std::string material = std::string(kKeySalt) + "|" + machine_id;

    // crypto_pwhash 要求 salt 恰好 crypto_pwhash_SALTBYTES 字节，不能为 null。
    unsigned char salt[crypto_pwhash_SALTBYTES] = {};
    const size_t salt_len = sizeof(salt);
    const size_t copy = material.size() < salt_len ? material.size() : salt_len;
    std::memcpy(salt, material.data(), copy);

    // OPSLIMIT/MEMLIMIT 取 interactive 档位：这是导入/读取令牌时才做一次的操作，
    // 用强度换掉一点延迟，不必为速度让步。
    return crypto_pwhash(key, kKeyBytes, material.c_str(), material.size(), salt,
                         crypto_pwhash_OPSLIMIT_INTERACTIVE, crypto_pwhash_MEMLIMIT_INTERACTIVE,
                         crypto_pwhash_ALG_ARGON2ID13) == 0;
}

class SodiumVault : public ITokenVault {
public:
    bool Encrypt(const std::string& plaintext, std::string* ciphertext, std::string* error_message) override;
    bool Decrypt(const std::string& ciphertext, std::string* plaintext, std::string* error_message) override;
    const char* BackendName() const override { return "linux-libsodium"; }
};

bool SodiumVault::Encrypt(const std::string& plaintext, std::string* ciphertext, std::string* error_message) {
    if (ciphertext == nullptr) {
        if (error_message) {
            *error_message = "输出缓冲为空";
        }
        return false;
    }

    const std::string machine_id = platform::Get()->MachineId();
    if (machine_id.empty()) {
        if (error_message) {
            *error_message = "读取 /etc/machine-id 失败，无法派生密钥";
        }
        return false;
    }

    unsigned char key[kKeyBytes] = {};
    if (!DeriveKey(machine_id, key)) {
        if (error_message) {
            *error_message = "密钥派生失败";
        }
        return false;
    }

    unsigned char nonce[kNonceBytes] = {};
    if (!platform::Get()->RandomBytes(nonce, sizeof(nonce))) {
        sodium_memzero(key, sizeof(key));
        if (error_message) {
            *error_message = "生成随机 nonce 失败";
        }
        return false;
    }

    std::string blob;
    blob.append(kBlobMagic, sizeof(kBlobMagic));
    blob.append(reinterpret_cast<const char*>(nonce), sizeof(nonce));

    std::string box(crypto_secretbox_MACBYTES + plaintext.size(), '\0');
    if (crypto_secretbox_easy(reinterpret_cast<unsigned char*>(box.data()),
                              reinterpret_cast<const unsigned char*>(plaintext.data()), plaintext.size(),
                              nonce, key) != 0) {
        sodium_memzero(key, sizeof(key));
        if (error_message) {
            *error_message = "加密失败";
        }
        return false;
    }
    sodium_memzero(key, sizeof(key));

    blob += box;
    *ciphertext = std::move(blob);
    return true;
}

bool SodiumVault::Decrypt(const std::string& ciphertext, std::string* plaintext, std::string* error_message) {
    if (plaintext == nullptr) {
        if (error_message) {
            *error_message = "输出缓冲为空";
        }
        return false;
    }
    plaintext->clear();

    if (ciphertext.size() < sizeof(kBlobMagic) + kNonceBytes + crypto_secretbox_MACBYTES ||
        std::memcmp(ciphertext.data(), kBlobMagic, sizeof(kBlobMagic)) != 0) {
        if (error_message) {
            *error_message = "密文格式无法识别";
        }
        return false;
    }

    const std::string machine_id = platform::Get()->MachineId();
    if (machine_id.empty()) {
        if (error_message) {
            *error_message = "读取 /etc/machine-id 失败，无法派生密钥";
        }
        return false;
    }

    unsigned char key[kKeyBytes] = {};
    if (!DeriveKey(machine_id, key)) {
        if (error_message) {
            *error_message = "密钥派生失败";
        }
        return false;
    }

    const unsigned char* nonce = reinterpret_cast<const unsigned char*>(ciphertext.data() + sizeof(kBlobMagic));
    const unsigned char* box = reinterpret_cast<const unsigned char*>(ciphertext.data() + sizeof(kBlobMagic) + kNonceBytes);
    const size_t box_len = ciphertext.size() - sizeof(kBlobMagic) - kNonceBytes;

    std::string out(box_len - crypto_secretbox_MACBYTES, '\0');
    if (crypto_secretbox_open_easy(reinterpret_cast<unsigned char*>(out.data()), box, box_len, nonce, key) != 0) {
        sodium_memzero(key, sizeof(key));
        if (error_message) {
            *error_message = "解密失败：密钥不匹配或密文损坏";
        }
        return false;
    }
    sodium_memzero(key, sizeof(key));

    *plaintext = std::move(out);
    return true;
}

} // namespace

ITokenVault* ITokenVault::Create() {
    return new SodiumVault();
}

namespace vault {

ITokenVault* Get() {
    static ITokenVault* instance = ITokenVault::Create();
    return instance;
}

} // namespace vault

} // namespace ghb
