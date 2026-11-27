#pragma once

#include <string>

namespace ghb {

// GraphQL 的权限键由 query 内容决定：含 mutation 就是写操作。
// 这是权限开关对 GraphQL 唯一的约束手段——mutation 没有具体目标，
// 无法套用 confirm_token 的目标绑定语义。
//
// 返回 "write" 或 "read"；arguments_json 缺 query 时返回空串，
// 交给工具实现去报参数错误，这里不另造错误。
std::string ResolveGraphQLPermissionKey(const std::string& arguments_json);

// 剥离字符串字面量（含块字符串 """..."""）与 # 注释，只留结构关键字。
// 不这么做的话，查询里出现的 "mutation" 字样会被误判成写操作。
std::string StripGraphQLNoise(const std::string& query);

} // namespace ghb
