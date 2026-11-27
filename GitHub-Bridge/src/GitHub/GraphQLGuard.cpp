#include "GitHub/GraphQLGuard.h"

#include <nlohmann/json.hpp>

#include <functional>

namespace ghb {

namespace {

bool IsIdentifierChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

// 按词边界找关键字：myMutation 不算，mutation 才算。
bool ContainsWord(const std::string& text, const char* word) {
    const size_t len = std::char_traits<char>::length(word);
    size_t pos = text.find(word);
    while (pos != std::string::npos) {
        const bool left_ok = (pos == 0) || !IsIdentifierChar(text[pos - 1]);
        const bool right_ok = (pos + len >= text.size()) || !IsIdentifierChar(text[pos + len]);
        if (left_ok && right_ok) {
            return true;
        }
        pos = text.find(word, pos + 1);
    }
    return false;
}

} // namespace

std::string StripGraphQLNoise(const std::string& query) {
    std::string out;
    out.reserve(query.size());
    size_t i = 0;
    while (i < query.size()) {
        const char c = query[i];

        // 块字符串 """..."""：GraphQL 用它写多行说明文字
        if (c == '"' && i + 2 < query.size() && query[i + 1] == '"' && query[i + 2] == '"') {
            i += 3;
            while (i + 2 < query.size()) {
                if (query[i] == '"' && query[i + 1] == '"' && query[i + 2] == '"') {
                    i += 3;
                    break;
                }
                i++;
            }
            out.push_back(' ');
            continue;
        }

        // 普通字符串：处理 \" 与 \\ 两种转义
        if (c == '"') {
            i++;
            while (i < query.size()) {
                if (query[i] == '\\' && i + 1 < query.size()) {
                    i += 2;
                    continue;
                }
                if (query[i] == '"') {
                    i++;
                    break;
                }
                i++;
            }
            out.push_back(' ');
            continue;
        }

        // 注释到行尾
        if (c == '#') {
            while (i < query.size() && query[i] != '\n') {
                i++;
            }
            continue;
        }

        out.push_back(c);
        i++;
    }
    return out;
}

std::string ResolveGraphQLPermissionKey(const std::string& arguments_json) {
    const nlohmann::json args = nlohmann::json::parse(arguments_json, nullptr, false);
    if (!args.is_object()) {
        return {};
    }
    const auto it = args.find("query");
    if (it == args.end() || !it->is_string()) {
        return {};
    }

    const std::string stripped = StripGraphQLNoise(it->get<std::string>());
    return ContainsWord(stripped, "mutation") ? "write" : "read";
}

} // namespace ghb
