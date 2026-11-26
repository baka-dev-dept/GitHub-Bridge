#pragma once

namespace ghb {

// 内建工具注册。Phase 1 只有一个自检工具，用于验证 tools/call 通路；
// 后续各阶段在这里挂载 GitHub 领域工具。
void RegisterBuiltinTools();

} // namespace ghb
