#include "Core/BuiltinTools.h"
#include "Core/Gateway.h"
#include "Security/PermissionManager.h"
#include "Utils/Logger.h"

#include <cstdio>

#ifdef _WIN32
#include <io.h>
#include <windows.h>

#include <fcntl.h>

// stdio 必须切二进制模式：CRT 文本模式会把 \n 改写成 \r\n，JSON-RPC 的
// 换行分帧会当场错位，且报错信息与协议内容无关，极难定位。
// 必须在任何输出之前完成，包括日志。
static void SetStdioBinaryMode() {
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
}

// 控制台默认按系统代码页解码（简体中文是 936/GBK），而本工程以 /utf-8 编译，
// 日志写出的就是 UTF-8 字节。不切代码页的话中文日志在 conhost 里会变成乱码。
// 只影响本进程挂接的控制台，进程退出即恢复，不动系统全局设置；
// stdout 被重定向成管道时该调用不改变字节流，MCP 协议不受影响。
static void SetConsoleUtf8() {
    if (GetConsoleWindow() == nullptr) {
        return;
    }
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
}
#else

// POSIX 上 stdin/stdout 本来就是二进制，也没有代码页概念，
// 这两步在 Linux 上是空实现，保留函数是为了让 main 保持单一形态。
static void SetStdioBinaryMode() {}
static void SetConsoleUtf8() {}

#endif

int main(int argc, char** argv) {
    using namespace ghb;

    SetStdioBinaryMode();
    SetConsoleUtf8();

    (void)argc;
    (void)argv;

    LogConfig config;
    config.min_level = LogLevel::Info;
    config.echo_to_stderr = true;
    config.write_to_file = false;
    Logger::Get()->Configure(config);

    // 权限配置必须在注册工具之后、处理请求之前加载。
    PermissionManager::Get()->LoadConfig();

    RegisterBuiltinTools();
    return Gateway::Get()->Run();
}
