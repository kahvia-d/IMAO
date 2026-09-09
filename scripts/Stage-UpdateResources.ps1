[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Destination,
    [string]$SourceRoot,
    [string]$Version,
    [string]$BaselineId,
    [string]$SourceCommit,
    [string]$AppxManifest
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
    foreach ($name in @('Destination','SourceRoot','Version','BaselineId','SourceCommit','AppxManifest')) {
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
function Assert-OutputInventory([string]$Source, [string]$Output, [string[]]$Generated = @(), [switch]$Complete) {
    $expected = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $sourcePrefix = [IO.Path]::GetFullPath($Source).TrimEnd('\','/') + [IO.Path]::DirectorySeparatorChar
    foreach ($file in [IO.Directory]::EnumerateFiles($Source, '*', [IO.SearchOption]::AllDirectories)) {
        [void]$expected.Add($file.Substring($sourcePrefix.Length).Replace('\','/'))
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
                if (-not $expected.Contains($relative)) { throw "Unexpected stale resource file '$relative'; use a new output directory. Existing files were not deleted." }
                [void]$present.Add($relative)
            }
        }
    }
    if ($Complete -and -not $expected.SetEquals($present)) { throw 'Staged resource inventory is incomplete; use a new output directory.' }
}
$assets = Join-Path $Destination 'Assets'
$mapData = Join-Path $assets 'KuroMap'
$sceneNames = @('World','Tethys','Fabricatorium','Avinoleum','Lahai','LowerVault','Darkplain','TimeRiftRuins')
$runtimeFiles = @($sceneNames | ForEach-Object { "runtime/itemsData_$_.json" })
Assert-OutputInventory (Join-Path $SourceRoot 'Assets/KuroMap') $mapData $runtimeFiles
[IO.Directory]::CreateDirectory($mapData) | Out-Null
Copy-Item -Path (Join-Path $SourceRoot 'Assets/KuroMap/*') -Destination $mapData -Recurse -Force
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
Assert-OutputInventory (Join-Path $SourceRoot 'Assets/KuroMap') $mapData $runtimeFiles -Complete
$packages = [Collections.Generic.List[object]]::new()
$packages.Add([ordered]@{id='map-data';version=$Version;kind='map-data';directory='KuroMap';sha256='';files=@()})
$tileRegistry = Get-Content -LiteralPath (Join-Path $SourceRoot 'Assets/FeaturesDatas/kuro-tile-packs.json') -Encoding UTF8 -Raw | ConvertFrom-Json
foreach ($name in $tileRegistry.packs) {
    $relative = "FeaturesDatas/KuroTilePacks/$name"
    $sourcePack = Join-Path $SourceRoot "Assets/$relative"
    if (-not (Test-Path -LiteralPath (Join-Path $sourcePack 'manifest.json'))) { continue }
    $manifest = Get-Content -LiteralPath (Join-Path $sourcePack 'manifest.json') -Encoding UTF8 -Raw | ConvertFrom-Json
    if (-not $manifest.referenceVerification.passed) { continue }
    $feature = Join-Path $sourcePack ([string]$manifest.features.file)
    if (-not (Test-Path -LiteralPath $feature)) { throw "Approved tile pack missing features: $name" }
    if ((Get-Sha256 $feature) -ne $manifest.features.sha256) { throw "Approved tile feature hash mismatch: $name" }
    Assert-OutputInventory $sourcePack (Join-Path $assets $relative)
    [IO.Directory]::CreateDirectory((Join-Path $assets $relative)) | Out-Null
    Copy-Item -Path (Join-Path $sourcePack '*') -Destination (Join-Path $assets $relative) -Recurse -Force
    Assert-OutputInventory $sourcePack (Join-Path $assets $relative) -Complete
    $packages.Add([ordered]@{id=[string]$manifest.packId;version=$Version;kind='tile';directory=$relative;sha256='';files=@()})
}
$candidateRegistry = Get-Content -LiteralPath (Join-Path $SourceRoot 'Assets/FeaturesDatas/candidate-packs.json') -Encoding UTF8 -Raw | ConvertFrom-Json
foreach ($name in $candidateRegistry.packs) {
    $relative = "FeaturesDatas/$name"
    $sourcePack = Join-Path $SourceRoot "Assets/$relative"
    $manifest = Get-Content -LiteralPath (Join-Path $sourcePack 'manifest.json') -Encoding UTF8 -Raw | ConvertFrom-Json
    if (-not (Test-Path -LiteralPath (Join-Path $sourcePack 'visual-index.imx'))) { throw "Candidate pack missing visual index: $name" }
    foreach ($reference in $manifest.references) {
        $referenceFile = Join-Path $sourcePack ([string]$reference.reference.image)
        if ((Get-Sha256 $referenceFile) -ne $reference.reference.sha256) { throw "Candidate reference hash mismatch: $name" }
    }
    Assert-OutputInventory $sourcePack (Join-Path $assets $relative)
    [IO.Directory]::CreateDirectory((Join-Path $assets $relative)) | Out-Null
    Copy-Item -Path (Join-Path $sourcePack '*') -Destination (Join-Path $assets $relative) -Recurse -Force
    Assert-OutputInventory $sourcePack (Join-Path $assets $relative) -Complete
    $packages.Add([ordered]@{id=[string]$manifest.packId;version=$Version;kind='candidate';directory=$relative;sha256='';files=@()})
}
Write-Json ([ordered]@{formatVersion=1;snapshotId="bundled-$Version";sequence=0;baselineId=$BaselineId;baselineRoot='.';mapDataRoot='KuroMap';bundled=$true;packages=@($packages.ToArray())}) (Join-Path $assets 'Updates/bundled-snapshot.json')
$keyFile = Join-Path $SourceRoot 'Assets/Updates/trusted-keys.json'
if (Test-Path -LiteralPath $keyFile) { Copy-Item -LiteralPath $keyFile -Destination (Join-Path $assets 'Updates/trusted-keys.json') -Force }
$baseFiles = @('FeaturesDatas/Map_features.imf','FeaturesDatas/Map_visual_index.imx')
$hashes = @($baseFiles | ForEach-Object { $p=Join-Path $SourceRoot "Assets/$_"; [ordered]@{path=$_;sha256=(Get-Sha256 $p)} })
Write-Json ([ordered]@{baselineId=$BaselineId;files=$hashes}) (Join-Path $assets 'Updates/baseline-files.json')
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
