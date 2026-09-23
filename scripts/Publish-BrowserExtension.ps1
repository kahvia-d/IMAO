# Requires PowerShell 7 and authenticated GitHub CLI. Builds the player-facing archive of the
# KuroMapSync browser extension and, with -Publish, attaches it to its own release line.
#
# The extension is normally installed from the Edge Add-ons store, which is still under review. Until
# that clears, players load the same code as an unpacked extension; the manifest pins the release
# public key in "key", so an unpacked copy derives the same CRX ID the desktop bridge is registered
# for, and no desktop change is needed for the pre-review path.
[CmdletBinding()]
param(
    [string]$SourceRoot,
    [string]$OutputRoot,
    [string]$NotesFile,
    [switch]$Publish
)
$ErrorActionPreference = 'Stop'
if ($PSVersionTable.PSVersion.Major -lt 7) { throw 'PowerShell 7 or newer is required.' }
if (-not $SourceRoot) { $SourceRoot = Split-Path -Parent $PSScriptRoot }
$SourceRoot = [IO.Path]::GetFullPath($SourceRoot)
$extensionRoot = Join-Path $SourceRoot 'BrowserExtensions/KuroMapSync'
if (-not (Test-Path -LiteralPath $extensionRoot -PathType Container)) { throw "Extension source is missing: $extensionRoot" }
if (-not $OutputRoot) { $OutputRoot = Join-Path $SourceRoot 'out/browser-extension' }
$OutputRoot = [IO.Path]::GetFullPath($OutputRoot, $SourceRoot)
[IO.Directory]::CreateDirectory($OutputRoot) | Out-Null

# The desktop bridge allows exactly one origin, so a build that would change the derived ID has to
# fail here instead of shipping an extension no registered host accepts.
$expectedExtensionId = 'ohmikfaeobbffhlhoocklplniobcfdbg'
function Get-ExtensionIdFromKey([string]$key) {
    $bytes = [Convert]::FromBase64String($key)
    $hash = [Security.Cryptography.SHA256]::HashData($bytes)
    $builder = [Text.StringBuilder]::new(32)
    for ($index = 0; $index -lt 16; $index++) {
        [void]$builder.Append([char](97 + ($hash[$index] -shr 4)))
        [void]$builder.Append([char](97 + ($hash[$index] -band 0x0F)))
    }
    return $builder.ToString()
}

$manifestPath = Join-Path $extensionRoot 'manifest.json'
$manifest = Get-Content -LiteralPath $manifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
if ($manifest.manifest_version -ne 3) { throw 'The extension must stay on manifest version 3.' }
if (-not $manifest.key) { throw 'manifest.json must pin the release public key so unpacked and store copies share one CRX ID.' }
$extensionId = Get-ExtensionIdFromKey ([string]$manifest.key)
if ($extensionId -ne $expectedExtensionId) {
    throw "manifest.json derives extension ID $extensionId, but the desktop bridge is registered for $expectedExtensionId."
}
$version = [string]$manifest.version
if ($version -notmatch '^\d+\.\d+\.\d+$') { throw "Extension version must be three numbers: $version" }

# Everything the runtime loads, plus the install notes players read after unzipping.
$requiredFiles = @('manifest.json', 'service-worker.js', 'popup.html', 'popup.js', 'extract-main.js',
    'relay.js', 'README.md', 'icons/icon-16.png', 'icons/icon-32.png', 'icons/icon-48.png', 'icons/icon-128.png')
foreach ($relative in $requiredFiles) {
    if (-not (Test-Path -LiteralPath (Join-Path $extensionRoot $relative) -PathType Leaf)) {
        throw "Extension file is missing: $relative"
    }
}

