# Shared resolver for "which coordinate frame does this frame's map coordinate live in?".
#
# Why this exists
# ---------------
# Every map coordinate the tool carries (PlayerImgMapCoordinate, a marker position, the frame a
# layered floor's occupancy grid is expressed in) is `game * scale + origin`, and the origin is
# PER FRAME: World is (2474,1957) @1.205, 隐海试验场 (Fabricatorium, frame 905) is (7437,13783),
# 下层金库 (LowerVault, 902) is (-3.5,-2.5) @1.2053.
#
# The compiled table in IMao-Core/src/Coordinate/CoordinateStruct.h is what the runtime itself
# uses, refined by Assets/KuroMap/scene-calibrations.json under exactly the gate
# Scene::LoadExternalConfig applies (passed == true AND maxErrorPixels <= 8).
#
# Getting this wrong is silent. The numbers are plausible either way, and the only symptom is
# `containing=[]` / `winnerContained=0` in the runtime log - forever, because a footprint that is
# never tested against the player's real position can never contain them. The shipped 隐海试验场
# index was built with World's origin instead of its own: 4963 px in x and 11826 in y, about eleven
# tiles. The floor was identified correctly on every frame (ownShare 0.33-0.89) and could never be
# adopted (events-20260926.jsonl 21:31-21:34).
#
# Two things keep that from happening again, because a resolver cannot police a caller that ignores
# it: this file is now the only place the transform is derived (New-LayeredFloorIndex.ps1 uses it
# rather than hardcoding World), and scripts/Test-KuroMapFeaturePack.ps1 checks every shipped
# layered-floor index against it. What this file still refuses on its own is a frame whose origin is
# not actually known yet - see the end of Get-SceneCoordinateTransform.
#
# Dot-source this file:
#   . (Join-Path $PSScriptRoot 'SceneCoordinateTransform.ps1')

$script:RuntimeSceneHeaderRelative = 'IMao-Core/src/Coordinate/CoordinateStruct.h'
$script:SceneCalibrationRelative = 'Assets/KuroMap/scene-calibrations.json'
$script:SceneCalibrationMaxErrorPixels = 8.0

# The compiled scene table, keyed by the Kuro frame (state) the scene's map lives in. Parsed rather
# than restated so that a new subworld is added in one place - the same regex
# scripts/New-MapRegionRegistry.ps1 uses.
function Read-RuntimeSceneTable {
    [CmdletBinding()]
    param([Parameter(Mandatory = $true)][string]$SourceRoot)

    $headerPath = Join-Path $SourceRoot $script:RuntimeSceneHeaderRelative
    if (-not (Test-Path -LiteralPath $headerPath)) { throw "Runtime scene table is missing: $headerPath" }
    $header = Get-Content -LiteralPath $headerPath -Raw -Encoding UTF8
    $pattern = '\{\s*(\d+)\s*,\s*"([A-Za-z]+)"\s*,\s*(\d+)\s*,\s*([-\d.]+)\s*,\s*([-\d.]+)\s*,\s*([-\d.]+)\s*,\s*(true|false)\s*\}'
    $byFrame = @{}
    foreach ($match in [regex]::Matches($header, $pattern)) {
        $frame = [int]$match.Groups[3].Value
        if ($byFrame.ContainsKey($frame)) { continue }
        $byFrame[$frame] = [pscustomobject]@{
            SceneId = [int]$match.Groups[1].Value
            Scene = $match.Groups[2].Value
            Frame = $frame
            OriginX = [double]::Parse($match.Groups[4].Value, [Globalization.CultureInfo]::InvariantCulture)
            OriginY = [double]::Parse($match.Groups[5].Value, [Globalization.CultureInfo]::InvariantCulture)
            Scale = [double]::Parse($match.Groups[6].Value, [Globalization.CultureInfo]::InvariantCulture)
            RequiresGameValidation = $match.Groups[7].Value -eq 'true'
        }
    }
    if ($byFrame.Count -eq 0) { throw "No scene definitions were parsed from $headerPath." }
    return $byFrame
}

# The frame's effective transform. -Scene is optional; when given it must agree with the table, which
# is what catches a region registry that has drifted away from the runtime.
function Get-SceneCoordinateTransform {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$SourceRoot,
        [Parameter(Mandatory = $true)][int]$Frame,
        [string]$Scene
    )

    $table = Read-RuntimeSceneTable -SourceRoot $SourceRoot
    if (-not $table.ContainsKey($Frame)) {
        throw "Frame $Frame is not in the runtime scene table ($script:RuntimeSceneHeaderRelative); add it there before building anything that depends on its coordinate frame."
    }
    $entry = $table[$Frame]
    if ($Scene -and $entry.Scene -ne $Scene) {
        throw "Frame $Frame is '$($entry.Scene)' in the runtime scene table but '$Scene' in the region registry."
    }
    $originX = $entry.OriginX
    $originY = $entry.OriginY
    $scale = $entry.Scale
    $source = 'runtime-scene-table'

    $calibrationPath = Join-Path $SourceRoot $script:SceneCalibrationRelative
    if (Test-Path -LiteralPath $calibrationPath) {
        $calibrations = Get-Content -LiteralPath $calibrationPath -Raw -Encoding UTF8 | ConvertFrom-Json
        $scenes = $calibrations.PSObject.Properties['scenes']
        if ($null -ne $scenes) {
            $calibration = $scenes.Value.PSObject.Properties[$entry.Scene]
            if ($null -ne $calibration) {
                $transform = $calibration.Value.PSObject.Properties['coordinateTransform']
                $passed = $calibration.Value.PSObject.Properties['passed']
                $maxError = $calibration.Value.PSObject.Properties['maxErrorPixels']
                # Exactly the runtime's gate. Reading a calibration more loosely here would put the
                # index in a frame the runtime does not use - which is half of the bug above.
                if ($null -ne $transform -and $null -ne $passed -and $passed.Value -eq $true -and
                    $null -ne $maxError -and
                    [double]$maxError.Value -le $script:SceneCalibrationMaxErrorPixels) {
                    $originX = [double]$transform.Value.originX
                    $originY = [double]$transform.Value.originY
                    $scale = [double]$transform.Value.scale
                    $source = 'calibration'
                }
            }
        }
    }

    # A frame the runtime itself gates on four-anchor validation (CoordinateStruct.h's last field)
    # ships the (0,0) placeholder until a calibration lands. (0,0) is as plausible-looking as World's
    # origin and exactly as wrong, so refuse rather than build a footprint grid nobody can ever be
    # contained by.
    if ($source -ne 'calibration' -and $entry.RequiresGameValidation -and $originX -eq 0.0 -and $originY -eq 0.0) {
        throw "Frame $Frame ($($entry.Scene)) has no calibrated origin yet - the runtime table still carries the (0,0) placeholder, and no passed entry exists in $script:SceneCalibrationRelative. Refusing to build in a guessed coordinate frame."
    }

    return [pscustomobject]@{
        Scene = $entry.Scene; Frame = $Frame
        OriginX = $originX; OriginY = $originY; Scale = $scale; Source = $source
    }
}
