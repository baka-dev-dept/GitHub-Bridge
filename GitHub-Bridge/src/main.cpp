#include "Core/BuiltinTools.h"
#include "Core/Gateway.h"
#include "Utils/Logger.h"

#include <fcntl.h>
#include <io.h>

// stdio 必须切二进制模式：CRT 文本模式会把 \n 改写成 \r\n，
// JSON-RPC 的换行分帧会当场错位，且报错信息与协议内容无关，极难定位。
// 必须在任何输出之前完成，包括日志。
static void SetStdioBinaryMode() {
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
}

int main(int argc, char** argv) {
    using namespace ghb;

    SetStdioBinaryMode();

    (void)argc;
    (void)argv;

    LogConfig config;
    config.min_level = LogLevel::Info;
    config.echo_to_stderr = true;
    config.write_to_file = false;
    Logger::Get()->Configure(config);

    RegisterBuiltinTools();
    return Gateway::Get()->Run();
}
