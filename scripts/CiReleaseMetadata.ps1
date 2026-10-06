# Read-only checks shared by cloud verification and the trusted local signer.
function Invoke-CiGh([string[]]$Arguments) { $result=& gh @Arguments; if ($LASTEXITCODE -ne 0) { throw 'GitHub metadata operation failed.' }; return $result }
function Assert-CiRehearsal([long]$RunId, [string]$SourceCommit) {
    if ($RunId -le 0) { throw 'Set CLOUD_RELEASE_REHEARSAL_RUN_ID only after a green full cloud rehearsal.' }
    $run=Invoke-CiGh @('api',"repos/kahvia-d/IMAO/actions/runs/$RunId") | ConvertFrom-Json
    $artifacts=(Invoke-CiGh @('api',"repos/kahvia-d/IMAO/actions/runs/$RunId/artifacts?per_page=100") | ConvertFrom-Json).artifacts
    if ($run.conclusion -cne 'success' -or $run.head_sha -cne $SourceCommit -or $run.path -cne '.github/workflows/cloud-release-build.yml' -or
        $run.head_branch -cne 'main' -or $run.event -cne 'workflow_dispatch' -or $run.repository.full_name -cne 'kahvia-d/IMAO' -or
        -not @($artifacts | Where-Object { $_.name -ceq 'release-rehearsal-evidence' -and -not $_.expired }).Count) { throw 'A green publish=false full rehearsal of this exact main source is required.' }
}
function Get-CiBuildArtifact([long]$RunId, [long]$ArtifactId, [string]$ExpectedSourceCommit, [switch]$AllowRehearsal) {
    $repo='kahvia-d/IMAO'
    $run=Invoke-CiGh @('api',"repos/$repo/actions/runs/$RunId") | ConvertFrom-Json
    if ($run.repository.full_name -cne $repo -or $run.head_repository.full_name -cne $repo -or $run.head_sha -cne $ExpectedSourceCommit -or
        $run.path -cne '.github/workflows/cloud-release-build.yml' -or $run.event -cne 'workflow_dispatch' -or
        (-not $AllowRehearsal -and ($run.head_branch -cne 'main' -or $run.conclusion -cne 'success'))) { throw 'Build run has an unexpected repository, workflow, source or result.' }
    $artifact=Invoke-CiGh @('api',"repos/$repo/actions/artifacts/$ArtifactId") | ConvertFrom-Json
    if ($artifact.expired -or $artifact.name -cne 'release-build' -or $artifact.workflow_run.id -ne $RunId -or $artifact.workflow_run.head_sha -cne $ExpectedSourceCommit -or $artifact.digest -notmatch '^sha256:[a-f0-9]{64}$') { throw 'Frozen build artifact identity, digest or retention differs.' }
    return $artifact
}
function Assert-CiArtifactDigest([string]$Archive, [string]$Digest) {
    if ($Digest -notmatch '^(sha256:)?[a-f0-9]{64}$' -or (Get-FileHash $Archive -Algorithm SHA256).Hash.ToLowerInvariant() -cne ($Digest -replace '^sha256:','')) { throw 'Artifact service digest differs from downloaded bytes.' }
}
function Save-CiArtifactArchive([long]$ArtifactId, [string]$Archive, [long]$MaxBytes=4GB) {
    $start=[Diagnostics.ProcessStartInfo]::new('gh'); $start.UseShellExecute=$false; $start.CreateNoWindow=$true; $start.RedirectStandardOutput=$true
    foreach ($arg in @('api',"repos/kahvia-d/IMAO/actions/artifacts/$ArtifactId/zip")) { $start.ArgumentList.Add($arg) }
    $p=[Diagnostics.Process]::Start($start); $f=[IO.File]::Create($Archive)
    try {
        $buffer=[byte[]]::new(65536); [long]$total=0
        while (($read=$p.StandardOutput.BaseStream.Read($buffer,0,$buffer.Length)) -gt 0) { $total+=$read; if ($total -gt $MaxBytes) { $p.Kill($true); throw 'Artifact download exceeded its allowed size.' }; $f.Write($buffer,0,$read) }
        $p.WaitForExit(); if ($p.ExitCode -ne 0) { throw 'Artifact download failed.' }
    } finally { $f.Dispose(); $p.Dispose() }
}
function Expand-CiSmallRequest([string]$Archive, [string]$Destination) {
    $allowed=@('request.json','catalog-payload.json','approval-payload.json','asset-inventory.json','preparation-report.json','previous-stable.json')
    $zip=[IO.Compression.ZipFile]::OpenRead($Archive)
    try {
        if ($zip.Entries.Count -ne 6 -or ($zip.Entries | Select-Object -ExpandProperty FullName -Unique).Count -ne 6) { throw 'Request archive has unexpected or duplicate files.' }
        [long]$total=0
        foreach ($e in $zip.Entries) {
            $total+=$e.Length
            if ($e.FullName -cnotin $allowed -or $total -gt 16MB -or (($e.ExternalAttributes -shr 16) -band 0xF000) -eq 0xA000) { throw 'Request archive is oversized or contains unsafe entries.' }
        }
        if (Test-Path $Destination) { throw 'Request extraction destination must be new.' }
        [IO.Directory]::CreateDirectory($Destination) | Out-Null
        foreach ($e in $zip.Entries) {
            $stream=$e.Open(); $file=[IO.File]::Open((Join-Path $Destination $e.FullName),[IO.FileMode]::CreateNew)
            try {
                $buffer=[byte[]]::new(65536); [long]$written=0
                while (($read=$stream.Read($buffer,0,$buffer.Length)) -gt 0) { $written+=$read; if ($written -gt $e.Length -or $written -gt 16MB) { throw 'Expanded entry exceeds advertised size.' }; $file.Write($buffer,0,$read) }
                if ($written -ne $e.Length) { throw 'Request entry was truncated.' }
            } finally { $stream.Dispose(); $file.Dispose() }
        }
    } finally { $zip.Dispose() }
}
