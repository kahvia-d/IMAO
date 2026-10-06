[CmdletBinding()]
param([Parameter(Mandatory)][string]$SourceCommit, [Parameter(Mandatory)][string]$OutputRoot, [bool]$Rehearsal=$true)
$ErrorActionPreference='Stop'
$repoRoot=Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'Enter-DevEnvironment.ps1')
# A pointer-only checkout cannot silently become a small, incomplete release.
$tracked=@(& git -C $repoRoot lfs ls-files -n)
if ($LASTEXITCODE -ne 0) { throw 'Cannot enumerate LFS inputs.' }
foreach ($file in $tracked) {
    $stream=[IO.File]::OpenRead((Join-Path $repoRoot $file))
    try { $prefix=[byte[]]::new(128); $length=$stream.Read($prefix,0,$prefix.Length); if ([Text.Encoding]::UTF8.GetString($prefix,0,$length).StartsWith('version https://git-lfs.github.com/spec/v1')) { throw "LFS pointer remains: $file" } } finally { $stream.Dispose() }
}
& (Join-Path $PSScriptRoot 'Build-ReleaseCandidate.ps1') -OutputRoot $OutputRoot -SourceCommit $SourceCommit -NativeConfigurationDirectory 'out/ci-native-config'
& (Join-Path $PSScriptRoot 'Test-Runtime.ps1') -NativeBuildDirectory (Join-Path $OutputRoot 'native-build') -OutputDirectory (Join-Path $OutputRoot 'tests')
& (Join-Path $PSScriptRoot 'Test-ReleaseTransactions.ps1')
& (Join-Path $PSScriptRoot 'Test-ReleaseGitCas.ps1')
& (Join-Path $PSScriptRoot 'Test-CiSmallRequest.ps1')
& dotnet build (Join-Path $repoRoot 'tools/UpdatePublisher/UpdatePublisher.csproj') -c Release
if ($LASTEXITCODE -ne 0) { throw 'Publisher build failed.' }
$dll=Join-Path $repoRoot 'tools/UpdatePublisher/bin/Release/net8.0/UpdatePublisher.dll'
& dotnet $dll cloud-self-test --output (Join-Path $OutputRoot 'publisher-tests')
if ($LASTEXITCODE -ne 0) { throw 'Detached signing regression tests failed.' }
[xml]$props=Get-Content (Join-Path $repoRoot 'Version.props') -Raw
$version=[string]$props.Project.PropertyGroup.IMaoVersion
$programRoot=Join-Path $OutputRoot 'program'
$app=Join-Path $programRoot "IMao-v$version-windows-x64"
$manual=Join-Path $programRoot "IMao-v$version-windows-x64.zip"
if (-not (Test-Path $app) -or -not (Test-Path $manual)) { throw 'Candidate output layout differs from packaging contract.' }
$prepared=Join-Path $OutputRoot 'prepared'
$previous=Join-Path $OutputRoot 'previous-stable.json'
# Public channel read: build has no production credentials.
Invoke-WebRequest -Uri 'https://raw.githubusercontent.com/kahvia-d/IMAO/main/updates/stable.json' -OutFile $previous -TimeoutSec 60
$verified=& dotnet $dll verify-manifest --input $previous --public-key (Join-Path $repoRoot 'Assets/Updates/trusted-keys.json')
if ($LASTEXITCODE -ne 0) { throw 'Published baseline cannot be verified.' }
$sequence=[long]($verified | ConvertFrom-Json).sequence + 1 # provisional, never a formal allocation
& dotnet $dll prepare-artifacts --app-root $app --output $prepared --previous $previous --public-key (Join-Path $repoRoot 'Assets/Updates/trusted-keys.json') --sequence $sequence --resource-version $version --tag "v$version" --program-release true --program-shards true --core-host (Join-Path $app 'IMao-CoreHost.exe') --rehearsal $Rehearsal.ToString().ToLowerInvariant() --notes-file (Join-Path $repoRoot 'Docs/CloudReleaseNotes.md')
if ($LASTEXITCODE -ne 0) { throw 'Unsigned preparation failed.' }
[IO.Directory]::CreateDirectory((Join-Path $prepared 'manual')) | Out-Null
Copy-Item $manual (Join-Path $prepared 'manual')
Copy-Item ([IO.Path]::ChangeExtension($manual,'.report.json')) (Join-Path $prepared 'manual')
if ($env:GITHUB_OUTPUT) { "prepared=$prepared" >> $env:GITHUB_OUTPUT; "version=$version" >> $env:GITHUB_OUTPUT }
