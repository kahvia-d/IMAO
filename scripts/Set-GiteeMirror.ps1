# Pushes the signed stable envelope to the Gitee mirror that UpdateService.ManifestMirrors reads.
#
# Why this exists: the canonical entry point (raw.githubusercontent.com) cannot be reached from mainland
# China without a proxy, and the whole update check used to die with it. The mirror carries the *same
# signed bytes*, and every client verifies them against the pinned P-256 key before using any of it.
# The mirror therefore provides availability and never authority: a hostile or broken mirror can at
# worst refuse to serve. That is also why a mirror failure must never fail a release - the caller
# decides whether it is fatal, and Publish-ResourceUpdate.ps1 reports it as a warning because the
# canonical channel has already advanced by the time this runs.
#
# Standalone use, which is how the mirror is verified without staging a whole release:
#   scripts/Set-GiteeMirror.ps1 -Manifest updates/stable.json
#
# The token is read from outside the repository and travels in the request body, never in the URL, so it
# cannot be committed and cannot leak through a logged or reported request line.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Manifest,
    # Defaults to hashing -Manifest. Pass the reviewed digest to also prove the local file is that file.
    [string]$ExpectedSha256,
    # The single source of truth for the mirror address on the publishing side.
    # UpdateService.ManifestMirrors compiles the same address into the client; Tests/ResourceUpdates
    # asserts the two agree, because renaming one without the other silently stops serving the clients
    # that need the mirror most.
    [string]$MirrorUrl = 'https://gitee.com/tan-xuedong/imao-updates/raw/main/stable.json',
    [string]$TokenFile
)
$ErrorActionPreference = 'Stop'
if ($PSVersionTable.PSVersion.Major -lt 7) { throw 'PowerShell 7 or newer is required.' }
if ($MirrorUrl -notmatch '^https://gitee\.com/(?<owner>[^/]+)/(?<repo>[^/]+)/raw/(?<branch>[^/]+)/(?<path>.+)$') { throw "Cannot parse the Gitee mirror URL: $MirrorUrl" }
$owner = $Matches.owner; $repo = $Matches.repo; $branch = $Matches.branch; $path = $Matches.path
if (-not $TokenFile) { $TokenFile = Join-Path $env:LOCALAPPDATA 'WWMAP-TOOLS-Publisher/gitee-token.txt' }
if (-not (Test-Path -LiteralPath $TokenFile)) { throw "No Gitee token at $TokenFile. Create a personal access token with the projects scope and save it there; the token must stay outside the repository." }
$token = (Get-Content -LiteralPath $TokenFile -Raw).Trim()
if (-not $token) { throw "The Gitee token file is empty: $TokenFile" }
$Manifest = [IO.Path]::GetFullPath($Manifest)
if (-not (Test-Path -LiteralPath $Manifest)) { throw "No manifest at $Manifest" }
$bytes = [IO.File]::ReadAllBytes($Manifest)
$sha256 = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($bytes))
if ($ExpectedSha256 -and $sha256 -ne $ExpectedSha256.ToUpperInvariant()) { throw "The manifest does not match the reviewed digest, refusing to mirror it: $Manifest" }

$api = "https://gitee.com/api/v5/repos/$owner/$repo/contents/$path"
# Gitee replaces an existing file only when the blob sha being replaced is named, exactly like the GitHub
# contents API. An absent file means this is the first mirror push of this path and the file is created.
$existingSha = $null
try { $existingSha = [string](Invoke-RestMethod -Uri "$api`?ref=$branch" -Method Get -TimeoutSec 30 -Headers @{'User-Agent'='WWMAP-TOOLS-publisher'}).sha } catch { $existingSha = $null }
$body = [ordered]@{
    access_token = $token
    content      = [Convert]::ToBase64String($bytes)
    message      = "Mirror signed stable channel $($sha256.Substring(0, 12))"
    branch       = $branch
}
if ($existingSha) { $body.sha = $existingSha; $method = 'Put' } else { $method = 'Post' }
$null = Invoke-RestMethod -Uri $api -Method $method -ContentType 'application/json;charset=UTF-8' -Body ([Text.Encoding]::UTF8.GetBytes(($body | ConvertTo-Json -Depth 5))) -TimeoutSec 300

# Read the mirrored bytes back and compare, the same way the GitHub promotion is verified. Reading it
# back through the API (rather than the raw URL) keeps Gitee's CDN cache out of the verification.
$mirrored = (Invoke-RestMethod -Uri "$api`?ref=$branch" -Method Get -TimeoutSec 60 -Headers @{'User-Agent'='WWMAP-TOOLS-publisher'}).content
$mirroredBytes = [Convert]::FromBase64String(($mirrored -replace '\s', ''))
if ([Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($mirroredBytes)) -ne $sha256) { throw 'Mirrored bytes do not match the local manifest.' }
Write-Host "Mirrored $path ($($bytes.Length) bytes) to $MirrorUrl"
