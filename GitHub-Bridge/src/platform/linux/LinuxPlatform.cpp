#include "Platform/Platform.h"

#include "Utils/Logger.h"

#include <sys/random.h>
#include <sys/stat.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <pwd.h>
#include <string>
#include <unistd.h>

namespace ghb {

namespace {

class LinuxPlatform : public IPlatform {
public:
    bool RandomBytes(void* buffer, size_t size) override;
    void SecureZero(void* data, size_t size) override;
    void SleepMs(int milliseconds) override;
    std::string MachineId() override;
    std::string AppDataDir() override;
    bool RestrictFileToCurrentUser(const std::string& path, std::string* error_message) override;
    bool RestrictDirToCurrentUser(const std::string& path, std::string* error_message) override;
};

bool LinuxPlatform::RandomBytes(void* buffer, size_t size) {
    if (size == 0) {
        return true;
    }
    auto* cursor = static_cast<unsigned char*>(buffer);
    size_t remaining = size;
    while (remaining > 0) {
        const ssize_t got = getrandom(cursor, remaining, 0);
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        cursor += got;
        remaining -= static_cast<size_t>(got);
    }
    return true;
}

void LinuxPlatform::SecureZero(void* data, size_t size) {
    // explicit_bzero 保证不被优化掉；glibc 2.25+ 提供。
    explicit_bzero(data, size);
}

void LinuxPlatform::SleepMs(int milliseconds) {
    usleep(static_cast<useconds_t>(milliseconds) * 1000);
}

std::string LinuxPlatform::MachineId() {
    // systemd 与大多数发行版都提供这个文件；读不到时返回空串由调用方处理。
    std::ifstream in("/etc/machine-id");
    if (!in) {
        return {};
    }
    std::string id;
    std::getline(in, id);
    // 去掉可能的空白
    const size_t first = id.find_first_not_of(" \t\r\n");
    const size_t last = id.find_last_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    return id.substr(first, last - first + 1);
}

std::string LinuxPlatform::AppDataDir() {
    // 遵循 XDG：$XDG_CONFIG_HOME，缺省 ~/.config
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg != nullptr && xdg[0] != '\0') {
        return std::string(xdg) + "/GitHub-Bridge";
    }

    const char* home = std::getenv("HOME");
    if (home == nullptr || home[0] == '\0') {
        const passwd* pw = getpwuid(getuid());
        if (pw != nullptr && pw->pw_dir != nullptr) {
            home = pw->pw_dir;
        }
    }
    if (home == nullptr || home[0] == '\0') {
        return {};
    }
    return std::string(home) + "/.config/GitHub-Bridge";
}

bool LinuxPlatform::RestrictFileToCurrentUser(const std::string& path, std::string* error_message) {
    if (chmod(path.c_str(), S_IRUSR | S_IWUSR) != 0) {
        if (error_message) {
            *error_message = "chmod 0600 失败: " + std::string(std::strerror(errno));
        }
        return false;
    }
    return true;
}

bool LinuxPlatform::RestrictDirToCurrentUser(const std::string& path, std::string* error_message) {
    if (chmod(path.c_str(), S_IRWXU) != 0) {
        if (error_message) {
            *error_message = "chmod 0700 失败: " + std::string(std::strerror(errno));
        }
        return false;
    }
    return true;
}

} // namespace

IPlatform* IPlatform::Create() {
    return new LinuxPlatform();
}

namespace platform {

IPlatform* Get() {
    static IPlatform* instance = IPlatform::Create();
    return instance;
}

} // namespace platform

} // namespace ghb
