#include "Core/TokenTools.h"

#include "Core/ToolRegistry.h"
#include "GitHub/GitHubClient.h"
#include "Security/TokenManager.h"
#include "Security/TokenStorage.h"
#include "Types/GitHubResponse.h"
#include "Types/McpTool.h"

#include <nlohmann/json.hpp>

namespace ghb {

namespace {

std::string LoadStatusName(TokenLoadStatus status) {
    switch (status) {
        case TokenLoadStatus::Ok: return "ok";
        case TokenLoadStatus::NotFound: return "not_found";
        case TokenLoadStatus::Corrupted: return "corrupted";
        case TokenLoadStatus::EntropyMismatch: return "entropy_mismatch";
        case TokenLoadStatus::IoError: return "io_error";
    }
    return "unknown";
}

std::string HandleTokenStatus(const std::string&) {
    TokenStatusSnapshot snapshot = TokenManager::Get()->Snapshot();
    const TokenLoadStatus load_status = TokenManager::Get()->LastLoadStatus();

    std::string message;
    if (!snapshot.configured) {
        switch (load_status) {
            case TokenLoadStatus::NotFound:
                message = "尚未配置令牌，请用 github_token_import 导入";
                break;
            case TokenLoadStatus::Corrupted:
                message = "令牌文件已损坏，请重新导入";
                break;
            case TokenLoadStatus::EntropyMismatch:
                message = "令牌无法解密（通常因为更换了机器或用户），请重新导入";
                break;
            case TokenLoadStatus::IoError:
                message = "读取令牌文件失败，请检查 %APPDATA%\\GitHub-Bridge 权限";
                break;
            default:
                message = "令牌不可用";
                break;
        }
    } else {
        // Phase 2 没有接 GitHub API，不能声称令牌有效。
        message = "令牌已配置且可解密；有效性尚未校验";
    }

    // configured 只说明本地能解密，valid 必须是真实 API 调用的结果。
    bool valid = false;
    bool validated = false;
    std::string login;
    std::string scopes = snapshot.scopes_csv;
    std::string validation_error;

    if (snapshot.configured) {
        TokenHandle handle = TokenManager::Get()->AcquireHandle();
        const GitHubResponse response = GitHubClient::Get()->GetUser(handle.authorization_header);
        // 响应头里的 scope 比本地推断更权威，有就覆盖。
        if (!response.headers.oauth_scopes.empty()) {
            scopes = response.headers.oauth_scopes;
        }
        if (response.ok) {
            const nlohmann::json body = nlohmann::json::parse(response.body, nullptr, false);
            if (body.is_object()) {
                login = body.value("login", "");
                valid = true;
                validated = true;
                message = "令牌有效，账号 " + login;
            }
        } else if (response.error_kind == GitHubErrorKind::Transport ||
                   response.error_kind == GitHubErrorKind::Decode) {
            // 网络不通不等于令牌无效。这种情况下必须保持 validated=false，
            // 否则会把「没连上」误报成「令牌坏了」，让用户白换令牌。
            validation_error = "网络不可达，无法校验令牌有效性（这不代表令牌无效）";
            message = "令牌已配置且可解密；本次无法联网校验";
        } else {
            validated = true;
            validation_error = response.error_message;
            message = "令牌被 GitHub 拒绝: " + response.error_message;
        }
    }

    const nlohmann::json result = {
        {"configured", snapshot.configured},
        {"valid", valid},
        {"validated", validated},
        {"login", login},
        {"scopes", scopes},
        {"token_type", snapshot.token_type},
        {"storage_status", LoadStatusName(load_status)},
        {"storage_path", TokenStorage::Get()->ResolvePaths().data_file},
        {"validation_error", validation_error},
        {"message", message},
    };
    return result.dump();
}

std::string HandleTokenImport(const std::string& arguments_json) {
    const nlohmann::json arguments = nlohmann::json::parse(arguments_json, nullptr, false);
    if (arguments.is_discarded() || !arguments.is_object()) {
        return R"({"ok":false,"message":"arguments 必须是对象"})";
    }

    const auto it = arguments.find("token");
    if (it == arguments.end() || !it->is_string()) {
        return R"({"ok":false,"message":"缺少字符串参数 token"})";
    }

    // 明文只在这一行存在：立刻交给 TokenManager，后续不再引用。
    const TokenImportResult result = TokenManager::Get()->Import(it->get<std::string>());

    // 返回值里绝不包含令牌本身，只回状态与说明。
    const nlohmann::json out = {
        {"ok", result.ok},
        {"message", result.message},
        {"configured", result.ok},
    };
    return out.dump();
}

std::string HandleTokenClear(const std::string&) {
    const bool cleared = TokenManager::Get()->Clear();
    const nlohmann::json out = {
        {"ok", cleared},
        {"configured", false},
        {"message", cleared ? "令牌已从本地清除" : "清除令牌失败，请查看日志"},
    };
    return out.dump();
}

} // namespace

void RegisterTokenTools() {
    ToolRegistry* registry = ToolRegistry::Get();

    ToolRegistration status;
    status.tool.name = "github_token_status";
    status.tool.title = "Token status";
    status.tool.description =
        "返回本地令牌的配置与存储状态。configured 表示能否解密读取，valid 表示 GitHub 是否认可该令牌——"
        "Phase 2 尚未联网校验，valid 恒为 false。返回值不含令牌明文。";
    status.tool.input_schema_json = R"({"type":"object","properties":{},"additionalProperties":false})";
    status.tool.annotations.read_only_hint = true;
    status.tool.annotations.idempotent_hint = true;
    status.tool.annotations.open_world_hint = false;
    status.tool.safety.kind = OperationKind::Read;
    status.handler = &HandleTokenStatus;
    registry->Register(status);

    ToolRegistration import;
    import.tool.name = "github_token_import";
    import.tool.title = "Import token";
    import.tool.description =
        "导入 GitHub Personal Access Token，使用 Windows DPAPI 加密后保存到本机，并把文件权限收紧到当前用户。"
        "返回值不含令牌明文。";
    import.tool.input_schema_json =
        R"({"type":"object","properties":{"token":{"type":"string","description":"GitHub PAT，需带 ghp_/github_pat_ 等前缀"}},"required":["token"],"additionalProperties":false})";
    import.tool.annotations.read_only_hint = false;
    import.tool.annotations.destructive_hint = false;
    import.tool.annotations.idempotent_hint = true;
    import.tool.annotations.open_world_hint = false;
    import.tool.safety.kind = OperationKind::Write;
    import.tool.safety.permission_key = "write";
    import.handler = &HandleTokenImport;
    registry->Register(import);

    ToolRegistration clear;
    clear.tool.name = "github_token_clear";
    clear.tool.title = "Clear token";
    clear.tool.description = "删除本地保存的令牌文件并清空内存缓存。不可撤销，需要先导入才能恢复。";
    clear.tool.input_schema_json = R"({"type":"object","properties":{},"additionalProperties":false})";
    clear.tool.annotations.read_only_hint = false;
    clear.tool.annotations.destructive_hint = true;
    clear.tool.annotations.idempotent_hint = true;
    clear.tool.annotations.open_world_hint = false;
    clear.tool.safety.kind = OperationKind::Delete;
    clear.tool.safety.requires_confirmation = true;
    clear.tool.safety.permission_key = "delete_repository";
    clear.handler = &HandleTokenClear;
    registry->Register(clear);
}

} // namespace ghb
