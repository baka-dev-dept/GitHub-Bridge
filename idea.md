# GitHubGateway

**A Local GitHub MCP Gateway for AI Agents**

GitHubGateway 是一个基于 C++20 开发的本地 GitHub MCP Server，允许 AI Agent 通过 MCP（Model Context Protocol）安全地访问和操作 GitHub。

项目采用本地 stdio 通信方式，不开放 HTTP 服务。GitHub Personal Access Token（PAT）由 Gateway 内部管理，AI Agent 无法直接读取 Token。

## 定位与边界

GitHubGateway 走 **GitHub REST / GraphQL API**，不经过 git 协议，也不接管 git 的凭据体系。

| 场景 | 由谁负责 |
|---|---|
| 建仓库、改仓库设置、删仓库 | **Gateway**（通过 API） |
| 读写仓库文件、Issue、PR、Branch、Release | **Gateway**（通过 API） |
| 本地 `git push` / `git pull` / 克隆 | **git 自身**（credential helper） |
| 本地多账号凭据切换 | **git 配置**（`credential.helper`、`useHttpPath`） |

两者互补，但不可互相替代：

- Gateway **不能**修复 `git push` 弹出登录框之类的凭据问题。
- git **不能**替代 Gateway 的平台级操作能力（Issue、PR、Release 等 API 能力）。
- Gateway 的权限配置**不能**扩大 PAT 本身拥有的权限。

## Features

勾选框含义：`[x]` 已实现，`[ ]` 未实现。实现状态以本节为唯一准绳，其余章节描述的是设计目标。

### Core

- [x] Windows 11 x64
- [x] C++20
- [x] Visual Studio 2022
- [x] 本地 MCP Server
- [x] MCP stdio 通信架构
- [x] MCP JSON-RPC 完整协议实现
- [x] MCP Tool 自动注册与分发

### Security

- [x] Windows DPAPI Token 加密存储（含 `pOptionalEntropy` 第二因子）
- [x] Token 自动加载
- [x] Token 状态检查（实际调用 API 校验，而非仅读配置）
- [x] Token 不暴露给 AI Agent
- [x] 敏感信息脱敏
- [x] 操作权限管理
- [x] 危险操作独立授权开关 + 运行时二次确认

### GitHub Integration

- [x] GitHub REST API
- [x] GitHub GraphQL API
- [x] 仓库查询与管理
- [x] 文件读取与写入
- [x] Issue 管理
- [x] Pull Request 管理
- [x] Branch 管理
- [x] Release 管理
- [x] API Rate Limit 查询

> 当前 README 描述的是项目设计目标。尚未实现的功能不会被视为已交付功能。

---

## Architecture

```text
┌──────────────────────────────┐
│          AI Agent            │
│                              │
│       MCP Client             │
└──────────────┬───────────────┘
               │
               │ MCP stdio
               │
┌──────────────▼───────────────┐
│       GitHubGateway        │
│                              │
│  ┌────────────────────────┐  │
│  │     MCP Server         │  │
│  │                        │  │
│  │ Initialize             │  │
│  │ Tools List             │  │
│  │ Tools Call             │  │
│  └───────────┬────────────┘  │
│              │               │
│  ┌───────────▼────────────┐  │
│  │     Tool Registry     │  │
│  └───────────┬────────────┘  │
│              │               │
│  ┌───────────▼────────────┐  │
│  │     Gateway Core      │  │
│  │                        │  │
│  │ Permission Manager    │  │
│  │ Request Dispatcher    │  │
│  │ Response Formatter    │  │
│  └───────────┬────────────┘  │
│              │               │
│  ┌───────────▼────────────┐  │
│  │    GitHub Client      │  │
│  │                        │  │
│  │ REST API               │  │
│  │ GraphQL API            │  │
│  └───────────┬────────────┘  │
│              │               │
│  ┌───────────▼────────────┐  │
│  │     Token Manager     │  │
│  │                        │  │
│  │ DPAPI Encryption      │  │
│  │ Token Storage         │  │
│  └────────────────────────┘  │
└──────────────┬───────────────┘
               │
               │ HTTPS
               │
┌──────────────▼───────────────┐
│          GitHub API          │
└──────────────────────────────┘
```

### TokenManager 与 TokenStorage 的职责边界

两者容易混淆，职责严格分离：

