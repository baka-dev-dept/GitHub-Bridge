#include "Platform/Platform.h"

#include "Utils/Logger.h"

#include <windows.h>
#include <aclapi.h>
#include <bcrypt.h>
#include <shlobj.h>
#include <sddl.h>

#include <cstring>
#include <filesystem>
#include <vector>

namespace ghb {

namespace {

std::string ToUtf8(const std::wstring& wide) {
    if (wide.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return {};
    }
    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, out.data(), size, nullptr, nullptr);
    out.resize(static_cast<size_t>(size) - 1);
    return out;
}

std::wstring ToWide(const std::string& utf8) {
    if (utf8.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (size <= 0) {
        return {};
    }
    std::wstring out(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, out.data(), size);
    out.resize(static_cast<size_t>(size) - 1);
    return out;
}

class WindowsPlatform : public IPlatform {
public:
    bool RandomBytes(void* buffer, size_t size) override;
    void SecureZero(void* data, size_t size) override;
    void SleepMs(int milliseconds) override;
    std::string MachineId() override;
    std::string AppDataDir() override;
    bool RestrictFileToCurrentUser(const std::string& path, std::string* error_message) override;
    bool RestrictDirToCurrentUser(const std::string& path, std::string* error_message) override;

private:
    bool ApplyOwnerOnlyAcl(const std::string& path, bool is_directory, std::string* error_message);
};

bool WindowsPlatform::RandomBytes(void* buffer, size_t size) {
    // BCryptGenRandom 返回 NTSTATUS，成功是 0；写成 !调用 会把成功当失败。
    const NTSTATUS status = BCryptGenRandom(nullptr, static_cast<PUCHAR>(buffer), static_cast<ULONG>(size), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return status == 0;
}

void WindowsPlatform::SecureZero(void* data, size_t size) {
    SecureZeroMemory(data, size);
}

void WindowsPlatform::SleepMs(int milliseconds) {
    Sleep(static_cast<DWORD>(milliseconds));
}

std::string WindowsPlatform::MachineId() {
    wchar_t buffer[256] = {};
    DWORD size = sizeof(buffer);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Cryptography", L"MachineGuid", RRF_RT_REG_SZ, nullptr, buffer, &size) != ERROR_SUCCESS) {
        return {};
    }
    return ToUtf8(buffer);
}

std::string WindowsPlatform::AppDataDir() {
    std::string base;
    PWSTR roaming = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &roaming))) {
        base = ToUtf8(roaming);
        CoTaskMemFree(roaming);
    }
    if (base.empty()) {
        char expanded[MAX_PATH] = {};
        if (ExpandEnvironmentStringsA("%APPDATA%", expanded, MAX_PATH) > 0) {
            base = expanded;
        }
    }
    if (base.empty()) {
        return {};
    }
    return base + "\\GitHub-Bridge";
}

bool WindowsPlatform::RestrictFileToCurrentUser(const std::string& path, std::string* error_message) {
    return ApplyOwnerOnlyAcl(path, false, error_message);
}

bool WindowsPlatform::RestrictDirToCurrentUser(const std::string& path, std::string* error_message) {
    // 目录的 ACL 由文件继承处理；这里只需保证目录本身也排除其他用户。
    return ApplyOwnerOnlyAcl(path, true, error_message);
}

bool WindowsPlatform::ApplyOwnerOnlyAcl(const std::string& path, bool is_directory, std::string* error_message) {
    // 只给当前用户完全控制，并移除继承：默认继承会把「本机 Users」等组带进来。
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        if (error_message) {
            *error_message = "打开进程令牌失败，错误码 " + std::to_string(GetLastError());
        }
        return false;
    }

    DWORD token_size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &token_size);
    std::vector<BYTE> token_buffer(token_size);
    if (!GetTokenInformation(token, TokenUser, token_buffer.data(), token_size, &token_size)) {
        CloseHandle(token);
        if (error_message) {
            *error_message = "查询当前用户 SID 失败";
        }
        return false;
    }
    CloseHandle(token);

    const PSID user_sid = reinterpret_cast<TOKEN_USER*>(token_buffer.data())->User.Sid;
    LPWSTR sid_string = nullptr;
    if (!ConvertSidToStringSidW(user_sid, &sid_string)) {
        if (error_message) {
            *error_message = "SID 转字符串失败";
        }
        return false;
    }

    std::wstring sddl = L"D:P(A;;FA;;;";
    sddl += sid_string;
    sddl += L")";
    LocalFree(sid_string);

    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) {
        if (error_message) {
            *error_message = "构造安全描述符失败";
        }
        return false;
    }

    BOOL dacl_present = FALSE;
    PACL dacl = nullptr;
    BOOL dacl_defaulted = FALSE;
    if (!GetSecurityDescriptorDacl(descriptor, &dacl_present, &dacl, &dacl_defaulted) || !dacl_present) {
        LocalFree(descriptor);
        if (error_message) {
            *error_message = "安全描述符缺少 DACL";
        }
        return false;
    }

    const std::wstring wide_path = ToWide(path);
    const DWORD rc = SetNamedSecurityInfoW(const_cast<LPWSTR>(wide_path.c_str()), SE_FILE_OBJECT,
                                           DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                                           nullptr, nullptr, dacl, nullptr);
    LocalFree(descriptor);

    if (rc != ERROR_SUCCESS) {
        if (error_message) {
            *error_message = std::string(is_directory ? "设置目录 ACL 失败，错误码 " : "设置 ACL 失败，错误码 ") + std::to_string(rc);
        }
        return false;
    }
    return true;
}

} // namespace

IPlatform* IPlatform::Create() {
    return new WindowsPlatform();
}

} // namespace ghb
