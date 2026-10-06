[CmdletBinding()]
param([Parameter(Mandatory)][long]$BuildRunId, [Parameter(Mandatory)][long]$RequestArtifactId,
    [Parameter(Mandatory)][string]$ExpectedSourceCommit, [Parameter(Mandatory)][string]$ConfirmVersion,
    [Parameter(Mandatory)][string]$ResponseJson, [Parameter(Mandatory)][string]$OutputRoot)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'CiReleaseMetadata.ps1')
[IO.Directory]::CreateDirectory($OutputRoot) | Out-Null
$stateFile=Invoke-CiGh @('api','repos/kahvia-d/IMAO/contents/updates/release-state.json?ref=main') | ConvertFrom-Json
$state=[Text.Encoding]::UTF8.GetString([Convert]::FromBase64String(($stateFile.content -replace '\s',''))) | ConvertFrom-Json
$done=@($state.completed | Where-Object { $_.buildRunId -eq $BuildRunId -and $_.requestArtifactId -eq $RequestArtifactId -and $_.sourceCommit -ceq $ExpectedSourceCommit -and $_.version -ceq $ConfirmVersion -and $_.status -ceq 'completed' })
if ($done.Count -eq 1) {
    if ($env:GITHUB_OUTPUT) { 'already_completed=true' >> $env:GITHUB_OUTPUT; "tag=$($done[0].tag)" >> $env:GITHUB_OUTPUT }
    Write-Host 'Transaction already completed; no rebuild or repeated Release is needed.'
    exit 0
}
if ($state.active.buildRunId -ne $BuildRunId -or $state.active.requestArtifactId -ne $RequestArtifactId -or $state.active.sourceCommit -cne $ExpectedSourceCommit -or $state.active.version -cne $ConfirmVersion) { throw 'Signature submission differs from active transaction.' }
$meta=$null; $release=$null
try { $meta=Invoke-CiGh @('api',"repos/kahvia-d/IMAO/actions/artifacts/$RequestArtifactId") | ConvertFrom-Json } catch { $meta=$null }
$archive=Join-Path $OutputRoot 'request.zip'; $request=Join-Path $OutputRoot 'request'
if ($meta -and -not $meta.expired) {
    if ($meta.name -cne 'release-signing-request' -or $meta.workflow_run.id -ne $BuildRunId -or $meta.size_in_bytes -gt 16MB -or $state.active.requestArtifactDigest -cne ($meta.digest -replace '^sha256:','')) { throw 'Request artifact identity differs.' }
    Save-CiArtifactArchive $RequestArtifactId $archive 16MB
    Assert-CiArtifactDigest $archive $meta.digest
} else {
    if (-not $state.active.releaseId) { throw 'Unpublished request expired; explicitly abandon the reservation and create a new build.' }
    $release=Invoke-CiGh @('api',"repos/kahvia-d/IMAO/releases/$($state.active.releaseId)") | ConvertFrom-Json
    if ($release.draft -or $release.tag_name -cne $state.active.tag) { throw 'Expired request requires the original public release.' }
    $asset=@($release.assets | Where-Object name -CEQ 'release-signing-request.zip')
    if ($asset.Count -ne 1 -or $asset[0].size -gt 16MB) { throw 'Original public audit request is missing or oversized.' }
    Invoke-CiGh @('release','download',$release.tag_name,'--repo','kahvia-d/IMAO','--pattern','release-signing-request.zip','--dir',$OutputRoot) | Out-Null
    $archive=Join-Path $OutputRoot 'release-signing-request.zip'
    Assert-CiArtifactDigest $archive $asset[0].digest
}
Expand-CiSmallRequest $archive $request
$a=Get-Content (Join-Path $request 'approval-payload.json') -Raw | ConvertFrom-Json
if ($a.sourceCommit -cne $ExpectedSourceCommit -or $a.version -cne $ConfirmVersion -or $a.buildRunId -ne $BuildRunId) { throw 'Request provenance differs.' }
$index=Get-Content (Join-Path $request 'request.json') -Raw | ConvertFrom-Json
if ($index.requestId -cne $state.active.requestId) { throw 'Request differs from registered transaction.' }
$artifact=$null
try { $artifact=Get-CiBuildArtifact $BuildRunId $a.artifactId $ExpectedSourceCommit } catch {
    if (-not $state.active.releaseId) { throw 'Frozen artifact unavailable before publication; abandon and rebuild, never substitute new bytes.' }
    $release=Invoke-CiGh @('api',"repos/kahvia-d/IMAO/releases/$($state.active.releaseId)") | ConvertFrom-Json
    if ($release.draft) { throw 'Frozen artifact expired while release is still a draft; manual recovery is required before publication.' }
}
if ($a.artifactId -ne $state.active.artifactId -or $a.artifactDigest -cne $state.active.artifactDigest -or ($artifact -and $a.artifactDigest -cne ($artifact.digest -replace '^sha256:',''))) { throw 'Approval and frozen artifact identity differ.' }
if ([Text.Encoding]::UTF8.GetByteCount($ResponseJson) -gt 16KB) { throw 'Oversized signature response.' }
$response=Join-Path $OutputRoot 'response.json'; [IO.File]::WriteAllText($response,$ResponseJson,[Text.UTF8Encoding]::new($false))
if ($artifact) {
    $bulkArchive=Join-Path $OutputRoot 'build.zip'; Save-CiArtifactArchive $a.artifactId $bulkArchive
    Assert-CiArtifactDigest $bulkArchive $a.artifactDigest
    Expand-Archive -LiteralPath $bulkArchive -DestinationPath (Join-Path $OutputRoot 'prepared')
    Remove-Item -LiteralPath $bulkArchive
} else {
    . (Join-Path $PSScriptRoot 'Restore-CiPublishedBuild.ps1')
    Restore-CiPublishedBuild $a $release $request (Join-Path $OutputRoot 'prepared')
}
if ($env:GITHUB_OUTPUT) { "source_commit=$($a.sourceCommit)" >> $env:GITHUB_OUTPUT; "tag=$($a.tag)" >> $env:GITHUB_OUTPUT }
