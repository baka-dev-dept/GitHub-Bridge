#include "Utils/Logger.h"

#include <cstdio>
#include <regex>

namespace ghb {

namespace {

const char* LevelName(LogLevel level) {
    switch (level) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO";
        case LogLevel::Warn:  return "WARN";
        case LogLevel::Error: return "ERROR";
        case LogLevel::Fatal: return "FATAL";
    }
    return "INFO";
}

// 匹配 GitHub 经典令牌（ghp_/gho_/ghu_/ghs_/ghr_）与 fine-grained 令牌（github_pat_）。
// 允许前缀后紧跟任意非空白字符，宁严勿漏：误脱敏只会让日志变短，漏脱敏会泄露凭据。
const std::regex kSecretPattern(R"((gh[pousr]_[A-Za-z0-9]{16,}|github_pat_[A-Za-z0-9_]{20,}))");

} // namespace

void Logger::Configure(const LogConfig& config) {
    config_ = config;
}

std::string Logger::RedactSecrets(const std::string& text) {
    return std::regex_replace(text, kSecretPattern, "***");
}

void Logger::Log(LogLevel level, const std::string& message) {
    if (!ShouldLog(level)) {
        return;
    }
    const std::string line = std::string("[") + LevelName(level) + "] " + RedactSecrets(message) + "\n";
    std::fwrite(line.data(), 1, line.size(), stderr);
    std::fflush(stderr);
}

void Logger::Trace(const std::string& message) { Log(LogLevel::Trace, message); }
void Logger::Debug(const std::string& message) { Log(LogLevel::Debug, message); }
void Logger::Info(const std::string& message)  { Log(LogLevel::Info, message); }
void Logger::Warn(const std::string& message)  { Log(LogLevel::Warn, message); }
void Logger::Error(const std::string& message) { Log(LogLevel::Error, message); }
void Logger::Fatal(const std::string& message) { Log(LogLevel::Fatal, message); }

} // namespace ghb
