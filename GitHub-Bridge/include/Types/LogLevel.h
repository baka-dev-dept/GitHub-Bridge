#pragma once

namespace ghb {

enum class LogLevel : int {
    Trace = 0,
    Debug = 1,
    Info = 2,
    Warn = 3,
    Error = 4,
    Fatal = 5,
};

// 低于该等级的日志一律丢弃；阈值本身可在运行时调整。
struct LogConfig {
    LogLevel min_level = LogLevel::Info;
    bool echo_to_stderr = false;
    bool write_to_file = true;
};

} // namespace ghb
