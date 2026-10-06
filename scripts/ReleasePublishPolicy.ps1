function Get-AuthorizedManualInstallArchive($Inventory,[string]$PreparedRoot,[string]$Version,[string]$CallerPath) {
    $name="IMao-v$Version-windows-x64.zip"
    $record=@($Inventory | Where-Object { $_.name -ceq $name -and $_.path -ceq "manual/$name" })
    if ($record.Count -ne 1) { throw 'Exactly one installer bound by the verified approval is required.' }
    $approvedPath=[IO.Path]::GetFullPath((Join-Path $PreparedRoot "manual/$name"))
    if ($CallerPath -and [IO.Path]::GetFullPath($CallerPath) -cne $approvedPath) { throw 'Installer argument differs from the verified approved path.' }
    if (-not [IO.File]::Exists($approvedPath) -or [IO.FileInfo]::new($approvedPath).Length -ne $record[0].size -or
        (Get-FileHash -LiteralPath $approvedPath -Algorithm SHA256).Hash.ToLowerInvariant() -cne $record[0].sha256) { throw 'Actual installer bytes differ from the signed approval.' }
    return [pscustomobject]@{path=$approvedPath;name=$name;sha256=$record[0].sha256}
}
function Assert-ExactReleaseAssetSet([object[]]$Expected,[object[]]$Remote,[bool]$RequireComplete=$true) {
    $expectedNames=[Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    foreach ($asset in $Expected) { if (-not $expectedNames.Add([string]$asset.name)) { throw 'Duplicate expected release attachment.' } }
    $remoteNames=[Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    foreach ($asset in $Remote) {
        if (-not $remoteNames.Add([string]$asset.name) -or -not $expectedNames.Contains([string]$asset.name)) { throw 'Draft contains unexpected or duplicate attachments; nothing was deleted or published.' }
    }
    if ($RequireComplete -and -not $remoteNames.SetEquals($expectedNames)) { throw 'Release attachment name set differs from the complete authorized set.' }
}
function Assert-ReleaseSourceVersion([string]$Confirmation,[string]$ApprovedSourceVersion) {
    if ($Confirmation -cne $ApprovedSourceVersion) { throw 'confirm_version must exactly match Version.props at the approved source SHA.' }
}
function Assert-RetainedAssetMetadata($Expected,[object[]]$RemoteAssets) {
    $remote=@($RemoteAssets | Where-Object name -CEQ $Expected.name)
    if ($remote.Count -ne 1 -or $remote[0].size -ne $Expected.size) { throw 'Retained asset is missing, ambiguous or has an unexpected size.' }
    $digest=[string]$remote[0].digest -replace '^sha256:',''
    if (-not $digest) { return $false }
    if ($digest -cne $Expected.sha256) { throw 'Retained release attachment changed after approval.' }
    return $true
}