| 组件 | 职责 | 不做的事 |
|---|---|---|
| `TokenStorage` | 纯持久化层：DPAPI 加密/解密、文件读写、权限设置（ACL） | 不校验 Token 有效性，不了解 GitHub API |
| `TokenManager` | 业务层：加载/保存 Token、内存缓存、状态检查、对上层提供获取接口 | 不直接接触文件，全部经由 `TokenStorage` |

调用关系：

```text
Gateway Core / GitHub Client
        │  请求 Token
        ▼
   TokenManager  ── 内存缓存命中则直接返回
        │  未命中
        ▼
   TokenStorage  ── DPAPI 解密 ──► tokens.dat
```

约定：

- Token 明文只在内存中短暂存在，使用后立即 `SecureZeroMemory` 清零。
- `TokenManager` 对外只暴露「拿到 Authorization Header 值」和「Token 是否可用」，不暴露明文本身。
- 日志模块不得接触这两个组件的内部数据。


## Project Structure

```text
GitHubGateway/
│
├── CMakeLists.txt
├── README.md
├── LICENSE
│
├── include/
│   ├── Core/
│   │   ├── Gateway.h
│   │   ├── ToolRegistry.h
│   │   └── PermissionManager.h
│   │
│   ├── MCP/
│   │   ├── MCPServer.h
│   │   ├── MCPProtocol.h
│   │   └── MCPTransport.h
│   │
│   ├── GitHub/
│   │   ├── GitHubClient.h
│   │   ├── GitHubRepository.h
│   │   ├── GitHubIssue.h
│   │   └── GitHubPullRequest.h
│   │
│   ├── Security/
│   │   ├── TokenManager.h
│   │   └── TokenStorage.h
│   │
│   ├── Cache/
│   │   └── CacheManager.h
│   │
│   └── Utils/
│       ├── Logger.h
│       └── ResponseFormatter.h
│
├── src/
│   ├── Core/
│   ├── MCP/
│   ├── GitHub/
│   ├── Security/
│   ├── Cache/
│   └── Utils/
│
├── config/
│   └── README.md
│
├── docs/
│   ├── ARCHITECTURE.md
│   ├── SECURITY.md
│   └── MCP_TOOLS.md
│
└── tests/
    ├── MCP/
    ├── GitHub/
    └── Security/
```

## Technology Stack

| Component | Technology |
|---|---|
| Language | C++20 |
| Platform | Windows 11 x64 |
| IDE | Visual Studio 2022 |
| Build System | CMake |
| HTTP Client | Windows WinHTTP |
| JSON | nlohmann/json |
| Encryption | Windows DPAPI |
| Communication | MCP stdio |
| API | GitHub REST API |
| Optional API | GitHub GraphQL API |

### Dependencies

- Windows SDK
- Visual Studio 2022 C++ Desktop Development
- CMake 3.25+
- nlohmann/json

建议通过 CMake FetchContent 或项目依赖管理统一管理第三方库版本。

### WinHTTP 实现的硬性要求

WinHTTP 能跑通 GitHub API，但有四个坑必须在实现时处理，否则会在真机上失败：

1. **必须显式处理 gzip**：GitHub 默认返回 `Content-Encoding: gzip`。WinHTTP 不会自动解压，要么在请求头里发 `Accept-Encoding: identity` 禁用压缩，要么自行调用 `decompress`。**不处理会导致 JSON 解析失败**。
2. **TLS 版本必须显式指定**：通过 `WINHTTP_OPTION_SECURE_PROTOCOLS` 设定 TLS 1.2 及以上，不要依赖系统默认。
3. **必须显式禁止自动重定向**：GitHub 部分接口会 301/302，WinHTTP 默认跟随会把 `Authorization` 头带到别的主机。设为 `WINHTTP_OPTION_DISABLE_FEATURE` + `WINHTTP_DISABLE_REDIRECTS` 后自行处理。
4. **超时必须设置**：`WinHttpSetTimeouts` 设 Connect/Receive/Total，否则网络异常时会无限挂起。

如改用 libcurl，则 1、2、4 由库处理，但会引入第三方依赖。当前方案选 WinHTTP，上述四点属实现必做项。

### MCP 协议版本

本项目**固定使用 MCP 规范版本 `2024-11-05`**。

