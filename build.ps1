# Build GitHub-Bridge with a sanitized environment.
# MSBuild's ToolTask crashes (MSB6001) when the environment has keys that differ
# only by case (HTTP_PROXY / http_proxy), which the proxy setup produces.
# This script rebuilds a clean, deduplicated environment and launches MSBuild once.

param(
    [string]$Configuration = 'Debug',
    [string]$Platform = 'x64',
    [string]$Target = ''
)

$ErrorActionPreference = 'Stop'

$sln = 'D:\xiaoyu\source\C++\GitHub-Bridge\GitHub-Bridge.slnx'
$msbuild = 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe'

# Start from the machine environment, then overlay user variables, deduplicating by lowercase.
$merged = [ordered]@{}
foreach ($scope in @('Machine', 'User')) {
    foreach ($item in [System.Environment]::GetEnvironmentVariables($scope).GetEnumerator()) {
        $merged[$item.Key.ToLower()] = @{ Name = $item.Key; Value = $item.Value }
    }
}

# Process-level overrides (highest priority), deduplicated the same way.
foreach ($item in [System.Environment]::GetEnvironmentVariables('Process').GetEnumerator()) {
    $merged[$item.Key.ToLower()] = @{ Name = $item.Key; Value = $item.Value }
}

# Proxy variables are the actual conflict source and are irrelevant to compiling.
foreach ($key in @('http_proxy', 'https_proxy', 'all_proxy', 'no_proxy')) {
    if ($merged.Contains($key)) {
        $merged.Remove($key)
    }
}

$separator = [System.IO.Path]::PathSeparator
$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $msbuild
$arguments = '"' + $sln + '" -p:Configuration=' + $Configuration + ' -p:Platform=' + $Platform + ' -nologo -v:m'
if ($Target) {
    $arguments += ' -t:' + $Target
}
$psi.Arguments = $arguments
$psi.UseShellExecute = $false
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$psi.CreateNoWindow = $true

$envBlock = $psi.Environment
foreach ($entry in $merged.Values) {
    try {
        $envBlock[$entry.Name] = [string]$entry.Value
    } catch {
        # Skip names the provider rejects; they are irrelevant to the compiler.
    }
}

$process = [System.Diagnostics.Process]::Start($psi)
$stdout = $process.StandardOutput.ReadToEnd()
$stderr = $process.StandardError.ReadToEnd()
$process.WaitForExit()

$stdout
if ($stderr) {
    [Console]::Error.Write($stderr)
}
exit $process.ExitCode
