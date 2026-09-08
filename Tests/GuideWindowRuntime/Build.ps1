$ErrorActionPreference = 'Stop'
$guideRepo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
. (Join-Path $guideRepo 'scripts\Enter-DevEnvironment.ps1')
$guideVswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$guideVcvars = & $guideVswhere -latest -products * -find 'VC\Auxiliary\Build\vcvars64.bat' | Select-Object -First 1
$guideMsbuild = & $guideVswhere -latest -products * -find 'MSBuild\Current\Bin\MSBuild.exe' | Select-Object -First 1
if (-not $guideVcvars -or -not $guideMsbuild) { throw 'Visual Studio MSBuild and desktop C++ tools are required.' }
$guideSdkDirectory = Join-Path $guideRepo 'tools\dotnet-sdk-8.0.424\sdk\8.0.424\Sdks'
if (-not (Test-Path -LiteralPath $guideSdkDirectory)) { throw 'The repository .NET SDK 8.0.424 is unavailable.' }
$guideLog = Join-Path $guideRepo 'out\guide-window-harness-build.log'
[IO.Directory]::CreateDirectory((Split-Path -Parent $guideLog)) | Out-Null
$guideInfo = [Diagnostics.ProcessStartInfo]::new()
$guideInfo.FileName = "$env:SystemRoot\System32\cmd.exe"
$guideInfo.Arguments = '/d /c call "' + $guideVcvars + '" >nul && "' + $guideMsbuild +
    '" Tests\GuideWindowRuntime\GuideWindowRuntime.csproj /t:Build /p:Configuration=Release /p:Platform=x64 /p:NuGetAudit=false /m > "' + $guideLog + '" 2>&1'
$guideInfo.UseShellExecute = $false
$guideInfo.CreateNoWindow = $true
$guideInfo.WorkingDirectory = $guideRepo
[void]$guideInfo.EnvironmentVariables.Remove('Path')
[void]$guideInfo.EnvironmentVariables.Remove('PATH')
$guideInfo.EnvironmentVariables['PATH'] = [Environment]::GetEnvironmentVariable('Path', 'Process')
$guideInfo.EnvironmentVariables['USERPROFILE'] = Join-Path $guideRepo 'third_party\dotnet-user-profile'
$guideInfo.EnvironmentVariables['APPDATA'] = Join-Path $guideRepo 'third_party\dotnet-user-profile\AppData\Roaming'
$guideInfo.EnvironmentVariables['LOCALAPPDATA'] = Join-Path $guideRepo 'third_party\dotnet-user-profile\AppData\Local'
$guideInfo.EnvironmentVariables['MSBuildSDKsPath'] = $guideSdkDirectory
$guideInfo.EnvironmentVariables['DOTNET_MSBUILD_SDK_RESOLVER_CLI_DIR'] = Join-Path $guideRepo 'tools\dotnet-sdk-8.0.424'
$guideInfo.EnvironmentVariables['MSBuildEnableWorkloadResolver'] = 'false'
$guideBuild = [Diagnostics.Process]::Start($guideInfo)
try {
    $guideBuild.WaitForExit()
    [IO.File]::ReadAllLines($guideLog) | Select-Object -Last 12
    if ($guideBuild.ExitCode -ne 0) { throw "Guide window harness build failed: $guideLog" }
}
finally { $guideBuild.Dispose() }