- `initialize` 请求中 `protocolVersion` 返回该固定值。
- stdio 消息 framing 采用**换行分隔的 JSON-RPC 2.0 消息**（每条消息一行）。
- 不接受客户端提议的其他版本，若客户端版本不兼容，在 `initialize` 响应中明确返回服务端支持的版本。

版本一经确定不做隐式漂移，升级需显式修改并更新本文档。


---

## Requirements

- Windows 11 x64
- Visual Studio 2022
- MSVC 支持 C++20
- CMake 3.25 或更高版本
- GitHub Personal Access Token

## Build

### 1. Clone

```powershell
git clone https://github.com/your-name/GitHubGateway.git

cd GitHubGateway
```

### 2. Configure

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
```

### 3. Build

```powershell
cmake --build build --config Release
```

### 4. Run

```powershell
.\build\Release\GitHubGateway.exe
```

程序启动后通过标准输入和标准输出处理 MCP 消息。

### stdout / stdin 必须切二进制模式（必做）

Windows 上 CRT 默认以文本模式打开 `stdout`/`stdin`，会把 `\n` 自动转成 `\r\n`，导致 JSON-RPC 的消息边界错乱，MCP 客户端解析失败。此问题报错信息通常与协议内容无关，极难定位。

**程序最开头（任何输出之前）必须执行：**

```cpp
#include <io.h>
#include <fcntl.h>

_setmode(_fileno(stdin),  _O_BINARY);
_setmode(_fileno(stdout), _O_BINARY);
```

同时注意：

- 日志、调试信息一律写 `stderr`，不得写 `stdout`。
- `stdout` 不得有任何非 MCP 消息的输出（包括 `printf` 调试残留、第三方库的 banner）。
- 若使用 `std::cout`，同时确认未被 `printf` 等其他路径复用。


---

## GitHub Token

GitHubGateway 使用 GitHub Personal Access Token 访问 GitHub API。

### Token Type

推荐使用：

**Fine-grained Personal Access Token**

可以根据实际使用需求限制：

- 指定可访问的 Repository
- Repository permissions
- Contents
- Issues
- Pull requests
- Metadata
- Administration（仅在确实需要时）

### Token Storage

Token 不应直接保存在：

- MCP 配置文件
- 源代码
- 命令行参数
- 普通 JSON 配置文件
- 日志文件

建议采用以下存储方式：

```text
%APPDATA%/
└── GitHubGateway/
    └── tokens.dat
```

加密方案：

```text
GitHub PAT
    │
    ▼
Windows DPAPI
    │
    ▼
Encrypted Token
    │
    ▼
tokens.dat
```

DPAPI 使用当前 Windows 用户上下文进行保护。默认情况下，只有对应用户上下文才能解密。

### DPAPI 实现要点

```text
GitHub PAT
    │
    ▼
CryptProtectData(
    dwFlags = CRYPTPROTECT_UI_FORBIDDEN,
    pOptionalEntropy = <固定熵>
)
    │
    ▼
tokens.dat
```

- **必须提供 `pOptionalEntropy`（第二因子）**。DPAPI 默认仅靠用户登录态保护，同账号下的任意进程都能解密；加入固定熵后，攻击者还需知道该熵值才能解开。
- 熵值**不得**硬编码进源码明文，建议由可执行文件内一段固定常量派生，或与另一处本地数据组合生成。
- 设置 `CRYPTPROTECT_UI_FORBIDDEN`，避免解密时弹出 UI 阻塞 stdio 服务。
- 解密失败（用户换机、系统重装、熵值变更）时，**不得崩溃**，应提示重新配置 Token。
- `tokens.dat` 落盘后限制 ACL 为当前用户可读，其他用户无权限。

---

## MCP Integration

GitHubGateway 通过 MCP stdio 与 AI Agent 通信。

### Example Configuration

以下为 MCP Client 的配置示例：

```json
{
  "mcpServers": {
    "github": {
      "command": "D:\\service\\GitHubGateway\\GitHubGateway.exe",
      "args": [],
      "env": {}
    }
  }
}
```

根据实际编译输出位置修改 `command`。

### Communication

```text
AI Agent
    │
    ▼
MCP Client
    │
    ▼
GitHubGateway.exe
    │
    ▼
