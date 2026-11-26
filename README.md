# GitHub-Bridge

本机 MCP Server（stdio），让 AI Agent 能调 GitHub API，而**永远拿不到 Personal Access Token**。

Windows 11 x64 · C++20 · Visual Studio 2026 · MCP 规范固定 `2024-11-05`

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

| 阶段 | 状态 |
|---|---|
| MCP stdio 传输 + JSON-RPC 2.0 收发 | ✅ 已实现，实测通过 |
| `initialize` / `tools/list` / `tools/call` / `ping` / 通知 | ✅ 已实现，实测通过 |
| DPAPI 令牌加密存储（第二因子 + ACL + 原子写入） | ✅ 已实现，实测通过 |
| WinHTTP 传输 + `/user` + `/rate_limit` | ✅ 已实现，详见下文「GitHub API 支持情况」 |
| 仓库 / 文件 / Issue / PR / Release 工具 | ⬜ 未开始 |
| PermissionManager 权限开关 + 二次确认 | ⬜ 契约已定义，未实现 |
| GraphQL API | ⬜ 未开始 |
| 请求重试 / 响应缓存 | ⬜ 未开始 |

## 已实现的工具（6 个）

| 工具 | 说明 | 联网 | 写操作 |
|---|---|---|---|
| `github_server_info` | 返回服务版本、协议版本、工具数量、服务器时间 | 否 | 否 |
| `github_token_status` | 令牌状态：`configured`（本地可解密）、`valid`（GitHub 认可）、`validated`（是否真的校验过） | 是 | 否 |
| `github_token_import` | 导入 PAT，DPAPI 加密落盘并收紧 ACL | 否 | 是 |
| `github_token_clear` | 删除本地令牌文件并清空内存缓存 | 否 | 是 |
| `github_get_user` | 调 `/user`，返回当前令牌对应的账号 | 是 | 否 |
| `github_rate_limit` | 调 `/rate_limit`，无令牌时以匿名身份查询 | 是 | 否 |

`github_token_status` 的三个布尔值刻意分开：**本地能解密 ≠ GitHub 认可该令牌**。网络不通时 `validated` 保持 `false`，避免把「没连上」误报成「令牌坏了」。

## GitHub API 支持情况

目标 **31** 个端点（取自 [idea.md](./idea.md) 的 MCP Tools 章节），已实现 **3** 个，其余 **28** 个未实现（含 GraphQL）。

### 已实现的 API

| 工具 | REST 端点 | 说明 | 验证状态 |
|---|---|---|---|
| `github_get_user` | `GET /user` | 当前令牌对应的账号 login / id / name / type，回填 `X-OAuth-Scopes` | ✅ 真令牌实测 |
| `github_rate_limit` | `GET /rate_limit` | 各资源限额；匿名 60/h，认证 5000/h | ✅ 匿名与认证均实测 |
| `github_token_status` | `GET /user` | 用真实请求校验令牌有效性，401 即 `valid: false` | ✅ 有效与吊销两种状态均实测 |

这三项共享 `include/GitHub/` 的传输层（TLS 1.2/1.3、gzip、禁重定向、超时、10 类错误分类、响应头抽取），后续 API 直接复用，不需要重做网络层。

### 未实现的 API

#### Repository（5）

- ⬜ `github_search_repositories` — `GET /search/repositories`
- ⬜ `github_get_repository` — `GET /repos/{owner}/{repo}`
- ⬜ `github_create_repository` — `POST /user/repos`
- ⬜ `github_update_repository` — `PATCH /repos/{owner}/{repo}`
- ⬜ `github_delete_repository` — `DELETE /repos/{owner}/{repo}`

#### Files（4）

- ⬜ `github_get_file` — `GET /repos/{owner}/{repo}/contents/{path}`
- ⬜ `github_get_file_content` — 同上，并解码 base64 正文
- ⬜ `github_create_or_update_file` — `PUT /repos/{owner}/{repo}/contents/{path}`
- ⬜ `github_delete_file` — `DELETE /repos/{owner}/{repo}/contents/{path}`

#### Issues（5）

- ⬜ `github_list_issues` — `GET /repos/{owner}/{repo}/issues`
- ⬜ `github_get_issue` — `GET /repos/{owner}/{repo}/issues/{number}`
- ⬜ `github_create_issue` — `POST /repos/{owner}/{repo}/issues`
- ⬜ `github_update_issue` — `PATCH /repos/{owner}/{repo}/issues/{number}`
- ⬜ `github_close_issue` — `PATCH` 改 `state`

#### Branches（4）

- ⬜ `github_list_branches` — `GET /repos/{owner}/{repo}/branches`
- ⬜ `github_get_branch` — `GET /repos/{owner}/{repo}/branches/{branch}`
- ⬜ `github_create_branch` — `POST /repos/{owner}/{repo}/git/refs`
- ⬜ `github_delete_branch` — `DELETE /repos/{owner}/{repo}/git/refs/heads/{branch}`

#### Pull Requests（5）

- ⬜ `github_list_pull_requests` — `GET /repos/{owner}/{repo}/pulls`
- ⬜ `github_get_pull_request` — `GET /repos/{owner}/{repo}/pulls/{number}`
- ⬜ `github_create_pull_request` — `POST /repos/{owner}/{repo}/pulls`
- ⬜ `github_update_pull_request` — `PATCH /repos/{owner}/{repo}/pulls/{number}`
- ⬜ `github_merge_pull_request` — `PUT /repos/{owner}/{repo}/pulls/{number}/merge`

#### Releases（4）

- ⬜ `github_list_releases` — `GET /repos/{owner}/{repo}/releases`
- ⬜ `github_get_release` — `GET /repos/{owner}/{repo}/releases/{id}`
- ⬜ `github_create_release` — `POST /repos/{owner}/{repo}/releases`
- ⬜ `github_update_release` — `PATCH /repos/{owner}/{repo}/releases/{id}`

#### 其他

- ⬜ GraphQL API — `POST /graphql`

> 工具名与端点取自 `idea.md`，参数结构在实现时按该文档风格补充。
> **写操作与删除类端点**建议先落地 PermissionManager 与 `confirm_token` 二次确认再开放，避免无防护的不可逆操作。

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
│   ├── Types/      公共契约：RPC 消息、工具元数据、GitHub 响应、权限、安全
│   ├── MCP/        JSON-RPC 编解码 + stdio 行分帧传输
│   ├── Core/       工具注册表、请求分派、内建/令牌/GitHub 工具
│   ├── Security/   DPAPI 存储（TokenStorage）与业务层（TokenManager）
│   ├── GitHub/     WinHTTP 传输 + GitHub REST 语义层
│   └── Utils/      日志（仅 stderr + 脱敏）
├── src/            与 include 一一对应的实现
├── third_party/    nlohmann/json 3.12.0（vendored）
└── GitHub-Bridge.slnx + GitHub-Bridge.settings.props
```

## 已知限制

- 只支持**单令牌**，多账号 profile 未实现；
- 只实现了 `/user` 与 `/rate_limit` 两个端点，其余 GitHub 能力待补齐；
- 无自动重试、无响应缓存、无 GraphQL；
- PermissionManager 与危险操作二次确认机制**尚未落地**（`idea.md` 已定义设计）；
- 数据文件位置：`%APPDATA%\GitHub-Bridge\tokens.dat`。

## 文档

- [idea.md](./idea.md) — 完整设计目标、工具清单、权限模型与安全约定
- [LICENSE](./LICENSE) — MIT
