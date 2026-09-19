[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Destination,
    [string]$SourceRoot,
    [string]$Version,
    [string]$BaselineId,
    [string]$SourceCommit,
    [string]$AppxManifest,
    [string]$PreviousCatalog
)
$ErrorActionPreference = 'Stop'
# Release resource bytes must not depend on which PowerShell launched a build.
# In particular PS5/PS7 serialize the canonicalized map manifest differently.
# Always run this Windows staging entrypoint in the same Windows PowerShell 5.1 host.
if ($PSVersionTable.PSVersion.Major -gt 5) {
    $windowsPowerShell = Join-Path $env:SystemRoot 'System32/WindowsPowerShell/v1.0/powershell.exe'
    if (-not [IO.File]::Exists($windowsPowerShell)) { throw 'Canonical resource staging requires Windows PowerShell 5.1.' }
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $windowsPowerShell
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.WorkingDirectory = (Get-Location).ProviderPath
    foreach ($argument in @('-NoProfile','-ExecutionPolicy','Bypass','-File',$PSCommandPath)) { $start.ArgumentList.Add($argument) }
    foreach ($name in @('Destination','SourceRoot','Version','BaselineId','SourceCommit','AppxManifest','PreviousCatalog')) {
        if (-not $PSBoundParameters.ContainsKey($name) -or $null -eq $PSBoundParameters[$name] -or [string]$PSBoundParameters[$name] -eq '') { continue }
        $start.ArgumentList.Add('-' + $name)
        $start.ArgumentList.Add([string]$PSBoundParameters[$name])
    }
    $process = [Diagnostics.Process]::Start($start)
    try {
        $process.WaitForExit()
        if ($process.ExitCode -ne 0) { throw "Canonical Windows PowerShell staging failed (exit $($process.ExitCode))." }
    } finally { $process.Dispose() }
    return
}
if ($PSVersionTable.PSVersion.Major -ne 5 -or $PSVersionTable.PSVersion.Minor -lt 1) { throw 'Canonical resource staging requires Windows PowerShell 5.1.' }
if (-not $SourceRoot) { $SourceRoot = Split-Path -Parent $PSScriptRoot }
$SourceRoot = [IO.Path]::GetFullPath($SourceRoot)
$Destination = [IO.Path]::GetFullPath($Destination)
if ($Destination -eq $SourceRoot -or $Destination -eq (Join-Path $SourceRoot 'Assets')) { throw 'Stage into an output directory, never source Assets.' }
[xml]$versionXml = Get-Content -LiteralPath (Join-Path $SourceRoot 'Version.props') -Encoding UTF8 -Raw
if (-not $Version) { $Version = [string]$versionXml.Project.PropertyGroup.IMaoVersion }
if (-not $BaselineId) { $BaselineId = [string]$versionXml.Project.PropertyGroup.IMaoBaselineId }
. (Join-Path $PSScriptRoot 'ResourceBuildProvenance.ps1')
$sourceBefore = Get-ResourceBuildProvenance $SourceRoot $SourceCommit
$SourceCommit = $sourceBefore.sourceCommit
function Write-Json($Value, [string]$Path) {
    [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($Path)) | Out-Null
    [IO.File]::WriteAllText($Path, ($Value | ConvertTo-Json -Depth 100) + "`n", [Text.UTF8Encoding]::new($false))
}
function Get-Sha256([string]$Path) {
    $stream = [IO.File]::OpenRead($Path)
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($algorithm.ComputeHash($stream))).Replace('-','').ToLowerInvariant() }
    finally { $algorithm.Dispose(); $stream.Dispose() }
}
# A staged inventory may be a subtree of the source: the icon package is carved out of
# Assets/KuroMap, so both halves have to be asserted against the same source tree.
function Test-InventoryScope([string]$Relative, [string[]]$Only, [string[]]$Skip) {
    foreach ($entry in $Skip) {
        $prefix = $entry.TrimEnd('/')
        if ($Relative -eq $prefix -or $Relative.StartsWith($prefix + '/', [StringComparison]::OrdinalIgnoreCase)) { return $false }
    }
    if ($Only.Count -eq 0) { return $true }
    foreach ($entry in $Only) {
        $prefix = $entry.TrimEnd('/')
        if ($Relative -eq $prefix -or $Relative.StartsWith($prefix + '/', [StringComparison]::OrdinalIgnoreCase)) { return $true }
    }
    return $false
}
function Assert-OutputInventory([string]$Source, [string]$Output, [string[]]$Generated = @(), [switch]$Complete,
    [string[]]$Only = @(), [string[]]$Skip = @()) {
    $expected = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $sourcePrefix = [IO.Path]::GetFullPath($Source).TrimEnd('\','/') + [IO.Path]::DirectorySeparatorChar
    foreach ($file in [IO.Directory]::EnumerateFiles($Source, '*', [IO.SearchOption]::AllDirectories)) {
        $relative = $file.Substring($sourcePrefix.Length).Replace('\','/')
        if (Test-InventoryScope $relative $Only $Skip) { [void]$expected.Add($relative) }
    }
    foreach ($relative in $Generated) { [void]$expected.Add($relative.Replace('\','/')) }
    $present = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    if ([IO.Directory]::Exists($Output)) {
        $outputPrefix = [IO.Path]::GetFullPath($Output).TrimEnd('\','/') + [IO.Path]::DirectorySeparatorChar
        $pending = [Collections.Generic.Stack[string]]::new()
        $pending.Push([IO.Path]::GetFullPath($Output))
        while ($pending.Count -gt 0) {
            $directory = $pending.Pop()
            if (([IO.File]::GetAttributes($directory) -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw 'Staged resource directory is a link; use a new output directory.' }
            foreach ($path in [IO.Directory]::EnumerateFileSystemEntries($directory)) {
                if (([IO.File]::GetAttributes($path) -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw 'Staged resource path is a link; use a new output directory.' }
                if ([IO.Directory]::Exists($path)) { $pending.Push($path); continue }
                $relative = $path.Substring($outputPrefix.Length).Replace('\','/')
                if (-not (Test-InventoryScope $relative $Only $Skip)) { continue }
                if (-not $expected.Contains($relative)) { throw "Unexpected stale resource file '$relative'; use a new output directory. Existing files were not deleted." }
                [void]$present.Add($relative)
            }
        }
    }
    if ($Complete -and -not $expected.SetEquals($present)) { throw 'Staged resource inventory is incomplete; use a new output directory.' }
}
# Published package versions are content identities: UpdatePublisher keeps the previous version and
# download URL whenever a package's file list is unchanged. The bundled descriptor must name the same
# versions, otherwise a client treats bytes that already ship inside the program as missing and
# downloads the complete resource set again on its first check.
if (-not $PreviousCatalog) { $PreviousCatalog = Join-Path $SourceRoot 'updates/stable.json' }
$previousPackages = @{}
if (Test-Path -LiteralPath $PreviousCatalog) {
    $previousEnvelope = Get-Content -LiteralPath $PreviousCatalog -Encoding UTF8 -Raw | ConvertFrom-Json
    $previousPayload = [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String([string]$previousEnvelope.payload)) | ConvertFrom-Json
    foreach ($previousRelease in @($previousPayload.resources)) {
        foreach ($previousPackage in @($previousRelease.packages)) {
            $previousId = [string]$previousPackage.id
            if ($previousId -and -not $previousPackages.ContainsKey($previousId)) { $previousPackages[$previousId] = $previousPackage }
        }
    }
}
function Get-StagedPackageFileMap([string]$Directory) {
    $prefix = [IO.Path]::GetFullPath($Directory).TrimEnd('\','/') + [IO.Path]::DirectorySeparatorChar
    $map = @{}
    foreach ($file in [IO.Directory]::EnumerateFiles($Directory,'*',[IO.SearchOption]::AllDirectories)) {
        $relative = $file.Substring($prefix.Length).Replace('\','/')
        $map[$relative] = ([IO.FileInfo]::new($file).Length).ToString() + ':' + (Get-Sha256 $file)
    }
    return $map
}
function Get-BundledPackageVersion([string]$Directory, [string]$Id) {
    $prior = $previousPackages[$Id]
    if ($null -ne $prior -and $null -ne $prior.files) {
        $files = @($prior.files)
        $map = Get-StagedPackageFileMap $Directory
        if ($map.Count -eq $files.Count) {
            $matches = $true
            foreach ($file in $files) {
                $key = [string]$file.path
                if (-not $map.ContainsKey($key) -or $map[$key] -ne (([long]$file.size).ToString() + ':' + ([string]$file.sha256).ToLowerInvariant())) { $matches = $false; break }
            }
            if ($matches) { return [string]$prior.version }
        }
    }
    return $Version
}
$assets = Join-Path $Destination 'Assets'
$mapData = Join-Path $assets 'KuroMap'
$sceneNames = @('World','Tethys','Fabricatorium','Avinoleum','Lahai','LowerVault','Darkplain','TimeRiftRuins')
$runtimeFiles = @($sceneNames | ForEach-Object { "runtime/itemsData_$_.json" })
$iconData = Join-Path $assets 'KuroMapIcons'
Assert-OutputInventory (Join-Path $SourceRoot 'Assets/KuroMap') $mapData $runtimeFiles -Skip @('icon-manifest.json', 'icons')
Assert-OutputInventory (Join-Path $SourceRoot 'Assets/KuroMap') $iconData -Only @('icon-manifest.json', 'icons')
[IO.Directory]::CreateDirectory($mapData) | Out-Null
Copy-Item -Path (Join-Path $SourceRoot 'Assets/KuroMap/*') -Destination $mapData -Recurse -Force
# The icon set is its own package: it is 68% of the map-data bytes, so a points-only change
# should not cost a full map-data download. Icons are read from the snapshot's mapIconRoot.
# Copy rather than move: staging must stay idempotent when a build reuses its output
# directory, and Move-Item fails once the icon copy already exists there.
[IO.Directory]::CreateDirectory($iconData) | Out-Null
Copy-Item -LiteralPath (Join-Path $mapData 'icon-manifest.json') -Destination (Join-Path $iconData 'icon-manifest.json') -Force
Copy-Item -LiteralPath (Join-Path $mapData 'icons') -Destination $iconData -Recurse -Force
Remove-Item -LiteralPath (Join-Path $mapData 'icon-manifest.json') -Force
Remove-Item -LiteralPath (Join-Path $mapData 'icons') -Recurse -Force
$runtime = Join-Path $mapData 'runtime'
[IO.Directory]::CreateDirectory($runtime) | Out-Null
foreach ($scene in $sceneNames) {
    $source = Join-Path $SourceRoot "IMao-Core/src/Resource/itemsData_$scene.json"
    if (-not (Test-Path -LiteralPath $source)) { $source = Join-Path $SourceRoot "Assets/KuroMap/runtime/itemsData_$scene.json" }
    $runtimeText = [IO.File]::ReadAllText($source, [Text.Encoding]::UTF8)
    if (-not $runtimeText.TrimStart().StartsWith('[')) { throw "Runtime points must be an array: $scene" }
    $null = ConvertFrom-Json -InputObject $runtimeText
    # Preserve number/string bytes. Windows PowerShell 5 serializes parsed arrays
    # with ETS metadata as {value:...,Count:...}; it must not rewrite point payloads.
    $aliasPattern = '("(?:id|typeId)"\s*:\s*)"sx(?:' + [char]0xB7 + '|\\u00[bB]7)(qq|lgn)"'
    $runtimeText = [regex]::Replace($runtimeText, $aliasPattern, { param($match) $match.Groups[1].Value + '"sx_' + $match.Groups[2].Value + '"' })
    [IO.File]::WriteAllText((Join-Path $runtime "itemsData_$scene.json"), $runtimeText, [Text.UTF8Encoding]::new($false))
}
# The archived upstream manifest predates the three runtime scene definitions.
# Complete only their identity mapping; preserve supported=false and the independent approval gates.
$stateScenes = @{8='World';900='Tethys';905='Fabricatorium';903='Avinoleum';906='Lahai';902='LowerVault';909='Darkplain';910='TimeRiftRuins'}
$mapManifest = Get-Content -LiteralPath (Join-Path $mapData 'manifest.json') -Encoding UTF8 -Raw | ConvertFrom-Json
foreach ($state in $mapManifest.states) {
    $name = $stateScenes[[int]$state.state]
    if (-not $name) { throw 'Map-data manifest contains an unknown upstream state.' }
    if ([string]::IsNullOrEmpty([string]$state.runtime)) { $state.runtime = $name }
    elseif ($state.runtime -ne $name) { throw 'Map-data state identity differs from the compiled scene mapping.' }
}
Write-Json $mapManifest (Join-Path $mapData 'manifest.json')
Assert-OutputInventory (Join-Path $SourceRoot 'Assets/KuroMap') $mapData $runtimeFiles -Complete -Skip @('icon-manifest.json', 'icons')
Assert-OutputInventory (Join-Path $SourceRoot 'Assets/KuroMap') $iconData -Only @('icon-manifest.json', 'icons') -Complete
$packages = [Collections.Generic.List[object]]::new()
$packages.Add([ordered]@{id='map-data';version=(Get-BundledPackageVersion $mapData 'map-data');kind='map-data';directory='KuroMap';sha256='';files=@()})
$packages.Add([ordered]@{id='map-icons';version=(Get-BundledPackageVersion $iconData 'map-icons');kind='map-icons';directory='KuroMapIcons';sha256='';files=@()})
$tileRegistry = Get-Content -LiteralPath (Join-Path $SourceRoot 'Assets/FeaturesDatas/kuro-tile-packs.json') -Encoding UTF8 -Raw | ConvertFrom-Json
foreach ($name in $tileRegistry.packs) {
    $relative = "FeaturesDatas/KuroTilePacks/$name"
    $sourcePack = Join-Path $SourceRoot "Assets/$relative"
    if (-not (Test-Path -LiteralPath (Join-Path $sourcePack 'manifest.json'))) { continue }
    $manifest = Get-Content -LiteralPath (Join-Path $sourcePack 'manifest.json') -Encoding UTF8 -Raw | ConvertFrom-Json
    if (-not $manifest.referenceVerification.passed) { continue }
    $feature = Join-Path $sourcePack ([string]$manifest.features.file)
    # The runtime reads only features.imf; the source XML/YAML is a build-time input that
    # is deliberately not shipped, so its provenance is checked through the hash the
    # binary manifest records rather than through the file itself.
    $binaryManifestPath = Join-Path $sourcePack 'features.imf.manifest.json'
    $binaryPath = Join-Path $sourcePack 'features.imf'
    if (-not (Test-Path -LiteralPath $binaryPath) -or -not (Test-Path -LiteralPath $binaryManifestPath)) {
        throw "Approved tile pack missing binary features: $name"
    }
    $binaryManifest = Get-Content -LiteralPath $binaryManifestPath -Encoding UTF8 -Raw | ConvertFrom-Json
    $recordedHash = ([string]$manifest.features.sha256).ToLowerInvariant()
    if (([string]$binaryManifest.sourceXmlSha256).ToLowerInvariant() -ne $recordedHash) {
        throw "Approved tile binary was not built from the recorded feature source: $name"
    }
    if ([int]$binaryManifest.keypointCount -ne [int]$manifest.features.keypointCount) {
        throw "Approved tile binary keypoint count mismatch: $name"
    }
    if ((Test-Path -LiteralPath $feature) -and (Get-Sha256 $feature) -ne $recordedHash) {
        throw "Approved tile feature hash mismatch: $name"
    }
    Assert-OutputInventory $sourcePack (Join-Path $assets $relative)
    [IO.Directory]::CreateDirectory((Join-Path $assets $relative)) | Out-Null
    Copy-Item -Path (Join-Path $sourcePack '*') -Destination (Join-Path $assets $relative) -Recurse -Force
    Assert-OutputInventory $sourcePack (Join-Path $assets $relative) -Complete
    $packages.Add([ordered]@{id=[string]$manifest.packId;version=(Get-BundledPackageVersion (Join-Path $assets $relative) ([string]$manifest.packId));kind='tile';directory=$relative;sha256='';files=@()})
}
# Curated candidate packs are optional and none ship today: the mengzhou region pack superseded the
# Dreamzhou curated candidate. A registry that names packs is still staged when one is present.
$candidateRegistryPath = Join-Path $SourceRoot 'Assets/FeaturesDatas/candidate-packs.json'
if (Test-Path -LiteralPath $candidateRegistryPath) {
    $candidateRegistry = Get-Content -LiteralPath $candidateRegistryPath -Encoding UTF8 -Raw | ConvertFrom-Json
    foreach ($name in $candidateRegistry.packs) {
        $relative = "FeaturesDatas/$name"
        $sourcePack = Join-Path $SourceRoot "Assets/$relative"
        $manifest = Get-Content -LiteralPath (Join-Path $sourcePack 'manifest.json') -Encoding UTF8 -Raw | ConvertFrom-Json
        if (-not (Test-Path -LiteralPath (Join-Path $sourcePack 'visual-index.imx'))) { throw "Candidate pack missing visual index: $name" }
        $references = if ($null -ne $manifest.PSObject.Properties['references']) { @($manifest.references) } else { @() }
        foreach ($reference in $references) {
            $referenceFile = Join-Path $sourcePack ([string]$reference.reference.image)
            if ((Get-Sha256 $referenceFile) -ne $reference.reference.sha256) { throw "Candidate reference hash mismatch: $name" }
        }
        Assert-OutputInventory $sourcePack (Join-Path $assets $relative)
        [IO.Directory]::CreateDirectory((Join-Path $assets $relative)) | Out-Null
        Copy-Item -Path (Join-Path $sourcePack '*') -Destination (Join-Path $assets $relative) -Recurse -Force
        Assert-OutputInventory $sourcePack (Join-Path $assets $relative) -Complete
        $packages.Add([ordered]@{id=[string]$manifest.packId;version=(Get-BundledPackageVersion (Join-Path $assets $relative) ([string]$manifest.packId));kind='candidate';directory=$relative;sha256='';files=@()})
    }
}
Write-Json ([ordered]@{formatVersion=1;snapshotId="bundled-$Version";sequence=0;baselineId=$BaselineId;baselineRoot='.';mapDataRoot='KuroMap';mapIconRoot='KuroMapIcons';bundled=$true;packages=@($packages.ToArray())}) (Join-Path $assets 'Updates/bundled-snapshot.json')
$keyFile = Join-Path $SourceRoot 'Assets/Updates/trusted-keys.json'
if (Test-Path -LiteralPath $keyFile) { Copy-Item -LiteralPath $keyFile -Destination (Join-Path $assets 'Updates/trusted-keys.json') -Force }
# The base map features are retired once both files are gone from the source tree; every region pack carries
# its own calibrated features and the runtime tolerates the base set being absent. While they are still there
# the manifest keeps verifying them, and once they are not, a manifest left behind by an earlier run into the
# same output directory is removed: it would name files that no longer exist and the native loader refuses a
# snapshot whose integrity manifest lists a missing file.
$baseFiles = @('FeaturesDatas/Map_features.imf','FeaturesDatas/Map_visual_index.imx') |
    Where-Object { Test-Path -LiteralPath (Join-Path $SourceRoot "Assets/$_") }
$baselineManifest = Join-Path $assets 'Updates/baseline-files.json'
if ($baseFiles.Count -gt 0) {
    $hashes = @($baseFiles | ForEach-Object { $p=Join-Path $SourceRoot "Assets/$_"; [ordered]@{path=$_;sha256=(Get-Sha256 $p)} })
    Write-Json ([ordered]@{baselineId=$BaselineId;files=$hashes}) $baselineManifest
} elseif (Test-Path -LiteralPath $baselineManifest) {
    Remove-Item -LiteralPath $baselineManifest -Force
}
$sourceAfter = Get-ResourceBuildProvenance $SourceRoot $SourceCommit
Assert-ResourceBuildUnchanged $sourceBefore $sourceAfter
Write-Json ([ordered]@{appVersion=$Version;baselineId=$BaselineId;sourceCommit=$SourceCommit;sourceDirty=$sourceAfter.sourceDirty;sourceTreeSha256=$sourceAfter.sourceTreeSha256}) (Join-Path $Destination 'build-info.json')
if ($AppxManifest) {
    [xml]$appx = Get-Content -LiteralPath (Join-Path $SourceRoot 'IMao-WinUI/Package.appxmanifest') -Encoding UTF8 -Raw
    $appx.Package.Identity.Version = $Version
    [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName([IO.Path]::GetFullPath($AppxManifest))) | Out-Null
    $appx.Save([IO.Path]::GetFullPath($AppxManifest))
}
Write-Host "Staged resource snapshot bundled-$Version with $($packages.Count) packages; source $SourceCommit"
