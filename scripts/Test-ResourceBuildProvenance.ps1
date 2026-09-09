[CmdletBinding()]
param([Parameter(Mandatory)][string]$OutputRoot)
$ErrorActionPreference = 'Stop'
if ($PSVersionTable.PSVersion.Major -lt 7) { throw 'PowerShell 7 or newer is required.' }
. (Join-Path $PSScriptRoot 'ResourceBuildProvenance.ps1')
$OutputRoot = [IO.Path]::GetFullPath($OutputRoot)
if (Test-Path -LiteralPath $OutputRoot) { throw 'Use a fresh provenance regression output directory.' }
$source = Join-Path $OutputRoot 'source'
$publish = Join-Path $OutputRoot 'managed'
$native = Join-Path $OutputRoot 'native'
foreach ($directory in @($source,$publish,$native)) { [IO.Directory]::CreateDirectory($directory) | Out-Null }
[IO.File]::WriteAllText((Join-Path $source 'fixture.txt'),'committed-source')
& git init --quiet $source
if ($LASTEXITCODE -ne 0) { throw 'Fixture repository initialization failed.' }
& git -C $source add -- fixture.txt
if ($LASTEXITCODE -ne 0) { throw 'Fixture source staging failed.' }
& git -C $source -c user.name=ReleaseFixture -c user.email=fixture@example.invalid -c commit.gpgsign=false commit --quiet -m 'Build provenance fixture'
if ($LASTEXITCODE -ne 0) { throw 'Fixture source commit failed.' }
$current = Get-ResourceBuildProvenance $source ''
$build = [pscustomobject]@{sourceCommit=$current.sourceCommit;sourceDirty=$false;sourceTreeSha256=$current.sourceTreeSha256;appVersion='2026.9.9.3';baselineId='fixture'}
$coreHost = Join-Path $native 'IMao-CoreHost.exe'
[IO.File]::WriteAllText($coreHost,'reviewed-native-binary')
$receipt = [pscustomobject]@{formatVersion=1;sourceCommit=$current.sourceCommit;sourceDirty=$false;sourceTreeSha256=$current.sourceTreeSha256;coreHostSha256=(Get-FileHash -LiteralPath $coreHost -Algorithm SHA256).Hash.ToLowerInvariant();appVersion=$build.appVersion;baselineId=$build.baselineId}
$passed = [Collections.Generic.List[string]]::new()
function Expect-Rejection([string]$Name, [string]$Message, [scriptblock]$Action) {
    try { & $Action }
    catch { if ($_.Exception.Message -notlike $Message) { throw }; $passed.Add($Name); return }
    throw "Expected rejection: $Name"
}
function Invoke-Package([string]$Name, [string]$Commit) {
    [IO.File]::WriteAllText((Join-Path $publish 'build-info.json'),($build | ConvertTo-Json))
    & (Join-Path $PSScriptRoot 'New-ProgramReleasePackage.ps1') -PublishRoot $publish -NativeRoot $native -OutputRoot (Join-Path $OutputRoot $Name) -SourceRoot $source -SourceCommit $Commit -RedistRoot (Join-Path $OutputRoot 'unused-redist')
}
Assert-ManagedBuildProvenance $build $current
Assert-NativeBuildProvenance $receipt $current $build $coreHost
$passed.Add('matching clean managed and native provenance accepted')
Expect-Rejection 'package forged source SHA rejected before writes' '*Selected SourceCommit differs*' { Invoke-Package 'forged' ('a'*40) }
if (Test-Path -LiteralPath (Join-Path $OutputRoot 'forged')) { throw 'Forged source created package output.' }
$build.sourceTreeSha256 = 'b'*64
Expect-Rejection 'clean managed hash mismatch rejected before writes' '*Clean managed build provenance*' { Invoke-Package 'mismatched-clean' $current.sourceCommit }
if (Test-Path -LiteralPath (Join-Path $OutputRoot 'mismatched-clean')) { throw 'Mismatched clean source created package output.' }
$build.sourceTreeSha256 = $current.sourceTreeSha256
Expect-Rejection 'clean package requires native build receipt' '*requires native-build-info.json*' { Invoke-Package 'missing-native' $current.sourceCommit }
if (Test-Path -LiteralPath (Join-Path $OutputRoot 'missing-native')) { throw 'Missing native receipt created package output.' }
$receipt.sourceCommit = 'c'*40
Expect-Rejection 'native receipt from another commit rejected' '*Native build receipt does not match*' { Assert-NativeBuildProvenance $receipt $current $build $coreHost }
$receipt.sourceCommit = $current.sourceCommit
$receipt.appVersion = '2026.9.9.2'
Expect-Rejection 'native receipt program version mismatch rejected' '*Native build receipt does not match*' { Assert-NativeBuildProvenance $receipt $current $build $coreHost }
$receipt.appVersion = $build.appVersion
[IO.File]::WriteAllText($coreHost,'replaced-native-binary')
Expect-Rejection 'changed native binary rejected' '*CoreHost binary differs*' { Assert-NativeBuildProvenance $receipt $current $build $coreHost }
Expect-Rejection 'failed git command cannot produce clean metadata' '*' { Get-ResourceBuildProvenance (Join-Path $source '.git') '' }
Expect-Rejection 'nested non-repository source rejected' '*SourceRoot must be the root*' { Get-ResourceBuildProvenance $native '' }
[IO.File]::WriteAllText((Join-Path $source 'fixture.txt'),'modified-source')
$dirty = Get-ResourceBuildProvenance $source $current.sourceCommit
if (-not $dirty.sourceDirty -or $dirty.sourceTreeSha256 -eq $current.sourceTreeSha256) { throw 'Source changes were not represented in provenance.' }
Expect-Rejection 'source mutation during staging rejected' '*Source changed during*' { Assert-ResourceBuildUnchanged $current $dirty }
Expect-Rejection 'clean managed build cannot package later dirty source' '*Clean managed build provenance*' { Assert-ManagedBuildProvenance $build $dirty }
$build.sourceDirty = $true
Assert-ManagedBuildProvenance $build $dirty
if ($build.sourceDirty -ne $true) { throw 'Dirty QA metadata was relabeled clean.' }
$passed.Add('explicitly dirty local QA remains allowed without relabeling clean')
$noModulesScript = Join-Path $OutputRoot 'ps5-without-module-autoload.ps1'
[IO.File]::WriteAllText($noModulesScript, @'
param([string]$Helper,[string]$Source,[string]$Commit,[string]$ExpectedHash)
$ErrorActionPreference = 'Stop'
$PSModuleAutoLoadingPreference = 'None'
. $Helper
$provenance = Get-ResourceBuildProvenance $Source $Commit
if (-not $provenance.sourceDirty -or $provenance.sourceTreeSha256 -cne $ExpectedHash) { throw 'Dirty source SHA256 requires module auto loading.' }
'@, [Text.UTF8Encoding]::new($false))
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $noModulesScript -Helper (Join-Path $PSScriptRoot 'ResourceBuildProvenance.ps1') -Source $source -Commit $current.sourceCommit -ExpectedHash $dirty.sourceTreeSha256
if ($LASTEXITCODE -ne 0) { throw 'PowerShell 5 source hashing failed without module auto loading.' }
$passed.Add('PS5 dirty source hashing works with module auto loading disabled')
[IO.File]::WriteAllText((Join-Path $OutputRoot 'test-report.json'),(@{passed=$passed.Count;tests=@($passed.ToArray())} | ConvertTo-Json -Depth 5),[Text.UTF8Encoding]::new($false))
Write-Host "PASS $($passed.Count) source provenance regressions."
