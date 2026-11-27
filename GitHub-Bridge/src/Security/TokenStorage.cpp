#include "Security/TokenStorage.h"

#include "Utils/Logger.h"

#include <windows.h>
#include <wincrypt.h>
#include <dpapi.h>
#include <sddl.h>
#include <shlobj.h>
#include <aclapi.h>

#include <vector>

namespace ghb {

namespace {

// 熵的静态部分。单独使用没有意义：同一台机器上人人相同，等于没有第二因子。
// 必须与 MachineGuid 组合，换机即失效，这正是「解密失败要重配」的正确语义。
constexpr char kEntropySalt[] = "GitHub-Bridge/DPAPI/v1/entropy";

constexpr char kAppDirName[] = "GitHub-Bridge";
constexpr char kDataFileName[] = "tokens.dat";

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

std::string ToUtf8(const wchar_t* wide) {
    if (wide == nullptr || *wide == L'\0') {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return {};
    }
    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), size, nullptr, nullptr);
    out.resize(static_cast<size_t>(size) - 1);
    return out;
}

// 读取 MachineGuid 文本。取证类工具经常改写它，读不到时返回空串由调用方降级。
std::string ReadMachineGuid() {
    wchar_t buffer[256] = {};
    DWORD size = sizeof(buffer) - sizeof(wchar_t);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Cryptography", L"MachineGuid", RRF_RT_REG_SZ, nullptr, buffer, &size) != ERROR_SUCCESS) {
        return {};
    }
    return ToUtf8(buffer);
}

std::string Sha256Hex(const std::string& input) {
    HCRYPTPROV provider = 0;
    HCRYPTHASH hash = 0;
    std::string result;

    if (!CryptAcquireContextW(&provider, nullptr, MS_ENH_RSA_AES_PROV_W, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) {
        return result;
    }
    if (CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash)) {
        if (CryptHashData(hash, reinterpret_cast<const BYTE*>(input.data()), static_cast<DWORD>(input.size()), 0)) {
            BYTE digest[32] = {};
            DWORD digest_size = sizeof(digest);
            if (CryptGetHashParam(hash, HP_HASHVAL, digest, &digest_size, 0)) {
                static constexpr char kHex[] = "0123456789abcdef";
                for (DWORD i = 0; i < digest_size; ++i) {
                    result.push_back(kHex[digest[i] >> 4]);
                    result.push_back(kHex[digest[i] & 0x0F]);
                }
            }
        }
        CryptDestroyHash(hash);
    }
    CryptReleaseContext(provider, 0);
    return result;
}

} // namespace

std::string TokenStorage::DeriveEntropy() {
    std::string material = kEntropySalt;

    const std::string machine_guid = ReadMachineGuid();
    if (machine_guid.empty()) {
        Logger::Get()->Warn("读取 MachineGuid 失败，DPAPI 第二因子降级为仅使用编译期常量");
    } else {
        material += "|";
        material += machine_guid;
    }

    const std::string digest = Sha256Hex(material);
    if (digest.empty()) {
        // 连哈希都算不出来时退化为原始材料，仍然比空熵强。
        return material;
    }
    return digest;
}

TokenStoragePaths TokenStorage::ResolvePaths() const {
    TokenStoragePaths paths;
    paths.data_file = DataFilePath();
    paths.log_dir = AppDirPath() + "\\logs";
    return paths;
}

std::string TokenStorage::AppDirPath() const {
    std::string base;
    PWSTR roaming = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &roaming))) {
        base = ToUtf8(roaming);
        CoTaskMemFree(roaming);
    }
    if (base.empty()) {
        // 兜底：已知文件夹 API 失败时用环境变量，仍在用户配置目录树下。
        char expanded[MAX_PATH] = {};
        if (ExpandEnvironmentStringsA("%APPDATA%", expanded, MAX_PATH) > 0) {
            base = expanded;
        }
    }
    if (base.empty()) {
        return {};
    }
    return base + "\\" + kAppDirName;
}

std::string TokenStorage::DataFilePath() const {
    const std::string dir = AppDirPath();
    if (dir.empty()) {
        return {};
    }
    return dir + "\\" + kDataFileName;
}

bool TokenStorage::EnsureAppDir(std::string* error_message) const {
    const std::string dir = AppDirPath();
    if (dir.empty()) {
        if (error_message) {
            *error_message = "无法解析 %APPDATA% 目录";
        }
        return false;
    }

    const std::wstring wide_dir = ToWide(dir);
    if (wide_dir.empty()) {
        if (error_message) {
            *error_message = "路径编码转换失败";
        }
        return false;
    }

    const int rc = SHCreateDirectoryExW(nullptr, wide_dir.c_str(), nullptr);
    if (rc != ERROR_SUCCESS && rc != ERROR_ALREADY_EXISTS) {
        if (error_message) {
            *error_message = "创建数据目录失败，错误码 " + std::to_string(rc);
        }
        return false;
    }
    return true;
}

