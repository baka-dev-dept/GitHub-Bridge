#include "MCP/JsonCodec.h"

#include "Utils/Logger.h"

#include <nlohmann/json.hpp>

namespace ghb {

using nlohmann::json;

namespace {

// id 必须原样回写：客户端可能用数字或字符串，类型变了会导致请求-响应配对失败。
json MakeIdNode(const RpcMessage& message) {
    if (!message.has_id) {
        return nullptr;
    }
    if (!message.id_is_number) {
        return json(message.id);
    }
    // 数字 id 已被 Decode 存为规范数字文本，这里按原类型还原。
    const json parsed = json::parse(message.id, nullptr, false);
    return parsed.is_discarded() ? json(message.id) : parsed;
}

std::string DumpLine(const json& node) {
    return node.dump() + "\n";
}

json MakeErrorObject(const RpcError& error) {
    json object = {{"code", error.code}, {"message", error.message}};
    if (!error.detail.empty()) {
        object["data"] = {{"detail", error.detail}};
    }
    return object;
}

// 解析 id 字段。返回 false 表示 id 类型非法（既非字符串、数字、null 之外的取值）。
bool ParseId(const json& parsed, RpcMessage* message) {
    const auto id_it = parsed.find("id");
    if (id_it == parsed.end() || id_it->is_null()) {
        return true;
    }
    if (id_it->is_number_integer() || id_it->is_number_unsigned()) {
        message->has_id = true;
        message->id_is_number = true;
        message->id = id_it->dump();
        return true;
    }
    if (id_it->is_string()) {
        message->has_id = true;
        message->id_is_number = false;
        // 存裸值：回写时由 MakeIdNode 负责加回引号，这里再 dump 会双重编码。
        message->id = id_it->get<std::string>();
        return true;
    }
    return false;
}

} // namespace

DecodeResult JsonCodec::Decode(const std::string& line) {
    DecodeResult result;

    const json parsed = json::parse(line, nullptr, false);
    if (parsed.is_discarded()) {
        result.error = {kJsonRpcParseError, "Parse error", "消息不是合法 JSON"};
        return result;
    }
    if (!parsed.is_object()) {
        // 数组是 JSON-RPC 批量请求，本项目按固定协议版本不做支持，显式拒绝而不是静默忽略。
        result.error = {kJsonRpcInvalidRequest, "Invalid Request", "仅支持单个对象消息，不支持批量请求"};
        return result;
    }

    // 先解析 id：只要 id 能识别，错误响应就要带上它，否则客户端无法把错误配回自己的请求。
    RpcMessage message;
    if (!ParseId(parsed, &message)) {
        result.error = {kJsonRpcInvalidRequest, "Invalid Request", "id 必须是字符串或数字"};
        return result;
    }
    result.message = message;

    const auto version_it = parsed.find("jsonrpc");
    if (version_it == parsed.end() || !version_it->is_string() || version_it->get<std::string>() != "2.0") {
        result.error = {kJsonRpcInvalidRequest, "Invalid Request", "jsonrpc 字段必须为 \"2.0\""};
        return result;
    }

    const auto method_it = parsed.find("method");
    if (method_it != parsed.end() && method_it->is_string()) {
        message.method = method_it->get<std::string>();
    }
    if (message.method.empty()) {
        result.error = {kJsonRpcInvalidRequest, "Invalid Request", "缺少 method 字段"};
        return result;
    }

    const auto params_it = parsed.find("params");
    if (params_it != parsed.end()) {
        message.params_json = params_it->dump();
    }

    result.ok = true;
    result.message = std::move(message);
    return result;
}

std::string JsonCodec::EncodeResponse(const RpcMessage& request, const std::string& result_json) {
    const json parsed_result = json::parse(result_json, nullptr, false);
    json node = {{"jsonrpc", "2.0"}, {"id", MakeIdNode(request)}};
    // 解析失败说明调用方拼了非法 JSON，此时按原始字符串返回，便于调用方定位问题。
    node["result"] = parsed_result.is_discarded() ? json(result_json) : parsed_result;
    return DumpLine(node);
}

std::string JsonCodec::EncodeError(const RpcMessage& request, const RpcError& error) {
    json node = {{"jsonrpc", "2.0"}, {"id", MakeIdNode(request)}, {"error", MakeErrorObject(error)}};
    return DumpLine(node);
}

std::string JsonCodec::EncodeErrorNullId(const RpcError& error) {
    json node = {{"jsonrpc", "2.0"}, {"id", nullptr}, {"error", MakeErrorObject(error)}};
    return DumpLine(node);
}

} // namespace ghb
