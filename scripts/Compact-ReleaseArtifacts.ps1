# Archives the small, unique evidence of released artifacts under out/ and removes the rebuildable
# payloads that dominate the directory: native build trees, published program trees, package archives
# and verification downloads. Every archived file is hash-verified against its source before that
# source directory is removed, and each archived directory keeps a manifest of what was kept and how
# many bytes were released. Published bytes remain available from the GitHub release itself.
[CmdletBinding(SupportsShouldProcess)]
param(
    [string]$OutputRoot = 'out',
    [string]$EvidenceRoot,
    [ValidateRange(0, 100)][int]$RetainFull = 0
)
$ErrorActionPreference = 'Stop'
if ($PSVersionTable.PSVersion.Major -lt 7) { throw 'PowerShell 7 or newer is required.' }
$repoRoot = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$outPrefix = [IO.Path]::GetFullPath((Join-Path $repoRoot 'out')).TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
$OutputRoot = [IO.Path]::GetFullPath($OutputRoot, $repoRoot)
if (-not $OutputRoot.StartsWith($outPrefix, [StringComparison]::OrdinalIgnoreCase)) { throw 'Compaction only operates on directories under the repository out directory.' }
if (-not (Test-Path -LiteralPath $OutputRoot -PathType Container)) { throw "Output directory does not exist: $OutputRoot" }
if (-not $EvidenceRoot) { $EvidenceRoot = Join-Path $OutputRoot 'evidence' }
$EvidenceRoot = [IO.Path]::GetFullPath($EvidenceRoot, $repoRoot)
if (-not $EvidenceRoot.StartsWith($outPrefix, [StringComparison]::OrdinalIgnoreCase)) { throw 'Evidence must be archived under the repository out directory.' }

# Small files that carry the release identity and the build/publish record. Everything else in a
# candidate or prepared directory is reproducible from the reviewed source or downloadable again.
$evidencePatterns = @(
    '*.log',
    'release-report.json',
    'update.json',
    'preflight-snapshot.json',
    'native-check.*',
    'program/*.report.json',
    'native/native-build-info.json',
    'publish/build-info.json',
    'launcher/launcher-build-info.json',
    'publish-verification-*/previous-stable.json',
    'publish-verification-*/stable-promotion.json',
    'publish-verification-*/channel-state-promotion.json'
)
function Get-EvidenceRelativePaths([string]$Directory) {
    $prefix = [IO.Path]::GetFullPath($Directory).TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    $found = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($pattern in $evidencePatterns) {
        foreach ($item in @(Get-ChildItem -Path (Join-Path $Directory $pattern) -File -ErrorAction SilentlyContinue)) {
            $relative = $item.FullName.Substring($prefix.Length).Replace('\', '/')
            [void]$found.Add($relative)
        }
    }
    return @($found | Sort-Object)
}
function Get-DirectoryBytes([string]$Directory) {
    return [long](Get-ChildItem -LiteralPath $Directory -Recurse -File -ErrorAction SilentlyContinue | Measure-Object Length -Sum).Sum
}

$targets = @(Get-ChildItem -LiteralPath $OutputRoot -Directory |
    Where-Object { $_.Name -match '^(release-candidate-|maps-)' } | Sort-Object LastWriteTime -Descending)
$retained = @($targets | Select-Object -First $RetainFull)
$pending = @($targets | Select-Object -Skip $RetainFull)
foreach ($keep in $retained) { Write-Host "Retaining $($keep.Name) in full." }
if ($pending.Count -eq 0) { Write-Host 'Nothing to compact.'; return }

$released = [long]0
$archivedCount = 0
foreach ($target in $pending) {
    $bytes = Get-DirectoryBytes $target.FullName
    $evidence = Get-EvidenceRelativePaths $target.FullName
    $evidenceBytes = [long]0
    foreach ($relative in $evidence) { $evidenceBytes += [IO.FileInfo]::new((Join-Path $target.FullName $relative)).Length }
    Write-Host ("{0}: archive {1} evidence file(s) ({2} KB), release {3} GB" -f $target.Name, $evidence.Count, [math]::Round($evidenceBytes / 1KB, 1), [math]::Round($bytes / 1GB, 2))
    foreach ($relative in $evidence) { Write-Host "    keep $relative" }
    if (-not $PSCmdlet.ShouldProcess($target.FullName, "archive evidence and delete the directory (releases $([math]::Round($bytes / 1GB, 2)) GB)")) { continue }

    $evidenceDirectory = Join-Path $EvidenceRoot $target.Name
    [IO.Directory]::CreateDirectory($evidenceDirectory) | Out-Null
    $prefix = [IO.Path]::GetFullPath($target.FullName).TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    $archived = [Collections.Generic.List[object]]::new()
    foreach ($relative in $evidence) {
        $source = [IO.Path]::GetFullPath((Join-Path $target.FullName $relative))
        if (-not $source.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) { throw "Evidence path escapes its artifact directory: $relative" }
        $destination = Join-Path $evidenceDirectory $relative
        [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($destination)) | Out-Null
        Copy-Item -LiteralPath $source -Destination $destination -Force
        # The archive replaces the directory, so a copy that does not match its source must stop this.
        $sourceHash = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($sourceHash -ne (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash.ToLowerInvariant()) { throw "Archived evidence differs from its source: $relative" }
        $archived.Add([ordered]@{ path = $relative; size = [IO.FileInfo]::new($source).Length; sha256 = $sourceHash })
    }
    $manifest = [ordered]@{
        formatVersion = 1
        artifact = $target.Name
        archivedAt = (Get-Date).ToUniversalTime().ToString('o')
        releasedBytes = $bytes
        evidence = @($archived.ToArray())
    }
    [IO.File]::WriteAllText((Join-Path $evidenceDirectory 'archive-manifest.json'), ($manifest | ConvertTo-Json -Depth 6) + "`n", [Text.UTF8Encoding]::new($false))
    Remove-Item -LiteralPath $target.FullName -Recurse -Force
    $released += $bytes
    $archivedCount += $archived.Count
}
Write-Host "Compacted $($pending.Count) artifact director$(if ($pending.Count -eq 1) { 'y' } else { 'ies' }): archived $archivedCount evidence file(s), released $([math]::Round($released / 1GB, 2)) GB. Evidence: $EvidenceRoot"
