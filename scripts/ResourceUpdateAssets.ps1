# Program archives of a shard release. The signed catalog is the only source of truth: every archive this
# release uploads has to be named there with the size and SHA-256 of the local file, and a shard whose URL
# points at an older tag keeps that release's copy instead of being uploaded into the new one.
function Get-ProgramShardAssets([object]$Catalog, [string]$PreparedRoot, [string]$Repo, [string]$Tag) {
    if ($null -eq $Catalog.app.package -or @($Catalog.app.package.shards).Count -eq 0) {
        throw 'This catalog does not publish a shard program release.'
    }
    $currentTag = "/$Repo/releases/download/$Tag/"
    $upload = [Collections.Generic.List[object]]::new()
    $retained = [Collections.Generic.List[object]]::new()
    # Local binding: the file the catalog names must exist with exactly the signed length and bytes.
    function Read-LocalProgramAsset([string]$Url, [object]$Record, [string]$Kind) {
        $name = [IO.Path]::GetFileName(([Uri]$Url).AbsolutePath)
        $path = Join-Path $PreparedRoot ('program/' + $name)
        if (-not [IO.File]::Exists($path)) { throw "$Kind archive is missing from the prepared output: $name" }
        if ([IO.FileInfo]::new($path).Length -ne [long]$Record.size) { throw "$Kind archive length differs from the signed catalog: $name" }
        $hash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($hash -ne ([string]$Record.sha256).ToLowerInvariant()) { throw "$Kind archive bytes differ from the signed catalog: $name" }
        return [pscustomobject]@{ path = $path; name = $name; sha256 = $hash; url = [string]$Url; isCurrent = ([Uri]$Url).AbsolutePath.StartsWith($currentTag, [StringComparison]::Ordinal) }
    }
    foreach ($shard in $Catalog.app.package.shards) {
        $asset = Read-LocalProgramAsset ([string]$shard.url) $shard 'Shard'
        if ($asset.isCurrent) { $upload.Add($asset) } else { $retained.Add($asset) }
    }
    $descriptor = Read-LocalProgramAsset ([string]$Catalog.app.package.url) $Catalog.app.package 'Shard descriptor'
    if ($descriptor.isCurrent) { $upload.Add($descriptor) } else { $retained.Add($descriptor) }
    return [pscustomobject]@{ Upload = $upload; Retained = $retained }
}

# Assets a preparation that did not build the program still has to prove are reachable: an installed
# client follows these URLs, so they have to answer even though this release does not upload them.
function Get-RetainedProgramAssets([object]$Catalog, [string]$Repo, [string]$Tag) {
    if ($null -eq $Catalog.app.package -or @($Catalog.app.package.shards).Count -eq 0) { return @() }
    $currentTag = "/$Repo/releases/download/$Tag/"
    $retained = [Collections.Generic.List[object]]::new()
    foreach ($record in @($Catalog.app.package) + @($Catalog.app.package.shards)) {
        $path = ([Uri]([string]$record.url)).AbsolutePath
        if (-not $path.StartsWith($currentTag, [StringComparison]::Ordinal)) {
            $retained.Add([pscustomobject]@{ name = [IO.Path]::GetFileName($path); url = [string]$record.url })
        }
    }
    # Emitted one by one on purpose: a caller that writes @(Get-RetainedProgramAssets ...) then gets one
    # entry per asset, not a single element holding the whole list.
    return $retained
}

# A brand-new installation still needs one complete program archive on the release page, even when the
# signed catalog ships shards instead of a whole archive. Nothing in the catalog names it, so the package
# report the packaging script wrote next to it is what ties it to this exact clean-source release.
function Assert-ManualInstallArchive([string]$ProgramZip, [object]$Report) {
    $zipPath = [IO.Path]::GetFullPath($ProgramZip)
    $reportPath = [IO.Path]::ChangeExtension($zipPath, '.report.json')
    if (-not [IO.File]::Exists($zipPath)) { throw "Manual install archive not found: $zipPath" }
    if (-not [IO.File]::Exists($reportPath)) { throw "Manual install archive has no package report: $reportPath" }
    $programReport = Get-Content -LiteralPath $reportPath -Raw | ConvertFrom-Json
    if (-not $programReport.passed -or $programReport.sourceDirty -ne $false -or
        $programReport.sourceCommit -ne $Report.sourceCommit -or $programReport.version -ne $Report.appVersion) {
        throw 'Manual install archive lacks a matching clean-source package validation report.'
    }
    if ([IO.FileInfo]::new($zipPath).Length -ne [long]$programReport.size) { throw 'Manual install archive length differs from its package report.' }
    $hash = (Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($hash -ne ([string]$programReport.sha256).ToLowerInvariant()) { throw 'Manual install archive changed after verification.' }
    return [pscustomobject]@{ path = $zipPath; name = [IO.Path]::GetFileName($zipPath); sha256 = $hash }
}
