[CmdletBinding()]
param([Parameter(Mandatory)][long]$BuildRunId, [Parameter(Mandatory)][long]$RequestArtifactId)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'CiReleaseMetadata.ps1')
$file=Invoke-CiGh @('api','repos/kahvia-d/IMAO/contents/updates/release-state.json?ref=main') | ConvertFrom-Json
$state=[Text.Encoding]::UTF8.GetString([Convert]::FromBase64String(($file.content -replace '\s',''))) | ConvertFrom-Json
$done=@($state.completed | Where-Object { $_.buildRunId -eq $BuildRunId -and $_.requestArtifactId -eq $RequestArtifactId -and $_.status -ceq 'completed' })
if ($done.Count -ne 1) { throw 'Only a completed, atomically promoted transaction can clean its frozen build.' }
$artifact=$null
try { $artifact=Invoke-CiGh @('api',"repos/kahvia-d/IMAO/actions/artifacts/$($done[0].artifactId)") | ConvertFrom-Json } catch { Write-Host 'Build artifact already unavailable; public release remains authoritative.'; exit 0 }
if ($artifact.name -cne 'release-build' -or $artifact.workflow_run.id -ne $BuildRunId -or $artifact.workflow_run.head_sha -cne $done[0].sourceCommit -or ($artifact.digest -replace '^sha256:','') -cne $done[0].artifactDigest) { throw 'Cleanup artifact identity differs from completed transaction.' }
Invoke-CiGh @('api',"repos/kahvia-d/IMAO/actions/artifacts/$($artifact.id)",'--method','DELETE') | Out-Null
Write-Host 'Removed only the completed large build artifact; small request retains its audit period.'
