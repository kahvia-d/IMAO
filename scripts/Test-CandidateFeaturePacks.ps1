[CmdletBinding()]
param(
    [string]$FeatureDataRoot = (Join-Path (Split-Path -Parent $PSScriptRoot) 'Assets\FeaturesDatas')
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$registryPath = Join-Path $FeatureDataRoot 'candidate-packs.json'
if (-not (Test-Path -LiteralPath $registryPath)) { throw "Missing candidate registry: $registryPath" }
$registry = Get-Content -LiteralPath $registryPath -Raw | ConvertFrom-Json
if ([int]$registry.formatVersion -ne 1 -or $null -eq $registry.packs -or @($registry.packs).Count -eq 0) {
    throw 'Candidate registry format is invalid or contains no packs.'
}

Add-Type -AssemblyName System.Drawing
$directories = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
foreach ($candidateDirectoryValue in @($registry.packs)) {
    $candidateDirectory = [string]$candidateDirectoryValue
    if ([string]::IsNullOrWhiteSpace($candidateDirectory) -or
        [IO.Path]::GetFileName($candidateDirectory) -ne $candidateDirectory -or
        -not $directories.Add($candidateDirectory)) {
        throw "Candidate registry contains an invalid or duplicate pack directory: $candidateDirectory"
    }

    $packDirectory = Join-Path $FeatureDataRoot $candidateDirectory
    $manifestPath = Join-Path $packDirectory 'manifest.json'
    $shardPath = Join-Path $packDirectory 'visual-index.imx'
    if (-not (Test-Path -LiteralPath $manifestPath)) { throw "Missing candidate manifest: $candidateDirectory" }
    if (-not (Test-Path -LiteralPath $shardPath)) { throw "Missing candidate visual index: $candidateDirectory" }
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    $supportedScenes = @('World', 'Tethys', 'Fabricatorium', 'Avinoleum', 'Lahai', 'LowerVault', 'Darkplain', 'TimeRiftRuins')
    if ([int]$manifest.formatVersion -notin 1, 2 -or $supportedScenes -notcontains [string]$manifest.scene -or
        [string]::IsNullOrWhiteSpace([string]$manifest.packId)) {
        throw "Candidate manifest is invalid: $candidateDirectory"
    }
    $references = if ([int]$manifest.formatVersion -eq 1) {
        @([pscustomobject]@{
            anchorWorldCoordinate = $manifest.anchorWorldCoordinate
            reference = $manifest.reference
            mask = $manifest.mask
        })
    } else {
        @($manifest.references)
    }
    if ($references.Count -eq 0) { throw "Candidate pack has no references: $candidateDirectory" }

    $anchors = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    foreach ($entry in $references) {
        $anchorX = [double]$entry.anchorWorldCoordinate.x
        $anchorY = [double]$entry.anchorWorldCoordinate.y
        if ([double]::IsNaN($anchorX) -or [double]::IsInfinity($anchorX) -or
            [double]::IsNaN($anchorY) -or [double]::IsInfinity($anchorY) -or
            -not $anchors.Add("$anchorX,$anchorY")) {
            throw "Candidate pack has an invalid or duplicate anchor: $candidateDirectory"
        }
        $imageName = [string]$entry.reference.image
        if ([string]::IsNullOrWhiteSpace($imageName) -or [IO.Path]::GetFileName($imageName) -ne $imageName) {
            throw "Candidate reference image name is unsafe: $candidateDirectory"
        }
        $imagePath = Join-Path $packDirectory $imageName
        if (-not (Test-Path -LiteralPath $imagePath)) { throw "Candidate reference is missing: $imagePath" }
        $actualHash = (Get-FileHash -LiteralPath $imagePath -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($actualHash -ne ([string]$entry.reference.sha256).ToLowerInvariant()) {
            throw "Candidate reference SHA-256 mismatch: $candidateDirectory/$imageName"
        }
        $image = [Drawing.Image]::FromFile($imagePath)
        try {
            if ($image.Width -ne [int]$entry.reference.width -or $image.Height -ne [int]$entry.reference.height) {
                throw "Candidate reference dimensions mismatch: $candidateDirectory/$imageName"
            }
        }
        finally {
            $image.Dispose()
        }
        $mask = if ($null -ne $entry.PSObject.Properties['mask']) { $entry.mask } else { $manifest.mask }
        if ([int]$mask.innerRadius -le 0 -or [int]$mask.outerRadius -le [int]$mask.innerRadius -or
            [int]$mask.outerRadius -gt [int]($entry.reference.width / 2)) {
            throw "Candidate mask is invalid: $candidateDirectory/$imageName"
        }
    }
    Write-Host "Candidate pack passed: $($manifest.packId), references=$($references.Count)."
}
