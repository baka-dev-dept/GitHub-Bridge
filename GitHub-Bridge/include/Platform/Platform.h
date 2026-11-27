#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace ghb {

// 各平台零散的系统能力。集中在这里，业务代码就不必到处写 #ifdef。
class IPlatform {
public:
    virtual ~IPlatform() = default;

    // 密码学安全随机数。确认票据靠它，用错等于没有防护。
    virtual bool RandomBytes(void* buffer, size_t size) = 0;

    // 清零且不被编译器优化掉。普通 memset 会被当死存储删除。
    virtual void SecureZero(void* data, size_t size) = 0;

    virtual void SleepMs(int milliseconds) = 0;

    // 机器标识。用作加密第二因子的一部分：换机即解密失败，这正是
    // 「需要重新导入」的正确语义。读不到时返回空串，由调用方降级。
    virtual std::string MachineId() = 0;

    // 应用数据目录，不带结尾分隔符。
    virtual std::string AppDataDir() = 0;

    // 把文件权限收紧到只有当前用户可读写。返回 false 表示设置失败。
    virtual bool RestrictFileToCurrentUser(const std::string& path, std::string* error_message) = 0;

    // 目录只允许当前用户访问（Linux 上是 0700；Windows 依靠父目录继承）。
    virtual bool RestrictDirToCurrentUser(const std::string& path, std::string* error_message) = 0;

    static IPlatform* Create();
};

namespace platform {

// 进程级唯一实例，首次调用时创建。
// inline 函数里的 static 局部变量由标准保证全程序只有一份，
// 所以定义放在头文件里不会产生多个实例，也省掉各平台实现里重复的同一段代码。
inline IPlatform* Get() {
    static IPlatform* instance = IPlatform::Create();
    return instance;
}

} // namespace platform

} // namespace ghb
