#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ghb {

// 权限开关。字段与 idea.md「Permission System」的配置 JSON 一一对应，
// 未列出的字段一律忽略（前向兼容）。
struct PermissionConfig {
    bool read = true;
    bool write = true;
    bool create = true;
    bool delete_repository = false;
    bool delete_branch = false;
    bool force_push = false;
    bool merge_pull_request = true;
};

// 已下发的确认票据。绑定具体目标指纹，改目标即失效。
struct ConfirmTicket {
    std::string token;
    std::string tool_name;
    std::string target_fingerprint;
    int64_t expires_at_unix_ms = 0;
    bool used = false;
};

struct PermissionDecision {
    bool allowed = false;
    // allowed=false 且 need_confirm=true 时，上层返回 confirmation_required
    // 而不是错误，以便 Agent 拿着 confirm_token 重试。
    bool need_confirm = false;
    std::string confirm_token;
    std::string reason;
};

} // namespace ghb
