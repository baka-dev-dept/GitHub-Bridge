#pragma once

#include "Types/RpcMessage.h"

#include <string>

namespace ghb {

struct DecodeResult {
    bool ok = false;
    RpcMessage message;
    RpcError error;
};

// JSON-RPC 2.0 单行编解码。所有输出都是紧凑单行，结尾只追加一个 '\n'，
// 这是 stdio 传输的分帧约定，多一个字节都会让客户端解析失败。
class JsonCodec {
public:
    static DecodeResult Decode(const std::string& line);

    static std::string EncodeResponse(const RpcMessage& request, const std::string& result_json);
    static std::string EncodeError(const RpcMessage& request, const RpcError& error);
    // 无法识别 id（解析失败）时使用，id 必须是 null。
    static std::string EncodeErrorNullId(const RpcError& error);

    // 通知没有 id，服务端不得回包；此方法只用于测试与诊断。
    static bool IsNotification(const RpcMessage& message) { return !message.has_id; }
};

} // namespace ghb
