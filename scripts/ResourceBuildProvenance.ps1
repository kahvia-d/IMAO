# Shared by resource staging and program packaging; compatible with Windows PowerShell 5.1.
function Get-ResourceBuildProvenance([string]$SourceRoot, [string]$SourceCommit) {
    $SourceRoot = [IO.Path]::GetFullPath($SourceRoot).TrimEnd('\','/')
    function Invoke-ProvenanceGit([string[]]$Arguments) {
        $result = @(& git -C $SourceRoot -c core.quotepath=false @Arguments)
        if ($LASTEXITCODE -ne 0) { throw ('Cannot establish source provenance: git ' + ($Arguments -join ' ') + ' failed.') }
        return $result
    }
    $gitRoot = [string](Invoke-ProvenanceGit @('rev-parse','--show-toplevel'))
    if ([IO.Path]::GetFullPath($gitRoot).TrimEnd('\','/') -ne $SourceRoot) { throw 'SourceRoot must be the root of its source repository.' }
    $head = [string](Invoke-ProvenanceGit @('rev-parse','HEAD'))
    if ($head -notmatch '^[a-f0-9]{40}$') { throw 'A real source commit is required.' }
    if ($SourceCommit -and ($SourceCommit -notmatch '^[a-f0-9]{40}$' -or $SourceCommit -cne $head)) {
        throw 'Selected SourceCommit differs from the current source HEAD. Check out the selected commit before building.'
    }
    $status = @(Invoke-ProvenanceGit @('status','--porcelain=v1','--untracked-files=all'))
    $changed = @(Invoke-ProvenanceGit @('ls-files','--modified','--others','--exclude-standard'))
    $changed += @(Invoke-ProvenanceGit @('diff','--cached','--name-only'))
    $provenance = [Collections.Generic.List[string]]::new()
    $provenance.Add($head)
    foreach ($entry in @($changed | Sort-Object -Unique)) {
        $file = Join-Path $SourceRoot $entry
        if ([IO.File]::Exists($file)) {
            $hash = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant()
            $provenance.Add($entry + ':' + $hash)
        } else { $provenance.Add($entry + ':deleted') }
    }
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try { $treeHash = ([BitConverter]::ToString($algorithm.ComputeHash([Text.Encoding]::UTF8.GetBytes(($provenance -join "`n"))))).Replace('-','').ToLowerInvariant() }
    finally { $algorithm.Dispose() }
    return [pscustomobject]@{ sourceCommit=$head; sourceDirty=($status.Count -ne 0); sourceTreeSha256=$treeHash }
}

function Assert-ResourceBuildUnchanged($Before, $After) {
    if ($Before.sourceCommit -cne $After.sourceCommit -or $Before.sourceDirty -ne $After.sourceDirty -or $Before.sourceTreeSha256 -cne $After.sourceTreeSha256) {
        throw 'Source changed during resource staging or packaging. Rebuild from the selected source commit.'
    }
}

function Assert-ManagedBuildProvenance($Build, $Current) {
    if ($Build.sourceCommit -cne $Current.sourceCommit) { throw 'Managed build source differs from the current source HEAD.' }
    # Dirty builds are useful local QA artifacts, but retain sourceDirty=true and cannot be published.
    if ($Build.sourceDirty -eq $false -and ($Current.sourceDirty -ne $false -or $Build.sourceTreeSha256 -notmatch '^[a-f0-9]{64}$' -or $Build.sourceTreeSha256 -cne $Current.sourceTreeSha256)) {
        throw 'Clean managed build provenance does not match current source. Rebuild before packaging.'
    }
}

function Assert-NativeBuildProvenance($Receipt, $Current, $Build, [string]$CoreHostPath) {
    if ($Receipt.formatVersion -ne 1 -or $Receipt.sourceCommit -cne $Current.sourceCommit -or
        $Receipt.sourceDirty -ne $Current.sourceDirty -or $Receipt.sourceTreeSha256 -cne $Current.sourceTreeSha256 -or
        $Receipt.appVersion -cne $Build.appVersion -or $Receipt.baselineId -cne $Build.baselineId -or
        $Receipt.coreHostSha256 -notmatch '^[a-f0-9]{64}$') {
        throw 'Native build receipt does not match the selected source and managed program version. Rebuild CoreHost.'
    }
    if ((Get-FileHash -LiteralPath $CoreHostPath -Algorithm SHA256).Hash.ToLowerInvariant() -cne $Receipt.coreHostSha256) {
        throw 'CoreHost binary differs from its reviewed native build receipt. Rebuild CoreHost.'
    }
}
