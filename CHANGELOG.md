# 更新日志

本文件记录每个版本的变化。项目尚未遵循严格语义化版本，`0.x` 期间接口仍可能调整。

## v0.1.0 — 首个可用版本

首个功能完整的版本。35 个工具、31 个 GitHub API 端点、89 条单元测试，Windows 与 Linux 双端可用。

### MCP 协议

- stdio 传输，一行一条 JSON-RPC 2.0 消息
- 协议版本固定 `2024-11-05`，不做隐式漂移
- stdout 只承载协议消息，日志一律走 stderr

### 工具（35 个，覆盖 31 个 API 端点）

| 类别 | 数量 | 内容 |
|---|---|---|
| 服务与令牌 | 7 | `server_info`、`token_status`、`token_import`、`token_use`、`token_clear` 等 |
| Repository | 5 | 搜索、查询、创建、更新、删除 |
| Files | 4 | 读取、元数据、写/更新、删除 |
| Issues | 5 | 列表、查询、创建、更新、关闭 |
| Branches | 4 | 列表、查询、创建、删除 |
| Pull Requests | 5 | 列表、查询、创建、更新、合并 |
| Releases | 4 | 列表、查询、创建、更新 |
| GraphQL | 1 | 任意查询，mutation 受权限开关约束 |

### 安全

- **令牌加密存储**：Windows 用 DPAPI（带 `pOptionalEntropy` 第二因子）；Linux 用 libsodium `crypto_secretbox`，密钥经 Argon2id 从 `/etc/machine-id` 派生
- **令牌不出现在**任何工具返回值、日志或标准输出里
- **权限开关**：`read` / `write` / `create` / `delete_repository` / `delete_branch` / `force_push` / `merge_pull_request` 七个独立开关
- **仓库白名单**：把 Agent 限制在指定仓库，支持 `*` 通配，大小写不敏感
- **运行时二次确认**：不可逆操作首次调用返回一次性 `confirm_token`，需在同一进程内携带票据重试
- **文件权限**：`tokens.dat` 收紧到仅当前用户

### 可靠性

- **限流处理**：读取 `X-RateLimit-Remaining` / `Retry-After`，主动退避而非撞墙
- **暂时性故障重试**：仅 GET，两次重试，500ms / 1500ms 退避；**写操作不重试**
- **ETag 响应缓存**：LRU 64 条，单条上限 256KB

### 工程质量

- **89 条单元测试**：JSON 编解码、注册表、权限、重试、限流、缓存、GraphQL 守卫、令牌 profile、仓库白名单
- **跨平台**：三层抽象（`IHttpTransport` / `ITokenVault` / `IPlatform`），Windows 用 WinHTTP + DPAPI，Linux 用 libcurl + libsodium
- **双构建系统**：MSBuild（`build.ps1`）与 CMake 并存

### 终端工具

- `check-env.ps1`：环境自检，检查二进制、令牌、权限配置、网络、工具注册，退出码可用于 CI
- `import-token.ps1`：在终端导入令牌，令牌不经过对话记录

### 已知限制

- 仅支持 x64
- macOS 未实现（接口已就位）
- Linux 令牌加密强度依赖 `/etc/machine-id`，弱于 Windows DPAPI
- GraphQL mutation 用词法判定，极少数构造可能漏判
- 写操作不做自动重试（有意为之）

### 实测结论

发版前做过的实测，结论已写进 README：

- `/rate_limit` **不消耗配额**
- 304 与 200 **同样消耗配额**，缓存价值仅在带宽与延迟
- `WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY` 会走系统代理，**绕过 hosts**
- `confirm_token` 绑定进程内存，**不可跨进程重放**
- `list_releases` 创建后立即查询可能为空，属 GitHub 最终一致性延迟
