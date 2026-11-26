#pragma once

#include "Types/LogLevel.h"

#include <string>

namespace ghb {

// Logger 是唯一允许写 stderr 的模块。stdout 由 StdioTransport 独占，
// 任何第三方库或调试残留都不得写 stdout，否则 JSON-RPC 分帧当场失效。
class Logger {
public:
    static Logger* Get() {
        static Logger i;
        return &i;
    }

    void Configure(const LogConfig& config);

    void Log(LogLevel level, const std::string& message);
    void Trace(const std::string& message);
    void Debug(const std::string& message);
    void Info(const std::string& message);
    void Warn(const std::string& message);
    void Error(const std::string& message);
    void Fatal(const std::string& message);

    // 日志输出前的兜底脱敏：即便调用方误把令牌拼进消息，也不会落盘。
    static std::string RedactSecrets(const std::string& text);

private:
    Logger() = default;
    ~Logger() = default;

    bool ShouldLog(LogLevel level) const { return static_cast<int>(level) >= static_cast<int>(config_.min_level); }

    LogConfig config_;
};

} // namespace ghb
