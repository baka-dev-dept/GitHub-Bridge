# GitHub-Bridge

本机 MCP Server（stdio），让 AI Agent 能调 GitHub API，而**永远拿不到 Personal Access Token**。

Windows 11 x64 · C++20 · Visual Studio 2026 · MCP 规范固定 `2024-11-05`

**35 个工具 · 31 个 GitHub API 端点全部实现 · 76 条单元测试通过**

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
| **限流主动处理 + 暂时性故障重试** | ✅ 已实现，单元验证 22/22 |
| 通用请求重试 | ✅ 已实现（暂时性故障，仅 GET） |
| 响应缓存 | ✅ 已实现（ETag 条件请求） |
| 多令牌 profile | ✅ 已实现（命名 + 显式切换） |
| 单元测试工程 | ✅ 已实现，**76 条用例** |
| CMake 工程 | ⬜ 未实现（仅 MSBuild） |

## 工具清单（35 个）

「确认」列表示该工具是**不可逆操作**，首次调用会返回一次性 `confirm_token`，需携带票据重试才真正执行。

### 服务与令牌（7）

| 工具 | 说明 | 联网 | 写 | 确认 |
|---|---|---|---|---|
| `github_server_info` | 版本、协议版本、工具数量、服务器时间 | 否 | 否 | |
| `github_token_status` | 令牌状态、active profile、全部 profile 列表 | 是 | 否 | |
| `github_token_import` | 导入 PAT 到指定 profile，DPAPI 加密落盘 | 否 | 是 | |
| `github_token_use` | 切换 active profile（本地操作，不联网校验） | 否 | 是 | |
| `github_token_clear` | 删除指定或当前 profile | 否 | 是 | **是** |
| `github_get_user` | 调 `/user` 返回当前账号 | 是 | 否 | |
| `github_rate_limit` | 调 `/rate_limit`，匿名可用 | 是 | 否 | |

### 多令牌 profile

磁盘上用一个 DPAPI 加密文件保存多个命名槽位：

```json
{
  "version": 2,
  "active": "work",
  "profiles": {
    "work":     { "token": "<加密>", "saved_at": 1795800000 },
    "personal": { "token": "<加密>", "saved_at": 1795800100 }
  }
}
```

| 项 | 说明 |
|---|---|
| profile 名 | `[A-Za-z0-9_-]`，1–32 字符，**大小写敏感** |
| 默认名 | `default` |
| 首个导入 | 自动成为 active |
| 切换 | `github_token_use`，**只改本地状态不联网** |
| 删除 | `github_token_clear` + `profile` 参数；删光后文件一并移除 |
| 旧版兼容 | v1 裸令牌文件**自动迁移**为 `default` profile，不丢令牌 |

**切换不校验令牌有效性**——若该令牌已失效，下一次真实调用会返回 401，这是调用方该看到的真实状态。

缓存按授权头隔离，不同 profile 天然不会串用。

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

**权限按 query 内容判定**：工具注册时默认挂 `read`，但执行前会扫描 query 文本——含 `mutation` 关键字的按 `write` 开关裁决。

判定做了保守处理：先剥离字符串字面量（`"..."`、`"""..."""`，含 `\"` 转义）与 `#` 注释，再按**词边界**匹配 `mutation`。因此 `myMutation`、`mutationCount` 这类标识符，以及写在字符串或注释里的 "mutation" 字样，都不会被误判为写操作。

> mutation 没有具体目标对象，无法套用 `confirm_token` 的目标绑定语义，所以它**只受 `write` 开关约束，不要求二次确认**。

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

产物：`x64\Release\GitHub-Bridge.exe`（输出目录基于解决方案根，中间文件在 `x64\out\<工程名>\Release\`）。

```powershell
# 推荐用 build.ps1：它先净化环境（见下），再调 MSBuild
.\build.ps1 -Configuration Release -Platform x64
```

> **`MSB6001` 的成因**：MSBuild 的 ToolTask 内部用大小写敏感的字典装环境变量，遇到 `HTTP_PROXY` 与 `http_proxy` 这类仅大小写不同的键会直接抛异常。常见于配置了代理的机器。`build.ps1` 会合并 Machine/User/Process 三层环境、按小写去重并剔除代理变量，从根上避开。

## 单元测试

76 条用例，纯内存运行——**不需要网络、不需要令牌、不写文件**。

```powershell
# 构建并运行（Debug 或 Release 产物相同）
.\build.ps1 -Configuration Debug -Platform x64
.\x64\Debug\GitHub-Bridge.Tests.exe
```

输出每个用例的 `PASS` / `FAIL`，全部通过时退出码 `0`，有失败时非 `0`（可直接接 CI）。

| 测试文件 | 覆盖内容 |
|---|---|
| `JsonCodecTests.cpp` | JSON-RPC 编解码、通知无 id、id 类型保真、非法输入、单行分帧 |
| `RateLimiterTests.cpp` | 限额状态机：耗尽/过期/缺头/垃圾值/超长窗口 |
| `RetryTests.cpp` | 可重试分类、退避常量、HTTP 状态到 `error_kind` 的翻译 |
| `PermissionTests.cpp` | 权限默认值、目标指纹、未知权限键拒绝、确认票据 |
| `ToolRegistryTests.cpp` | 注册/查找/重名拒绝/无处理函数拒绝 |
| `GraphQLGuardTests.cpp` | mutation 识别、字符串与注释噪声剥离、词边界匹配 |

> **不覆盖**的部分：WinHTTP 真实网络请求、DPAPI 加解密、文件落盘。这些需要真实凭据或系统环境，由手工集成测试负责。

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

```jsonc
// 存入默认 profile
{"token": "<你的 PAT>"}