# The archive is deterministic: sorted entries and a fixed timestamp, so republishing the same source
# produces the same bytes and the same hash.
$files = @(Get-ChildItem -LiteralPath $extensionRoot -Recurse -File |
    ForEach-Object { [IO.Path]::GetRelativePath($extensionRoot, $_.FullName).Replace('\', '/') } |
    Sort-Object -CaseSensitive)
$archiveName = "IMao-KuroMapSync-$version"
$archivePath = Join-Path $OutputRoot "$archiveName.zip"
$reportPath = Join-Path $OutputRoot "$archiveName.report.json"
$shaPath = "$archivePath.sha256"
foreach ($path in @($archivePath, $shaPath, $reportPath)) {
    if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path -Force }
}
$fixedTimestamp = [DateTimeOffset]::new([DateTime]::new(2026, 1, 1, 0, 0, 0, [DateTimeKind]::Utc))
$stream = [IO.File]::Open($archivePath, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
try {
    $archive = [IO.Compression.ZipArchive]::new($stream, [IO.Compression.ZipArchiveMode]::Create, $false)
    try {
        foreach ($relative in $files) {
            $entry = $archive.CreateEntry($relative, [IO.Compression.CompressionLevel]::Optimal)
            $entry.LastWriteTime = $fixedTimestamp
            $source = [IO.File]::OpenRead((Join-Path $extensionRoot $relative))
            try {
                $target = $entry.Open()
                try { $source.CopyTo($target) } finally { $target.Dispose() }
            } finally { $source.Dispose() }
        }
    } finally { $archive.Dispose() }
} finally { $stream.Dispose() }

$sha256 = (Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash.ToLowerInvariant()
[IO.File]::WriteAllText($shaPath, "$sha256  $archiveName.zip`n", [Text.UTF8Encoding]::new($false))
$sourceCommit = [string](& git -C $SourceRoot rev-parse HEAD)
$sourceClean = @(& git -C $SourceRoot status --porcelain=v1 --untracked-files=no -- 'BrowserExtensions/KuroMapSync').Count -eq 0
$report = [ordered]@{
    version = $version; extensionId = $extensionId; sourceCommit = $sourceCommit;
    extensionSourceDirty = (-not $sourceClean); sha256 = $sha256;
    size = (Get-Item -LiteralPath $archivePath).Length; files = $files
}
[IO.File]::WriteAllText($reportPath, ($report | ConvertTo-Json -Depth 4), [Text.UTF8Encoding]::new($false))
Write-Host "Extension archive: $archivePath"
Write-Host "  version $version  CRX ID $extensionId  SHA256 $sha256"
Write-Host "  source $sourceCommit  extensionSourceDirty=$(-not $sourceClean)"

if (-not $Publish) {
    Write-Host 'Dry run only. Re-run with -Publish to create the release and upload these bytes.' -ForegroundColor Yellow
    return
}
if (-not $sourceClean) { throw 'Commit the extension sources before publishing; a published archive must match a commit.' }
$repo = 'kahvia-d/IMAO'
$tag = "ext-v$version"
if (-not $NotesFile) { throw 'Publishing requires -NotesFile with the release announcement.' }
$NotesFile = [IO.Path]::GetFullPath($NotesFile, $SourceRoot)
if (-not (Test-Path -LiteralPath $NotesFile -PathType Leaf)) { throw "Notes file is missing: $NotesFile" }

function Invoke-Gh([string[]]$Arguments) {
    $output = & gh @Arguments 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw ('GitHub operation failed: gh ' + ($Arguments -join ' ') + ' :: ' + (($output | Out-String).Trim()))
    }
    return $output
}
# A release tag can only be created from a commit the remote already has.
$remoteCommit = & gh api "repos/$repo/commits/$sourceCommit" --jq '.sha' 2>$null
if ($LASTEXITCODE -ne 0 -or ([string]$remoteCommit).Trim() -ne $sourceCommit) {
    throw "Commit $sourceCommit is not on the remote yet; push it before publishing the extension."
}
$existing = & gh api "repos/$repo/releases/tags/$tag" 2>$null
if ($LASTEXITCODE -eq 0) {
    $release = $existing | ConvertFrom-Json
    if ($release.draft) { throw "Release $tag exists as a draft; finish or delete it before publishing." }
    $asset = @($release.assets | Where-Object name -EQ "$archiveName.zip")
    if ($asset.Count -ne 1) { throw "Published release $tag is missing $archiveName.zip; published bytes are immutable." }
    $verifyRoot = Join-Path $OutputRoot ('verify-' + [guid]::NewGuid().ToString('N'))
    [IO.Directory]::CreateDirectory($verifyRoot) | Out-Null
    Invoke-Gh @('release', 'download', $tag, '--repo', $repo, '--pattern', "$archiveName.zip", '--dir', $verifyRoot) | Out-Null
    if ((Get-FileHash -LiteralPath (Join-Path $verifyRoot "$archiveName.zip") -Algorithm SHA256).Hash.ToLowerInvariant() -ne $sha256) {
        throw "Release $tag already carries different bytes for $archiveName.zip; bump the extension version instead."
    }
    Write-Host "Release $tag already publishes these exact bytes."
    return
}
# The extension is a side artifact. Without --latest=false GitHub would put the repository's
# "Latest" badge on this release, and the README's Releases link would send players to a page
# that has no IMao-v*-windows-x64.zip in it.
Invoke-Gh @('release', 'create', $tag, '--repo', $repo, '--target', $sourceCommit, '--title', "IMao 库街区同步扩展 $version", '--notes-file', $NotesFile, '--latest=false') | Out-Null
foreach ($asset in @([pscustomobject]@{ path = $archivePath; name = "$archiveName.zip"; sha256 = $sha256 },
        [pscustomobject]@{ path = $shaPath; name = "$archiveName.zip.sha256"; sha256 = (Get-FileHash -LiteralPath $shaPath -Algorithm SHA256).Hash.ToLowerInvariant() })) {
    Invoke-Gh @('release', 'upload', $tag, $asset.path, '--repo', $repo) | Out-Null
}
$verify = Join-Path $OutputRoot ('verify-' + [guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($verify) | Out-Null
Invoke-Gh @('release', 'download', $tag, '--repo', $repo, '--pattern', "$archiveName.zip", '--dir', $verify) | Out-Null
if ((Get-FileHash -LiteralPath (Join-Path $verify "$archiveName.zip") -Algorithm SHA256).Hash.ToLowerInvariant() -ne $sha256) {
    throw 'Uploaded bytes failed verification.'
}
Write-Host "Published https://github.com/$repo/releases/tag/$tag" -ForegroundColor Green
