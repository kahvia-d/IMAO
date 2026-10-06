# Pure transaction rules plus GitHub Git-object CAS. Dot-sourced by the sole publishing entrypoint.
function Get-NextReleaseSequence($State, [long]$StableSequence, [long]$ChannelSequence) {
    if ($null -ne $State.active) { throw 'A release transaction is active. Resume it or explicitly abandon it before reserving another.' }
    return [long]([Math]::Max([Math]::Max($StableSequence, $ChannelSequence), [long]$State.highestAllocatedSequence) + 1)
}
function Assert-ReleaseTransaction($State, $Approval, [string]$StableHash, [long]$ChannelSequence) {
    $active = $State.active
    if ($null -eq $active -or $active.id -cne $Approval.transactionId -or $active.sequence -ne $Approval.sequence -or
        $active.sourceCommit -cne $Approval.sourceCommit -or $active.version -cne $Approval.version -or $active.tag -cne $Approval.tag -or
        $active.artifactId -ne $Approval.artifactId -or $active.artifactDigest -cne $Approval.artifactDigest -or $active.buildRunId -ne $Approval.buildRunId -or
        $active.previousStableSha256 -cne $StableHash -or $active.previousChannelSequence -ne $ChannelSequence) {
        throw 'Active transaction, frozen artifact or channel baseline differs. Do not change signed sequence; inspect and prepare a new request.'
    }
}
function Get-ReleaseSnapshot([string]$Repo) {
    $head = (Invoke-Gh @('api', "repos/$Repo/git/ref/heads/main") | ConvertFrom-Json).object.sha
    $commit = Invoke-Gh @('api', "repos/$Repo/git/commits/$head") | ConvertFrom-Json
    $tree = Invoke-Gh @('api', "repos/$Repo/git/trees/$($commit.tree.sha)?recursive=1") | ConvertFrom-Json
    if ($tree.truncated) { throw 'Main tree is truncated; cannot safely read release state.' }
    $files = @{}
    foreach ($path in @('updates/stable.json', 'updates/channel-state.json', 'updates/release-state.json')) {
        $entry = @($tree.tree | Where-Object path -CEQ $path)
        if ($entry.Count -ne 1) { throw "Required durable channel file missing: $path" }
        $blob = Invoke-Gh @('api', "repos/$Repo/git/blobs/$($entry[0].sha)") | ConvertFrom-Json
        $files[$path] = [Convert]::FromBase64String(($blob.content -replace '\s', ''))
    }
    return @{ head=$head; tree=$commit.tree.sha; files=$files;
        state=([Text.Encoding]::UTF8.GetString($files['updates/release-state.json']) | ConvertFrom-Json -AsHashtable);
        channel=([Text.Encoding]::UTF8.GetString($files['updates/channel-state.json']) | ConvertFrom-Json -AsHashtable);
        stableHash=[Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($files['updates/stable.json'])).ToLowerInvariant() }
}
function ConvertTo-ReleaseBytes($Value) { return ,([Text.UTF8Encoding]::new($false).GetBytes(($Value | ConvertTo-Json -Depth 50))) }
function Write-ReleaseCommit([string]$Repo, $Snapshot, [hashtable]$Files, [string]$Message, [string]$Scratch) {
    # One commit changes all supplied files; non-force ref update is the actual distributed lock.
    $entries = @()
    foreach ($path in ($Files.Keys | Sort-Object)) {
        $body = @{content=[Convert]::ToBase64String($Files[$path]);encoding='base64'}
        $bodyFile = Join-Path $Scratch ('git-' + [guid]::NewGuid().ToString('N') + '.json')
        [IO.File]::WriteAllBytes($bodyFile, (ConvertTo-ReleaseBytes $body))
        $blob = Invoke-Gh @('api', "repos/$Repo/git/blobs", '--method','POST','--input',$bodyFile) | ConvertFrom-Json
        $entries += @{path=$path;mode='100644';type='blob';sha=$blob.sha}
    }
    $bodyFile = Join-Path $Scratch 'git-tree.json'
    [IO.File]::WriteAllBytes($bodyFile, (ConvertTo-ReleaseBytes @{base_tree=$Snapshot.tree;tree=$entries}))
    $tree = Invoke-Gh @('api', "repos/$Repo/git/trees", '--method','POST','--input',$bodyFile) | ConvertFrom-Json
    [IO.File]::WriteAllBytes($bodyFile, (ConvertTo-ReleaseBytes @{message=$Message;tree=$tree.sha;parents=@($Snapshot.head)}))
    $commit = Invoke-Gh @('api', "repos/$Repo/git/commits", '--method','POST','--input',$bodyFile) | ConvertFrom-Json
    [IO.File]::WriteAllBytes($bodyFile, (ConvertTo-ReleaseBytes @{sha=$commit.sha;force=$false}))
    Invoke-Gh @('api', "repos/$Repo/git/refs/heads/main", '--method','PATCH','--input',$bodyFile) | Out-Null
    return $commit.sha
}
function Save-ReleaseState([string]$Repo, $Snapshot, [string]$Scratch, [string]$Message) {
    Write-ReleaseCommit $Repo $Snapshot @{'updates/release-state.json'=(ConvertTo-ReleaseBytes $Snapshot.state)} $Message $Scratch | Out-Null
}
