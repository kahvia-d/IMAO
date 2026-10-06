[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'ReleaseTransactions.ps1')
function Assert($Condition, $Message) { if (-not $Condition) { throw $Message } }
function Reject([scriptblock]$Action) { try { & $Action } catch { return }; throw 'Expected transaction rejection.' }
$state = @{highestAllocatedSequence=45;active=$null}
Assert ((Get-NextReleaseSequence $state 39 42) -eq 46) 'Allocated floor was reused.'
Assert ((Get-NextReleaseSequence $state 51 48) -eq 52) 'Locked stable was not reread.'
$a = @{transactionId='txn';sequence=52;sourceCommit=('a'*40);version='2026.10.6.2';tag='v2026.10.6.2';artifactId=123;artifactDigest=('b'*64);buildRunId=321}
$state.active = @{id='txn';sequence=52;sourceCommit=$a.sourceCommit;version=$a.version;tag=$a.tag;artifactId=123;artifactDigest=$a.artifactDigest;buildRunId=321;previousStableSha256=('c'*64);previousChannelSequence=48}
Reject { Get-NextReleaseSequence $state 51 48 }
Assert-ReleaseTransaction $state $a ('c'*64) 48
Reject { Assert-ReleaseTransaction $state $a ('d'*64) 48 }
Reject { Assert-ReleaseTransaction $state $a ('c'*64) 49 }
$changed = $a.Clone(); $changed.artifactId = 124
Reject { Assert-ReleaseTransaction $state $changed ('c'*64) 48 }
$state.highestAllocatedSequence=52; $state.active=$null
Assert ((Get-NextReleaseSequence $state 51 48) -eq 53) 'Abandoned sequence was reused.'
Write-Host 'PASS sequence floors, offline reservation, stale baseline and artifact substitution checks.'