GitHub API
```

- 使用 stdio 通信。
- 不需要开放本地 HTTP 端口。
- 不需要配置公网地址。
- Token 由 Gateway 自行管理。
- MCP 协议消息与日志必须分离。

MCP 协议版本及 stdio 消息 framing 已在上文「MCP 协议版本」章节固定为 `2024-11-05` 与换行分隔 JSON-RPC 2.0。

---

## MCP Tools

以下是计划提供给 AI Agent 的工具接口。

### Authentication

| Tool | Description |
|---|---|
| `github_token_status` | 校验 Token 实际可用性（调用 `GET /user`），返回有效期/scope/剩余额度 |
| `github_get_user` | 获取当前认证用户信息 |
| `github_rate_limit` | 查询 GitHub API Rate Limit |

`github_token_status` 返回结构（约定）：

```json
{
  "configured": true,
  "valid": true,
  "login": "Sakiora-AI",
  "user_id": 361398620,
  "scopes": ["repo", "workflow"],
  "token_type": "fine_grained | classic",
  "expires_at": "2026-12-31T00:00:00Z",
  "rate_limit": {
    "limit": 5000,
    "remaining": 4821,
    "reset_at": "2026-11-26T22:00:00Z"
  }
}
```

约定：

- **`configured` 与 `valid` 必须分开**：只读到本地配置文件不代表 Token 可用。
- `valid` 的判定必须来自真实 API 调用，HTTP 401 即 `false`。
- `scopes` 来自响应头 `X-OAuth-Scopes`；fine-grained token 该头可能为空，此时填 `token_type: "fine_grained"` 并说明权限需查 API 说明。
- 任何情况下**不得**在返回值中包含 Token 明文或其片段。

### Repository

| Tool | Description |
|---|---|
| `github_search_repositories` | 搜索仓库 |
| `github_get_repository` | 获取仓库信息 |
| `github_create_repository` | 创建仓库 |
| `github_update_repository` | 修改仓库信息 |
| `github_delete_repository` | 删除仓库 |

### Files

| Tool | Description |
|---|---|
| `github_get_file` | 读取仓库文件 |
| `github_create_or_update_file` | 创建或更新文件 |
| `github_delete_file` | 删除文件 |
| `github_get_file_content` | 获取文件内容 |

### Issues

| Tool | Description |
|---|---|
| `github_list_issues` | 获取 Issue 列表 |
| `github_get_issue` | 获取 Issue 详情 |
| `github_create_issue` | 创建 Issue |
| `github_update_issue` | 更新 Issue |
| `github_close_issue` | 关闭 Issue |

### Branches

| Tool | Description |
|---|---|
| `github_list_branches` | 获取分支列表 |
| `github_get_branch` | 获取分支信息 |
| `github_create_branch` | 创建分支 |
| `github_delete_branch` | 删除分支 |

### Pull Requests

| Tool | Description |
|---|---|
| `github_list_pull_requests` | 获取 PR 列表 |
| `github_get_pull_request` | 获取 PR 详情 |
| `github_create_pull_request` | 创建 PR |
| `github_update_pull_request` | 更新 PR |
| `github_merge_pull_request` | 合并 PR |

### Releases

| Tool | Description |
|---|---|
| `github_list_releases` | 获取 Release 列表 |
| `github_get_release` | 获取 Release 详情 |
| `github_create_release` | 创建 Release |
| `github_update_release` | 更新 Release |

> 上述工具为目标接口清单。工具名与语义即最终约定；参数结构需在实现时按本文档风格补充，不得与本节命名冲突。

---

## Permission System

项目采用 Gateway 内部权限管理机制。

默认目标：

**AI Agent 可以自动读取和写入授权范围内的 GitHub 资源。**

权限设计：

| Operation | Permission |
|---|---|
| Read Repository | Allow |
| Read Files | Allow |
| Write Files | Allow |
| Create Issue | Allow |
| Update Issue | Allow |
| Create Branch | Allow |
| Create Pull Request | Allow |
| Merge Pull Request | Configurable |
| Delete Branch | Configurable |
| Delete Repository | Disabled by default |

危险操作建议单独配置：

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

该配置**即最终配置格式**。字段含义见上表，未列出的字段一律视为未知字段并忽略（前向兼容）。

### 危险操作的运行时二次确认

仅靠配置开关防护不足：开关一旦打开，任何一次 Agent 调用都可直接执行不可逆操作。因此对**不可逆操作**必须额外加运行时确认。

适用范围：

| 操作 | 不可逆 | 需二次确认 |
|---|---|---|
| `github_delete_repository` | 是 | **是** |
| `github_delete_branch` | 是 | **是** |
| `github_merge_pull_request` | 难回退 | **是** |
| `github_delete_file` | 是 | **是** |
| 其他读写操作 | 否 | 否 |

确认机制约定：

- 首次调用返回 `status: "confirmation_required"` 与一个一次性 `confirm_token`（含目标对象指纹、过期时间，默认 60 秒）。
- Agent 必须携带 `confirm_token` 再次调用同一工具同一参数，才真正执行。
- `confirm_token` 与具体目标绑定，改目标即失效。
- 超时未确认自动作废。
- 该机制**不得**仅实现为「调用两次即可」，必须校验 token 内容与目标一致。

注意：Gateway 权限策略不能扩大 PAT 本身拥有的 GitHub 权限。实际操作仍受 GitHub Token 权限和仓库访问范围限制。

---

## Security

安全设计目标：

1. Token 使用 Windows DPAPI 加密保存。
2. Token 不允许通过 MCP Tool 直接读取。
3. 不在 stdout 输出调试信息。
4. 不在日志中记录 Token。
5. HTTP 请求统一由 GitHub Client 管理。
6. 所有写入操作经过 Permission Manager。
7. 删除、强制覆盖等高风险操作默认关闭。
8. 限制 Token 文件访问权限。
9. 对敏感内存数据尽可能进行及时清理。
10. 不开放不必要的网络监听端口。

### Security Boundary

```text
AI Agent
    │
    │ MCP Tool
    ▼
