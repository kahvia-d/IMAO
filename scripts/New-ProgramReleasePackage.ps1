[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$PublishRoot,
    [Parameter(Mandatory)][string]$NativeRoot,
    [Parameter(Mandatory)][string]$OutputRoot,
    [Parameter(Mandatory)][string]$SourceCommit,
    [Parameter(Mandatory)][string]$RedistRoot,
    [string]$SourceRoot,
    [string]$NativeBuildReceipt,
    [string]$LauncherRoot
)
$ErrorActionPreference = 'Stop'
if ($PSVersionTable.PSVersion.Major -lt 7) { throw 'PowerShell 7 or newer is required.' }
if (-not $SourceRoot) { $SourceRoot = Split-Path -Parent $PSScriptRoot }
$SourceRoot = [IO.Path]::GetFullPath($SourceRoot)
$PublishRoot = [IO.Path]::GetFullPath($PublishRoot)
$NativeRoot = [IO.Path]::GetFullPath($NativeRoot)
$OutputRoot = [IO.Path]::GetFullPath($OutputRoot)
if ($SourceCommit -notmatch '^[a-f0-9]{40}$') { throw 'SourceCommit must be an exact source SHA.' }
. (Join-Path $PSScriptRoot 'ResourceBuildProvenance.ps1')
$sourceBefore = Get-ResourceBuildProvenance $SourceRoot $SourceCommit
$build = Get-Content -LiteralPath (Join-Path $PublishRoot 'build-info.json') -Raw -Encoding UTF8 | ConvertFrom-Json
if ($build.sourceCommit -ne $SourceCommit) { throw 'Managed build source differs from selected source SHA.' }
Assert-ManagedBuildProvenance $build $sourceBefore
if (-not $NativeBuildReceipt) { $NativeBuildReceipt = Join-Path $NativeRoot 'native-build-info.json' }
$nativeReceipt = $null
if (Test-Path -LiteralPath $NativeBuildReceipt -PathType Leaf) {
    $nativeReceipt = Get-Content -LiteralPath $NativeBuildReceipt -Raw -Encoding UTF8 | ConvertFrom-Json
    Assert-NativeBuildProvenance $nativeReceipt $sourceBefore $build (Join-Path $NativeRoot 'IMao-CoreHost.exe')
} elseif ($build.sourceDirty -eq $false) {
    throw 'A clean program package requires native-build-info.json from the same clean source build.'
}
$version = [string]$build.appVersion
if ([version]$version -ge [version]'2026.9.9.4') {
    if (-not $LauncherRoot) { throw 'Self-updating program packages require -LauncherRoot from the same source build.' }
    $launcherBuild = Get-Content -LiteralPath (Join-Path $LauncherRoot 'launcher-build-info.json') -Raw | ConvertFrom-Json
    Assert-ManagedBuildProvenance $launcherBuild $sourceBefore
    if ($launcherBuild.appVersion -ne $version) { throw 'Launcher version differs from application version.' }
    if ($launcherBuild.launcherSha256 -ne (Get-FileHash -LiteralPath (Join-Path $LauncherRoot 'IMao-Launcher.exe') -Algorithm SHA256).Hash) { throw 'Launcher bytes differ from their build receipt.' }
    if ([Diagnostics.FileVersionInfo]::GetVersionInfo((Join-Path $LauncherRoot 'IMao-Launcher.exe')).FileVersion -ne $version) { throw 'Launcher executable version does not match the package.' }
}
$name = "IMao-v$version-windows-x64"
$package = Join-Path $OutputRoot $name
if (Test-Path -LiteralPath $package) { throw 'Package staging already exists; reviewed files will not be overwritten.' }
[IO.Directory]::CreateDirectory($package) | Out-Null
if ($LauncherRoot) {
    Copy-Item -LiteralPath (Join-Path $LauncherRoot 'IMao-Launcher.exe') -Destination $package
    Copy-Item -LiteralPath (Join-Path $LauncherRoot 'launcher-build-info.json') -Destination $package
}
foreach ($file in Get-ChildItem -LiteralPath $PublishRoot -Recurse -File) {
    if ($file.Extension -eq '.pdb') { continue }
    $relative = [IO.Path]::GetRelativePath($PublishRoot,$file.FullName)
    $dest = Join-Path $package $relative
    [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($dest)) | Out-Null
    Copy-Item -LiteralPath $file.FullName -Destination $dest
}
foreach ($name in @('IMao-CoreHost.exe','paddle_inference.dll','common.dll','mklml.dll','libiomp5md.dll','mkldnn.dll','opencv_world4110.dll')) {
    Copy-Item -LiteralPath (Join-Path $NativeRoot $name) -Destination $package
}
if ($nativeReceipt) { Assert-NativeBuildProvenance $nativeReceipt $sourceBefore $build (Join-Path $package 'IMao-CoreHost.exe') }
Get-ChildItem -LiteralPath (Join-Path $RedistRoot 'Microsoft.VC143.CRT') -File -Filter '*.dll' | Copy-Item -Destination $package
Copy-Item -LiteralPath (Join-Path $RedistRoot 'Microsoft.VC143.OpenMP/vcomp140.dll') -Destination $package
$cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
if (-not $cmake) {
    $vswhere = 'C:/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe'
    $cmake = & $vswhere -latest -products '*' -find 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe' | Select-Object -First 1
}
if (-not $cmake) { throw 'CMake not found.' }
& $cmake "-DIMAO_SOURCE_ASSETS=$(Join-Path $SourceRoot 'Assets')" "-DIMAO_DESTINATION_ROOT=$package" '-DIMAO_CONFIGURATION=Release' '-P' (Join-Path $SourceRoot 'cmake/StageAssets.cmake')
if ($LASTEXITCODE -ne 0) { throw 'Asset staging failed.' }
& (Join-Path $PSScriptRoot 'Stage-UpdateResources.ps1') -SourceRoot $SourceRoot -Destination $package -SourceCommit $SourceCommit
$stagedBuild = Get-Content -LiteralPath (Join-Path $package 'build-info.json') -Raw -Encoding UTF8 | ConvertFrom-Json
Assert-ResourceBuildUnchanged $sourceBefore $stagedBuild
Assert-ManagedBuildProvenance $build $stagedBuild
if ($stagedBuild.appVersion -ne $build.appVersion -or $stagedBuild.baselineId -ne $build.baselineId) {
    throw 'Source version or baseline changed after the managed build. Rebuild before packaging.'
}
if (-not (Test-Path -LiteralPath (Join-Path $package 'Assets/Updates/trusted-keys.json'))) { throw 'Production package needs a pinned release public key.' }
$keys = Get-Content -LiteralPath (Join-Path $package 'Assets/Updates/trusted-keys.json') -Raw | ConvertFrom-Json
if (-not @($keys.keys | Where-Object { -not $_.testOnly }).Count -or @($keys.keys | Where-Object testOnly).Count) { throw 'Production package must contain production public keys only.' }
Copy-Item -LiteralPath (Join-Path $SourceRoot 'LICENSE') -Destination $package
$noticeRoot = Join-Path $package 'Licenses'
[IO.Directory]::CreateDirectory($noticeRoot) | Out-Null
$notices = @{
    'DotNet-LICENSE.txt'='third_party/nuget-packages/microsoft.netcore.app.runtime.win-x64/8.0.30/LICENSE.TXT';
    'DotNet-THIRD-PARTY-NOTICES.txt'='third_party/nuget-packages/microsoft.netcore.app.runtime.win-x64/8.0.30/THIRD-PARTY-NOTICES.TXT';
    'WindowsAppSDK-LICENSE.txt'='third_party/nuget-packages/microsoft.windowsappsdk/1.7.250606001/license.txt';
    'WindowsAppSDK-NOTICE.txt'='third_party/nuget-packages/microsoft.windowsappsdk/1.7.250606001/NOTICE.txt';
    'OpenCV-LICENSE.txt'='third_party/src/opencv-4.11.0/LICENSE';
    'OpenCV-LICENSE-BSD.txt'='third_party/src/opencv-4.11.0/doc/LICENSE_BSD.txt';
    'Upstream-ReleaseAssets-v1.0.2.md'='Docs/ReleaseAssets_v1.0.2.md';
    'ReleaseAssets_v1.0.2.sha256'='Docs/ReleaseAssets_v1.0.2.sha256'
}
foreach ($entry in $notices.GetEnumerator()) { Copy-Item -LiteralPath (Join-Path $SourceRoot $entry.Value) -Destination (Join-Path $noticeRoot $entry.Key) }
Copy-Item -LiteralPath (Join-Path $SourceRoot 'Docs/ResourceUpdates.md') -Destination (Join-Path $package 'README-Updates.md')
Copy-Item -LiteralPath (Join-Path $SourceRoot 'Docs/ProgramUpdates.md') -Destination (Join-Path $package 'ProgramUpdates.md')
$unwanted = @(Get-ChildItem -LiteralPath $package -Recurse -File | Where-Object {
    $_.FullName -match '(?i)[\\/](SavedPoints|SavedRoutes|Logs|diagnostics|obj|\.git|ResourceUpdates|ProgramUpdates)[\\/]|\.pdb$|\.log$|\.user$|[\\/]Map_features\.yml$|[\\/]imgui\.ini$|[\\/]IMao.*Tests\.exe$|private.*key|signing-key|\.pfx$|\.pem$'
})
if ($unwanted.Count) { throw ('Private/development files in program package: ' + ($unwanted.Name -join ', ')) }
foreach ($relative in @('IMao-WinUI.exe','IMao-WinUI.dll','IMao-WinUI.Core.dll','IMao-CoreHost.exe','coreclr.dll','hostfxr.dll','hostpolicy.dll','Microsoft.ui.xaml.dll','Microsoft.WindowsAppRuntime.dll','resources.pri','vcomp140.dll','msvcp140.dll','vcruntime140.dll','vcruntime140_1.dll','Assets/Updates/bundled-snapshot.json','build-info.json')) {
    if (-not (Test-Path -LiteralPath (Join-Path $package $relative) -PathType Leaf)) { throw "Missing runtime file: $relative" }
}
# The base map features are retired when they are not in the source tree; the pack simply leaves without them.
# A copy that is present must be complete, because the runtime loads the pair together.
foreach ($relative in @('Assets/FeaturesDatas/Map_features.imf','Assets/FeaturesDatas/Map_visual_index.imx')) {
    $present = Test-Path -LiteralPath (Join-Path $package $relative) -PathType Leaf
    $expected = Test-Path -LiteralPath (Join-Path $SourceRoot $relative) -PathType Leaf
    if ($present -ne $expected) { throw "Base map feature packaging does not match the source tree: $relative" }
}
$runtime = Get-Content -LiteralPath (Join-Path $package 'IMao-WinUI.runtimeconfig.json') -Raw | ConvertFrom-Json
if (-not $runtime.runtimeOptions.includedFrameworks -or $runtime.runtimeOptions.frameworks -or $runtime.runtimeOptions.framework) { throw 'Program package must include its .NET runtime.' }
& (Join-Path $PSScriptRoot 'Test-ProgramReleasePackage.ps1') -PackageRoot $package -OutputRoot (Join-Path $OutputRoot 'verification')
if (-not $?) { throw 'Program package probe failed.' }
$archive = "$package.zip"
if (Test-Path -LiteralPath $archive) { throw 'Archive already exists.' }
[IO.Compression.ZipFile]::CreateFromDirectory($package,$archive,[IO.Compression.CompressionLevel]::Optimal,$false)
$sha = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
$report = [ordered]@{passed=$true;version=$version;sourceCommit=$SourceCommit;sourceDirty=($build.sourceDirty -ne $false -or $stagedBuild.sourceDirty -ne $false);sourceTreeSha256=$stagedBuild.sourceTreeSha256;managedSourceTreeSha256=$build.sourceTreeSha256;nativeBuildReceipt=$nativeReceipt;baselineId=$build.baselineId;sha256=$sha;size=(Get-Item -LiteralPath $archive).Length;files=@(Get-ChildItem -LiteralPath $package -Recurse -File).Count}
[IO.File]::WriteAllText([IO.Path]::ChangeExtension($archive,'.report.json'),($report | ConvertTo-Json),[Text.UTF8Encoding]::new($false))
Write-Host "Verified program archive: $archive (SHA256 $sha)"
