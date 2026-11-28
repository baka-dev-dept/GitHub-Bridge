# GitHub-Bridge 环境自检
#
# 用途：在接入 MCP 客户端前，快速确认二进制、令牌、权限配置、网络连通性是否就绪。
# 只读——不导入令牌、不改配置、不发写请求。
#
# 用法：
#   .\check-env.ps1                      # 检查 Release 版本
#   .\check-env.ps1 -Configuration Debug # 检查 Debug 版本

param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$exe = Join-Path $root "x64\$Configuration\GitHub-Bridge.exe"
$dataDir = Join-Path $env:APPDATA 'GitHub-Bridge'
$tokenFile = Join-Path $dataDir 'tokens.dat'
$permFile = Join-Path $dataDir 'permissions.json'

$problems = 0

function Report($ok, $label, $detail) {
    $mark = if ($ok) { 'OK  ' } else { 'FAIL' }
    Write-Host ("  [{0}] {1}" -f $mark, $label) -ForegroundColor $(if ($ok) { 'Green' } else { 'Red' })
    if ($detail) { Write-Host ("         {0}" -f $detail) -ForegroundColor Gray }
    if (-not $ok) { $script:problems++ }
}

# 调用一次工具，返回解析后的内容。每次调用都是独立进程。
function Invoke-Tool($toolName, $argumentsJson) {
    $request = '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"' + $toolName + '","arguments":' + $argumentsJson + '}}'
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $exe
    $psi.UseShellExecute = $false
    $psi.RedirectStandardInput = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.CreateNoWindow = $true
    $p = [System.Diagnostics.Process]::Start($psi)
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($request + "`n")
    $p.StandardInput.BaseStream.Write($bytes, 0, $bytes.Length)
    $p.StandardInput.BaseStream.Flush()
    $p.StandardInput.Close()
    $ms = New-Object System.IO.MemoryStream
    $p.StandardOutput.BaseStream.CopyTo($ms)
    $p.StandardError.ReadToEnd() | Out-Null
    $p.WaitForExit()
    $out = [System.Text.Encoding]::UTF8.GetString($ms.ToArray())
    $ms.Dispose()
    $idx = $out.IndexOf([char]10)
    $line = if ($idx -ge 0) { $out.Substring(0, $idx) } else { $out }
    try {
        return ($line | ConvertFrom-Json).result.content[0].text | ConvertFrom-Json
    } catch {
        return $null
    }
}

Write-Host "GitHub-Bridge 环境自检" -ForegroundColor Cyan
Write-Host ""

Write-Host "1. 可执行文件" -ForegroundColor White
if (Test-Path $exe) {
    $sizeKb = [math]::Round((Get-Item $exe).Length / 1KB)
    Report $true "二进制存在" ("$exe ($sizeKb KB)")
} else {
    Report $false "二进制不存在" "先构建：.\build.ps1 -Configuration $Configuration -Platform x64"
}

Write-Host ""
Write-Host "2. 令牌存储" -ForegroundColor White
if (Test-Path $tokenFile) {
    $len = (Get-Item $tokenFile).Length
    $bytes = [System.IO.File]::ReadAllBytes($tokenFile)
    $head = ($bytes[0..([Math]::Min(7, $bytes.Length - 1))] | ForEach-Object { $_.ToString('x2') }) -join ' '
    $isEncrypted = $head.Contains('d0 8c 9d df')
    Report $isEncrypted "密文文件存在" ("$len 字节，头部 $head")
    if (-not $isEncrypted) { Write-Host "         DPAPI 标志缺失，文件可能损坏" -ForegroundColor Yellow }
} else {
    Report $false "尚未导入令牌" "用 github_token_import 导入，或接受匿名额度（60/h）"
}

if ((Test-Path $exe) -and (Test-Path $tokenFile)) {
    $status = Invoke-Tool 'github_token_status' '{}'
    if ($status) {
        Report ([bool]$status.configured) "令牌可解密" ("active profile: " + $status.active_profile)
        if ($status.validated) {
            Report ([bool]$status.valid) "令牌经 GitHub 校验" $(if ($status.valid) { "账号 $($status.login)" } else { "被拒绝: $($status.validation_error)" })
        } else {
            Write-Host "  [INFO] 本次无法联网校验（网络不可达），这不代表令牌无效" -ForegroundColor Yellow
        }
    }
}

Write-Host ""
Write-Host "3. 权限配置" -ForegroundColor White
if (Test-Path $permFile) {
    try {
        $cfg = Get-Content $permFile -Raw | ConvertFrom-Json
        $repos = @($cfg.allowed_repositories | Where-Object { $_ })
        Report $true "配置文件存在" $permFile
        if ($repos.Count -gt 0) {
            Report $true "仓库白名单已启用" ($repos -join ', ')
        } else {
            Write-Host "  [WARN] 白名单未启用——Agent 可操作账号下任意仓库" -ForegroundColor Yellow
            Write-Host "         建议加 allowed_repositories，把 Agent 限制在测试仓库" -ForegroundColor Yellow
        }
    } catch {
        Report $false "配置文件不是合法 JSON" $permFile
    }
} else {
    Report $false "无权限配置（使用默认值）" "默认 read/write/create 全开，白名单未启用"
}

Write-Host ""
Write-Host "4. 网络连通" -ForegroundColor White
if (Test-Path $exe) {
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $rl = Invoke-Tool 'github_rate_limit' '{}'
    $sw.Stop()
    if ($rl -and $rl.ok) {
        Report $true "可访问 GitHub API" ("$($sw.ElapsedMilliseconds) ms，额度 $($rl.remaining)/$($rl.limit)")
    } else {
        Report $false "无法访问 GitHub API" "检查网络或系统代理设置"
    }
}

Write-Host ""
Write-Host "5. 工具清单" -ForegroundColor White
if (Test-Path $exe) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $exe
    $psi.UseShellExecute = $false
    $psi.RedirectStandardInput = $true
    $psi.RedirectStandardOutput = $true
    $psi.CreateNoWindow = $true
    $p = [System.Diagnostics.Process]::Start($psi)
    $bytes = [System.Text.Encoding]::UTF8.GetBytes('{"jsonrpc":"2.0","id":1,"method":"tools/list"}' + "`n")
    $p.StandardInput.BaseStream.Write($bytes, 0, $bytes.Length)
    $p.StandardInput.BaseStream.Flush()
    $p.StandardInput.Close()
    $out = $p.StandardOutput.ReadToEnd()
    $p.WaitForExit()
    $idx = $out.IndexOf([char]10)
    $line = if ($idx -ge 0) { $out.Substring(0, $idx) } else { $out }
    $tools = ($line | ConvertFrom-Json).result.tools
    Report ($tools.Count -eq 35) "工具注册" ("$($tools.Count) 个")
}

Write-Host ""
if ($problems -eq 0) {
    Write-Host "全部检查通过。" -ForegroundColor Green
    Write-Host ""
    Write-Host "MCP 客户端接入配置：" -ForegroundColor Cyan
    $jsonPath = $exe.Replace([string][char]92, [string][char]92 + [char]92)
    Write-Host ('  {"mcpServers":{"github-bridge":{"command":"' + $jsonPath + '","args":[]}}}')
    exit 0
} else {
    Write-Host ("{0} 项需要处理，见上面标 FAIL 的条目。" -f $problems) -ForegroundColor Yellow
    exit 1
}