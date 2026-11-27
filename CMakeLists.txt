cmake_minimum_required(VERSION 3.20)
project(GitHubBridge LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# 源文件与执行字符集统一 UTF-8：本机系统代码页在 Windows 上是 GBK，
# 不明示声明会把中文字面量编错。
if(MSVC)
    add_compile_options(/utf-8 /Zc:__cplusplus /W4)
else()
    add_compile_options(-finput-charset=UTF-8 -fexec-charset=UTF-8 -Wall -Wextra)
endif()

option(GHB_BUILD_TESTS "Build unit tests" ON)

# ---------------------------------------------------------------------------
# 公共源码：与平台无关的部分
# ---------------------------------------------------------------------------
set(GHB_COMMON_SOURCES
    GitHub-Bridge/src/MCP/JsonCodec.cpp
    GitHub-Bridge/src/MCP/StdioTransport.cpp
    GitHub-Bridge/src/Core/ToolRegistry.cpp
    GitHub-Bridge/src/Core/Gateway.cpp
    GitHub-Bridge/src/Core/BuiltinTools.cpp
    GitHub-Bridge/src/Core/TokenTools.cpp
    GitHub-Bridge/src/Core/GitHubTools.cpp
    GitHub-Bridge/src/Security/TokenStorage.cpp
    GitHub-Bridge/src/Security/TokenManager.cpp
    GitHub-Bridge/src/Security/PermissionManager.cpp
    GitHub-Bridge/src/Cache/CacheManager.cpp
    GitHub-Bridge/src/GitHub/GitHubClient.cpp
    GitHub-Bridge/src/GitHub/RateLimiter.cpp
    GitHub-Bridge/src/GitHub/GraphQLGuard.cpp
    GitHub-Bridge/src/Utils/Logger.cpp
)

# 平台无关的测试源码：这些不依赖任何系统 API，Windows 与 Linux 都直接编。
set(GHB_TEST_SOURCES
    GitHub-Bridge.Tests/TestMain.cpp
    GitHub-Bridge.Tests/JsonCodecTests.cpp
    GitHub-Bridge.Tests/RateLimiterTests.cpp
    GitHub-Bridge.Tests/RetryTests.cpp
    GitHub-Bridge.Tests/PermissionTests.cpp
    GitHub-Bridge.Tests/ToolRegistryTests.cpp
    GitHub-Bridge.Tests/GraphQLGuardTests.cpp
    GitHub-Bridge.Tests/CacheManagerTests.cpp
    GitHub-Bridge.Tests/TokenProfileTests.cpp
    GitHub-Bridge.Tests/RepositoryWhitelistTests.cpp
)

# ---------------------------------------------------------------------------
# 平台实现
# ---------------------------------------------------------------------------
add_library(ghb_platform STATIC)

if(WIN32)
    target_sources(ghb_platform PRIVATE
        GitHub-Bridge/src/platform/windows/WinHttpTransport.cpp
        GitHub-Bridge/src/platform/windows/WindowsPlatform.cpp
        GitHub-Bridge/src/platform/windows/DpapiVault.cpp
    )
    target_link_libraries(ghb_platform PUBLIC winhttp crypt32 advapi32 bcrypt shlwapi shell32)
    target_compile_definitions(ghb_platform PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    set(GHB_TEST_NEEDS_PLATFORM ON)
else()
    find_package(CURL REQUIRED)
    find_library(SODIUM_LIBRARY sodium REQUIRED)

    target_sources(ghb_platform PRIVATE
        GitHub-Bridge/src/platform/linux/CurlTransport.cpp
        GitHub-Bridge/src/platform/linux/LinuxPlatform.cpp
        GitHub-Bridge/src/platform/linux/SodiumVault.cpp
    )
    target_link_libraries(ghb_platform PUBLIC CURL::libcurl ${SODIUM_LIBRARY})
    set(GHB_TEST_NEEDS_PLATFORM ON)
endif()

target_include_directories(ghb_platform
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/GitHub-Bridge/include
        ${CMAKE_CURRENT_SOURCE_DIR}/GitHub-Bridge/third_party
)

# ---------------------------------------------------------------------------
# 主程序
# ---------------------------------------------------------------------------
add_executable(GitHub-Bridge
    GitHub-Bridge/src/main.cpp
    ${GHB_COMMON_SOURCES}
)
target_link_libraries(GitHub-Bridge PRIVATE ghb_platform)

# ---------------------------------------------------------------------------
# 单元测试
# ---------------------------------------------------------------------------
if(GHB_BUILD_TESTS)
    add_executable(ghb_tests ${GHB_TEST_SOURCES})
    target_include_directories(ghb_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/GitHub-Bridge.Tests)

    if(GHB_TEST_NEEDS_PLATFORM)
        # 测试覆盖了带平台依赖的模块，必须把它依赖的实现一并编进来。
        target_sources(ghb_tests PRIVATE ${GHB_COMMON_SOURCES})
        target_link_libraries(ghb_tests PRIVATE ghb_platform)
    endif()

    enable_testing()
    add_test(NAME ghb_tests COMMAND ghb_tests)
endif()
