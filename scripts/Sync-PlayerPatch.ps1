[CmdletBinding(SupportsShouldProcess)]
param(
    # The installation to patch in place. Defaults to the player build this workstation tests against.
    [string]$InstallRoot = 'C:\Dapps\IMao',
    [string]$OutputDirectory = 'out/player-patch',
    # Where the replaced files are kept. Defaults to a timestamped sibling of the installation.
    [string]$BackupRoot
)

# Patches a player installation with a locally built managed payload, so a fix can be tried before a release
# exists. Three things are baked in, because breaking a real installation taught each of them:
#
#   1. A managed DLL is only half of a UI change. Every page's XAML is compiled into resources.pri, so a new
#      IMao-WinUI.dll beside the old index crashes the moment that page is opened - Microsoft.UI.Xaml.dll with
#      0xc000027b. resources.pri is part of the payload, not an afterthought.
#   2. It has to be a *publish*, with the flags the release uses. A plain `dotnet build` indexes the same XAML
#      under a name rooted at its output folder ("binMainWindow.xbf"), which ms-appx:///MainWindow.xaml cannot
#      resolve, and the application dies in MainWindow's constructor with XamlParseException. The index is
#      therefore verified here before anything is copied: App.xbf and MainWindow.xbf must be present and no
#      deliberately prefixed variant may be.
#   3. Everything is stamped with the version the installation already reports. The published
#      IMao-WinUI.deps.json names its assemblies by version (IMao-WinUI.Core/2026.9.26.3 for example), so a
#      patched DLL carrying a newer version is a mismatch this script has no reason to take on.
#
# Deliberately not copied: *.exe, *.deps.json and *.runtimeconfig.json. The installation already has the
# published ones; only the fixed assemblies and the index they belong to are replaced.

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Enter-DevEnvironment.ps1')
$taskRoot = Split-Path -Parent $PSScriptRoot
$install = [IO.Path]::GetFullPath($InstallRoot, $taskRoot)
$patchRoot = [IO.Path]::GetFullPath($OutputDirectory, $taskRoot)
$publish = Join-Path $patchRoot 'publish'

$descriptor = Join-Path $install 'build-info.json'
if (-not (Test-Path -LiteralPath $descriptor)) { throw "Not an installation root (no build-info.json): $install" }
$version = (Get-Content -LiteralPath $descriptor -Raw | ConvertFrom-Json).appVersion
if ([string]::IsNullOrWhiteSpace($version)) { throw 'The installation does not report an appVersion.' }

# A running application holds both the DLLs and the resource index open, and half a patch is worse than none.
$running = Get-Process -Name 'IMao-WinUI', 'IMao-Launcher', 'IMao-CoreHost' -ErrorAction SilentlyContinue
if ($running) { throw ('Close IMao first; these are still running: ' + (($running | Select-Object -ExpandProperty ProcessName) -join ', ')) }

Write-Host "Patching $install as version $version" -ForegroundColor Cyan
# Only the application's own version is forced. Version/AssemblyVersion/FileVersion must NOT be passed
# globally: they also stamp IMao-WinUI.Core, which has been pinned to IMaoCoreVersion (1.0.0) since
# 2026-10-10 so the bridge can embed a byte-identical copy in its self-contained single file - and the
# installation's deps.json declares IMao-WinUI.Core/1.0.0, so a Core carrying the application version
# contradicts the file the runtime is told to expect (measured 2026-10-12: the patched Core reported
# 2026.10.10.1 where the installation declared 1.0.0). IMaoVersion alone carries the application to the
# installed version, because Version.props derives Version, AssemblyVersion and FileVersion from it.
# The +player-patch marker stays on InformationalVersion: it is not an identity property, it is how a
# patched installation is recognised afterwards, and it deliberately lands on every assembly built here.
$stamp = @("-p:IMaoVersion=$version", "-p:InformationalVersion=$version+player-patch")
& $env:IMAO_DOTNET publish (Join-Path $taskRoot 'IMao-WinUI/IMao-WinUI.csproj') -c Release -r win-x64 --self-contained true `
    -p:Platform=x64 -p:WindowsPackageType=None -p:GenerateAppxPackageOnBuild=false -p:AppxPackageSigningEnabled=false `
    -p:NuGetAudit=false "-p:BaseOutputPath=$patchRoot\managed-build\" --source $env:NUGET_PACKAGES @stamp -o $publish
if ($LASTEXITCODE -ne 0) { throw 'The managed publish failed.' }

function Get-ResourceIndexNames([string]$pri) {
    $makepri = Get-ChildItem 'C:\Program Files (x86)\Windows Kits\10\bin' -Recurse -Filter 'makepri.exe' -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -like '*\x64\*' } | Sort-Object FullName | Select-Object -First 1
    if (-not $makepri) { throw 'makepri.exe (Windows SDK) not found, so the resource index cannot be verified.' }
    $dump = Join-Path ([IO.Path]::GetTempPath()) ('imao-pri-' + [guid]::NewGuid().ToString('N') + '.xml')
    try {
        & $makepri.FullName dump /if $pri /of $dump /o | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "makepri could not read $pri." }
        $document = [xml](Get-Content -LiteralPath $dump -Raw)
        return @($document.SelectNodes('//NamedResource') | ForEach-Object { $_.Name })
    }
    finally { if (Test-Path -LiteralPath $dump) { Remove-Item -LiteralPath $dump -Force } }
}

