[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$BaseRoot,
    [Parameter(Mandatory=$true)][string]$TargetRoot,
    [Parameter(Mandatory=$true)][string]$OutputRoot,
    [Parameter(Mandatory=$true)][ValidatePattern('^[a-fA-F0-9]{64}$')][string]$BaseZipSha256
)
. (Join-Path $PSScriptRoot 'TestDeltaPackage.Common.ps1')
$BaseRoot = Get-DeltaRoot $BaseRoot
$TargetRoot = Get-DeltaRoot $TargetRoot
$OutputRoot = Get-DeltaRoot $OutputRoot
foreach ($source in @($BaseRoot, $TargetRoot)) {
    if ($OutputRoot -eq $source -or $OutputRoot.StartsWith($source + '\', [StringComparison]::OrdinalIgnoreCase) -or $source.StartsWith($OutputRoot + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Output and input trees must not overlap.' }
}
if ((Test-Path -LiteralPath $OutputRoot) -or (Test-Path -LiteralPath "$OutputRoot.zip") -or (Test-Path -LiteralPath "$OutputRoot.reconstructed")) { throw 'Output must be a fresh directory and ZIP name.' }
$baseFiles = @(Get-DeltaFiles $BaseRoot | Sort-Object path)
$targetFiles = @(Get-DeltaFiles $TargetRoot | Sort-Object path)
$baseMap = Get-DeltaMap $baseFiles
$targetMap = Get-DeltaMap $targetFiles
foreach ($required in @('build-info.json', 'IMao-WinUI.runtimeconfig.json', 'resources.pri')) {
    if (-not $baseMap.ContainsKey($required) -or -not $targetMap.ContainsKey($required)) { throw "Required package file missing: $required" }
}
foreach ($path in $baseMap.Keys) { if (-not $targetMap.ContainsKey($path)) { throw "Overlay delta refuses removal: $path. Ship a full package instead." } }
$baseVersion = (Get-Content -LiteralPath (Join-Path $BaseRoot 'build-info.json') -Raw | ConvertFrom-Json).appVersion
$targetVersion = (Get-Content -LiteralPath (Join-Path $TargetRoot 'build-info.json') -Raw | ConvertFrom-Json).appVersion
if (-not $baseVersion -or -not $targetVersion -or $baseVersion -eq $targetVersion) { throw 'Distinct, nonempty package versions are required.' }
Assert-DeltaPackage $BaseRoot $baseFiles $baseVersion
Assert-DeltaPackage $TargetRoot $targetFiles $targetVersion
$payload = @($targetFiles | Where-Object { $_.path -eq 'resources.pri' -or -not $baseMap.ContainsKey($_.path) -or $baseMap[$_.path].sha256 -ne $_.sha256 })
[void][IO.Directory]::CreateDirectory((Join-Path $OutputRoot '_test-delta'))
foreach ($file in $payload) {
    $destination = Join-Path $OutputRoot $file.path
    [void][IO.Directory]::CreateDirectory((Split-Path -Parent $destination))
    [IO.File]::Copy((Join-Path $TargetRoot $file.path), $destination, $false)
}
$manifest = [ordered]@{
    schema = 1; mode = 'overlay-no-removals'; baseVersion = $baseVersion; targetVersion = $targetVersion
    baseZipSha256 = $BaseZipSha256.ToLowerInvariant(); baseIdentity = Get-DeltaIdentity $baseFiles; targetIdentity = Get-DeltaIdentity $targetFiles
    baseFiles = $baseFiles; targetFiles = $targetFiles; payloadFiles = $payload
}
$encoding = New-Object Text.UTF8Encoding($false)
[IO.File]::WriteAllText((Join-Path $OutputRoot '_test-delta/manifest.json'), ($manifest | ConvertTo-Json -Depth 20), $encoding)
foreach ($script in @('Apply-TestDeltaPackage.ps1', 'TestDeltaPackage.Common.ps1')) { [IO.File]::Copy((Join-Path $PSScriptRoot $script), (Join-Path $OutputRoot "_test-delta/$script")) }
$instructions = @"
IMao 测试差量包：仅适用于 $baseVersion → $targetVersion
原始完整包 SHA256：$BaseZipSha256

先从托盘完全退出 IMao，并保留原测试目录备份。

直接覆盖方式：
1. 将 ZIP 的内容解压到原 $baseVersion 程序目录，与 IMao-WinUI.exe 同级。
2. 覆盖所有同名文件，保持子目录结构。
3. 双击 _test-delta\Verify-Delta.cmd，出现 differing=0 后再启动程序。
目录名仍可保留旧版本号，实际版本以程序和 build-info.json 为准。

推荐的带基线检查方式：先解压到独立目录，在该目录运行：
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\_test-delta\Apply-TestDeltaPackage.ps1 -PackageRoot "D:\原测试程序目录"
它会在写入前检查全部原包文件，写入后逐文件检查目标版本。
玩家额外的 Logs、Diagnostics 和配置文件会保留，本差量不删除文件。

直接解压无法在覆盖前核验旧文件，仅限上述原始版本，覆盖后必须校验。
复制过程若意外中断，请从备份/完整包恢复后重试，本工具不提供原地事务回滚。
禁止覆盖 C:\Dapps\IMao 玩家安装目录，仅用于这次独立测试包。

启动 CPU/GPU 脚本后，进入诊断页确认实际后端，不要只凭脚本名称判断模式。
"@
$verifyCommand = @'
@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Apply-TestDeltaPackage.ps1" -PackageRoot "%~dp0.." -VerifyOnly
set "verifyResult=%errorlevel%"
pause
exit /b %verifyResult%
'@
[IO.File]::WriteAllText((Join-Path $OutputRoot '_test-delta/Verify-Delta.cmd'), $verifyCommand, [Text.Encoding]::ASCII)
[IO.File]::WriteAllText((Join-Path $OutputRoot '_test-delta/README.txt'), $instructions, $encoding)
# Reconstruct a fresh COPY, never modify either input tree; compare the entire file inventory.
$reconstructed = "$OutputRoot.reconstructed"
[void][IO.Directory]::CreateDirectory($reconstructed)
foreach ($file in $baseFiles) {
    $destination = Join-Path $reconstructed $file.path
    [void][IO.Directory]::CreateDirectory((Split-Path -Parent $destination))
    [IO.File]::Copy((Join-Path $BaseRoot $file.path), $destination)
}
& (Join-Path $OutputRoot '_test-delta/Apply-TestDeltaPackage.ps1') -PackageRoot $reconstructed
$actual = @(Get-DeltaFiles $reconstructed)
if ($actual.Count -ne $targetFiles.Count -or (Get-DeltaIdentity $actual) -ne $manifest.targetIdentity) { throw 'Full reconstructed tree mismatch.' }
# A source mutation during packaging must not create a falsely labelled delta.
if ((Get-DeltaIdentity @(Get-DeltaFiles $BaseRoot)) -ne $manifest.baseIdentity -or (Get-DeltaIdentity @(Get-DeltaFiles $TargetRoot)) -ne $manifest.targetIdentity) { throw 'Input tree changed while generating delta.' }
Add-Type -AssemblyName System.IO.Compression.FileSystem
[IO.Compression.ZipFile]::CreateFromDirectory($OutputRoot, "$OutputRoot.zip", [IO.Compression.CompressionLevel]::Optimal, $false)
$report = [ordered]@{ baseVersion = $baseVersion; targetVersion = $targetVersion; baseIdentity = $manifest.baseIdentity; targetIdentity = $manifest.targetIdentity; baseFiles = $baseFiles.Count; targetFiles = $targetFiles.Count; payloadFiles = $payload.Count; differing = 0; zipSha256 = (Get-FileHash -LiteralPath "$OutputRoot.zip").Hash.ToLowerInvariant(); zipBytes = (Get-Item -LiteralPath "$OutputRoot.zip").Length }
[IO.File]::WriteAllText("$OutputRoot.report.json", ($report | ConvertTo-Json), $encoding)
$report | ConvertTo-Json
