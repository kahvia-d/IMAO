[CmdletBinding()]
param([Parameter(Mandatory)][string]$PreparedRoot, [Parameter(Mandatory)][long]$ArtifactId,
    [Parameter(Mandatory)][string]$ArtifactDigest, [Parameter(Mandatory)][long]$BuildRunId)
$ErrorActionPreference='Stop'
$repoRoot=Split-Path -Parent $PSScriptRoot
$dll=Join-Path $repoRoot 'tools/UpdatePublisher/bin/Release/net8.0/UpdatePublisher.dll'
$root=Join-Path (Split-Path $PreparedRoot) 'rehearsal'
[IO.Directory]::CreateDirectory($root) | Out-Null
$private=Join-Path $root 'test-key.json'; $testPublic=Join-Path $root 'test-public.json'; $public=Join-Path $root 'combined-public.json'
& dotnet $dll init-key --repo $repoRoot --private-key $private --public-key $testPublic --key-id ci-rehearsal --test true
if ($LASTEXITCODE -ne 0) { throw 'Cannot create disposable rehearsal key.' }
$productionKeys=Get-Content (Join-Path $repoRoot 'Assets/Updates/trusted-keys.json') -Raw | ConvertFrom-Json
$testKeys=Get-Content $testPublic -Raw | ConvertFrom-Json
[IO.File]::WriteAllText($public,(@{keys=@($productionKeys.keys)+@($testKeys.keys)} | ConvertTo-Json -Depth 10),[Text.UTF8Encoding]::new($false))
$report=Get-Content (Join-Path $PreparedRoot 'release-report.json') -Raw | ConvertFrom-Json
$previous=Join-Path (Split-Path $PreparedRoot) 'previous-stable.json'
$request=Join-Path $root 'request'; $response=Join-Path $root 'response.json'
& dotnet $dll create-signing-request --input $PreparedRoot --output $request --previous $previous --public-key $public --sequence $report.sequence --transaction-id ci-rehearsal --build-run-id $BuildRunId --artifact-id $ArtifactId --artifact-digest $ArtifactDigest --test true
if ($LASTEXITCODE -ne 0) { throw 'Rehearsal request failed.' }
& dotnet $dll sign-request --input $request --output $response --private-key $private --public-key $public --expected-source-commit $report.sourceCommit --confirm-version $report.appVersion --test true
if ($LASTEXITCODE -ne 0) { throw 'Disposable-key rehearsal signing failed.' }
& dotnet $dll finalize --input $PreparedRoot --request $request --response $response --public-key $public --expected-source-commit $report.sourceCommit --confirm-version $report.appVersion --test true
if ($LASTEXITCODE -ne 0) { throw 'Cloud full-artifact rehearsal finalization failed.' }
# The disposable DPAPI key is never included in uploaded evidence.
Remove-Item -LiteralPath $private
Copy-Item (Join-Path $PreparedRoot 'release-report.json') (Join-Path $root 'rehearsal-report.json')
# Bind the disposable-key result to the exact frozen artifact, so a reviewer can confirm that the
# artifact digest an approval releases is the one that was rehearsed, not merely one that was built.
$rehearsalReport=Join-Path $root 'rehearsal-report.json'
$evidence=@{buildRunId=$BuildRunId;artifactId=$ArtifactId;artifactDigest=($ArtifactDigest -replace '^sha256:','')
    sourceCommit=[string]$report.sourceCommit;appVersion=[string]$report.appVersion;baselineId=[string]$report.baselineId
    provisionalSequence=[long]$report.sequence
    rehearsalReportSha256=(Get-FileHash -LiteralPath $rehearsalReport -Algorithm SHA256).Hash.ToLowerInvariant()}
[IO.File]::WriteAllText((Join-Path $root 'rehearsal-evidence.json'),($evidence | ConvertTo-Json -Depth 5),[Text.UTF8Encoding]::new($false))
