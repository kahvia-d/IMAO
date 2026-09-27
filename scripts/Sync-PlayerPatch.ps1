[CmdletBinding(SupportsShouldProcess)]
param(
    # The installation to patch in place. Defaults to the player build this workstation tests against.
    [string]$InstallRoot = 'C:\Dapps\IMao',
    [string]$OutputDirectory = 'out/player-patch',
    # Where the replaced files are kept. Defaults to a timestamped sibling of the installation.
    [string]$BackupRoot
)

# Patches a player installation with a locally built managed payload, so a fix can be tried before a release
# exists. Two lessons are baked in, because both were learned by breaking a real installation:
#
#   1. A managed DLL is only half of a UI change. The XAML of every page is compiled into resources.pri, so a new
#      IMao-WinUI.dll beside the old index crashes the moment that page is opened - Microsoft.UI.Xaml.dll with
#      0xc000027b - and only pages whose XAML changed are affected. resources.pri is therefore part of the patch,
#      not an afterthought.
#   2. Everything is stamped with the version the installation already reports. The published
#      IMao-WinUI.deps.json names its assemblies by version (IMao-WinUI.Core/2026.9.26.3 for example), so a patched
#      DLL carrying a newer version is a mismatch this script has no reason to take on.
#
# Deliberately not copied: *.exe, *.deps.json and *.runtimeconfig.json. A `dotnet build` produces different ones
# from the `dotnet publish` the installation came from - the published deps.json carries the runtime packs the
# self-contained Windows App SDK needs, and the apphost differs for reasons that have nothing to do with the fix.

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Enter-DevEnvironment.ps1')
$taskRoot = Split-Path -Parent $PSScriptRoot
$install = [IO.Path]::GetFullPath($InstallRoot, $taskRoot)
$bin = [IO.Path]::GetFullPath($OutputDirectory, $taskRoot) + '/bin'

$descriptor = Join-Path $install 'build-info.json'
if (-not (Test-Path -LiteralPath $descriptor)) { throw "Not an installation root (no build-info.json): $install" }
$version = (Get-Content -LiteralPath $descriptor -Raw | ConvertFrom-Json).appVersion
if ([string]::IsNullOrWhiteSpace($version)) { throw 'The installation does not report an appVersion.' }

# A running application holds both the DLLs and the resource index open, and half a patch is worse than none.
$running = Get-Process -Name 'IMao-WinUI', 'IMao-Launcher', 'IMao-CoreHost' -ErrorAction SilentlyContinue
if ($running) { throw ('Close IMao first; these are still running: ' + (($running | Select-Object -ExpandProperty ProcessName) -join ', ')) }

Write-Host "Patching $install as version $version" -ForegroundColor Cyan
[IO.Directory]::CreateDirectory($bin) | Out-Null
& $env:IMAO_DOTNET build (Join-Path $taskRoot 'IMao-WinUI/IMao-WinUI.csproj') -c Release -p:Platform=x64 -p:NuGetAudit=false `
    -p:IMaoVersion=$version -p:Version=$version -p:AssemblyVersion=$version -p:FileVersion=$version `
    -p:InformationalVersion="$version+player-patch" -o $bin
if ($LASTEXITCODE -ne 0) { throw 'The managed build failed.' }

# The payload is exactly what a UI change can consist of; the index is required rather than optional.
$payload = @('IMao-WinUI.Core.dll', 'IMao-WinUI.dll', 'resources.pri') +
    @(Get-ChildItem -LiteralPath $bin -Recurse -Filter '*.xbf' -ErrorAction SilentlyContinue | ForEach-Object {
        [IO.Path]::GetRelativePath($bin, $_.FullName) })
foreach ($name in $payload) {
    if (-not (Test-Path -LiteralPath (Join-Path $bin $name))) { throw "The build produced no $name; refusing to patch half a payload." }
}

if (-not $BackupRoot) { $BackupRoot = Join-Path (Split-Path -Parent $install) ('IMao-backup-' + (Get-Date -Format 'yyyyMMdd-HHmmss')) }
$backup = [IO.Path]::GetFullPath($BackupRoot, $taskRoot)
[IO.Directory]::CreateDirectory($backup) | Out-Null

$rows = foreach ($name in $payload) {
    $source = Join-Path $bin $name
    $target = Join-Path $install $name
    $before = if (Test-Path -LiteralPath $target) { (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash } else { '' }
    $after = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash
    if ($before -eq $after) { [pscustomobject]@{ File = $name; Result = 'already current'; Bytes = (Get-Item $source).Length }; continue }
    if (-not $PSCmdlet.ShouldProcess($target, "copy $name")) { continue }
    if ($before) { Copy-Item -LiteralPath $target -Destination (Join-Path $backup $name) -Force }
    Copy-Item -LiteralPath $source -Destination $target -Force
    $written = (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash
    if ($written -ne $after) { throw "Verification failed after copying $name." }
    [pscustomobject]@{ File = $name; Result = if ($before) { 'replaced' } else { 'added' }; Bytes = (Get-Item $source).Length }
}

$rows | Format-Table -AutoSize
Write-Host "Backup of replaced files: $backup" -ForegroundColor Cyan
Write-Host 'Not copied on purpose: *.exe, *.deps.json, *.runtimeconfig.json (a build is not the published payload).'
