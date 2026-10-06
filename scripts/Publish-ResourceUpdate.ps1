# Requires PowerShell 7 and authenticated GitHub CLI. This script is the sole remote mutation entrypoint.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$PreparedRoot,
    [string]$NotesFile,
    [ValidateSet('reserve','register-request','publish','abandon')][string]$Phase = 'publish',
    [bool]$Publish = $false,
    [string]$ConfirmVersion,
    [string]$RequestRoot,
    [string]$ResponseFile,
    [long]$ArtifactId,
    [long]$BuildRunId,
    [string]$ArtifactDigest,
    [long]$RequestArtifactId,
    [string]$RequestArtifactDigest,
    [string]$TransactionId,
    [switch]$SkipGitee,
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
. (Join-Path $PSScriptRoot 'ReleaseTransactions.ps1')
if (-not $Dotnet) { $Dotnet = Join-Path $sourceRoot 'tools/dotnet-sdk-8.0.424/dotnet.exe' }
if (-not $PublicKey) { $PublicKey = Join-Path $sourceRoot 'Assets/Updates/trusted-keys.json' }
if (-not $PublisherDll) { $PublisherDll = Join-Path $sourceRoot 'tools/UpdatePublisher/bin/Release/net8.0/UpdatePublisher.dll' }
$PreparedRoot = [IO.Path]::GetFullPath($PreparedRoot)
$repo = 'kahvia-d/IMAO'
function Invoke-Gh([string[]]$Arguments) {
    $result = & gh @Arguments
    if ($LASTEXITCODE -ne 0) { throw ('GitHub operation failed: gh ' + ($Arguments -join ' ') + '; stable channel has not been advanced by this failed operation.') }
    return $result
}
if (-not $Publish) { throw 'Remote release mutation requires explicit -Publish $true. Use the build workflow for a side-effect-free rehearsal.' }
[xml]$versionProps = Get-Content -LiteralPath (Join-Path $sourceRoot 'Version.props') -Raw
if ($Phase -ne 'abandon' -and $ConfirmVersion -cne [string]$versionProps.Project.PropertyGroup.IMaoVersion) { throw 'confirm_version must exactly match Version.props.' }
if ($env:GITHUB_ACTIONS -eq 'true' -and $env:GITHUB_REF -ne 'refs/heads/main') { throw 'Production workflows must run from main.' }
$scratch = Join-Path $PreparedRoot ('transaction-' + [guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($scratch) | Out-Null
$snapshot = Get-ReleaseSnapshot $repo
if ($Phase -eq 'abandon') {
    if ($snapshot.state.active.id -cne $TransactionId -or $snapshot.state.active.version -cne $ConfirmVersion) { throw 'Explicit transaction identity required.' }
    if ($snapshot.state.active.status -ne 'awaiting-signature' -or $snapshot.state.active.releaseId) { throw 'Publishing has begun; resume instead of abandoning.' }
    $snapshot.state.completed += @(@{id=$TransactionId;sequence=$snapshot.state.active.sequence;status='abandoned'})
    $snapshot.state.active=$null
    Save-ReleaseState $repo $snapshot $scratch "Abandon release reservation $TransactionId"
    exit 0
}
if ($Phase -eq 'register-request') {
    $index = Get-Content (Join-Path $RequestRoot 'request.json') -Raw | ConvertFrom-Json
    if ($snapshot.state.active.id -cne $TransactionId -or $snapshot.state.active.version -cne $ConfirmVersion -or $RequestArtifactId -le 0 -or $RequestArtifactDigest -notmatch '^(sha256:)?[a-f0-9]{64}$') { throw 'Request registration does not match the reservation.' }
    if ($snapshot.state.active.requestId -and $snapshot.state.active.requestId -cne $index.requestId) { throw 'Reservation already has a different signing request.' }
    $snapshot.state.active.requestId = $index.requestId
    $snapshot.state.active.requestArtifactId = $RequestArtifactId
    $snapshot.state.active.requestArtifactDigest = $RequestArtifactDigest -replace '^sha256:', ''
    Save-ReleaseState $repo $snapshot $scratch "Register signing request $TransactionId"
    exit 0
}
$report = Get-Content -LiteralPath (Join-Path $PreparedRoot 'release-report.json') -Raw | ConvertFrom-Json
if (-not $report.production -or -not $report.nativePassed) { throw 'Only production-signed, native-verified artifacts may be published.' }
if ($report.sourceDirty -ne $false -or $report.sourceTreeSha256 -notmatch '^[a-f0-9]{64}$') { throw 'A working-tree QA build cannot be published. Commit the reviewed source and rebuild from that exact clean commit.' }
if ($report.appVersion -cne $ConfirmVersion) { throw 'Prepared version differs from confirm_version.' }
if ($Phase -eq 'reserve') {
    if (-not $RequestRoot -or $ArtifactId -le 0 -or $BuildRunId -le 0 -or $ArtifactDigest -notmatch '^(sha256:)?[a-f0-9]{64}$') { throw 'Frozen build artifact and new request destination required.' }
    $ArtifactDigest = $ArtifactDigest -replace '^sha256:', ''
    $stableFile = Join-Path $scratch 'previous-stable.json'
    [IO.File]::WriteAllBytes($stableFile, $snapshot.files['updates/stable.json'])
    $verified = & $Dotnet $PublisherDll verify-manifest --input $stableFile --public-key $PublicKey
    if ($LASTEXITCODE -ne 0) { throw 'Current stable signature cannot be verified.' }
    $stableSequence = [long]($verified | ConvertFrom-Json).sequence
    if ($report.previousStableSha256 -cne $snapshot.stableHash) { throw 'Stable changed after cloud build; rebuild from the new baseline.' }
    if ($snapshot.state.active) {
        $active=$snapshot.state.active
        if ($active.artifactId -ne $ArtifactId -or $active.artifactDigest -cne $ArtifactDigest -or $active.buildRunId -ne $BuildRunId -or $active.sourceCommit -cne $report.sourceCommit -or $active.version -cne $ConfirmVersion) { throw 'Another transaction is waiting. Resume or abandon it.' }
        if ($active.previousStableSha256 -cne $snapshot.stableHash -or $active.previousChannelSequence -ne $snapshot.channel.maxSequence) { throw 'Reserved channel baseline changed.' }
    } else {
        $sequence = Get-NextReleaseSequence $snapshot.state $stableSequence ([long]$snapshot.channel.maxSequence)
        $snapshot.state.highestAllocatedSequence = $sequence
        $snapshot.state.active = @{id=[guid]::NewGuid().ToString('N');sequence=$sequence;version=$ConfirmVersion;tag=$report.tag;
            sourceCommit=$report.sourceCommit;artifactId=$ArtifactId;artifactDigest=$ArtifactDigest;buildRunId=$BuildRunId;
            previousStableSha256=$snapshot.stableHash;previousChannelSequence=[long]$snapshot.channel.maxSequence;status='awaiting-signature';releaseId=$null}
        Save-ReleaseState $repo $snapshot $scratch "Reserve release sequence $sequence"
    }
    $active=$snapshot.state.active
    & $Dotnet $PublisherDll create-signing-request --input $PreparedRoot --output $RequestRoot --previous $stableFile --public-key $PublicKey --sequence $active.sequence --transaction-id $active.id --build-run-id $BuildRunId --artifact-id $ArtifactId --artifact-digest $ArtifactDigest
    if ($LASTEXITCODE -ne 0) { throw 'Signing request preparation failed; reservation remains resumable.' }
    if ($env:GITHUB_OUTPUT) { "transaction_id=$($active.id)" >> $env:GITHUB_OUTPUT }
    exit 0
}
if (-not $NotesFile -or -not $RequestRoot -or -not $ResponseFile) { throw 'Publishing requires notes, original request and original signature response.' }
$approval=Get-Content (Join-Path $RequestRoot 'approval-payload.json') -Raw | ConvertFrom-Json
& $Dotnet $PublisherDll verify-authorization --input $PreparedRoot --request $RequestRoot --response $ResponseFile --public-key $PublicKey --expected-source-commit $report.sourceCommit --confirm-version $ConfirmVersion
if ($LASTEXITCODE -ne 0) { throw 'Detached production release authorization failed.' }
if (-not $snapshot.state.active) {
    $done=@($snapshot.state.completed | Where-Object { $_.id -ceq $approval.transactionId -and $_.status -ceq 'completed' -and $_.manifestSha256 -ceq $report.signedManifestSha256 })
    if ($done.Count -eq 1 -and $snapshot.stableHash -ceq $report.signedManifestSha256 -and $snapshot.channel.maxSequence -eq $report.sequence) { Write-Host 'This exact transaction is already stable.'; exit 0 }
}
Assert-ReleaseTransaction $snapshot.state $approval $snapshot.stableHash ([long]$snapshot.channel.maxSequence)

$report=Get-Content (Join-Path $PreparedRoot 'release-report.json') -Raw | ConvertFrom-Json
$requestIndex=Get-Content (Join-Path $RequestRoot 'request.json') -Raw | ConvertFrom-Json
if ($snapshot.state.active.requestId -cne $requestIndex.requestId) { throw 'Signing request was not registered in the durable transaction.' }
$responseHash=(Get-FileHash $ResponseFile -Algorithm SHA256).Hash.ToLowerInvariant()
if ($snapshot.state.active.responseSha256 -and $snapshot.state.active.responseSha256 -cne $responseHash) { throw 'Use the original response when resuming; do not re-sign.' }
$snapshot.state.active.responseSha256=$responseHash
$snapshot.state.active.response=[Text.Encoding]::UTF8.GetString([IO.File]::ReadAllBytes($ResponseFile))
$snapshot.state.active.status='publishing'
Save-ReleaseState $repo $snapshot $scratch "Accept detached signature for $($approval.transactionId)"
$manifest = Join-Path $PreparedRoot 'update.json'
if ((Get-FileHash -LiteralPath $manifest -Algorithm SHA256).Hash -ne $report.signedManifestSha256) { throw 'Manifest differs from reviewed release report.' }
& $Dotnet $PublisherDll verify --input $PreparedRoot --public-key $PublicKey --snapshot-id $report.snapshotId
if ($LASTEXITCODE -ne 0) { throw 'Prepared artifacts failed verification.' }
$tag = [string]$report.tag
$signedCatalogBytes=[Convert]::FromBase64String((Get-Content (Join-Path $PreparedRoot 'update.json') -Raw | ConvertFrom-Json).payload)
$signedNotes=([Text.Encoding]::UTF8.GetString($signedCatalogBytes) | ConvertFrom-Json).app.notes
$NotesFile=Join-Path $scratch 'authorized-notes.md'
[IO.File]::WriteAllText($NotesFile,$signedNotes,[Text.UTF8Encoding]::new($false))
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
$approvedInventory=Get-Content (Join-Path $RequestRoot 'asset-inventory.json') -Raw | ConvertFrom-Json
foreach ($audit in @($approvedInventory | Where-Object { $_.path -like 'manual/*.report.json' })) {
    $assets.Add([pscustomobject]@{path=(Join-Path $PreparedRoot $audit.path);name=$audit.name;sha256=$audit.sha256})
}
$authorizationFile=Join-Path $PreparedRoot 'release-authorization.json'
$assets.Add([pscustomobject]@{path=$authorizationFile;name='release-authorization.json';sha256=(Get-FileHash $authorizationFile -Algorithm SHA256).Hash.ToLowerInvariant()})
foreach ($name in @('release-signing-request.zip','release-report.json')) {
    $path=Join-Path $PreparedRoot $name
    $assets.Add([pscustomobject]@{path=$path;name=$name;sha256=(Get-FileHash $path -Algorithm SHA256).Hash.ToLowerInvariant()})
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
$immutability=Invoke-Gh @('api',"repos/$repo/immutable-releases") | ConvertFrom-Json
if (-not $immutability.enabled) { throw 'Enable repository release immutability before uploading a formal release.' }
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
$snapshot=Get-ReleaseSnapshot $repo
Assert-ReleaseTransaction $snapshot.state $approval $snapshot.stableHash ([long]$snapshot.channel.maxSequence)
if ($snapshot.state.active.releaseId -and $snapshot.state.active.releaseId -ne $release.id) { throw 'Reserved release ID differs; never create a replacement release.' }
$snapshot.state.active.releaseId=$release.id
Save-ReleaseState $repo $snapshot $scratch "Record release ID $($release.id)"

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
# This release is the one a new player should land on, so it claims the Latest badge. It used to pass
# --latest=false, which left /releases/latest pointing at an older version until someone edited it by
# hand; the badge was on v2026.9.25.1 while 9.26.1 through 9.26.3 shipped. The release always carries a
# first-install archive (-ManualInstallZip is mandatory for a shard release), so pointing new players here
# is safe. The browser-extension releases keep their own --latest=false in Publish-BrowserExtension.ps1,
# which is a different reason: an ext-* release must never be what releases/latest opens.
if ($release.draft) { Invoke-Gh @('release','edit',$tag,'--repo',$repo,'--draft=false','--latest=true') | Out-Null }
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
# The reservation already burned the sequence. Promotion writes stable, floor and completed transaction together.
$promoted=$false
for ($retry=0; $retry -lt 5 -and -not $promoted; $retry++) {
    $snapshot=Get-ReleaseSnapshot $repo
    if (-not $snapshot.state.active -and $snapshot.stableHash -eq $report.signedManifestSha256 -and $snapshot.channel.maxSequence -eq $report.sequence) { $promoted=$true; break }
    Assert-ReleaseTransaction $snapshot.state $approval $snapshot.stableHash ([long]$snapshot.channel.maxSequence)
    $completed=@{id=$approval.transactionId;sequence=[long]$report.sequence;status='completed';releaseId=$release.id;requestId=$requestIndex.requestId;manifestSha256=$report.signedManifestSha256;version=$approval.version;tag=$approval.tag;sourceCommit=$approval.sourceCommit;artifactId=$approval.artifactId;artifactDigest=$approval.artifactDigest;buildRunId=$approval.buildRunId;requestArtifactId=$snapshot.state.active.requestArtifactId;requestArtifactDigest=$snapshot.state.active.requestArtifactDigest;responseSha256=$snapshot.state.active.responseSha256}
    $snapshot.state.completed += @($completed)
    $snapshot.state.active=$null
    $files=@{'updates/stable.json'=[IO.File]::ReadAllBytes($manifest);'updates/channel-state.json'=(ConvertTo-ReleaseBytes @{maxSequence=[long]$report.sequence});'updates/release-state.json'=(ConvertTo-ReleaseBytes $snapshot.state)}
    try { Write-ReleaseCommit $repo $snapshot $files "Publish verified stable sequence $($report.sequence)" $scratch | Out-Null } catch { if ($retry -eq 4) { throw }; continue }
    $after=Get-ReleaseSnapshot $repo
    $promoted=(-not $after.state.active -and $after.stableHash -eq $report.signedManifestSha256 -and $after.channel.maxSequence -eq $report.sequence)
}
if (-not $promoted) { throw 'Atomic promotion not confirmed; resume the original transaction.' }
Write-Host "Verified release https://github.com/$repo/releases/tag/$tag and promoted stable sequence $($report.sequence)."

# ---------------------------------------------------------------- Gitee mirror of the stable channel
# See scripts/Set-GiteeMirror.ps1 for what the mirror is and why it is never authority. It runs strictly
# after the canonical channel advanced, so the mirror can never lead it, and a failure is reported as a
# warning: GitHub is already live, and a release must not fail because a convenience copy did not.
if (-not $SkipGitee) { try {
    & (Join-Path $PSScriptRoot 'Set-GiteeMirror.ps1') -Manifest $manifest -ExpectedSha256 $report.signedManifestSha256
} catch {
    Write-Warning "Gitee mirror update failed and does not affect this release: $($_.Exception.Message)"
}

}
