# 在终端里导入 GitHub PAT，令牌不进入对话记录。
#
# 用法：
#   .\import-token.ps1                                  # 交互输入，存到 ai
#   .\import-token.ps1 -Profile work                     # 交互输入，存到 work
#   .\import-token.ps1 -TokenFile 'D:\t.txt'            # 从文件读
#   .\import-token.ps1 -ShowStatus                       # 只看当前状态
#
# 令牌走 stdin 传给程序，不进命令行参数（避免进程列表留痕），也不写日志。

param(
    [ValidatePattern('^[A-Za-z0-9_-]+$')]
    [string]$Profile = 'ai',
    [string]$TokenFile,
    [switch]$ShowStatus,
    [string]$Exe
)

$ErrorActionPreference = 'Stop'

# 找可执行文件：显式指定 > .dsh 部署位置 > 源码构建产物
if (-not $Exe) {
    $candidates = @(
        'D:\xiaoyu\.dsh\GitHub-Bridge.exe',
        (Join-Path $PSScriptRoot 'x64\Release\GitHub-Bridge.exe'),
        (Join-Path $PSScriptRoot 'x64\Debug\GitHub-Bridge.exe')
    )
    $Exe = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
}
if (-not $Exe -or -not (Test-Path $Exe)) {
    Write-Host "找不到 GitHub-Bridge.exe。" -ForegroundColor Red
    Write-Host "先构建，或用 -Exe 指定路径。" -ForegroundColor Gray
    exit 1
}

# 用一次独立进程发若干 JSON-RPC 请求，按顺序返回解析后的 content。
function Invoke-Bridge($requests) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $Exe
    $psi.UseShellExecute = $false
    $psi.RedirectStandardInput = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.CreateNoWindow = $true
    $p = [System.Diagnostics.Process]::Start($psi)

    foreach ($r in $requests) {
        $bytes = [System.Text.Encoding]::UTF8.GetBytes($r + "`n")
        $p.StandardInput.BaseStream.Write($bytes, 0, $bytes.Length)
        $p.StandardInput.BaseStream.Flush()
    }
    $p.StandardInput.Close()

    $ms = New-Object System.IO.MemoryStream
    $p.StandardOutput.BaseStream.CopyTo($ms)
    $p.StandardError.ReadToEnd() | Out-Null
    $p.WaitForExit()
    $text = [System.Text.Encoding]::UTF8.GetString($ms.ToArray())
    $ms.Dispose()

    $results = @()
    foreach ($line in ($text -split "`n")) {
        if ($line -notmatch '\S') { continue }
        try {
            $msg = $line | ConvertFrom-Json
        } catch {
            continue
        }
        # initialize 的响应没有 content，只有 tools/call 的结果才有
        if (-not $msg.result.content) { continue }
        try {
            $results += ( $msg.result.content[0].text | ConvertFrom-Json )
        } catch {
            continue
        }
    }
    # PowerShell 会把单元素数组展开成标量，用逗号强制保留数组语义
    return ,$results
}

$init = '{"jsonrpc":"2.0","id":0,"method":"initialize","params":{"protocolVersion":"2024-11-05"}}'

if ($ShowStatus) {
    $r = Invoke-Bridge @($init, '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"github_token_status","arguments":{}}}')
    if ($r.Count -lt 1) { Write-Host "无法读取状态。" -ForegroundColor Red; exit 1 }
    $s = $r[0]
    Write-Host "令牌状态" -ForegroundColor Cyan
    Write-Host ("  已配置:   {0}" -f $s.configured)
    Write-Host ("  当前 profile: {0}" -f $s.active_profile)
    Write-Host ("  账号:     {0}" -f $(if ($s.login) { $s.login } else { '(未验证)' }))
    Write-Host ("  有效:     {0}" -f $s.valid)
    if ($s.validation_error) { Write-Host ("  错误:     {0}" -f $s.validation_error) -ForegroundColor Yellow }
    $names = ($s.profiles | ForEach-Object { if ($_.active) { "$($_.name)*" } else { $_.name } }) -join ', '
    Write-Host ("  profiles: {0}" -f $names)
    exit 0
}

# 取令牌：文件优先，否则交互输入（SecureString，不回显）
$token = $null
if ($TokenFile) {
    if (-not (Test-Path $TokenFile)) {
        Write-Host "文件不存在: $TokenFile" -ForegroundColor Red
        exit 1
    }
    $token = ([System.IO.File]::ReadAllText($TokenFile)).Trim()
} else {
    Write-Host "粘贴 GitHub PAT 后回车（输入不显示）：" -ForegroundColor Gray
    $secure = Read-Host -AsSecureString
    $bstr = [System.Runtime.InteropServices.Marshal]::SecureStringToBSTR($secure)
    try { $token = [System.Runtime.InteropServices.Marshal]::PtrToStringBSTR($bstr) }
    finally { [System.Runtime.InteropServices.Marshal]::ZeroFreeBSTR($bstr) }
    $token = $token.Trim()
}

if (-not $token) {
    Write-Host "没有拿到令牌，已取消。" -ForegroundColor Red
    exit 1
}

# 只做形态校验，不联网——联网校验留给导入后的 status
if ($token -notmatch '^(ghp_|gho_|ghu_|ghs_|ghr_|github_pat_)[A-Za-z0-9_]+$') {
    Write-Host "令牌格式不对：应以 ghp_ / github_pat_ 等开头，只含字母数字下划线。" -ForegroundColor Red
    Write-Host ("当前长度 {0}。" -f $token.Length) -ForegroundColor Gray
    $token = $null
    exit 1
}

$args = @{ token = $token; profile = $Profile } | ConvertTo-Json -Compress
# 立刻清掉明文引用，后续只用 $args
$token = $null

$importReq = '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"github_token_import","arguments":' + $args + '}}'
$statusReq = '{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"github_token_status","arguments":{}}}'

$r = Invoke-Bridge @($init, $importReq, $statusReq)
$args = $null

if ($r.Count -lt 2) {
    Write-Host "程序没有返回预期结果。" -ForegroundColor Red
    exit 1
}

$imp = $r[0]
$st = $r[1]

if (-not $imp.ok) {
    Write-Host ("导入失败: {0}" -f $imp.message) -ForegroundColor Red
    exit 1
}

Write-Host "已保存到 profile '$Profile'。" -ForegroundColor Green
Write-Host ""
Write-Host "GitHub 校验：" -ForegroundColor Cyan
if ($st.validated) {
    if ($st.valid) {
        Write-Host ("  账号 {0}，scope: {1}" -f $st.login, $st.scopes) -ForegroundColor Green
    } else {
        Write-Host ("  被拒绝：{0}" -f $st.validation_error) -ForegroundColor Red
        Write-Host "  令牌已存下，但 GitHub 不认它——检查是否已过期或被吊销。" -ForegroundColor Yellow
    }
} else {
    Write-Host "  无法联网校验（网络不可达），这不代表令牌无效。" -ForegroundColor Yellow
}
