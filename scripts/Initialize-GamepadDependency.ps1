[CmdletBinding()]
param([string]$SourceRoot, [string]$ArchiveRoot)
$ErrorActionPreference = 'Stop'
if ($PSVersionTable.PSVersion.Major -lt 7) { throw 'PowerShell 7 is required. Run this installer with pwsh.exe.' }
. (Join-Path $PSScriptRoot 'NativeImageReproducibility.ps1')
if (-not $SourceRoot) { $SourceRoot = Split-Path -Parent $PSScriptRoot }
if (-not $ArchiveRoot) { $ArchiveRoot = Join-Path $SourceRoot 'third_party/downloads' }
$lock = Get-Content (Join-Path $SourceRoot '.github/dependencies.lock.json') -Raw | ConvertFrom-Json
$dependencies = @($lock.dependencies | Where-Object name -eq 'sdl3')
if ($dependencies.Count -ne 1) { throw 'Exactly one locked SDL3 dependency is required.' }
$dep = $dependencies[0]
$archive = Join-Path $ArchiveRoot "sdl3-source-$($dep.version).zip"
[IO.Directory]::CreateDirectory($ArchiveRoot) | Out-Null
if (-not (Test-Path -LiteralPath $archive)) { Invoke-WebRequest -Uri $dep.url -OutFile $archive -TimeoutSec 120 }
$stream = [IO.File]::OpenRead($archive)
$algorithm = [Security.Cryptography.SHA256]::Create()
try { $archiveHash = ([BitConverter]::ToString($algorithm.ComputeHash($stream))).Replace('-','').ToLowerInvariant() }
finally { $algorithm.Dispose(); $stream.Dispose() }
if ((Get-Item -LiteralPath $archive).Length -ne $dep.size -or $archiveHash -cne $dep.sha256) {
    throw 'SDL3 archive hash/size differs from dependency lock.'
}
$destination = Join-Path $SourceRoot "third_party/sdl3-$($dep.version)"
$patch = Join-Path $SourceRoot "patches/sdl3-$($dep.version)-input-only.patch"
function Get-SdlHash([string]$Path) {
    $input = [IO.File]::OpenRead($Path); $sha = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($sha.ComputeHash($input))).Replace('-','').ToLowerInvariant() }
    finally { $input.Dispose(); $sha.Dispose() }
}
if ((Get-SdlHash $patch) -cne $dep.patchSha256) { throw 'SDL input-only patch differs from dependency lock.' }
$recipe = "$($dep.sha256):$($dep.patchSha256):$($dep.buildRecipe):$($lock.msvc):$($lock.windowsSdk)"
$receiptPath = Join-Path $destination 'build-receipt.json'
if (Test-Path -LiteralPath $receiptPath) {
    $receipt = Get-Content $receiptPath -Raw -Encoding UTF8 | ConvertFrom-Json
    if ($receipt.recipe -ceq $recipe -and (Test-Path -LiteralPath (Join-Path $destination 'SDL3.dll')) -and
        (Get-SdlHash (Join-Path $destination 'SDL3.dll')) -ceq $receipt.dllSha256 -and
        (Test-Path -LiteralPath (Join-Path $destination 'LICENSE.txt')) -and
        (Get-SdlHash (Join-Path $destination 'LICENSE.txt')) -ceq $receipt.licenseSha256) {
        Assert-ReproducibleNativeImage (Join-Path $destination 'SDL3.dll')
        Write-Host "Verified cached input-only SDL3 $($dep.version)"; return
    }
}
Add-Type -AssemblyName System.IO.Compression.FileSystem
[IO.Directory]::CreateDirectory($destination) | Out-Null
$sourceParent = Join-Path $SourceRoot 'third_party/src'
[IO.Directory]::CreateDirectory($sourceParent) | Out-Null
$zip = [IO.Compression.ZipFile]::OpenRead($archive)
try {
    foreach ($entry in $zip.Entries) {
        $target = [IO.Path]::GetFullPath((Join-Path $sourceParent $entry.FullName))
        if (-not $target.StartsWith([IO.Path]::GetFullPath($sourceParent).TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'SDL source ZIP path escaped extraction root.' }
        if ($entry.FullName.EndsWith('/')) { [IO.Directory]::CreateDirectory($target) | Out-Null; continue }
        [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($target)) | Out-Null
        $input = $entry.Open()
        $output = [IO.File]::Create($target)
        try { $input.CopyTo($output) } finally { $input.Dispose(); $output.Dispose() }
    }
} finally { $zip.Dispose() }
$source = [IO.Path]::GetFullPath((Join-Path $sourceParent "SDL3-$($dep.version)"))
& git apply --check --ignore-whitespace --unsafe-paths "--directory=$source" $patch
if ($LASTEXITCODE -ne 0) { throw 'SDL input-only patch does not apply to locked source.' }
& git apply --ignore-whitespace --unsafe-paths "--directory=$source" $patch
if ($LASTEXITCODE -ne 0) { throw 'SDL patch failed.' }
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vcvars = & $vswhere -latest -products * -find 'VC/Auxiliary/Build/vcvars64.bat' | Select-Object -First 1
$cmake = & $vswhere -latest -products * -find 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe' | Select-Object -First 1
$ninja = & $vswhere -latest -products * -find 'Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe' | Select-Object -First 1
if (-not $vcvars -or -not $cmake -or -not $ninja) { throw 'Locked SDL build requires Visual Studio CMake/Ninja and C++ tools.' }
$buildRoot = Join-Path $SourceRoot "third_party/build/sdl3-$($dep.version)-input-only"
function Invoke-SdlBuild([string]$Command) {
    [IO.Directory]::CreateDirectory($buildRoot) | Out-Null
    $commandFile = Join-Path $buildRoot 'invoke-build.cmd'
    [IO.File]::WriteAllText($commandFile, "@echo off`r`ncall `"$vcvars`" $($lock.windowsSdk) -vcvars_ver=$($lock.msvc) >nul`r`nif errorlevel 1 exit /b 1`r`n$Command`r`nexit /b %errorlevel%`r`n", [Text.Encoding]::Default)
    $info = [Diagnostics.ProcessStartInfo]::new()
    $info.FileName = Join-Path $env:SystemRoot 'System32\cmd.exe'
    $info.WorkingDirectory = $buildRoot
    $info.Arguments = '/d /c invoke-build.cmd'
    $info.UseShellExecute = $false; $info.CreateNoWindow = $true
    $info.RedirectStandardOutput = $true; $info.RedirectStandardError = $true
    [void]$info.EnvironmentVariables.Remove('Path'); [void]$info.EnvironmentVariables.Remove('PATH')
    $info.EnvironmentVariables['PATH'] = [Environment]::GetEnvironmentVariable('Path', 'Process')
    $child = [Diagnostics.Process]::Start($info)
    try {
        $stdout = $child.StandardOutput.ReadToEndAsync(); $stderr = $child.StandardError.ReadToEndAsync()
        $child.WaitForExit()
        Write-Host $stdout.GetAwaiter().GetResult(); Write-Host $stderr.GetAwaiter().GetResult()
        if ($child.ExitCode -ne 0) { throw 'Pinned input-only SDL build failed.' }
    }
    finally { $child.Dispose() }
}
Invoke-SdlBuild ('"' + $cmake + '" -S "' + $source + '" -B "' + $buildRoot + '" -G Ninja "-DCMAKE_MAKE_PROGRAM=' + $ninja + '" "-DCMAKE_C_FLAGS=/Brepro" "-DCMAKE_CXX_FLAGS=/Brepro" "-DCMAKE_SHARED_LINKER_FLAGS=/Brepro" "-DCMAKE_EXE_LINKER_FLAGS=/Brepro" -DCMAKE_BUILD_TYPE=Release -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST_LIBRARY=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF')
Invoke-SdlBuild ('"' + $cmake + '" --build "' + $buildRoot + '" --target SDL3-shared --parallel 4')
$dll = Join-Path $buildRoot 'SDL3.dll'
if (-not (Test-Path -LiteralPath $dll)) { throw 'SDL build did not produce SDL3.dll.' }
Assert-ReproducibleNativeImage $dll
Copy-Item -LiteralPath $dll -Destination (Join-Path $destination 'SDL3.dll') -Force
Copy-Item -LiteralPath (Join-Path $source 'LICENSE.txt') -Destination (Join-Path $destination 'LICENSE.txt') -Force
[ordered]@{ version = $dep.version; recipe = $recipe; sourceSha256 = $dep.sha256; patchSha256 = $dep.patchSha256;
    dllSha256 = Get-SdlHash (Join-Path $destination 'SDL3.dll'); licenseSha256 = Get-SdlHash (Join-Path $destination 'LICENSE.txt') } |
    ConvertTo-Json | Set-Content -LiteralPath $receiptPath -Encoding UTF8
Write-Host "Built verified input-only SDL3 $($dep.version): $destination"