Gateway
    │
    ├── Permission Check
    │
    ├── Token Manager
    │
    └── GitHub Client
             │
             ▼
         GitHub API
```

AI Agent 只能调用已注册的 MCP Tools，不能直接访问 Token 存储。

---

## Logging

日志建议统一输出到：

```text
%APPDATA%/
└── GitHubGateway/
    └── logs/
        └── gateway.log
```

日志级别：

| Level | Description |
|---|---|
| TRACE | 详细调试信息 |
| DEBUG | 调试信息 |
| INFO | 一般运行信息 |
| WARN | 警告 |
| ERROR | 错误 |
| FATAL | 致命错误 |

禁止记录：

- GitHub PAT
- Authorization Header
- DPAPI 明文数据
- 其他敏感凭据

---

## Roadmap

### Phase 1 — Core

- [x] CMake 项目结构
- [x] MCP stdio Transport
- [x] MCP JSON-RPC Protocol
- [x] MCP Initialize
- [x] Tool Registry
- [x] Tool Dispatcher
- [x] Logger

### Phase 2 — Security

- [x] DPAPI Token Storage
- [x] Token Manager
- [x] Token Status
- [x] Permission Manager
- [x] Sensitive Data Protection

### Phase 3 — GitHub API

- [x] WinHTTP Client
- [x] GitHub Authentication
- [x] Repository API
- [x] File API
- [x] Issue API
- [x] Branch API
- [x] Pull Request API
- [x] Release API
- [x] Rate Limit API

### Phase 4 — Advanced

- [x] GraphQL API
- [x] Cache Manager
- [x] Request Retry
- [x] Rate Limit Handling
- [ ] Batch Operations —— 暂不做：GitHub 无批量端点，内部只能循环且无事务性；MCP 客户端本身可并发调用
- [x] Advanced Permission Rules（仓库白名单）
- [x] Unit Tests

---

## Design Principles

- **Local First**：仅作为本地 MCP Server 运行。
- **Security First**：Token 不暴露给 AI Agent。
- **Agent Friendly**：工具接口清晰、参数明确、返回结果结构化。
- **Automatic Workflow**：允许 AI Agent 在授权范围内自动执行读写操作。
- **Modular Architecture**：各模块独立，方便后续扩展。
- **Native Windows**：优先使用 Windows 原生 API。

## License

[License](./LICENSE)

---

**Project:** GitHubGateway  
**Language:** C++20  
**Platform:** Windows 11 x64  
**IDE:** Visual Studio 2022  
**Protocol:** Model Context Protocol (MCP)  
**Transport:** stdio
