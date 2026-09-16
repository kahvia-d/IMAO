[CmdletBinding()]
param([Parameter(Mandatory)][ValidatePattern('^[a-p]{32}$')][string]$ExtensionId,
      [ValidateSet('Chrome','Edge')][string[]]$Browser = @('Chrome','Edge'))
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Enter-DevEnvironment.ps1')
$repo = Split-Path -Parent $PSScriptRoot
$project = Join-Path $repo 'tools\KuroSyncBridge\KuroSyncBridge.csproj'
$destination = Join-Path $env:LOCALAPPDATA 'IMao-WinUI\KuroSync\Bridge'
& $env:IMAO_DOTNET publish $project -c Release -r win-x64 --self-contained true -p:PublishSingleFile=true -o $destination --source $env:NUGET_PACKAGES -p:NuGetAudit=false
if ($LASTEXITCODE -ne 0) { throw '无法构建 KuroSyncBridge。' }
$origin = "chrome-extension://$ExtensionId/"
@{ AllowedOrigin = $origin } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $destination 'KuroSyncBridge.settings.json') -Encoding utf8
$manifestPath = Join-Path $destination 'com.imao.kuro_sync.json'
@{ name = 'com.imao.kuro_sync'; description = 'IMao KuroMap Sync bridge'; path = (Join-Path $destination 'KuroSyncBridge.exe'); type = 'stdio'; allowed_origins = @($origin) } | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath $manifestPath -Encoding utf8
foreach ($name in $Browser) {
  $key = if ($name -eq 'Chrome') { 'HKCU:\Software\Google\Chrome\NativeMessagingHosts\com.imao.kuro_sync' } else { 'HKCU:\Software\Microsoft\Edge\NativeMessagingHosts\com.imao.kuro_sync' }
  New-Item -Path $key -Force | Out-Null
  Set-ItemProperty -Path $key -Name '(Default)' -Value $manifestPath
}
Write-Host "KuroSyncBridge 已注册到 $($Browser -join '、')。扩展 ID：$ExtensionId"
