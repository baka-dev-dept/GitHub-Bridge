# GitHub-Bridge

本机 MCP Server（stdio），让 AI Agent 能调 GitHub API，而**永远拿不到 Personal Access Token**。

Windows 11 x64 · C++20 · Visual Studio 2026 · MCP 规范固定 `2024-11-05`

**34 个工具 · 31 个 GitHub API 端点全部实现 · 84 项测试通过**

---

## 这是什么

GitHub-Bridge 是一个跑在本机的 MCP Server。Agent 通过 MCP 协议调用它，由它代持 GitHub PAT 去访问 GitHub REST API。
令牌用 Windows DPAPI 加密存储，**不出现在任何工具返回值、日志或标准输出里**。

它只走 **REST API**，不经过 git 协议，也不接管 git 的凭据体系。

## 与 git 的分工

两者互补，不能互相替代：

| 场景 | 由谁负责 |
|---|---|
| 建仓库、改仓库设置、删仓库 | **GitHub-Bridge**（REST API） |
| 读写文件、Issue、PR、Branch、Release | **GitHub-Bridge**（REST API） |
| 本地 `git push` / `pull` / `clone` | **git 自身**（`credential.helper`） |
| 本地多账号凭据切换 | **git 配置** |

- GitHub-Bridge **不能**修复 `git push` 弹登录框这类问题；
- git **不能**替代 API 层的 Issue / PR / Release 能力；
- 权限策略**不能**扩大 PAT 本身拥有的权限。

## 当前状态

**以代码为准，未实现的不会被说成已实现。**

| 模块 | 状态 |
|---|---|
| MCP stdio 传输 + JSON-RPC 2.0 | ✅ 已实现，21 项协议测试通过 |
| DPAPI 令牌加密存储（第二因子 + ACL + 原子写入） | ✅ 已实现，实测通过 |
| WinHTTP 传输（gzip / TLS / 禁重定向 / 超时） | ✅ 已实现，实测通过 |
| **31 个 GitHub API 端点** | ✅ **全部实现**，端到端实测通过 |
| **PermissionManager + 危险操作二次确认** | ✅ **已实现**，实测通过 |
| **限流主动处理** | ✅ 已实现，单元验证 9/9 |
| 通用请求重试 | ⬜ 未实现（限流重试已有，见下文） |
| 响应缓存 | ⬜ 未实现 |
| 多令牌 profile | ⬜ 未实现 |
| 单元测试工程 | ⬜ 未实现（现有为手工测试脚本） |
| CMake 工程 | ⬜ 未实现（仅 MSBuild） |

## 工具清单（34 个）

「确认」列表示该工具是**不可逆操作**，首次调用会返回一次性 `confirm_token`，需携带票据重试才真正执行。

### 服务与令牌（6）

| 工具 | 说明 | 联网 | 写 | 确认 |
|---|---|---|---|---|
| `github_server_info` | 版本、协议版本、工具数量、服务器时间 | 否 | 否 | |
| `github_token_status` | 令牌状态：`configured` / `valid` / `validated` | 是 | 否 | |
| `github_token_import` | 导入 PAT，DPAPI 加密落盘并收紧 ACL | 否 | 是 | |
| `github_token_clear` | 删除本地令牌并清空内存缓存 | 否 | 是 | **是** |
| `github_get_user` | 调 `/user` 返回当前账号 | 是 | 否 | |
| `github_rate_limit` | 调 `/rate_limit`，匿名可用 | 是 | 否 | |

### Repository（5）

| 工具 | REST 端点 | 写 | 确认 |
|---|---|---|---|
| `github_search_repositories` | `GET /search/repositories` | 否 | |
| `github_get_repository` | `GET /repos/{owner}/{repo}` | 否 | |
| `github_create_repository` | `POST /user/repos` | 是 | |
| `github_update_repository` | `PATCH /repos/{owner}/{repo}` | 是 | |
| `github_delete_repository` | `DELETE /repos/{owner}/{repo}` | 是 | **是** |

### Files（4）

