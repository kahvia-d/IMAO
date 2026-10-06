[CmdletBinding()]
param([Parameter(Mandatory)][ValidatePattern('^v\d+\.\d+\.\d+\.\d+$')][string]$Tag)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'CiReleaseMetadata.ps1')
$file=Invoke-CiGh @('api','repos/kahvia-d/IMAO/contents/updates/stable.json?ref=main') | ConvertFrom-Json
$bytes=[Convert]::FromBase64String(($file.content -replace '\s',''))
$envelope=[Text.Encoding]::UTF8.GetString($bytes) | ConvertFrom-Json
$catalog=[Text.Encoding]::UTF8.GetString([Convert]::FromBase64String($envelope.payload)) | ConvertFrom-Json
if ($catalog.app.url -cne "https://github.com/kahvia-d/IMAO/releases/tag/$Tag") { Write-Host 'A newer stable exists; skipped stale mirror job.'; exit 0 }
$root=Join-Path (Split-Path -Parent $PSScriptRoot) 'out/gitee'
[IO.Directory]::CreateDirectory($root) | Out-Null
$manifest=Join-Path $root 'stable.json'; [IO.File]::WriteAllBytes($manifest,$bytes)
& dotnet build (Join-Path $PSScriptRoot '../tools/UpdatePublisher/UpdatePublisher.csproj') -c Release
if ($LASTEXITCODE -ne 0) { throw 'Public verifier build failed.' }
& dotnet (Join-Path $PSScriptRoot '../tools/UpdatePublisher/bin/Release/net8.0/UpdatePublisher.dll') verify-manifest --input $manifest --public-key (Join-Path $PSScriptRoot '../Assets/Updates/trusted-keys.json')
if ($LASTEXITCODE -ne 0) { throw 'Stable signature cannot be verified.' }
& (Join-Path $PSScriptRoot 'Set-GiteeMirror.ps1') -Manifest $manifest -ExpectedSha256 (Get-FileHash $manifest -Algorithm SHA256).Hash