bool TokenStorage::Save(const std::string& plaintext, std::string* error_message) {
    if (!EnsureAppDir(error_message)) {
        return false;
    }

    const std::string entropy = DeriveEntropy();
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

    const std::string final_path = DataFilePath();
    const std::string tmp_path = final_path + ".tmp";
    const std::wstring wide_final = ToWide(final_path);
    const std::wstring wide_tmp = ToWide(tmp_path);

    bool ok = false;
    HANDLE file = CreateFileW(wide_tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        if (error_message) {
            *error_message = "创建临时文件失败，错误码 " + std::to_string(GetLastError());
        }
    } else {
        DWORD written = 0;
        const BOOL write_ok = WriteFile(file, output.pbData, output.cbData, &written, nullptr);
        if (write_ok && written == output.cbData) {
            // 先落盘再改名，避免断电留下半截密文。
            FlushFileBuffers(file);
            ok = true;
        } else if (error_message) {
            *error_message = "写入临时文件失败，错误码 " + std::to_string(GetLastError());
        }
        CloseHandle(file);
    }
    LocalFree(output.pbData);

    if (!ok) {
        DeleteFileW(wide_tmp.c_str());
        return false;
    }

    if (!MoveFileExW(wide_tmp.c_str(), wide_final.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        if (error_message) {
            *error_message = "替换令牌文件失败，错误码 " + std::to_string(GetLastError());
        }
        DeleteFileW(wide_tmp.c_str());
        return false;
    }

    // ACL 收紧失败不应该让保存整体失败：密文已经落盘，只是权限没达到最严。
    std::string acl_error;
    if (!RestrictAcl(final_path, &acl_error)) {
        Logger::Get()->Warn("收紧令牌文件 ACL 失败: " + acl_error);
    }
    return true;
}

TokenLoadStatus TokenStorage::Load(std::string* plaintext) {
    if (plaintext == nullptr) {
        return TokenLoadStatus::IoError;
    }
    plaintext->clear();

    const std::wstring wide_path = ToWide(DataFilePath());
    if (wide_path.empty()) {
        return TokenLoadStatus::IoError;
    }

    HANDLE file = CreateFileW(wide_path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD err = GetLastError();
        return (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) ? TokenLoadStatus::NotFound
                                                                           : TokenLoadStatus::IoError;
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > 1024 * 1024) {
        CloseHandle(file);
        return TokenLoadStatus::Corrupted;
    }

    std::vector<BYTE> buffer(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    const BOOL read_ok = ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr);
    CloseHandle(file);
    if (!read_ok || read != buffer.size()) {
        return TokenLoadStatus::IoError;
    }

    const std::string entropy = DeriveEntropy();
    DATA_BLOB input{};
    input.pbData = buffer.data();
    input.cbData = read;

    DATA_BLOB entropy_blob{};
    entropy_blob.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(entropy.data()));
    entropy_blob.cbData = static_cast<DWORD>(entropy.size());

    DATA_BLOB output{};
    if (!CryptUnprotectData(&input, nullptr, &entropy_blob, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output)) {
        const DWORD err = GetLastError();
        // DPAPI 对「熵不对」和「数据被破坏」返回同一个错误，按经验区分：
        // 文件头不是 DPAPI 签名视为损坏，其余视为熵/用户上下文不匹配。
        const bool looks_like_dpapi = read >= 32 && buffer[0] != 0x00;
        if (!looks_like_dpapi || err == ERROR_INVALID_DATA) {
            return TokenLoadStatus::Corrupted;
        }
        return TokenLoadStatus::EntropyMismatch;
    }

    plaintext->assign(reinterpret_cast<const char*>(output.pbData), output.cbData);
    SecureZeroMemory(output.pbData, output.cbData);
    LocalFree(output.pbData);
    return TokenLoadStatus::Ok;
}

bool TokenStorage::Erase(std::string* error_message) {
    const std::wstring wide_path = ToWide(DataFilePath());
    if (wide_path.empty()) {
        if (error_message) {
            *error_message = "无法解析令牌文件路径";
        }
        return false;
    }
    if (DeleteFileW(wide_path.c_str())) {
        return true;
    }
    const DWORD err = GetLastError();
    if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
        return true;
    }
    if (error_message) {
        *error_message = "删除令牌文件失败，错误码 " + std::to_string(err);
    }
    return false;
}

bool TokenStorage::Exists() const {
    const std::wstring wide_path = ToWide(DataFilePath());
    if (wide_path.empty()) {
        return false;
    }
    return GetFileAttributesW(wide_path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool TokenStorage::RestrictAcl(const std::string& path, std::string* error_message) const {
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
    const DWORD rc = SetNamedSecurityInfoW(const_cast<LPWSTR>(wide_path.c_str()), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, dacl, nullptr);
    LocalFree(descriptor);

    if (rc != ERROR_SUCCESS) {
        if (error_message) {
            *error_message = "设置 ACL 失败，错误码 " + std::to_string(rc);
        }
        return false;
    }
    return true;
}

} // namespace ghb
