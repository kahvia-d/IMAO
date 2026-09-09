[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$OutputRoot,
    [string]$SourceCommit,
    [string]$NativeConfigurationDirectory = 'out/build/windows-x64-release-vs144',
    [string]$RedistRoot,
    [ValidateRange(1, 32)][int]$Parallel = 4
)
$ErrorActionPreference = 'Stop'
if ($PSVersionTable.PSVersion.Major -lt 7) { throw 'PowerShell 7 or newer is required.' }
$taskRepo = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
. (Join-Path $PSScriptRoot 'Enter-DevEnvironment.ps1')
. (Join-Path $PSScriptRoot 'ResourceBuildProvenance.ps1')
$taskSource = Get-ResourceBuildProvenance $taskRepo $SourceCommit
if ($taskSource.sourceDirty) { throw 'Commit the reviewed source before building a release candidate.' }
$SourceCommit = $taskSource.sourceCommit
$OutputRoot = [IO.Path]::GetFullPath($OutputRoot, $taskRepo)
$taskOutPrefix = [IO.Path]::GetFullPath((Join-Path $taskRepo 'out')).TrimEnd('\','/') + [IO.Path]::DirectorySeparatorChar
if (-not $OutputRoot.StartsWith($taskOutPrefix, [StringComparison]::OrdinalIgnoreCase)) { throw 'Candidate output must be a new directory under the repository out directory.' }
if (Test-Path -LiteralPath $OutputRoot) { throw 'Candidate output already exists. Use a new directory; prior evidence is preserved.' }
$taskCachePath = Join-Path ([IO.Path]::GetFullPath($NativeConfigurationDirectory, $taskRepo)) 'CMakeCache.txt'
$taskCache = [IO.File]::ReadAllLines($taskCachePath)
function Get-NativeConfiguration([string]$Name) {
    $line = @($taskCache | Where-Object { $_ -cmatch ('^' + [regex]::Escape($Name) + ':[^=]+=(.*)$') })
    if ($line.Count -ne 1) { throw "Configured native dependency is missing: $Name" }
    return ($line[0] -split '=', 2)[1]
}
$taskGenerator = Get-NativeConfiguration 'CMAKE_GENERATOR'
if ($taskGenerator -ne 'Visual Studio 17 2022') { throw 'This Windows candidate builder requires a validated Visual Studio 2022 native configuration.' }
$taskInstance = Get-NativeConfiguration 'CMAKE_GENERATOR_INSTANCE'
$taskToolset = Get-NativeConfiguration 'CMAKE_GENERATOR_TOOLSET'
$taskPaddle = Get-NativeConfiguration 'PADDLE_LIB'
$taskOpenCv = Get-NativeConfiguration 'OPENCV_DIR'
$taskCmake = Get-NativeConfiguration 'CMAKE_COMMAND'
$taskCxxFlags = Get-NativeConfiguration 'CMAKE_CXX_FLAGS'
$taskNativeOutput = Join-Path $taskRepo 'x64/Release'
foreach ($taskRunning in @(Get-Process -Name 'IMao-CoreHost','IMao-WinUI' -ErrorAction SilentlyContinue)) {
    try { $taskRunningPath = $taskRunning.Path } catch { throw 'Cannot verify the running application path. Close IMao before rebuilding native output.' }
    if (-not $taskRunningPath -or $taskRunningPath.StartsWith($taskNativeOutput + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'The development runtime may be running from x64/Release. Close that instance before rebuilding.'
    }
}
if (-not $RedistRoot) {
    $taskRedistVersion = Get-ChildItem -LiteralPath (Join-Path $taskInstance 'VC/Redist/MSVC') -Directory |
        Where-Object { $_.Name -match '^\d+\.\d+\.\d+$' } | Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
    if (-not $taskRedistVersion) { throw 'Specify -RedistRoot for the matching x64 Visual C++ runtime.' }
    $RedistRoot = Join-Path $taskRedistVersion.FullName 'x64'
}
[IO.Directory]::CreateDirectory($OutputRoot) | Out-Null
function Invoke-CandidateProcess([string]$Executable, [string[]]$Arguments, [string]$LogName) {
    $start = [Diagnostics.ProcessStartInfo]::new($Executable)
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.WindowStyle = [Diagnostics.ProcessWindowStyle]::Hidden
    $start.WorkingDirectory = $taskRepo
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    [void]$start.Environment.Remove('Path')
    [void]$start.Environment.Remove('PATH')
    $start.Environment['PATH'] = [Environment]::GetEnvironmentVariable('Path', 'Process')
    $start.Environment['USERPROFILE'] = Join-Path $taskRepo 'third_party/dotnet-user-profile'
    $start.Environment['APPDATA'] = Join-Path $taskRepo 'third_party/dotnet-user-profile/AppData/Roaming'
    $start.Environment['LOCALAPPDATA'] = Join-Path $taskRepo 'third_party/dotnet-user-profile/AppData/Local'
    foreach ($argument in $Arguments) { $start.ArgumentList.Add($argument) }
    $process = [Diagnostics.Process]::Start($start)
    $stdout = $process.StandardOutput.ReadToEndAsync()
    $stderr = $process.StandardError.ReadToEndAsync()
    try {
        $process.WaitForExit()
        $log = Join-Path $OutputRoot $LogName
        [IO.File]::WriteAllText($log, $stdout.GetAwaiter().GetResult() + $stderr.GetAwaiter().GetResult(), [Text.UTF8Encoding]::new($false))
        Get-Content -LiteralPath $log -Tail 12
        if ($process.ExitCode -ne 0) { throw "Candidate build failed: $LogName (exit $($process.ExitCode))." }
    } finally { $process.Dispose() }
}
$taskNativeBuild = Join-Path $OutputRoot 'native-build'
# A fresh CMake build directory has no prior objects to reuse. Dependency and
# toolset paths come from the validated local configuration, not PATH guesses.
# MSBuild's project parallelism does not parallelize C++ files within CoreHost.
# Preserve the configured flags and give MSVC the same explicit worker limit.
Invoke-CandidateProcess $taskCmake @('-S',$taskRepo,'-B',$taskNativeBuild,'-G',$taskGenerator,'-A','x64','-T',$taskToolset,
    "-DCMAKE_GENERATOR_INSTANCE=$taskInstance","-DPADDLE_LIB=$taskPaddle","-DOPENCV_DIR=$taskOpenCv",
    "-DCMAKE_CXX_FLAGS=$taskCxxFlags /MP$Parallel",
    '-DIMAO_ENABLE_DIAGNOSTICS=OFF','-DIMAO_ALLOW_XML_FEATURE_FALLBACK=OFF') 'native-configure.log'
Invoke-CandidateProcess $taskCmake @('--build',$taskNativeBuild,'--config','Release','--target','IMao-CoreHost','IMaoOptimizationTests',
    'IMaoMarkerTests','IMaoRoutePlanningTests','IMaoRoutePlanningServiceTests','IMaoVisualRegression','IMaoResourceSnapshotTests','--parallel',"$Parallel") 'native-build.log'
Assert-ResourceBuildUnchanged $taskSource (Get-ResourceBuildProvenance $taskRepo $SourceCommit)
$taskNative = Join-Path $OutputRoot 'native'
[IO.Directory]::CreateDirectory($taskNative) | Out-Null
foreach ($taskName in @('IMao-CoreHost.exe','paddle_inference.dll','common.dll','mklml.dll','libiomp5md.dll','mkldnn.dll')) {
    Copy-Item -LiteralPath (Join-Path $taskNativeOutput $taskName) -Destination $taskNative
}
$taskOpenCvRuntimes = @(Get-ChildItem -LiteralPath $taskOpenCv -Recurse -File -Filter 'opencv_world4110.dll')
if ($taskOpenCvRuntimes.Count -ne 1) { throw 'The selected OpenCV dependency must contain exactly one Release opencv_world4110.dll.' }
Copy-Item -LiteralPath $taskOpenCvRuntimes[0].FullName -Destination $taskNative
Copy-Item -LiteralPath $taskOpenCvRuntimes[0].FullName -Destination $taskNativeOutput
[xml]$taskVersion = Get-Content -LiteralPath (Join-Path $taskRepo 'Version.props') -Raw
$taskReceipt = [ordered]@{
    formatVersion = 1
    sourceCommit = $SourceCommit
    sourceDirty = $false
    sourceTreeSha256 = $taskSource.sourceTreeSha256
    coreHostSha256 = (Get-FileHash -LiteralPath (Join-Path $taskNative 'IMao-CoreHost.exe') -Algorithm SHA256).Hash.ToLowerInvariant()
    appVersion = [string]$taskVersion.Project.PropertyGroup.IMaoVersion
    baselineId = [string]$taskVersion.Project.PropertyGroup.IMaoBaselineId
}
[IO.File]::WriteAllText((Join-Path $taskNative 'native-build-info.json'), ($taskReceipt | ConvertTo-Json), [Text.UTF8Encoding]::new($false))
$taskPublish = Join-Path $OutputRoot 'publish'
$taskManagedBuild = (Join-Path $OutputRoot 'managed-build') + [IO.Path]::DirectorySeparatorChar
$taskManagedArguments = @('IMao-WinUI/IMao-WinUI.csproj','-c','Release','-r','win-x64','--self-contained','true',
    '-p:Platform=x64','-p:WindowsPackageType=None','-p:GenerateAppxPackageOnBuild=false','-p:AppxPackageSigningEnabled=false',
    '-p:NuGetAudit=false',"-p:BaseOutputPath=$taskManagedBuild",'--source',$env:NUGET_PACKAGES)
Invoke-CandidateProcess $env:IMAO_DOTNET (@('build') + $taskManagedArguments + @('-t:Rebuild')) 'managed-rebuild.log'
# WinUI's publish build targets generate and collect resources.pri. Skipping
# that build drops the application's resource index even after Rebuild.
Invoke-CandidateProcess $env:IMAO_DOTNET (@('publish') + $taskManagedArguments + @('--no-restore','-o',$taskPublish)) 'managed-publish.log'
$taskLauncher = Join-Path $OutputRoot 'launcher'
Invoke-CandidateProcess $env:IMAO_DOTNET @('publish','tools/ProgramLauncher/ProgramLauncher.csproj','-c','Release','-r','win-x64',
    '--self-contained','true','-o',$taskLauncher,'--source',$env:NUGET_PACKAGES,'-p:NuGetAudit=false') 'launcher-publish.log'
$taskLauncherReceipt = Get-Content -LiteralPath (Join-Path $taskPublish 'build-info.json') -Raw | ConvertFrom-Json
$taskLauncherReceipt | Add-Member -NotePropertyName launcherSha256 -NotePropertyValue ((Get-FileHash -LiteralPath (Join-Path $taskLauncher 'IMao-Launcher.exe') -Algorithm SHA256).Hash.ToLowerInvariant())
[IO.File]::WriteAllText((Join-Path $taskLauncher 'launcher-build-info.json'), ($taskLauncherReceipt | ConvertTo-Json), [Text.UTF8Encoding]::new($false))
Assert-ResourceBuildUnchanged $taskSource (Get-ResourceBuildProvenance $taskRepo $SourceCommit)
# Native runtime regressions use the development output; refresh its versioned
# resource descriptor from the same clean source before those checks run.
& (Join-Path $PSScriptRoot 'Stage-UpdateResources.ps1') -SourceRoot $taskRepo -Destination $taskNativeOutput -SourceCommit $SourceCommit
& (Join-Path $PSScriptRoot 'New-ProgramReleasePackage.ps1') -PublishRoot $taskPublish -NativeRoot $taskNative -LauncherRoot $taskLauncher -OutputRoot (Join-Path $OutputRoot 'program') -SourceCommit $SourceCommit -RedistRoot $RedistRoot
Assert-ResourceBuildUnchanged $taskSource (Get-ResourceBuildProvenance $taskRepo $SourceCommit)
Write-Host "Clean-source candidate complete: $OutputRoot; source $SourceCommit"
