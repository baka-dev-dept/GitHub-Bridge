#pragma once

#include "Types/McpTool.h"

#include <string>
#include <vector>

namespace ghb {

// 工具注册表。保持注册顺序，tools/list 的输出顺序即注册顺序，便于客户端缓存稳定。
class ToolRegistry {
public:
    static ToolRegistry* Get() {
        static ToolRegistry i;
        return &i;
    }

    // 重名注册会被拒绝并记录警告，不做静默覆盖：静默覆盖会让后续排查无从下手。
    bool Register(const ToolRegistration& registration);

    const ToolRegistration* Find(const std::string& name) const;
    const std::vector<ToolRegistration>& List() const { return tools_; }
    size_t Size() const { return tools_.size(); }

private:
    ToolRegistry() = default;
    ~ToolRegistry() = default;

    std::vector<ToolRegistration> tools_;
};

} // namespace ghb