// 存入命名 profile
{"token": "<你的 PAT>", "profile": "work"}
```

返回值不含令牌明文。

```jsonc
// 切换
github_token_use  {"profile": "work"}

// 查看全部 profile
github_token_status  {}
```

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
| `write` | `true` | 更新类 + `token_import` / `token_clear` / `delete_file` + **GraphQL mutation** |
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

## 限流与重试

GitHub 的额度随每个响应的 `X-RateLimit-*` 头返回，客户端会跟踪最近一次状态。

### 限流处理

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

### 暂时性故障重试

网络抖动与服务端临时故障会自动重试：

| 错误类型 | 重试 | 说明 |
|---|---|---|
| `Transport`（DNS / TLS / 超时 / 连接中断） | ✅ | 最多 2 次 |
| `Server`（5xx） | ✅ | 最多 2 次 |
| `Unauthorized` / `Forbidden` / `NotFound` / `Validation` / `Redirect` / `Decode` | ❌ | 重发结果不变 |

**同样只有 GET 重试。** `POST` / `PATCH` 重发可能重复创建；`PUT` / `DELETE` 虽幂等，但网络失败时无法确定服务端是否已执行，宁可报错让调用方判断。

退避间隔 **500ms → 1500ms**，单次调用最多多花约 2 秒。

> **设计取舍**：MCP 调用是同步阻塞的，Agent 在等结果。重试太多次会把一次调用拖到几十秒，比直接失败还难受，所以只给两次。

## 响应缓存

对 **GET** 请求启用 `ETag` 条件请求，内容未变时复用本地副本。

### 实测结论：304 **不省配额**

匿名交叉验证（同一路径连续请求）：

| 请求类型 | 配额变化 |
|---|---|
| 普通 GET ×3 | 200、200、200 → remaining 40→39→38 |
| 带 `If-None-Match` ×3 | **304**、304、304 → remaining 36→35→34 |

**304 与 200 消耗相同**（各扣 1）。所以缓存的价值是：

| 收益 | 说明 |
|---|---|
| 省带宽 | 304 无响应体，大列表只传几十字节 |
| 省往返延迟 | 304 处理更快 |
| 省解析开销 | 无需反序列化 JSON |
| ~~省配额~~ | **不省**，别指望这个 |

### 机制

| 项 | 说明 |
|---|---|
| 缓存范围 | 仅 `GET` 的成功响应 |
| 缓存键 | `method + path + 授权头散列前缀`（换账号不串用，内存里不留令牌原文） |
| 回放条件 | **只有收到 304** 才用缓存副本，其余一律以服务端返回为准 |
| 容量 | 最多 64 条，单条正文上限 256 KB |
| 淘汰 | LRU |
| TTL | **不设**——ETag 本身就代表内容版本 |
| 开关 | `GHB_CACHE=0` 关闭（默认开启） |

### 哪些端点会被缓存

| 端点 | 返回 ETag | 可缓存 |
|---|---|---|
| `GET /repos/{owner}/{repo}` | ✅ | ✅ |
| `GET .../contents/{path}` | ✅ | ✅ |
| `GET .../branches` | ✅ | ✅ |
| `GET .../issues` | ✅ | ✅ |
| `GET .../releases` | ✅ | ✅ |
| `GET .../pulls` | ✅ | ✅ |
| `GET /rate_limit` | ❌ | ❌ |
| `GET /search/*` | ❌ | ❌ |

响应里带 `from_cache: true` 表示内容来自本地副本。

> **注意**：MCP 服务是长驻进程，缓存在**同一条 stdin 连接内**跨调用生效；进程退出即失效，不做持久化。

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
│   ├── GitHub/     WinHTTP 传输、REST 语义层、限流跟踪、GraphQL 守卫（4）
│   └── Utils/      日志（仅 stderr + 脱敏）（1）
├── src/            与 include 一一对应的实现（16 个 .cpp）
├── third_party/    nlohmann/json 3.12.0（vendored）
├── GitHub-Bridge/          主程序工程
├── GitHub-Bridge.Tests/    单元测试工程（59 条用例）
├── build.ps1               净化环境后调 MSBuild（规避 MSB6001）
└── GitHub-Bridge.slnx + GitHub-Bridge.settings.props
```

## 已知限制


- 重试只覆盖 GET；写操作失败后需由调用方自行判断是否重发；
- `github_graphql` 的 mutation 判定基于词法扫描而非完整语法解析，极少数特殊构造可能漏判；
- 无 CMake 工程，仅 MSBuild；
- `/rate_limit` 不消耗配额，容易被误判为「额度没下降」；
- 数据文件位置：`%APPDATA%\GitHub-Bridge\tokens.dat`；
- 权限配置：`%APPDATA%\GitHub-Bridge\permissions.json`。

## 文档

- [idea.md](./idea.md) — 完整设计目标、工具清单、权限模型与安全约定
- [LICENSE](./LICENSE) — MIT
