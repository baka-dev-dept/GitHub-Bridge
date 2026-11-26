#include "Types/Types.h"

#include <fcntl.h>
#include <io.h>

#include <cstdio>

// stdio 必须切二进制模式：CRT 文本模式会把 \n 改写成 \r\n，
// JSON-RPC 的换行分帧会当场错位，且报错信息与协议内容无关，极难定位。
static void SetStdioBinaryMode() {
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
}

int main(int argc, char** argv) {
    SetStdioBinaryMode();

    // Phase 1 之前只做契约自检：stdout 保持干净，任何诊断走 stderr。
    std::fprintf(stderr, "GitHub-Bridge %s (MCP %s), argc=%d\n", ghb::McpServerInfo{}.version.c_str(), ghb::kMcpProtocolVersion, argc);
    return 0;
}
