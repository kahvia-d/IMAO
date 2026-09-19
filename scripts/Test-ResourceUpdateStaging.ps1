[CmdletBinding()]
param([Parameter(Mandatory)][string]$OutputRoot)
$ErrorActionPreference = 'Stop'
$OutputRoot = [IO.Path]::GetFullPath($OutputRoot)
if (Test-Path -LiteralPath $OutputRoot) { throw 'Use a fresh staging regression output directory.' }
$source = Join-Path $OutputRoot 'fixture-source'
function Write-Fixture([string]$Relative, [string]$Text) {
    $file = Join-Path $source $Relative
    [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($file)) | Out-Null
    [IO.File]::WriteAllText($file,$Text,[Text.UTF8Encoding]::new($false))
}
Write-Fixture 'Version.props' '<Project><PropertyGroup><IMaoVersion>2026.9.9.1</IMaoVersion><IMaoBaselineId>staging-fixture</IMaoBaselineId></PropertyGroup></Project>'
$scenes=@('World','Tethys','Fabricatorium','Avinoleum','Lahai','LowerVault','Darkplain','TimeRiftRuins')
$states=@(8,900,905,903,906,902,909,910)
$manifest=@{formatVersion=1;states=@()}
for($i=0;$i -lt $scenes.Count;$i++) {
    $manifest.states += @{state=$states[$i];runtime=$scenes[$i];supported=($i -lt 5)}
    Write-Fixture "IMao-Core/src/Resource/itemsData_$($scenes[$i]).json" '[]'
}
Write-Fixture 'Assets/KuroMap/manifest.json' ($manifest | ConvertTo-Json -Depth 8)
# The icon set is carved out of KuroMap into its own package before staging, so the fixture has to
# carry one; staging copies these two entries out and deletes them from the map-data tree.
Write-Fixture 'Assets/KuroMap/icon-manifest.json' '{"formatVersion":1,"icons":[]}'
Write-Fixture 'Assets/KuroMap/icons/icon-0001.png' 'fixture-icon'
Write-Fixture 'Assets/FeaturesDatas/Map_features.imf' 'fixture-base-features'
Write-Fixture 'Assets/FeaturesDatas/Map_visual_index.imx' 'fixture-base-visual'
Write-Fixture 'Assets/FeaturesDatas/kuro-tile-packs.json' '{"formatVersion":1,"packs":["Fixture"]}'
Write-Fixture 'Assets/FeaturesDatas/candidate-packs.json' '{"formatVersion":1,"packs":[]}'
Write-Fixture 'Assets/FeaturesDatas/KuroTilePacks/Fixture/features.yml' 'fixture-tile-features'
$tileHash=(Get-FileHash -LiteralPath (Join-Path $source 'Assets/FeaturesDatas/KuroTilePacks/Fixture/features.yml') -Algorithm SHA256).Hash.ToLowerInvariant()
# An approved pack also needs its compiled binary and the provenance manifest that ties it to the
# feature source, because staging refuses a pack whose binary was not built from the recorded source.
Write-Fixture 'Assets/FeaturesDatas/KuroTilePacks/Fixture/features.imf' 'fixture-tile-binary'
Write-Fixture 'Assets/FeaturesDatas/KuroTilePacks/Fixture/features.imf.manifest.json' (@{formatVersion=1;sourceXmlSha256=$tileHash;keypointCount=7} | ConvertTo-Json -Depth 8)
Write-Fixture 'Assets/FeaturesDatas/KuroTilePacks/Fixture/manifest.json' (@{formatVersion=1;packId='fixture';referenceVerification=@{passed=$true};features=@{file='features.yml';sha256=$tileHash;keypointCount=7}} | ConvertTo-Json -Depth 8)
& git init --quiet $source
if ($LASTEXITCODE -ne 0) { throw 'Fixture repository initialization failed.' }
& git -C $source add -- Version.props Assets IMao-Core
if ($LASTEXITCODE -ne 0) { throw 'Fixture source staging failed.' }
& git -C $source -c user.name=ReleaseFixture -c user.email=fixture@example.invalid -c commit.gpgsign=false commit --quiet -m 'Staging fixture'
if ($LASTEXITCODE -ne 0) { throw 'Fixture source commit failed.' }
$fixtureCommit = [string](& git -C $source rev-parse HEAD)
if ($LASTEXITCODE -ne 0) { throw 'Fixture source commit lookup failed.' }
$scriptPath=Join-Path $PSScriptRoot 'Stage-UpdateResources.ps1'
$passed=[Collections.Generic.List[string]]::new()
function Invoke-Stage([string]$Name,[bool]$ExpectFailure) {
    $destination=Join-Path $OutputRoot $Name
    $output=& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $scriptPath -SourceRoot $source -Destination $destination -SourceCommit $fixtureCommit 2>&1
    $code=$LASTEXITCODE
    [IO.File]::WriteAllText((Join-Path $OutputRoot "$Name-last-run.log"),($output -join "`n"))
    if($ExpectFailure) { if($code -eq 0 -or ($output -join "`n") -notlike '*Unexpected stale resource file*use a new output directory*') { throw "Expected stale inventory rejection for $Name" } }
    elseif($code -ne 0) { throw "Staging fixture failed: $($output -join ' ')" }
}
Invoke-Stage 'clean' $false
Invoke-Stage 'clean' $false
$passed.Add('PS5 unchanged restage accepted')
$direct = Join-Path $OutputRoot 'direct-caller'
Push-Location $OutputRoot
try { & $scriptPath -SourceRoot 'fixture-source' -Destination 'direct-caller' -SourceCommit $fixtureCommit }
finally { Pop-Location }
$canonicalRoot=Join-Path $OutputRoot 'clean/Assets/KuroMap'
$directRoot=Join-Path $direct 'Assets/KuroMap'
$canonicalFiles=@([IO.Directory]::EnumerateFiles($canonicalRoot,'*',[IO.SearchOption]::AllDirectories))
$directFiles=@([IO.Directory]::EnumerateFiles($directRoot,'*',[IO.SearchOption]::AllDirectories))
if($canonicalFiles.Count -ne $directFiles.Count){throw 'Caller PowerShell changed map-data file inventory.'}
foreach($file in $canonicalFiles){
    $relative=$file.Substring($canonicalRoot.Length).TrimStart('\','/')
    $other=Join-Path $directRoot $relative
    if(-not [IO.File]::Exists($other) -or (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash -ne (Get-FileHash -LiteralPath $other -Algorithm SHA256).Hash){throw "Caller PowerShell changed map-data bytes: $relative"}
}
$passed.Add("PS$($PSVersionTable.PSVersion.Major) relative-path caller and direct PS5 produce byte-identical map-data inventories")
$mapStale=Join-Path $OutputRoot 'map-stale/Assets/KuroMap/removed-guide.json'
[IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($mapStale)) | Out-Null
[IO.File]::WriteAllText($mapStale,'{"sentinel":"keep"}')
Invoke-Stage 'map-stale' $true
if([IO.File]::ReadAllText($mapStale) -ne '{"sentinel":"keep"}') { throw 'Map stale sentinel was modified.' }
$passed.Add('PS5 stale map-data rejected without deleting sentinel')
Invoke-Stage 'tile-stale' $false
$tileStale=Join-Path $OutputRoot 'tile-stale/Assets/FeaturesDatas/KuroTilePacks/Fixture/removed-visual.imx'
[IO.File]::WriteAllText($tileStale,'keep-old-feature-evidence')
Invoke-Stage 'tile-stale' $true
if([IO.File]::ReadAllText($tileStale) -ne 'keep-old-feature-evidence') { throw 'Tile stale sentinel was modified.' }
$passed.Add('PS5 stale feature file rejected without deleting sentinel')
$forgedDestination = Join-Path $OutputRoot 'forged-source-commit'
$forgedOutput = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $scriptPath -SourceRoot $source -Destination $forgedDestination -SourceCommit aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa 2>&1
if ($LASTEXITCODE -eq 0 -or ($forgedOutput -join "`n") -notlike '*Selected SourceCommit differs*' -or (Test-Path -LiteralPath $forgedDestination)) { throw 'Forged source SHA was not rejected before resource writes.' }
$passed.Add('PS5 forged source SHA rejected before writing output')
# A published package keeps its version while its content is unchanged. The bundled descriptor must
# name that same version, otherwise clients treat bytes shipped inside the program as missing.
function Get-StagedInventory([string]$Root) {
    $prefix = [IO.Path]::GetFullPath($Root).TrimEnd('\','/') + [IO.Path]::DirectorySeparatorChar
    $files = [Collections.Generic.List[object]]::new()
    foreach ($file in ([IO.Directory]::EnumerateFiles($Root,'*',[IO.SearchOption]::AllDirectories) | Sort-Object)) {
        $files.Add([ordered]@{path=$file.Substring($prefix.Length).Replace('\','/');size=[long]([IO.FileInfo]::new($file).Length);sha256=(Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant()})
    }
    return ,$files
}
function Write-FixtureCatalog([string]$Path,[string]$MapDataVersion,[string]$TileVersion,[switch]$BreakMapData) {
    $mapFiles=Get-StagedInventory (Join-Path $OutputRoot 'clean/Assets/KuroMap')
    if($BreakMapData){$mapFiles[0].sha256='f'*64}
    $tileFiles=Get-StagedInventory (Join-Path $OutputRoot 'clean/Assets/FeaturesDatas/KuroTilePacks/Fixture')
    $payload=[ordered]@{schemaVersion=1;sequence=9;app=[ordered]@{version='2026.9.9.1';url='https://github.com/kahvia-d/WWMAP-TOOLS/releases/tag/fixture'};resources=@([ordered]@{snapshotId='resources-fixture';sequence=9;baselineId='staging-fixture';minAppVersion='2026.9.9.1';packages=@(
        [ordered]@{id='map-data';version=$MapDataVersion;kind='map-data';files=$mapFiles},
        [ordered]@{id='fixture';version=$TileVersion;kind='tile';files=$tileFiles})})}
    $envelope=[ordered]@{keyId='fixture';payload=[Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes(($payload | ConvertTo-Json -Depth 10 -Compress)));signature='fixture'}
    [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($Path)) | Out-Null
    [IO.File]::WriteAllText($Path,($envelope | ConvertTo-Json -Depth 5),[Text.UTF8Encoding]::new($false))
}
function Commit-Fixture([string]$Message) {
    & git -C $source add -- updates
    if ($LASTEXITCODE -ne 0) { throw 'Fixture catalog staging failed.' }
    & git -C $source -c user.name=ReleaseFixture -c user.email=fixture@example.invalid -c commit.gpgsign=false commit --quiet -m $Message
    if ($LASTEXITCODE -ne 0) { throw 'Fixture catalog commit failed.' }
    return [string](& git -C $source rev-parse HEAD)
}
function Invoke-StageAt([string]$Name,[string]$Commit) {
    $destination=Join-Path $OutputRoot $Name
    $output=& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $scriptPath -SourceRoot $source -Destination $destination -SourceCommit $Commit 2>&1
    [IO.File]::WriteAllText((Join-Path $OutputRoot "$Name-last-run.log"),($output -join "`n"))
    if($LASTEXITCODE -ne 0) { throw "Staging fixture failed: $($output -join ' ')" }
    return (Get-Content -LiteralPath (Join-Path $destination 'Assets/Updates/bundled-snapshot.json') -Raw | ConvertFrom-Json)
}
$catalogPath=Join-Path $source 'updates/stable.json'
Write-FixtureCatalog $catalogPath '2020.1.1.1' '2020.2.2.2'
$aligned=Invoke-StageAt 'aligned' (Commit-Fixture 'Published fixture catalog')
if($aligned.snapshotId -ne 'bundled-2026.9.9.1' -or $aligned.sequence -ne 0){throw 'Bundled snapshot identity changed.'}
if(($aligned.packages | Where-Object id -EQ 'map-data').version -ne '2020.1.1.1' -or ($aligned.packages | Where-Object id -EQ 'fixture').version -ne '2020.2.2.2'){throw 'Bundled descriptor did not reuse published package versions for identical content.'}
$passed.Add('PS5 bundled descriptor reuses published package versions for unchanged content')
Write-FixtureCatalog $catalogPath '2020.1.1.1' '2020.2.2.2' -BreakMapData
$changed=Invoke-StageAt 'changed-content' (Commit-Fixture 'Changed fixture map data')
if(($changed.packages | Where-Object id -EQ 'map-data').version -ne '2026.9.9.1'){throw 'Changed map-data must fall back to the program version.'}
if(($changed.packages | Where-Object id -EQ 'fixture').version -ne '2020.2.2.2'){throw 'Unchanged pack must keep its published version.'}
$passed.Add('PS5 bundled descriptor falls back to the program version when package content differs')
[IO.File]::WriteAllText((Join-Path $OutputRoot 'test-report.json'),(@{passed=$passed.Count;tests=@($passed.ToArray())}|ConvertTo-Json -Depth 5),[Text.UTF8Encoding]::new($false))
Write-Host "PASS $($passed.Count) PS5 staging regressions."