| 工具 | REST 端点 | 写 | 确认 |
|---|---|---|---|
| `github_get_file` | `GET /repos/{owner}/{repo}/contents/{path}` | 否 | |
| `github_get_file_content` | 同上，并解码 base64 正文 | 否 | |
| `github_create_or_update_file` | `PUT .../contents/{path}` | 是 | |
| `github_delete_file` | `DELETE .../contents/{path}` | 是 | **是** |

### Issues（5）

| 工具 | REST 端点 | 写 | 确认 |
|---|---|---|---|
| `github_list_issues` | `GET /repos/{owner}/{repo}/issues` | 否 | |
| `github_get_issue` | `GET .../issues/{number}` | 否 | |
| `github_create_issue` | `POST .../issues` | 是 | |
| `github_update_issue` | `PATCH .../issues/{number}` | 是 | |
| `github_close_issue` | `PATCH` 改 `state` | 是 | |

### Branches（4）

| 工具 | REST 端点 | 写 | 确认 |
|---|---|---|---|
| `github_list_branches` | `GET /repos/{owner}/{repo}/branches` | 否 | |
| `github_get_branch` | `GET .../branches/{branch}` | 否 | |
| `github_create_branch` | `POST .../git/refs` | 是 | |
| `github_delete_branch` | `DELETE .../git/refs/heads/{branch}` | 是 | **是** |

### Pull Requests（5）

| 工具 | REST 端点 | 写 | 确认 |
|---|---|---|---|
| `github_list_pull_requests` | `GET /repos/{owner}/{repo}/pulls` | 否 | |
| `github_get_pull_request` | `GET .../pulls/{number}` | 否 | |
| `github_create_pull_request` | `POST .../pulls` | 是 | |
| `github_update_pull_request` | `PATCH .../pulls/{number}` | 是 | |
| `github_merge_pull_request` | `PUT .../pulls/{number}/merge` | 是 | **是** |

### Releases（4）

| 工具 | REST 端点 | 写 | 确认 |
|---|---|---|---|
| `github_list_releases` | `GET /repos/{owner}/{repo}/releases` | 否 | |
| `github_get_release` | `GET .../releases/{id}` | 否 | |
| `github_create_release` | `POST .../releases` | 是 | |
| `github_update_release` | `PATCH .../releases/{id}` | 是 | |

### GraphQL（1）

| 工具 | 说明 | 写 | 确认 |
|---|---|---|---|
| `github_graphql` | 执行 GraphQL 查询，返回 `data` 或 `graphql_errors` | 视查询而定 | |

> ⚠️ **当前限制**：`github_graphql` 挂的是 `read` 权限键，但 GraphQL **可以执行 mutation**（写操作）。
> 也就是说 `read: true` 时，通过 GraphQL mutation 仍能修改仓库内容，权限开关对它不起约束作用。
> 要么只把它当查询工具用，要么在后续版本中为 mutation 增加识别与独立开关。

### 几个刻意的设计

- `github_token_status` 的三个布尔值分开：**本地能解密 ≠ GitHub 认可该令牌**。网络不通时 `validated` 保持 `false`，避免把「没连上」误报成「令牌坏了」。
- `github_list_issues` 会同时返回 PR（GitHub 的 issues 接口行为），用 `is_pull_request` 区分。
- `github_get_file_content` 同时返回 `sha`，更新或删除文件时要用。
- `github_graphql` 会检查 200 响应里的 `errors` 数组——GitHub 查询失败常返回 200。

## 构建

需要 Visual Studio 2026（含 MSVC v145 工具集与 Windows SDK）与 `nlohmann/json`（已 vendored 在 `third_party/`，无需联网）。

```powershell
# 用 VS 打开 GitHub-Bridge.slnx 直接构建，或使用 MSBuild 完整路径（msbuild 通常不在 PATH）：
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" `
    GitHub-Bridge.slnx -p:Configuration=Release -p:Platform=x64 -nologo
