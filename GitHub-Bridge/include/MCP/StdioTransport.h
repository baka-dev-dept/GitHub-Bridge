#pragma once

#include <string>

namespace ghb {

// stdio 行分帧传输。stdin 读一行 = 一条 JSON-RPC 消息，stdout 写一行 = 一条响应。
// 这里也是全工程唯一允许写 stdout 的地方。
class StdioTransport {
public:
    static StdioTransport* Get() {
        static StdioTransport i;
        return &i;
    }

    // 读下一行。返回 false 表示 EOF 或读取失败，调用方据此结束主循环。
    bool ReadLine(std::string* out_line);

    // 写出完整一行（内部保证末尾有且只有一个 '\n'）并立即 flush。
    bool WriteLine(const std::string& line);

    // 单条消息上限。超过即丢弃该行：无上限会让恶意或异常客户端撑爆内存。
    static constexpr size_t kMaxLineBytes = 8u * 1024u * 1024u;

private:
    StdioTransport() = default;
    ~StdioTransport() = default;
};

} // namespace ghb
