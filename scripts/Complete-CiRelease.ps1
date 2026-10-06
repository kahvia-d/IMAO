[CmdletBinding()]
param(
    [Parameter(Mandatory)][long]$RunId,
    [Parameter(Mandatory)][string]$ConfirmVersion,
    [Parameter(Mandatory)][ValidatePattern('^[a-f0-9]{40}$')][string]$ExpectedSourceCommit,
    [string]$PrivateKey=(Join-Path $env:LOCALAPPDATA 'WWMAP-TOOLS-Publisher/release-signing-key.json'),
    [string]$PublisherDll,
    [switch]$Approve
)
$ErrorActionPreference='Stop'
if ($env:GITHUB_ACTIONS -eq 'true') { throw 'Run this helper on the trusted local Windows signer.' }
$repoRoot=Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'Enter-DevEnvironment.ps1')
. (Join-Path $PSScriptRoot 'CiReleaseMetadata.ps1')
if (-not $PublisherDll) { $PublisherDll=Join-Path $repoRoot 'tools/UpdatePublisher/bin/Release/net8.0/UpdatePublisher.dll' }
if (-not (Test-Path $PublisherDll)) { throw 'Build UpdatePublisher from reviewed local source first. Never execute a downloaded signing binary.' }
$repo='kahvia-d/IMAO'
$run=Invoke-CiGh @('api',"repos/$repo/actions/runs/$RunId") | ConvertFrom-Json
if ($run.path -cne '.github/workflows/cloud-release-build.yml' -or $run.head_sha -cne $ExpectedSourceCommit -or $run.head_branch -cne 'main' -or $run.conclusion -cne 'success' -or $run.repository.full_name -cne $repo) { throw 'Expected a completed formal main build at the explicitly approved source SHA.' }
$artifacts=(Invoke-CiGh @('api',"repos/$repo/actions/runs/$RunId/artifacts?per_page=100") | ConvertFrom-Json).artifacts
$requests=@($artifacts | Where-Object name -CEQ 'release-signing-request')
if ($requests.Count -ne 1 -or $requests[0].expired -or $requests[0].size_in_bytes -gt 16MB -or $requests[0].digest -notmatch '^sha256:[a-f0-9]{64}$') { throw 'A unique, bounded, unexpired signing request is required.' }
$root=Join-Path $env:LOCALAPPDATA "WWMAP-TOOLS-Publisher/ci/$RunId/$($requests[0].id)"
[IO.Directory]::CreateDirectory($root) | Out-Null
$archive=Join-Path $root 'request.zip'; $request=Join-Path $root 'request'
if (-not (Test-Path $archive)) {
    # Exactly one artifact endpoint is downloaded, never release-build or the run's whole artifact set.
    Save-CiArtifactArchive $requests[0].id $archive 16MB
}
Assert-CiArtifactDigest $archive $requests[0].digest
if (-not (Test-Path $request)) { Expand-CiSmallRequest $archive $request }
$approval=Get-Content (Join-Path $request 'approval-payload.json') -Raw | ConvertFrom-Json
$artifact=Get-CiBuildArtifact $approval.buildRunId $approval.artifactId $ExpectedSourceCommit
if ($approval.buildRunId -ne $RunId -or $approval.artifactDigest -cne ($artifact.digest -replace '^sha256:','') -or $approval.version -cne $ConfirmVersion) { throw 'Approval does not bind the selected build or version.' }
$stateFile=Invoke-CiGh @('api',"repos/$repo/contents/updates/release-state.json?ref=main") | ConvertFrom-Json
$state=[Text.Encoding]::UTF8.GetString([Convert]::FromBase64String(($stateFile.content -replace '\s',''))) | ConvertFrom-Json
if ($state.active.id -cne $approval.transactionId -or $state.active.requestArtifactId -ne $requests[0].id -or $state.active.requestArtifactDigest -cne ($requests[0].digest -replace '^sha256:','')) { throw 'Request is not the active durable reservation.' }
$response=Join-Path $root 'response.json'
Write-Host "Version $ConfirmVersion; source $ExpectedSourceCommit; sequence $($approval.sequence); transaction $($approval.transactionId)"
Write-Host "Build artifact $($artifact.id); SHA-256 $($approval.artifactDigest)"
$inventory=Get-Content (Join-Path $request 'asset-inventory.json') -Raw | ConvertFrom-Json
$inventory | Where-Object { $_.path -like 'manual/*' } | Format-Table name,size,sha256
if (-not $Approve -and (Read-Host 'Type the exact version to approve local signing and dispatch publication') -cne $ConfirmVersion) { throw 'Local approval was not given.' }
if (-not (Test-Path $response)) {
    & $env:IMAO_DOTNET $PublisherDll sign-request --input $request --output $response --private-key $PrivateKey --public-key (Join-Path $repoRoot 'Assets/Updates/trusted-keys.json') --expected-source-commit $ExpectedSourceCommit --confirm-version $ConfirmVersion
    if ($LASTEXITCODE -ne 0) { throw 'Local request signing failed.' }
}
if ((Get-Item $response).Length -gt 16KB) { throw 'Signature response is oversized.' }
$body=@{ref='main';inputs=@{publish='true';confirm_version=$ConfirmVersion;build_run_id=[string]$RunId;request_artifact_id=[string]$requests[0].id;signature_response=[IO.File]::ReadAllText($response);expected_source_commit=$ExpectedSourceCommit}}
$dispatch=Join-Path $root 'dispatch.json'; [IO.File]::WriteAllText($dispatch,($body | ConvertTo-Json -Depth 10),[Text.UTF8Encoding]::new($false))
Invoke-CiGh @('api',"repos/$repo/actions/workflows/cloud-release-publish.yml/dispatches",'--method','POST','--input',$dispatch) | Out-Null
Write-Host "Submitted small detached signatures. Keep $response for recovery; no installation ZIP or shard was downloaded."
