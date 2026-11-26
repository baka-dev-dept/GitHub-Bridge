#pragma once

#include "Types/RpcMessage.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace ghb {

// 操作类别。PermissionManager 按类别裁决，不按工具名硬编码，
// 新增工具只要声明正确类别就自动纳入权限体系。
enum class OperationKind : int {
    Read = 0,
    Write = 1,
    Create = 2,
    Delete = 3,
    Admin = 4,
};

// 不可逆操作必须逐次运行时确认，见 idea.md「危险操作的运行时二次确认」。
struct ToolSafety {
    OperationKind kind = OperationKind::Read;
    // true 时工具首次返回 confirmation_required 并下发一次性 confirm_token。
    bool requires_confirmation = false;
    // 配置开关名，对应 permissions 配置里的字段；空表示没有独立开关（读操作）。
    std::string permission_key;
};

struct ToolAnnotations {
    bool read_only_hint = false;
    bool destructive_hint = false;
    bool idempotent_hint = false;
    bool open_world_hint = false;
};

struct McpTool {
    std::string name;
    std::string title;
    std::string description;
    // JSON Schema 文本；参数校验与展示都以它为准，避免两处定义漂移。
    std::string input_schema_json = R"({"type":"object","properties":{}})";
    ToolAnnotations annotations;
    ToolSafety safety;
};

// 工具实现签名。入参是已解析的 arguments 对象文本，出参是 MCP content/text 的正文。
// 实现内不得回显 Token；敏感字段在 Formatter 层再兜底脱敏。
using ToolHandler = std::string (*)(const std::string& arguments_json);

struct ToolRegistration {
    McpTool tool;
    ToolHandler handler = nullptr;
};

// 工具调用结果。ok=false 时回 JSON-RPC error，ok=true 时回 result。
struct ToolResult {
    bool ok = true;
    std::string payload_json = "{}";
    RpcError error;
    // 业务错误时给 Agent 的下一步建议，放在 error.detail 里，不进 stdout 噪声。
    std::vector<std::string> hints;
};

} // namespace ghb
