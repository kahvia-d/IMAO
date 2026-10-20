$ErrorActionPreference = 'Stop'
$helper = Join-Path $PSScriptRoot 'MapTestPackageIdentity.ps1'
. $helper
$files = @([pscustomobject]@{ path = 'features.imf'; size = 12; sha256 = ('c' * 64) })
$package = [pscustomobject]@{ id = 'region-kurotiles'; version = '2026.10.8.1'; kind = 'tile' }
$canonical = [pscustomobject]@{ id = $package.id; kind = 'tile'; version = '2026.10.8.1'; sha256 = ('a' * 64); files = $files }
$identity = Resolve-MapTestPackageIdentity $package $canonical $files
if ($identity.version -cne $canonical.version -or $identity.sha256 -cne $canonical.sha256) {
    throw 'Unchanged test resources must retain published package version and archive hash, not the app version or manifest hash.'
}
$changed = @([pscustomobject]@{ path = 'features.imf'; size = 12; sha256 = ('d' * 64) })
$identity = Resolve-MapTestPackageIdentity $package $canonical $changed
if (-not $identity.customized -or $identity.sha256 -ne '' -or $identity.files[0].sha256 -cne ('d' * 64)) {
    throw 'Customized test resources must be explicit local bundles with actual file hashes; never claim a signed archive hash.'
}
if ($identity.files -isnot [array]) { throw 'Even a one-file package must serialize its inventory as an array.' }
$older = [pscustomobject]@{ id = $package.id; kind = $package.kind; version = '2026.9.1.1' }
$identity = Resolve-MapTestPackageIdentity $older $canonical $changed
if ($identity.version -cne $older.version) { throw 'Different old test bytes must never be relabeled with the newest published version.' }
'PASS maptest package identity: exact publication identity for unchanged resources; explicit local identity for custom resources.'
