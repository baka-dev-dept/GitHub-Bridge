#include "Core/BuiltinTools.h"

#include "Core/GitHubTools.h"
#include "Core/ToolRegistry.h"
#include "Core/TokenTools.h"
#include "Types/McpTool.h"

#include <nlohmann/json.hpp>

#include <ctime>

namespace ghb {

namespace {

// Phase 1 自检工具：证明「注册 → 列出 → 调用 → 回包」整条链路可用。
// 不碰网络、不碰 Token，可以放心在无凭据环境下调用。
std::string HandleServerInfo(const std::string&) {
    std::time_t now = std::time(nullptr);
    std::tm utc{};
    gmtime_s(&utc, &now);
    char stamp[32] = {};
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%SZ", &utc);

    const nlohmann::json info = {
        {"name", McpServerInfo{}.name},
        {"version", McpServerInfo{}.version},
        {"protocol_version", kMcpProtocolVersion},
        {"tool_count", ToolRegistry::Get()->Size()},
        {"server_time_utc", stamp},
        {"phase", "3"},
    };
    return info.dump();
}

} // namespace

void RegisterBuiltinTools() {
    ToolRegistration info;
    info.tool.name = "github_server_info";
    info.tool.title = "Server info";
    info.tool.description = "返回 GitHub-Bridge 自身版本、MCP 协议版本、已注册工具数量与服务器时间。不访问网络。";
    info.tool.input_schema_json = R"({"type":"object","properties":{},"additionalProperties":false})";
    info.tool.annotations.read_only_hint = true;
    info.tool.annotations.idempotent_hint = true;
    info.tool.annotations.open_world_hint = false;
    info.tool.safety.kind = OperationKind::Read;
    info.handler = &HandleServerInfo;

    ToolRegistry::Get()->Register(info);

    RegisterTokenTools();
    RegisterGitHubTools();
}

} // namespace ghb
