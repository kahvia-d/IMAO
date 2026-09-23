# Requires PowerShell 7 and authenticated GitHub CLI. This script is the sole remote mutation entrypoint.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$PreparedRoot,
    [Parameter(Mandatory)][string]$NotesFile,
    [string]$Dotnet,
    [string]$PublicKey,
    [string]$PublisherDll,
    [string]$ProgramZip,
    # First-install archive of a shard release: uploaded for a brand-new installation, not named by the
    # signed catalog. Pass this instead of -ProgramZip when the catalog publishes shards.
    [string]$ManualInstallZip
)
$ErrorActionPreference = 'Stop'
if ($PSVersionTable.PSVersion.Major -lt 7) { throw 'PowerShell 7 or newer is required.' }
if ($ProgramZip -and $ManualInstallZip) { throw 'Pass -ProgramZip for a whole-archive program release, or -ManualInstallZip for the first-install archive of a shard release, not both.' }
$sourceRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'ResourceUpdateCatalog.ps1')
. (Join-Path $PSScriptRoot 'ResourceUpdateAssets.ps1')
if (-not $Dotnet) { $Dotnet = Join-Path $sourceRoot 'tools/dotnet-sdk-8.0.424/dotnet.exe' }
if (-not $PublicKey) { $PublicKey = Join-Path $sourceRoot 'Assets/Updates/trusted-keys.json' }
if (-not $PublisherDll) { $PublisherDll = Join-Path $sourceRoot 'tools/UpdatePublisher/bin/Release/net8.0/UpdatePublisher.dll' }
$PreparedRoot = [IO.Path]::GetFullPath($PreparedRoot)
$repo = 'kahvia-d/IMAO'
$report = Get-Content -LiteralPath (Join-Path $PreparedRoot 'release-report.json') -Raw | ConvertFrom-Json
if (-not $report.production -or -not $report.nativePassed) { throw 'Only production-signed, native-verified artifacts may be published.' }
if ($report.sourceDirty -ne $false -or $report.sourceTreeSha256 -notmatch '^[a-f0-9]{64}$') { throw 'A working-tree QA build cannot be published. Commit the reviewed source and rebuild from that exact clean commit.' }
$manifest = Join-Path $PreparedRoot 'update.json'
if ((Get-FileHash -LiteralPath $manifest -Algorithm SHA256).Hash -ne $report.signedManifestSha256) { throw 'Manifest differs from reviewed release report.' }
& $Dotnet $PublisherDll verify --input $PreparedRoot --public-key $PublicKey --snapshot-id $report.snapshotId
if ($LASTEXITCODE -ne 0) { throw 'Prepared artifacts failed verification.' }
function Invoke-Gh([string[]]$Arguments) {
    $result = & gh @Arguments
    if ($LASTEXITCODE -ne 0) { throw ('GitHub operation failed: gh ' + ($Arguments -join ' ') + '; stable channel has not been advanced by this failed operation.') }
    return $result
}
$tag = [string]$report.tag
$envelope = Get-Content -LiteralPath $manifest -Raw -Encoding UTF8 | ConvertFrom-Json
$catalog = [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String($envelope.payload)) | ConvertFrom-Json
$shardRelease = $null -ne $catalog.app.package -and @($catalog.app.package.shards).Count -gt 0
if ($catalog.app.url -eq "https://github.com/$repo/releases/tag/$tag" -and -not $ProgramZip -and -not $shardRelease) { throw 'A program release needs -ProgramZip with its verified archive report, or a signed shard program package.' }
$retainedProgramAssets = @()
$assets = [Collections.Generic.List[object]]::new()
foreach ($asset in $report.assets) {
    # Unchanged packages keep their old release URL; do not upload them into a new tag.
    if (([Uri]$asset.url).AbsolutePath -notlike "/$repo/releases/download/$tag/*") { continue }
    $assets.Add([pscustomobject]@{path=(Join-Path $PreparedRoot "packages/$($asset.name)");name=$asset.name;sha256=$asset.sha256})
}
$assets.Add([pscustomobject]@{path=$manifest;name='update.json';sha256=$report.signedManifestSha256})
if ($null -ne $report.offline) {
    $assets.Add([pscustomobject]@{path=(Join-Path $PreparedRoot $report.offline.name);name=$report.offline.name;sha256=$report.offline.sha256})
} else {
    Write-Host 'resource set unchanged: this release carries no offline archive (the last published one still matches the current resource set)'
}
if ($shardRelease) {
    if ($ProgramZip) { throw 'A shard program release ships no whole archive; do not pass -ProgramZip.' }
    if ($report.programPrepared) {
        # Each shard archive and the descriptor are bound to the identity the signed catalog names for them,
        # and only the ones this release owns are uploaded; an unchanged shard keeps its older release URL.
        $programAssets = Get-ProgramShardAssets $catalog $PreparedRoot $repo $tag
        foreach ($asset in $programAssets.Upload) { $assets.Add([pscustomobject]@{ path = $asset.path; name = $asset.name; sha256 = $asset.sha256 }) }
        $retainedProgramAssets = @($programAssets.Retained)
        Write-Host ("program release: {0} archive(s) to upload, {1} retained from earlier releases" -f $programAssets.Upload.Count, $programAssets.Retained.Count)
    } else {
        # This preparation only carried the published program forward; every archive stays where it is, but
        # clients still follow those URLs, so their reachability is checked below.
        $retainedProgramAssets = @(Get-RetainedProgramAssets $catalog $repo $tag)
        Write-Host ("program release carried forward: {0} archive(s) keep their published URL" -f $retainedProgramAssets.Count)
    }
    if ($report.programPrepared -and -not $ManualInstallZip) {
        Write-Host 'note: no -ManualInstallZip given, so this release carries no archive a brand-new installation can start from.' -ForegroundColor Yellow
    }
} elseif ($ProgramZip) {
    $programReport = Get-Content -LiteralPath ([IO.Path]::ChangeExtension($ProgramZip, '.report.json')) -Raw | ConvertFrom-Json
    if (-not $programReport.passed -or $programReport.sourceDirty -ne $false -or $programReport.sourceCommit -ne $report.sourceCommit -or $programReport.version -ne $report.appVersion) { throw 'Program archive lacks a matching clean-source package validation report.' }
    if ((Get-FileHash -LiteralPath $ProgramZip -Algorithm SHA256).Hash -ne $programReport.sha256) { throw 'Program archive changed after verification.' }
    if ([version]$programReport.version -ge [version]'2026.9.9.4' -and (-not $catalog.app.package -or
        $catalog.app.package.sha256 -ne $programReport.sha256 -or $catalog.app.package.size -ne (Get-Item -LiteralPath $ProgramZip).Length -or
        $catalog.app.package.sourceCommit -ne $programReport.sourceCommit -or
        $catalog.app.package.url -ne "https://github.com/$repo/releases/download/$tag/$([IO.Path]::GetFileName($ProgramZip))")) {
        throw 'Program archive is not bound to this release by the signed update catalog.'
    }
    $assets.Add([pscustomobject]@{path=[IO.Path]::GetFullPath($ProgramZip);name=[IO.Path]::GetFileName($ProgramZip);sha256=$programReport.sha256})
}
if ($ManualInstallZip) {
    $manual = Assert-ManualInstallArchive $ManualInstallZip $report
    $assets.Add([pscustomobject]@{ path = $manual.path; name = $manual.name; sha256 = $manual.sha256 })
    Write-Host ("first-install archive: {0} ({1:N1} MB)" -f $manual.name, ([IO.FileInfo]::new($manual.path).Length / 1MB))
}
foreach ($asset in $assets) {
    if ((Get-Item -LiteralPath $asset.path).Length -ge 2GB) { throw "GitHub release attachments must be smaller than 2 GiB: $($asset.name)" }
    if ((Get-FileHash -LiteralPath $asset.path -Algorithm SHA256).Hash -ne $asset.sha256) { throw "Asset changed after preparation: $($asset.name)" }
}
# The committed channel record is the durable floor for sequence numbers. The stable file alone is not
# enough: it can be reverted or rewritten, and clients keep the highest sequence they ever verified.
$channelOutput = & gh api "repos/$repo/contents/updates/channel-state.json?ref=main" 2>$null
$channelSha = $null
$publishedFloor = 0
if ($LASTEXITCODE -eq 0) {
    $channelFile = $channelOutput | ConvertFrom-Json
    $channelSha = $channelFile.sha
    $channelState = [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String(($channelFile.content -replace '\s',''))) | ConvertFrom-Json
    if ($null -ne $channelState.maxSequence) { $publishedFloor = [long]$channelState.maxSequence }
}
# Read stable before creating a release, and use its blob SHA as a compare-and-swap on promotion.
$stableOutput = & gh api "repos/$repo/contents/updates/stable.json?ref=main" 2>$null
$stableSha = $null
$verification = Join-Path $PreparedRoot ('publish-verification-' + [guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($verification) | Out-Null
if ($LASTEXITCODE -eq 0) {
    $old = $stableOutput | ConvertFrom-Json
    $stableSha = $old.sha
    $oldFile = Join-Path $verification 'previous-stable.json'
    [IO.File]::WriteAllBytes($oldFile, [Convert]::FromBase64String(($old.content -replace '\s','')))
    $oldCheck = & $Dotnet $PublisherDll verify-manifest --input $oldFile --public-key $PublicKey
    if ($LASTEXITCODE -ne 0) { throw 'Current stable signature could not be verified.' }
    $oldSequence = ($oldCheck | ConvertFrom-Json).sequence
    if ($oldSequence -gt $publishedFloor) { $publishedFloor = [long]$oldSequence }
    if ($oldSequence -gt $report.sequence) { throw 'Refusing to replace a newer stable channel.' }
    if ($oldSequence -eq $report.sequence) {
        if ((Get-FileHash -LiteralPath $oldFile -Algorithm SHA256).Hash -eq $report.signedManifestSha256) { Write-Host 'This exact update is already stable.'; exit 0 }
        throw 'Stable sequence already exists with different bytes.'
    }
    $oldEnvelope = Get-Content -LiteralPath $oldFile -Raw -Encoding UTF8 | ConvertFrom-Json
    $oldCatalog = [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String($oldEnvelope.payload)) | ConvertFrom-Json
    # Compare with the actual verified stable feed before creating/uploading/publishing anything.
    # URLs may change for identical bytes; package identity and its full inventory may not.
    Assert-ResourceCatalogTransition $oldCatalog $catalog
} else {
    # Distinguish an absent file from authentication/network errors by checking the parent branch.
    $tree = (Invoke-Gh @('api',"repos/$repo/git/trees/main?recursive=1")) | ConvertFrom-Json
    if ($tree.truncated -or @($tree.tree | Where-Object path -EQ 'updates/stable.json').Count) { throw 'Unable to safely determine current stable channel.' }
}
Assert-ChannelSequenceAdvance @{maxSequence = $publishedFloor} $catalog
$releaseOutput = & gh api "repos/$repo/releases/tags/$tag" 2>$null
if ($LASTEXITCODE -ne 0) {
    # Drafts without a created Git tag can be absent from the by-tag endpoint.
    # Discover and reuse them by ID instead of creating another draft on retry.
    $all = (Invoke-Gh @('api',"repos/$repo/releases?per_page=100")) | ConvertFrom-Json
    $matching = @($all | Where-Object tag_name -EQ $tag)
    if ($matching.Count -eq 0) {
        Invoke-Gh @('release','create',$tag,'--repo',$repo,'--target',[string]$report.sourceCommit,'--draft','--title',$tag,'--notes-file',[IO.Path]::GetFullPath($NotesFile)) | Out-Null
        # The releases list can lag a moment behind creation. Retry the lookup instead of failing a
        # promotion that has not touched a single attachment yet.
        for ($attempt = 0; $attempt -lt 5 -and $matching.Count -eq 0; $attempt++) {
            if ($attempt -gt 0) { Start-Sleep -Seconds 2 }
            $all = (Invoke-Gh @('api',"repos/$repo/releases?per_page=100")) | ConvertFrom-Json
            $matching = @($all | Where-Object tag_name -EQ $tag)
        }
    }
    if ($matching.Count -ne 1) { throw 'Cannot uniquely identify the release draft. No attachments were changed.' }
    $release = $matching[0]
} else {
    $release = $releaseOutput | ConvertFrom-Json
}
$release = (Invoke-Gh @('api',"repos/$repo/releases/$($release.id)")) | ConvertFrom-Json
if ($release.tag_name -ne $tag) { throw 'Release identity changed during lookup.' }
$tagCommit = [string](& gh api "repos/$repo/commits/$tag" --jq '.sha' 2>$null)
if ($LASTEXITCODE -eq 0) {
    if ($tagCommit.Trim() -ne $report.sourceCommit) { throw 'Release tag does not reference the reviewed source commit.' }
} elseif (-not $release.draft -or $release.target_commitish -ne $report.sourceCommit) {
    throw 'Cannot verify the reviewed source commit or pending draft tag target.'
}
# Confirm the remote bytes without downloading them. GitHub computes a SHA-256 for every stored
# asset and returns it as "digest" on the release API, so comparing that against the reviewed hash
# proves the server holds exactly the reviewed bytes while costing one API call instead of a full
# transfer. Downloading every attachment back used to cost as much traffic as the upload itself,
# which matters on a metered connection.
#
# The comparison is not weaker than hashing a re-download: the digest is what the service computed
# from what it actually stored, it is compared byte for byte, and it cannot be satisfied by a
# truncated or re-encoded upload. What it does not prove is that the asset is reachable and complete
# for an anonymous client, so the public check later still asks for each URL.
#
# An absent digest means an API that does not report one; that falls back to downloading rather than
# being skipped, because "could not check" must never read as "checked and fine".
function Assert-RemoteAssetBytes([object]$asset, [object[]]$remoteAssets) {
    $remote = @($remoteAssets | Where-Object name -EQ $asset.name)
    if ($remote.Count -eq 0) { throw "Remote release is missing an expected asset: $($asset.name)" }
    $stored = ([string]$remote[0].digest) -replace '^sha256:', ''
    if ($stored) {
        if ($stored -ne $asset.sha256) {
            throw "Remote asset bytes differ from the reviewed artifact: $($asset.name). Nothing was overwritten."
        }
        Write-Host ("  verified {0} by digest {1}" -f $asset.name, $stored.Substring(0, 12))
        return
    }
    Write-Host ("  {0}: no digest reported by the API; downloading to check" -f $asset.name) -ForegroundColor Yellow
    $checkRoot = Join-Path $verification ('nodigest-' + [guid]::NewGuid().ToString('N'))
    [IO.Directory]::CreateDirectory($checkRoot) | Out-Null
    Invoke-Gh @('release','download',$tag,'--repo',$repo,'--pattern',$asset.name,'--dir',$checkRoot) | Out-Null
    if ((Get-FileHash -LiteralPath (Join-Path $checkRoot $asset.name) -Algorithm SHA256).Hash -ne $asset.sha256) {
        throw "Remote asset bytes differ from the reviewed artifact: $($asset.name). Nothing was overwritten."
    }
}
$release = (Invoke-Gh @('api',"repos/$repo/releases/$($release.id)")) | ConvertFrom-Json
foreach ($asset in $assets) {
    $existing = @($release.assets | Where-Object name -EQ $asset.name)
    if ($existing.Count) {
        # Same name and same bytes is a retry and is reused; same name with different bytes is a
        # conflict and is never overwritten.
        Assert-RemoteAssetBytes $asset $release.assets
    } else {
        if (-not $release.draft) { throw "Published release is missing an expected asset: $($asset.name). Do not modify an immutable release." }
        Invoke-Gh @('release','upload',$tag,$asset.path,'--repo',$repo) | Out-Null
    }
}
# Read the release back once: a successful upload command is not proof of the stored bytes, and the
# digest is the service's own statement about them.
$release = (Invoke-Gh @('api',"repos/$repo/releases/$($release.id)")) | ConvertFrom-Json
$missing = @($assets | Where-Object { $name = $_.name; -not ($release.assets | Where-Object name -EQ $name) })
if ($missing.Count) { throw ('Upload did not produce every expected asset: ' + (($missing | ForEach-Object name) -join ', ')) }
foreach ($asset in $assets) { Assert-RemoteAssetBytes $asset $release.assets }
if ($release.draft) { Invoke-Gh @('release','edit',$tag,'--repo',$repo,'--draft=false','--latest=false') | Out-Null }
$publishedCommit = [string](Invoke-Gh @('api',"repos/$repo/commits/$tag",'--jq','.sha'))
if ($publishedCommit.Trim() -ne $report.sourceCommit) { throw 'Published tag does not match the reviewed source commit. Stable channel remains unchanged.' }
# Ask each published URL for its headers instead of its body. The bytes were already confirmed against
# the reviewed hashes by digest, so what is left to establish is that an anonymous client - which is
# what every installed copy is - can reach this exact URL and that the asset is there. A HEAD request
# answers that without transferring the release a second time, and it exercises the redirect from the
# release page to the asset host that the client depends on.
$handler = [Net.Http.SocketsHttpHandler]::new(); $handler.ConnectTimeout = [TimeSpan]::FromSeconds(20)
$handler.AllowAutoRedirect = $true
$client = [Net.Http.HttpClient]::new($handler); $client.Timeout = [TimeSpan]::FromMinutes(5)
try {
    $publicChecks = @($assets | ForEach-Object { [pscustomobject]@{name=$_.name;url="https://github.com/$repo/releases/download/$tag/$([Uri]::EscapeDataString($_.name))"} })
    # Unchanged packages keep their previous release URL, so their reachability has to be asked at that
    # URL rather than at this tag.
    $publicChecks += @($report.assets | Where-Object { ([Uri]$_.url).AbsolutePath -notlike "/$repo/releases/download/$tag/*" } |
        ForEach-Object { [pscustomobject]@{name=$_.name;url=[string]$_.url} })
    # Program archives retained from an earlier release answer at their own URL, never at this tag.
    $publicChecks += @($retainedProgramAssets | ForEach-Object { [pscustomobject]@{name=$_.name;url=[string]$_.url} })
    foreach ($asset in $publicChecks) {
        $request = [Net.Http.HttpRequestMessage]::new([Net.Http.HttpMethod]::Head, [string]$asset.url)
        $response = $client.SendAsync($request).GetAwaiter().GetResult()
        try {
            if (-not $response.IsSuccessStatusCode) {
                throw "Public asset is not reachable: $($asset.name) answered $([int]$response.StatusCode)"
            }
        } finally { $response.Dispose(); $request.Dispose() }
    }
    Write-Host ("public reachability confirmed for {0} assets by HEAD" -f $publicChecks.Count)
} finally { $client.Dispose(); $handler.Dispose() }
# Burn the sequence number before the stable channel moves, so a failed promotion can never free the
# number for reuse. After a failed promotion the reviewed artifacts must be re-prepared with a higher
# --sequence instead of retrying the same number.
$channelPromotion = [ordered]@{message="Record published sequence $($report.sequence)";branch='main';content=[Convert]::ToBase64String([Text.UTF8Encoding]::new($false).GetBytes(([ordered]@{maxSequence=[long]$report.sequence} | ConvertTo-Json)))}
if ($channelSha) { $channelPromotion.sha = $channelSha }
$channelPromotionFile = Join-Path $verification 'channel-state-promotion.json'
[IO.File]::WriteAllText($channelPromotionFile,($channelPromotion | ConvertTo-Json -Depth 5),[Text.UTF8Encoding]::new($false))
Invoke-Gh @('api',"repos/$repo/contents/updates/channel-state.json",'--method','PUT','--input',$channelPromotionFile) | Out-Null
$channelAfter = (Invoke-Gh @('api',"repos/$repo/contents/updates/channel-state.json?ref=main")) | ConvertFrom-Json
$channelAfterState = [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String(($channelAfter.content -replace '\s',''))) | ConvertFrom-Json
if ([long]$channelAfterState.maxSequence -ne [long]$report.sequence) { throw 'Channel sequence record verification failed. Stable channel remains unchanged.' }
$promotion = [ordered]@{message="Publish verified resource update $($report.snapshotId)";branch='main';content=[Convert]::ToBase64String([IO.File]::ReadAllBytes($manifest))}
if ($stableSha) { $promotion.sha = $stableSha }
$promotionFile = Join-Path $verification 'stable-promotion.json'
[IO.File]::WriteAllText($promotionFile,($promotion | ConvertTo-Json -Depth 5),[Text.UTF8Encoding]::new($false))
Invoke-Gh @('api',"repos/$repo/contents/updates/stable.json",'--method','PUT','--input',$promotionFile) | Out-Null
$after = (Invoke-Gh @('api',"repos/$repo/contents/updates/stable.json?ref=main")) | ConvertFrom-Json
$remoteBytes = [Convert]::FromBase64String(($after.content -replace '\s',''))
if ([Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($remoteBytes)) -ne $report.signedManifestSha256) { throw 'Stable channel verification failed after promotion.' }
Write-Host "Verified release https://github.com/$repo/releases/tag/$tag and promoted stable sequence $($report.sequence)."
