#pragma once

#include "Types/RpcMessage.h"

#include <string>

namespace ghb {

// MCP 服务端主循环与请求分派。传输、编解码、工具执行都在这里串起来，
// 它是唯一决定「哪些 method 被支持」的地方。
class Gateway {
public:
    static Gateway* Get() {
        static Gateway i;
        return &i;
    }

    // 阻塞读 stdin 直到 EOF。返回进程退出码：0 表示正常结束。
    int Run();

    // 处理一条已解码的消息，返回应写回 stdout 的完整行；返回空串表示不回包（通知）。
    std::string Handle(const RpcMessage& request);

private:
    Gateway() = default;
    ~Gateway() = default;

    std::string HandleInitialize(const RpcMessage& request);
    std::string HandleToolsList(const RpcMessage& request);
    std::string HandleToolsCall(const RpcMessage& request);

    bool initialized_ = false;
};

} // namespace ghb
