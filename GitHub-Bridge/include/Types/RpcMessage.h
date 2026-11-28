#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "Version.h"

namespace ghb {

// JSON-RPC 2.0 错误码。协议层固定值写入规范，见 idea.md「MCP 协议版本」。
// -32700 Parse error    -32600 Invalid Request
// -32601 Method not found -32602 Invalid params
// -32603 Internal error
// -32000 ~ -32099 服务端自定义区，本项目的业务错误统一用 BusinessError。
constexpr int kJsonRpcParseError = -32700;
constexpr int kJsonRpcInvalidRequest = -32600;
constexpr int kJsonRpcMethodNotFound = -32601;
constexpr int kJsonRpcInvalidParams = -32602;
constexpr int kJsonRpcInternalError = -32603;
constexpr int kJsonRpcBusinessError = -32000;

struct RpcError {
    int code = kJsonRpcInternalError;
    std::string message;
    // 只放结构化诊断，绝不放 Token 明文或其片段。
    std::string detail;

    bool empty() const { return message.empty(); }
};

// JSON-RPC 2.0 消息。id 用 has_id 区分「通知」与「请求」：
// 通知（notification）没有 id，服务端不得回响应。
struct RpcMessage {
    std::string jsonrpc = "2.0";
    std::string method;
    bool has_id = false;
    // id 可能是数字也可能是字符串，原样保存，回包时按同类型回写。
    std::string id;
    bool id_is_number = false;
    // params 原样保留为 JSON 文本，避免每层重复反序列化。
    std::string params_json = "null";
    bool has_result = false;
    std::string result_json = "null";
    bool has_error = false;
    RpcError error;
};

// MCP 规范版本。一经确定不做隐式漂移，升级需同步改 idea.md 并更新此常量。
constexpr const char* kMcpProtocolVersion = "2024-11-05";

// MCP initialize 握手结果，服务端在 tools 层不直接关心，交给 Core 组装。
struct McpServerInfo {
    std::string name = "GitHub-Bridge";
    std::string version = kVersion;
};

struct McpCapabilities {
    bool tools = true;
    bool logging = false;
    bool prompts = false;
    bool resources = false;
    bool experimental = false;
};

} // namespace ghb
