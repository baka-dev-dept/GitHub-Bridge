#include "Core/Gateway.h"

#include "Core/ToolRegistry.h"
#include "MCP/JsonCodec.h"
#include "MCP/StdioTransport.h"
#include "Security/PermissionManager.h"
#include "Utils/Logger.h"

#include <nlohmann/json.hpp>

namespace ghb {

using nlohmann::json;

namespace {

json ToolToJson(const McpTool& tool) {
    const json schema = json::parse(tool.input_schema_json, nullptr, false);
    json node = {
        {"name", tool.name},
        {"description", tool.description},
        {"inputSchema", schema.is_discarded() ? json{{"type", "object"}, {"properties", json::object()}} : schema},
        {"annotations", {
            {"readOnlyHint", tool.annotations.read_only_hint},
            {"destructiveHint", tool.annotations.destructive_hint},
            {"idempotentHint", tool.annotations.idempotent_hint},
            {"openWorldHint", tool.annotations.open_world_hint},
        }},
    };
    if (!tool.title.empty()) {
        node["title"] = tool.title;
    }
    return node;
}

RpcError MakeError(int code, const std::string& message, const std::string& detail) {
    RpcError error;
    error.code = code;
    error.message = message;
    error.detail = detail;
    return error;
}

} // namespace

int Gateway::Run() {
    Logger::Get()->Info("GitHub-Bridge 启动，等待 MCP 消息");

    std::string line;
    while (StdioTransport::Get()->ReadLine(&line)) {
        if (line.empty()) {
            continue;
        }

        const DecodeResult decoded = JsonCodec::Decode(line);
        if (!decoded.ok) {
            // 能识别出 id 就回显（便于客户端配对），否则按规范回 id=null。
            Logger::Get()->Warn("消息解码失败: " + decoded.error.detail);
            const std::string response = decoded.message.has_id
                                             ? JsonCodec::EncodeError(decoded.message, decoded.error)
                                             : JsonCodec::EncodeErrorNullId(decoded.error);
            StdioTransport::Get()->WriteLine(response);
            continue;
        }

        const std::string response = Handle(decoded.message);
        if (!response.empty() && !StdioTransport::Get()->WriteLine(response)) {
            Logger::Get()->Error("写入 stdout 失败，退出主循环");
            return 1;
        }
    }

    Logger::Get()->Info("stdin 已关闭，正常退出");
    return 0;
}

std::string Gateway::Handle(const RpcMessage& request) {
    const std::string& method = request.method;

    // 通知永远不回包，JSON-RPC 2.0 硬性要求。
    if (JsonCodec::IsNotification(request)) {
        Logger::Get()->Debug("收到通知: " + method);
        return {};
    }

    if (method == "initialize") {
        return HandleInitialize(request);
    }
    if (method == "ping") {
        return JsonCodec::EncodeResponse(request, "{}");
    }
    if (method == "tools/list") {
        return HandleToolsList(request);
    }
    if (method == "tools/call") {
        return HandleToolsCall(request);
    }

    Logger::Get()->Warn("未知方法: " + method);
    return JsonCodec::EncodeError(request, MakeError(kJsonRpcMethodNotFound, "Method not found", "未实现的方法: " + method));
}

std::string Gateway::HandleInitialize(const RpcMessage& request) {
    json client_version = nullptr;
    const json params = json::parse(request.params_json, nullptr, false);
    if (!params.is_discarded() && params.is_object()) {
        const auto it = params.find("protocolVersion");
        if (it != params.end()) {
            client_version = *it;
        }
    }

    // 协议版本固定 2024-11-05，不做协商漂移：客户端版本不同就如实回本服务端支持的版本。
    if (!client_version.is_null() && (!client_version.is_string() || client_version.get<std::string>() != kMcpProtocolVersion)) {
        Logger::Get()->Warn("客户端协议版本与固定版本不一致: " + client_version.dump());
    }

    const json result = {
        {"protocolVersion", kMcpProtocolVersion},
        {"capabilities", {{"tools", json::object()}}},
        {"serverInfo", {{"name", McpServerInfo{}.name}, {"version", McpServerInfo{}.version}}},
    };

    initialized_ = true;
    Logger::Get()->Info("initialize 完成，协议版本 " + std::string(kMcpProtocolVersion));
    return JsonCodec::EncodeResponse(request, result.dump());
}

std::string Gateway::HandleToolsList(const RpcMessage& request) {
    json tools = json::array();
    for (const auto& registration : ToolRegistry::Get()->List()) {
        tools.push_back(ToolToJson(registration.tool));
    }
    const json result = {{"tools", tools}};
    return JsonCodec::EncodeResponse(request, result.dump());
}

std::string Gateway::HandleToolsCall(const RpcMessage& request) {
    const json params = json::parse(request.params_json, nullptr, false);
    if (params.is_discarded() || !params.is_object()) {
        return JsonCodec::EncodeError(request, MakeError(kJsonRpcInvalidParams, "Invalid params", "params 必须是对象"));
    }

    const auto name_it = params.find("name");
    if (name_it == params.end() || !name_it->is_string()) {
        return JsonCodec::EncodeError(request, MakeError(kJsonRpcInvalidParams, "Invalid params", "缺少字符串类型的 name"));
    }
    const std::string tool_name = name_it->get<std::string>();

    const ToolRegistration* registration = ToolRegistry::Get()->Find(tool_name);
    if (registration == nullptr) {
        Logger::Get()->Warn("调用未注册的工具: " + tool_name);
        return JsonCodec::EncodeError(request, MakeError(kJsonRpcMethodNotFound, "Method not found", "未注册的工具: " + tool_name));
    }

    std::string arguments = "{}";
    const auto args_it = params.find("arguments");
    if (args_it != params.end() && !args_it->is_null()) {
        if (!args_it->is_object()) {
            return JsonCodec::EncodeError(request, MakeError(kJsonRpcInvalidParams, "Invalid params", "arguments 必须是对象"));
        }
        arguments = args_it->dump();
    }

    // 权限裁决必须在执行前完成，且对每个工具都走一遍：
    // 按工具声明的 permission_key 查配置开关，不可逆操作还要求一次性确认票据。
    std::string confirm_token;
    {
        const json parsed_args = json::parse(arguments, nullptr, false);
        if (parsed_args.is_object()) {
            const auto token_it = parsed_args.find("confirm_token");
            if (token_it != parsed_args.end() && token_it->is_string()) {
                confirm_token = token_it->get<std::string>();
            }
        }
    }
    const std::string fingerprint = PermissionManager::Fingerprint(tool_name, arguments);
    const PermissionOutcome permission = PermissionManager::Get()->Check(tool_name, registration->tool.safety.requires_confirmation, registration->tool.safety.permission_key, fingerprint, confirm_token);

    if (permission.verdict == PermissionVerdict::Denied ||
        permission.verdict == PermissionVerdict::BadConfirmToken) {
        const json denied = {
            {"ok", false},
            {"status", permission.verdict == PermissionVerdict::Denied ? "permission_denied" : "bad_confirm_token"},
            {"reason", permission.reason},
        };
        return JsonCodec::EncodeResponse(request, json{{"content", json::array({{{"type", "text"}, {"text", denied.dump()}}})},
                                                      {"isError", true}}
                                                    .dump());
    }

    if (permission.verdict == PermissionVerdict::NeedConfirm) {
        // 首次调用：不下发 status 为「已执行」，而是把票据交给 Agent，等它带着票据再来一次。
        const json pending = {
            {"ok", false},
            {"status", "confirmation_required"},
            {"confirm_token", permission.confirm_token},
            {"expires_at_unix_ms", permission.expires_at_unix_ms},
            {"reason", "这是不可逆操作，请携带相同的 arguments 再加上 confirm_token 字段重新调用"},
        };
        return JsonCodec::EncodeResponse(request, json{{"content", json::array({{{"type", "text"}, {"text", pending.dump()}}})},
                                                       {"isError", false}}
                                                     .dump());
    }

    std::string text;
    bool is_error = false;
    try {
        text = registration->handler(arguments);
    } catch (const std::exception& ex) {
        Logger::Get()->Error("工具执行异常: " + tool_name + " -> " + ex.what());
        return JsonCodec::EncodeError(request, MakeError(kJsonRpcInternalError, "Internal error", ex.what()));
    } catch (...) {
        Logger::Get()->Error("工具执行发生未知异常: " + tool_name);
        return JsonCodec::EncodeError(request, MakeError(kJsonRpcInternalError, "Internal error", "工具执行发生未知异常"));
    }

    if (text.empty()) {
        is_error = true;
        text = "工具没有返回内容";
    }

    const json result = {
        {"content", json::array({{{"type", "text"}, {"text", text}}})},
        {"isError", is_error},
    };
    return JsonCodec::EncodeResponse(request, result.dump());
}

} // namespace ghb
