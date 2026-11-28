#pragma once

// 版本号的唯一来源。用户代理、MCP 握手、Windows 版本资源都从这里取，
// 避免升版本时改了一处漏了另一处。
//
// 改这里的同时要改 GitHub-Bridge.rc 里的 FILEVERSION，那是编译期资源，
// 没法引用 C++ 常量。
#define GHB_VERSION_STRING "0.1.0"
#define GHB_VERSION_MAJOR 0
#define GHB_VERSION_MINOR 1
#define GHB_VERSION_PATCH 0

namespace ghb {

inline constexpr const char* kVersion = GHB_VERSION_STRING;

} // namespace ghb
