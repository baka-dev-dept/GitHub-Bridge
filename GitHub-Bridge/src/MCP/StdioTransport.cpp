#include "MCP/StdioTransport.h"

#include "Utils/Logger.h"

#include <cstdio>

namespace ghb {

bool StdioTransport::ReadLine(std::string* out_line) {
    out_line->clear();

    int ch = 0;
    while (true) {
        ch = std::fgetc(stdin);
        if (ch == EOF) {
            return !out_line->empty();
        }
        if (ch == '\n') {
            break;
        }
        out_line->push_back(static_cast<char>(ch));

        if (out_line->size() > kMaxLineBytes) {
            // 丢弃到本行结束，保持分帧对齐，然后返回空行让调用方跳过。
            Logger::Get()->Error("JSON-RPC 消息超过 8MiB 上限，已丢弃该行");
            int skip = 0;
            while ((skip = std::fgetc(stdin)) != EOF && skip != '\n') {
            }
            out_line->clear();
            // EOF 说明后面不会再有消息，直接结束主循环。
            return skip != EOF;
        }
    }

    // 客户端可能用 CRLF 结尾，行尾的 '\r' 属于分帧字符，不属于 JSON 内容。
    if (!out_line->empty() && out_line->back() == '\r') {
        out_line->pop_back();
    }
    return true;
}

bool StdioTransport::WriteLine(const std::string& line) {
    std::string payload = line;
    if (payload.empty() || payload.back() != '\n') {
        payload.push_back('\n');
    }

    const size_t written = std::fwrite(payload.data(), 1, payload.size(), stdout);
    if (written != payload.size()) {
        return false;
    }
    // 不 flush 会让客户端一直等不到消息：stdio 全缓冲，管道模式下尤其致命。
    return std::fflush(stdout) == 0;
}

} // namespace ghb
