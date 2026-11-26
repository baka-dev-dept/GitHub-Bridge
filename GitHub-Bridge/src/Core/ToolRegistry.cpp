#include "Core/ToolRegistry.h"

#include "Utils/Logger.h"

namespace ghb {

bool ToolRegistry::Register(const ToolRegistration& registration) {
    if (registration.tool.name.empty()) {
        Logger::Get()->Warn("拒绝注册名称为空的工具");
        return false;
    }
    if (registration.handler == nullptr) {
        Logger::Get()->Warn("拒绝注册没有处理函数的工具: " + registration.tool.name);
        return false;
    }
    if (Find(registration.tool.name) != nullptr) {
        Logger::Get()->Warn("工具重名，已拒绝注册: " + registration.tool.name);
        return false;
    }
    tools_.push_back(registration);
    Logger::Get()->Debug("已注册工具: " + registration.tool.name);
    return true;
}

const ToolRegistration* ToolRegistry::Find(const std::string& name) const {
    for (const auto& registration : tools_) {
        if (registration.tool.name == name) {
            return &registration;
        }
    }
    return nullptr;
}

} // namespace ghb