```

产物：`x64\Release\GitHub-Bridge.exe`（输出目录基于解决方案根，中间文件在 `x64\out\Release\`）。

> 若在已经加载过其他工具的终端里直接跑 MSBuild 撞到 `MSB6001`（环境变量大小写并存，如 `Path`/`PATH`、`HTTP_PROXY`/`http_proxy`），用干净环境启动 MSBuild 即可。

## 接入 MCP 客户端

```json
{
  "mcpServers": {
    "github-bridge": {
      "command": "D:\\service\\GitHub-Bridge\\GitHub-Bridge.exe",
      "args": [],
      "env": {}
    }
  }
}
```

程序启动后从标准输入读、往标准输出写一行为单位的 JSON-RPC 2.0 消息。**stdout 只承载协议消息**，日志一律走 stderr。

## 导入令牌

调用 `github_token_import`，参数 `{"token": "<你的 PAT>"}`。返回值只有 `{"ok": true}`，**不回显令牌**。

令牌需要的前缀：`ghp_` / `gho_` / `ghu_` / `ghs_` / `ghr_`（classic）或 `github_pat_`（fine-grained）。

**classic PAT 推荐 scope**：`repo`、`workflow`、`read:user`；若要支持删仓库再加 `delete_repo`。

**fine-grained PAT 需要**：Contents、Issues、Pull requests、Administration（删仓库必需）、Workflows 均为 Read and write，Metadata 自动只读。

## 权限与安全防护

两道互相独立的防线，缺一不可。

### 权限开关

配置文件：`%APPDATA%\GitHub-Bridge\permissions.json`（不存在时用默认值）。

```json
{
  "permissions": {
    "read": true,
    "write": true,
    "create": true,
    "delete_repository": false,
    "delete_branch": false,
    "force_push": false,
    "merge_pull_request": true
  }
}
```

| 键 | 默认 | 控制的工具 |
|---|---|---|
| `read` | `true` | 所有只读工具 |
| `write` | `true` | 更新类 + `token_import` / `token_clear` / `delete_file` |
| `create` | `true` | 建仓库 / Issue / 分支 / PR / Release |
| `delete_repository` | **`false`** | `github_delete_repository` |
| `delete_branch` | **`false`** | `github_delete_branch` |
| `force_push` | **`false`** | 预留（当前无对应工具） |
| `merge_pull_request` | `true` | `github_merge_pull_request` |

- 未知字段**忽略**（前向兼容）；**未知权限键一律拒绝**，防止新工具漏登记开关被静默放行；
- 配置文件损坏或不合法的 JSON → 回退默认值，不会启动失败。

### 运行时二次确认

仅靠开关不够：开关一旦打开，任意一次调用都能执行。所以**不可逆操作必须逐次确认**。

| 工具 | 需要确认 |
|---|---|
| `github_delete_repository` | ✅ |
| `github_delete_branch` | ✅ |
| `github_delete_file` | ✅ |
| `github_merge_pull_request` | ✅ |
| `github_token_clear` | ✅ |

流程：

1. 首次调用返回 `{"status": "confirmation_required", "confirm_token": "<32 位 hex>", "expires_at_unix_ms": ...}`；
2. Agent 携带**相同参数** + `confirm_token` 再调用一次，才真正执行；
3. 票据 **60 秒过期**、**一次性**（重放会被拒绝）；
4. 票据**绑定目标指纹**（工具名 + 参数哈希），改目标即失效。

`confirm_token` 由 `BCryptGenRandom` 生成，不是时间戳或随机数种子——可猜的票据等于没有防护。

## 限流处理

GitHub 的额度随每个响应的 `X-RateLimit-*` 头返回，客户端会跟踪最近一次状态。

**只在三个条件全满足时才自动等待**：

| 条件 | 理由 |
|---|---|
| 错误类型是限流 | 不碰其他错误 |
| **方法是 GET** | 幂等，重发无副作用 |
| 等待时长 ≤ **10 秒** | 超过就立即返回，不阻塞调用方 |

**写操作永不自动重试**——请求可能已被服务端处理，重发会造成重复创建或重复提交。这是安全红线，不是性能取舍。

等待超阈值或无法估算时，立即返回并附带可执行信息：

```json
{
  "ok": false,
  "error_kind": "rate_limited",
  "retry_after_seconds": 42,
  "rate_limit": { "limit": 5000, "remaining": 0, "reset_at": 1795800000 },
  "hint": "额度已耗尽（上限 5000），请在约 42 秒后重试"
}
```

> **实测发现**：`/rate_limit` 端点**不消耗配额**——连续调用 40 次 `remaining` 保持不变，而 `GET /users/:name` 会正常扣减。所以调用 `github_rate_limit` 观察到的数字不会因自查而下降，这是正常行为。

## 安全设计

| 措施 | 说明 |
|---|---|
| DPAPI 加密 | `CryptProtectData`，**不**启用 `CRYPTPROTECT_LOCAL_MACHINE`，保持绑定当前用户 |
| 第二因子 | 熵 = `SHA-256(编译期常量 \| MachineGuid)`。换机器或换用户即无法解密——这正是「需要重新导入」的正确语义 |
| 禁止弹窗 | `CRYPTPROTECT_UI_FORBIDDEN`，解密不会阻塞 stdio 服务 |
| 文件 ACL | 落盘后 DACL 替换为仅含当前用户 SID，移除继承 |
| 原子写入 | 先写临时文件 → flush → 改名覆盖，断电不会留下半截密文 |
| 内存清零 | 明文用 `SecureZeroMemory` 清除，避免被编译器优化掉 |
| 日志脱敏 | 写入前正则替换 `ghp_*` / `github_pat_*`，兜底防误拼 |
| 令牌不外泄 | 所有工具返回值、日志、stdout 均不含令牌明文 |
| 无监听端口 | 纯 stdio，不开放任何本地网络端口 |

### WinHTTP 的四个坑（都已处理）

1. **gzip**：WinHTTP 不自动解压，请求显式要求 `Accept-Encoding: identity`；
2. **TLS**：显式指定 TLS 1.2 / 1.3，不依赖系统默认；
3. **重定向**：禁止自动跟随——跟随便可能把 `Authorization` 头发到别的主机，这是凭据泄露；
4. **超时**：连接/发送 10 秒、接收 30 秒、总计 60 秒，避免网络异常时进程无限挂起。

## 目录结构

```text
GitHub-Bridge/
├── include/
│   ├── Types/      公共契约：RPC 消息、工具元数据、GitHub 响应、权限、安全（7）
│   ├── MCP/        JSON-RPC 编解码 + stdio 行分帧传输（2）
│   ├── Core/       工具注册表、请求分派、内建/令牌/GitHub 工具（5）
│   ├── Security/   DPAPI 存储、令牌管理、权限与确认票据（3）
│   ├── GitHub/     WinHTTP 传输、REST 语义层、限流跟踪（3）
│   └── Utils/      日志（仅 stderr + 脱敏）（1）
├── src/            与 include 一一对应的实现（15 个 .cpp，约 3500 行）
├── third_party/    nlohmann/json 3.12.0（vendored）
└── GitHub-Bridge.slnx + GitHub-Bridge.settings.props
```

## 已知限制

- 只支持**单令牌**，多账号 profile 未实现；
- 无**通用**请求重试（网络抖动直接失败），无响应缓存；
- **`github_graphql` 绕过权限开关**：它挂 `read` 键，但 GraphQL mutation 能写数据（见上文说明）；
- 无单元测试工程，现有验证为手工测试脚本；
- 无 CMake 工程，仅 MSBuild；
- `/rate_limit` 不消耗配额，容易被误判为「额度没下降」；
- 数据文件位置：`%APPDATA%\GitHub-Bridge\tokens.dat`；
- 权限配置：`%APPDATA%\GitHub-Bridge\permissions.json`。

## 文档

- [idea.md](./idea.md) — 完整设计目标、工具清单、权限模型与安全约定
- [LICENSE](./LICENSE) — MIT