# The payload is exactly what a UI change can consist of, and the index is the part that is easy to forget.
$payload = @('IMao-WinUI.Core.dll', 'IMao-WinUI.dll', 'resources.pri') +
    @(Get-ChildItem -LiteralPath $publish -Recurse -Filter '*.xbf' -ErrorAction SilentlyContinue | ForEach-Object {
        [IO.Path]::GetRelativePath($publish, $_.FullName) })
foreach ($name in $payload) {
    if (-not (Test-Path -LiteralPath (Join-Path $publish $name))) { throw "The publish produced no $name; refusing to patch half a payload." }
}

$indexNames = Get-ResourceIndexNames (Join-Path $publish 'resources.pri')
foreach ($required in 'App.xbf', 'MainWindow.xbf') {
    if ($indexNames -notcontains $required) { throw "resources.pri does not index ${required}, so this publish did not use the publishing recipe." }
}
$stray = @($indexNames | Where-Object { $_ -match '^(bin|obj|publish)[^/]*\.xbf$' })
if ($stray) { throw ('resources.pri indexes folder-prefixed XAML names the runtime cannot resolve: ' + ($stray -join ', ')) }
Write-Host ("Resource index verified: {0} entries, XAML rooted at the application." -f $indexNames.Count) -ForegroundColor Green

# The installation's deps.json names every assembly it expects by version. A patched DLL carrying another
# one is a mismatch this script must not introduce - that is what "a patched DLL carrying a newer version"
# below the payload list warns about, and it cost a round of "the patch changed nothing" on 2026-09-27.
# The check is written down here rather than trusted to whoever edits the version stamping next.
$depsPath = Join-Path $install 'IMao-WinUI.deps.json'
if (Test-Path -LiteralPath $depsPath) {
    # An assembly version always has four components (1.0.0 is reported as 1.0.0.0) while deps.json writes as
    # many as the project declared, and [version]'1.0.0' carries revision -1 rather than 0 - so the two are
    # compared as normalized strings, not as Version objects.
    function Get-FourPartVersion([string]$value) {
        $parsed = [version]$value
        '{0}.{1}.{2}.{3}' -f $parsed.Major, $parsed.Minor,
            $(if ($parsed.Build -lt 0) { 0 } else { $parsed.Build }),
            $(if ($parsed.Revision -lt 0) { 0 } else { $parsed.Revision })
    }
    $declared = (Get-Content -LiteralPath $depsPath -Raw | ConvertFrom-Json).libraries.PSObject.Properties.Name
    $expected = foreach ($name in 'IMao-WinUI', 'IMao-WinUI.Core') {
        $assembly = Join-Path $publish ($name + '.dll')
        if (-not (Test-Path -LiteralPath $assembly)) { continue }
        $wanted = $declared | Where-Object { $_ -like ($name + '/*') } | ForEach-Object { $_.Split('/')[1] } | Select-Object -First 1
        if ([string]::IsNullOrWhiteSpace($wanted)) { continue }
        $actual = [Reflection.AssemblyName]::GetAssemblyName($assembly).Version.ToString()
        if ((Get-FourPartVersion $actual) -ne (Get-FourPartVersion $wanted)) {
            throw "The publish produced $name $actual, but the installation declares $name/$wanted. Pass IMaoVersion alone (see the stamp above) so each project keeps its own version."
        }
        "$name $actual"
    }
    Write-Host ("Assembly versions match the installation: {0}" -f ($expected -join ', ')) -ForegroundColor Green
}

if (-not $BackupRoot) { $BackupRoot = Join-Path (Split-Path -Parent $install) ('IMao-backup-' + (Get-Date -Format 'yyyyMMdd-HHmmss')) }
$backup = [IO.Path]::GetFullPath($BackupRoot, $taskRoot)
[IO.Directory]::CreateDirectory($backup) | Out-Null

$rows = foreach ($name in $payload) {
    $source = Join-Path $publish $name
    $target = Join-Path $install $name
    $before = if (Test-Path -LiteralPath $target) { (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash } else { '' }
    $after = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash
    if ($before -eq $after) { [pscustomobject]@{ File = $name; Result = 'already current'; Bytes = (Get-Item $source).Length }; continue }
    if (-not $PSCmdlet.ShouldProcess($target, "copy $name")) { continue }
    if ($before) { Copy-Item -LiteralPath $target -Destination (Join-Path $backup $name) -Force }
    Copy-Item -LiteralPath $source -Destination $target -Force
    if ((Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash -ne $after) { throw "Verification failed after copying $name." }
    [pscustomobject]@{ File = $name; Result = if ($before) { 'replaced' } else { 'added' }; Bytes = (Get-Item $source).Length }
}

$rows | Format-Table -AutoSize
Write-Host "Backup of replaced files: $backup" -ForegroundColor Cyan
Write-Host 'Not copied on purpose: *.exe, *.deps.json, *.runtimeconfig.json (the installation already has the published ones).'
