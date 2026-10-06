[CmdletBinding()]
param([string]$OutputRoot)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'ReleaseTransactions.ps1')
if (-not $OutputRoot) { $OutputRoot=Join-Path (Split-Path $PSScriptRoot) ('out/git-cas-'+[guid]::NewGuid().ToString('N')) }
[IO.Directory]::CreateDirectory($OutputRoot) | Out-Null
$script:fixture=Join-Path $OutputRoot 'remote'
[IO.Directory]::CreateDirectory($script:fixture) | Out-Null
function Invoke-FixtureGit([string[]]$Arguments) { $result=& git.exe -C $script:fixture @Arguments 2>$null; if ($LASTEXITCODE -ne 0) { throw 'Fixture Git operation failed.' }; return $result }
Invoke-FixtureGit @('init','--quiet','--initial-branch=main') | Out-Null
Invoke-FixtureGit @('config','user.name','Release test') | Out-Null
Invoke-FixtureGit @('config','user.email','release-test@example.invalid') | Out-Null
[IO.Directory]::CreateDirectory((Join-Path $script:fixture 'updates')) | Out-Null
[IO.File]::WriteAllBytes((Join-Path $script:fixture 'updates/stable.json'), (ConvertTo-ReleaseBytes @{sequence=39}))
[IO.File]::WriteAllBytes((Join-Path $script:fixture 'updates/channel-state.json'), (ConvertTo-ReleaseBytes @{maxSequence=39}))
[IO.File]::WriteAllBytes((Join-Path $script:fixture 'updates/release-state.json'), (ConvertTo-ReleaseBytes @{formatVersion=1;highestAllocatedSequence=39;active=$null;completed=@()}))
Invoke-FixtureGit @('add','updates') | Out-Null; Invoke-FixtureGit @('commit','--quiet','-m','Initial state') | Out-Null
$script:loseResponse=$false
# Adapter replaces only HTTP transport, using real Git blobs, trees, commits and ref CAS.
function Invoke-Gh([string[]]$Arguments) {
    $route=$Arguments[1] -replace '^repos/fixture/', ''
    $body=$null
    if ($Arguments -contains '--input') { $body=Get-Content $Arguments[([array]::IndexOf($Arguments,'--input')+1)] -Raw | ConvertFrom-Json }
    $result=switch -Regex ($route) {
        '^git/ref/heads/main$' { @{object=@{sha=(Invoke-FixtureGit @('rev-parse','main'))}}; break }
        '^git/commits/([a-f0-9]{40})$' { @{tree=@{sha=(Invoke-FixtureGit @('rev-parse',"$($Matches[1])^{tree}"))}}; break }
        '^git/trees/([a-f0-9]{40})\?recursive=1$' {
            $rows=@(Invoke-FixtureGit @('ls-tree','-r',$Matches[1]) | ForEach-Object { $v=$_ -split '\s+',4; @{path=$v[3];sha=$v[2]} }); @{truncated=$false;tree=$rows}; break
        }
        '^git/blobs/([a-f0-9]{40})$' { @{content=[Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes(((Invoke-FixtureGit @('cat-file','blob',$Matches[1])) -join "`n")))}; break }
        '^git/blobs$' {
            $file=Join-Path $OutputRoot 'blob.json'; [IO.File]::WriteAllBytes($file,[Convert]::FromBase64String($body.content)); @{sha=(Invoke-FixtureGit @('hash-object','-w',$file))}; break
        }
        '^git/trees$' {
            $oldIndex=$env:GIT_INDEX_FILE; $env:GIT_INDEX_FILE=Join-Path $OutputRoot ('index-'+[guid]::NewGuid().ToString('N'))
            try { Invoke-FixtureGit @('read-tree',$body.base_tree) | Out-Null; foreach ($entry in $body.tree) { Invoke-FixtureGit @('update-index','--add','--cacheinfo',"100644,$($entry.sha),$($entry.path)") | Out-Null }; @{sha=(Invoke-FixtureGit @('write-tree'))} } finally { $env:GIT_INDEX_FILE=$oldIndex }
            break
        }
        '^git/commits$' { @{sha=(Invoke-FixtureGit @('commit-tree',$body.tree,'-p',$body.parents[0],'-m',$body.message))}; break }
        '^git/refs/heads/main$' {
            $current=Invoke-FixtureGit @('rev-parse','main')
            & git.exe -C $script:fixture merge-base --is-ancestor $current $body.sha
            if ($LASTEXITCODE -ne 0 -or $body.force) { throw 'Non-fast-forward rejected by remote.' }
            Invoke-FixtureGit @('update-ref','refs/heads/main',$body.sha,$current) | Out-Null
            if ($script:loseResponse) { $script:loseResponse=$false; throw 'Simulated lost HTTP response after committed ref update.' }
            @{}; break
        }
        default { throw "Unexpected fixture API: $route" }
    }
    return $result | ConvertTo-Json -Depth 50 -Compress
}
function Assert($Condition,$Message) { if (-not $Condition) { throw $Message } }
function Reject([scriptblock]$Action) { try { & $Action } catch { return }; throw 'Expected CAS conflict.' }
$first=Get-ReleaseSnapshot fixture; $competing=Get-ReleaseSnapshot fixture
$first.state.highestAllocatedSequence=40; $first.state.active=@{id='txn';sequence=40}
Save-ReleaseState fixture $first $OutputRoot 'Reserve 40'
$competing.state.highestAllocatedSequence=41
Reject { Save-ReleaseState fixture $competing $OutputRoot 'Competing stale reservation' }
$reserved=Get-ReleaseSnapshot fixture
Assert ($reserved.state.active.id -ceq 'txn' -and $reserved.channel.maxSequence -eq 39) 'Reservation changed the public channel.'
# A source commit after reservation must be preserved when promotion rereads current main.
Invoke-FixtureGit @('reset','--quiet','--hard','main') | Out-Null
[IO.File]::WriteAllText((Join-Path $script:fixture 'unrelated.txt'),'preserved')
Invoke-FixtureGit @('add','unrelated.txt') | Out-Null; Invoke-FixtureGit @('commit','--quiet','-m','Unrelated source change') | Out-Null
$current=Get-ReleaseSnapshot fixture; $current.state.active=$null; $current.state.completed+=@(@{id='txn';sequence=40;status='completed'})
$files=@{'updates/stable.json'=(ConvertTo-ReleaseBytes @{sequence=40});'updates/channel-state.json'=(ConvertTo-ReleaseBytes @{maxSequence=40});'updates/release-state.json'=(ConvertTo-ReleaseBytes $current.state)}
$script:loseResponse=$true
Reject { Write-ReleaseCommit fixture $current $files 'Atomic promotion' $OutputRoot }
$after=Get-ReleaseSnapshot fixture
Assert ($after.channel.maxSequence -eq 40 -and -not $after.state.active -and $after.state.completed.Count -eq 1) 'Lost response did not recover the atomic triple.'
Assert (([Text.Encoding]::UTF8.GetString($after.files['updates/stable.json']) | ConvertFrom-Json).sequence -eq 40) 'Stable disagrees with channel state.'
Assert ((Invoke-FixtureGit @('show','main:unrelated.txt')) -ceq 'preserved') 'Atomic commit dropped an unrelated source change.'
Assert ((Get-NextReleaseSequence $after.state 40 40) -eq 41) 'Completed sequence was reused.'
Write-Host 'PASS real Git CAS conflict, offline reservation, atomic triple, unrelated commit preservation and lost-response recovery.'
