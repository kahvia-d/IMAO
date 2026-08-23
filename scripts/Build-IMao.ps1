[CmdletBinding()]
param(
    [string]$PaddleLib = $env:IMAO_PADDLE_LIB,
    [string]$OpenCvDir = $env:IMAO_OPENCV_DIR,
    [string]$Dotnet = $env:IMAO_DOTNET,
    [switch]$ConfigureOnly
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$dotnetCliHome = Join-Path $repoRoot 'third_party\dotnet-cli-home'
$nugetPackages = Join-Path $repoRoot 'third_party\nuget-packages'
$dotnetUserProfile = Join-Path $repoRoot 'third_party\dotnet-user-profile'
$dotnetAppData = Join-Path $dotnetUserProfile 'AppData\Roaming'
$dotnetLocalAppData = Join-Path $dotnetUserProfile 'AppData\Local'
New-Item -ItemType Directory -Path $dotnetCliHome, $nugetPackages, $dotnetAppData, $dotnetLocalAppData -Force | Out-Null
$preflight = Join-Path $PSScriptRoot 'Test-BuildPrerequisites.ps1'
& $preflight -PaddleLib $PaddleLib -OpenCvDir $OpenCvDir -Dotnet $Dotnet
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

if ([string]::IsNullOrWhiteSpace($Dotnet)) {
    $Dotnet = (Get-Command dotnet -ErrorAction Stop).Source
}

$vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
$cmake = & $vswhere -latest -products * -find 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' | Select-Object -First 1
if ([string]::IsNullOrWhiteSpace($cmake)) { throw 'Unable to locate Visual Studio CMake.' }
$ninja = & $vswhere -latest -products * -find 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe' | Select-Object -First 1
if ([string]::IsNullOrWhiteSpace($ninja)) { throw 'Unable to locate Visual Studio Ninja.' }
$vcvars = & $vswhere -latest -products * -find 'VC\Auxiliary\Build\vcvars64.bat' | Select-Object -First 1
if ([string]::IsNullOrWhiteSpace($vcvars)) { throw 'Unable to locate the Visual Studio x64 developer environment.' }

function Invoke-VisualStudioCommand([string]$CommandLine) {
    $logPath = Join-Path $env:TEMP ("imao-build-" + [guid]::NewGuid().ToString() + '.log')
    $cmdCommand = 'call "' + $vcvars + '" >nul && (' + $CommandLine + ') > "' + $logPath + '" 2>&1'

    $startInfo = New-Object System.Diagnostics.ProcessStartInfo
    $startInfo.UseShellExecute = $false

    # The desktop host injects both Path and PATH. MSBuild rejects this as a
    # duplicate environment key, so pass exactly one spelling to the child.
    [void]$startInfo.EnvironmentVariables
    [void]$startInfo.EnvironmentVariables.Remove('Path')
    [void]$startInfo.EnvironmentVariables.Remove('PATH')
    $startInfo.EnvironmentVariables.Add('PATH', (Split-Path -Parent $ninja) + ';' + [System.Environment]::GetEnvironmentVariable('Path', 'Process'))
    $startInfo.EnvironmentVariables['DOTNET_CLI_HOME'] = $dotnetCliHome
    $startInfo.EnvironmentVariables['NUGET_PACKAGES'] = $nugetPackages
    $startInfo.EnvironmentVariables['HOME'] = $dotnetUserProfile
    $startInfo.EnvironmentVariables['USERPROFILE'] = $dotnetUserProfile
    $startInfo.EnvironmentVariables['APPDATA'] = $dotnetAppData
    $startInfo.EnvironmentVariables['LOCALAPPDATA'] = $dotnetLocalAppData
    $startInfo.EnvironmentVariables['DOTNET_CLI_TELEMETRY_OPTOUT'] = '1'
    $startInfo.EnvironmentVariables['DOTNET_NOLOGO'] = '1'

    $startInfo.FileName = "$env:SystemRoot\System32\cmd.exe"
    $startInfo.Arguments = '/d /c ' + $cmdCommand
    $startInfo.WorkingDirectory = $repoRoot

    $process = New-Object System.Diagnostics.Process
    $process.StartInfo = $startInfo
    [void]$process.Start()
    $process.WaitForExit()

    if (Test-Path -LiteralPath $logPath) {
        Get-Content -LiteralPath $logPath
        Remove-Item -LiteralPath $logPath -Force
    }

    if ($process.ExitCode -ne 0) {
        throw "Command failed with exit code $($process.ExitCode): $CommandLine"
    }
}

Push-Location $repoRoot
try {
    Invoke-VisualStudioCommand ('"' + $cmake + '" --preset windows-x64-release "-DPADDLE_LIB=' + $PaddleLib + '" "-DOPENCV_DIR=' + $OpenCvDir + '"')

    if ($ConfigureOnly) {
        Write-Host 'CMake configuration completed.' -ForegroundColor Green
        exit 0
    }

    Invoke-VisualStudioCommand ('"' + $cmake + '" --build --preset windows-x64-release-core')
    Invoke-VisualStudioCommand ('"' + $Dotnet + '" build "IMao-WinUI\IMao-WinUI.csproj" --configuration Release -p:Platform=x64 --packages "' + $nugetPackages + '"')

    $outputDirectory = Join-Path $repoRoot 'x64\Release'
    if (-not (Test-Path -LiteralPath $outputDirectory)) {
        throw "Expected build output directory was not created: $outputDirectory"
    }

    Copy-Item -LiteralPath (Join-Path $repoRoot 'Assets') -Destination $outputDirectory -Recurse -Force
    $coreDll = Join-Path $repoRoot 'x64\Release\IMao-Core.dll'
    if (-not (Test-Path -LiteralPath $coreDll)) { throw "Missing C++ build output: $coreDll" }

    $paddleDllDirectory = Join-Path $PaddleLib 'paddle\lib'
    $paddleRuntime = 'paddle_inference.dll', 'common.dll'
    $mklRuntimeDirectory = Join-Path $PaddleLib 'third_party\install\mklml\lib'
    $oneDnnRuntimeDirectory = Join-Path $PaddleLib 'third_party\install\onednn\lib'
    foreach ($file in $paddleRuntime) { Copy-Item -LiteralPath (Join-Path $paddleDllDirectory $file) -Destination $outputDirectory -Force }
    foreach ($file in 'mklml.dll', 'libiomp5md.dll') { Copy-Item -LiteralPath (Join-Path $mklRuntimeDirectory $file) -Destination $outputDirectory -Force }
    Copy-Item -LiteralPath (Join-Path $oneDnnRuntimeDirectory 'mkldnn.dll') -Destination $outputDirectory -Force

    $opencvRuntime = Get-ChildItem -Path $OpenCvDir -Recurse -Filter 'opencv_world*.dll' -File | Sort-Object FullName | Select-Object -First 1
    if ($null -eq $opencvRuntime) { throw 'Unable to find an opencv_world*.dll below OPENCV_DIR.' }
    Copy-Item -LiteralPath $opencvRuntime.FullName -Destination $outputDirectory -Force

    Write-Host "Build completed: $outputDirectory" -ForegroundColor Green
}
finally {
    Pop-Location
}
