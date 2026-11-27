#pragma once

#include <string>

namespace ghb {

// 令牌加解密接口。只管把一段明文变成密文，不碰文件、不碰 ACL。
//
// 两个平台的安全强度不同，这一点必须显式承认而不是假装一致：
//   Windows  —— DPAPI，密钥由系统托管并绑定当前用户；同机同账号的其他进程
//               在没有 entropy 的情况下也解不开。
//   Linux    —— 没有等价的系统级密钥保管，密钥从 /etc/machine-id 与编译期
//               常量派生。防的是「文件被拷到别的机器」，防不住同机同用户的
//               其他进程。这是真实的降级，不是实现疏忽。
class ITokenVault {
public:
    virtual ~ITokenVault() = default;

    virtual bool Encrypt(const std::string& plaintext, std::string* ciphertext, std::string* error_message) = 0;
    virtual bool Decrypt(const std::string& ciphertext, std::string* plaintext, std::string* error_message) = 0;

    static ITokenVault* Create();

    // 给上层如实展示用的平台说明，会写进状态与日志。
    virtual const char* BackendName() const = 0;
};

namespace vault {

// 进程级唯一实例。业务代码统一用它，避免每次加解密都新建对象。
ITokenVault* Get();

} // namespace vault

} // namespace ghb
