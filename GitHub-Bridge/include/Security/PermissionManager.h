#pragma once

#include "Types/Permission.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ghb {

// 权限裁决结果，供 Gateway 决定「直接执行 / 要求确认 / 拒绝」。
enum class PermissionVerdict : int {
    Allow = 0,
    // 需要二次确认：首次调用下发 confirm_token，携带正确的 token 再来才放行。
    NeedConfirm = 1,
    // 被配置开关禁止。
    Denied = 2,
    // 提供的 confirm_token 无效、过期、已用过或目标不匹配。
    BadConfirmToken = 3,
};

struct PermissionOutcome {
    PermissionVerdict verdict = PermissionVerdict::Allow;
    // NeedConfirm 时是一次性票据；其余情况为空。
    std::string confirm_token;
    // 票据过期时刻（unix 毫秒），供响应里告知 Agent。
    int64_t expires_at_unix_ms = 0;
    // Denied / BadConfirmToken 时的可读原因。
    std::string reason;
};

// 权限管理。两条互相独立的防线，缺一不可：
//   1. 配置开关：按 OperationKind 与工具的 permission_key 裁决；
//   2. 运行时二次确认：不可逆操作必须携带与目标绑定的一次性票据。
// 只有开关打开不代表可以随便删——开关只是必要条件，不是充分条件。
class PermissionManager {
public:
    static PermissionManager* Get() {
        static PermissionManager i;
        return &i;
    }

    // 从 %APPDATA%\GitHub-Bridge\permissions.json 加载。文件不存在时用默认值。
    // 未知字段忽略（前向兼容）。
    void LoadConfig();

    // 直接设置配置，主要给测试与后续的配置工具用。
    void ApplyConfig(const PermissionConfig& config) { config_ = config; }
    const PermissionConfig& Config() const { return config_; }

    // 裁决一次工具调用。
    //   requires_confirmation：工具是否声明为不可逆操作；
    //   permission_key：工具声明的配置开关名，空表示不受开关约束；
    //   target_fingerprint：目标对象指纹，票据必须与它一一对应；
    //   provided_token：调用方带来的 confirm_token，首次调用传空；
    //   arguments_json：本次调用的原始参数，用于仓库白名单判定。
    PermissionOutcome Check(const std::string& tool_name, bool requires_confirmation, const std::string& permission_key, const std::string& target_fingerprint, const std::string& provided_token, const std::string& arguments_json);

    // 从参数里取 owner/repo 与白名单比对。没有明确的单仓库目标时返回 true
    // （如 search_repositories、github_get_user 这类工具不受白名单约束）。
    bool IsRepositoryAllowed(const std::string& arguments_json) const;

    // 通配匹配：支持 '*' 匹配任意字符，比较时忽略大小写（GitHub 的
    // owner/repo 本身不区分大小写）。单独暴露以便单元测试。
    static bool MatchRepositoryPattern(const std::string& pattern, const std::string& owner_repo);

    // 计算目标指纹。同一目标得到同一指纹，目标一变即不同，
    // 这是「改目标即失效」的实现基础。
    static std::string Fingerprint(const std::string& tool_name, const std::string& arguments_json);

    // 票据有效期，默认 60 秒，符合 idea.md 的约定。
    static constexpr int64_t kTicketTtlMs = 60 * 1000;

private:
    PermissionManager() = default;
    ~PermissionManager() = default;

    // 按 permission_key 查配置开关。未知 key 一律按拒绝处理，
    // 避免新增工具忘记登记开关时被静默放行。
    bool IsSwitchEnabled(const std::string& permission_key) const;

    void PurgeExpired();

    PermissionConfig config_;
    // 只保留未过期的票据。数量很小，线性查找足够。
    std::vector<ConfirmTicket> tickets_;
};

} // namespace ghb
