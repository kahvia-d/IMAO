[CmdletBinding(DefaultParameterSetName = 'Check')]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('Tethys', 'Fabricatorium', 'Avinoleum', 'Lahai', 'LowerVault', 'Darkplain', 'TimeRiftRuins')]
    [string]$Scene,
    [Parameter(Mandatory = $true)]
    [string]$SamplesPath,
    [Parameter(ParameterSetName = 'Check')]
    [switch]$Check,
    [Parameter(Mandatory = $true, ParameterSetName = 'Apply')]
    [switch]$Apply
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$stateIds = @{ Tethys = 900; Fabricatorium = 905; Avinoleum = 903; Lahai = 906; LowerVault = 902; Darkplain = 909; TimeRiftRuins = 910 }
$calibrationPath = Join-Path $repoRoot 'Assets\KuroMap\scene-calibrations.json'

function Require-FiniteNumber([object]$Value, [string]$Name) {
    if ($Value -isnot [ValueType]) { throw "$Name must be numeric." }
    $number = [double]$Value
    if ([double]::IsNaN($number) -or [double]::IsInfinity($number)) { throw "$Name must be finite." }
    return $number
}

function Write-Utf8Json([object]$Value, [string]$Path) {
    [IO.File]::WriteAllText($Path, (($Value | ConvertTo-Json -Depth 32) + [Environment]::NewLine), [Text.UTF8Encoding]::new($false))
}

if (-not (Test-Path -LiteralPath $SamplesPath)) { throw "Calibration sample file is missing: $SamplesPath" }
$sampleDocument = Get-Content -LiteralPath $SamplesPath -Raw | ConvertFrom-Json
if ($sampleDocument.formatVersion -ne 1 -or [string]$sampleDocument.scene -ne $Scene -or [int]$sampleDocument.state -ne $stateIds[$Scene]) {
    throw 'Calibration sample format, scene, or Kuro state is invalid.'
}
$samples = @($sampleDocument.samples)
if ($samples.Count -ne 4) { throw "Exactly four calibration samples are required; received $($samples.Count)." }

$points = [Collections.Generic.List[object]]::new()
foreach ($sample in $samples) {
    if ($null -eq $sample.game -or $null -eq $sample.map) { throw 'Each sample needs game{x,y} and map{x,y}.' }
    $points.Add([pscustomobject]@{
        GameX = Require-FiniteNumber $sample.game.x 'game.x'
        GameY = Require-FiniteNumber $sample.game.y 'game.y'
        MapX = Require-FiniteNumber $sample.map.x 'map.x'
        MapY = Require-FiniteNumber $sample.map.y 'map.y'
        Capture = [string]$sample.capture
    })
}

# Both axes share one scale.  The Y sign follows the existing ROC convention:
# mapX = scale * gameX + originX; mapY = scale * gameY + originY.
$meanGameX = ($points | Measure-Object -Property GameX -Average).Average
$meanGameY = ($points | Measure-Object -Property GameY -Average).Average
$meanMapX = ($points | Measure-Object -Property MapX -Average).Average
$meanMapY = ($points | Measure-Object -Property MapY -Average).Average
$denominator = 0.0
$numerator = 0.0
foreach ($point in $points) {
    $deltaGameX = $point.GameX - $meanGameX
    $deltaGameY = $point.GameY - $meanGameY
    $denominator += $deltaGameX * $deltaGameX + $deltaGameY * $deltaGameY
    $numerator += $deltaGameX * ($point.MapX - $meanMapX) + $deltaGameY * ($point.MapY - $meanMapY)
}
if ($denominator -le 0.0) { throw 'The four samples do not span a usable area.' }
$scale = $numerator / $denominator
if ([double]::IsNaN($scale) -or [double]::IsInfinity($scale) -or $scale -le 0.0) { throw 'The fitted map scale is invalid.' }
$originX = $meanMapX - $scale * $meanGameX
$originY = $meanMapY - $scale * $meanGameY

$errors = [Collections.Generic.List[double]]::new()
foreach ($point in $points) {
    $expectedX = $scale * $point.GameX + $originX
    $expectedY = $scale * $point.GameY + $originY
    $errors.Add([Math]::Sqrt(($expectedX - $point.MapX) * ($expectedX - $point.MapX) + ($expectedY - $point.MapY) * ($expectedY - $point.MapY)))
}
$maxError = ($errors | Measure-Object -Maximum).Maximum
$rmsError = [Math]::Sqrt((($errors | ForEach-Object { $_ * $_ } | Measure-Object -Sum).Sum) / $errors.Count)
if ($maxError -gt 8.0) { throw "Calibration for $Scene is rejected: maximum error $([Math]::Round($maxError, 3)) exceeds 8 internal map pixels." }

$record = [ordered]@{
    scene = $Scene; state = $stateIds[$Scene]; sampleCount = $points.Count; passed = $true
    coordinateTransform = [ordered]@{ originX = $originX; originY = $originY; scale = $scale }
    maxErrorPixels = $maxError; rmsErrorPixels = $rmsError
    samples = @($points | ForEach-Object { [ordered]@{ game = [ordered]@{ x = $_.GameX; y = $_.GameY }; map = [ordered]@{ x = $_.MapX; y = $_.MapY }; capture = $_.Capture } })
}
Write-Host "Calibration $($PSCmdlet.ParameterSetName): scene=$Scene scale=$([Math]::Round($scale, 8)) origin=$([Math]::Round($originX, 3)),$([Math]::Round($originY, 3)) maxError=$([Math]::Round($maxError, 3))px"

if ($Apply) {
    $document = if (Test-Path -LiteralPath $calibrationPath) { Get-Content -LiteralPath $calibrationPath -Raw | ConvertFrom-Json -AsHashtable } else { [ordered]@{ formatVersion = 1; scenes = [ordered]@{} } }
    if ($document['formatVersion'] -ne 1 -or $null -eq $document['scenes']) { throw 'Existing calibration registry format is invalid.' }
    $document['scenes'][$Scene] = $record
    Write-Utf8Json $document $calibrationPath
    Write-Host "Applied calibration for $Scene. This does not release the scene to users." -ForegroundColor Green
}
