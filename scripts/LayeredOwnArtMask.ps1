# Shared helpers for the layered-floor index: which of a floor's descriptors were extracted from the
# layer's OWN art rather than from the surface base plate the composite carries underneath it.
#
# Why this exists
# ---------------
# New-LayeredTileComposite builds each floor's reference image as `surface x k` with that floor's
# RGBA overlay composited on top, because that is what the game itself draws (see
# Docs/LayeredMapFeatureMeasurement_20260922.md sections 7-8). The overlay is 93.7-100% transparent,
# so most of what the feature builder then extracts is the SURFACE map's own art at a lower
# brightness - measured at 71% of 寒雾深坑's 1381 descriptors, and 59.9% of all 275k across the 90
# floors of the game.
#
# That is harmless for localisation (the base plate gives the matcher something to lock on to inside a
# cave) and fatal for identity: the base plate is the SAME picture for every floor sharing a tile, and
# for a floor that owns its tile outright it is the surface the player may be standing on. On
# 2026-09-26 a frame captured on the surface at 虎口山脉 voted 寒雾深坑 10-17 against 3-7 on that base
# plate and the tool hid every surface marker. See Docs/LayeredMapFalsePositive_Hukou_20260926.md.
#
# The grid this file produces travels in floor-index.json next to the floor's tile list, one bit per
# descriptor of the floor's own .imf, so the runtime can ask "of these matches, how many hit art this
# layer actually draws". It does NOT replace the composite: the base plate stays, and keeps the
# in-cave recall it was added for.
#
# Dot-source this file:
#   . (Join-Path $PSScriptRoot 'LayeredOwnArtMask.ps1')

# Descriptor payload shape the .imf container uses (see Feature/Processing/FeatureBinaryCodec.cpp).
$script:ImfKeypointStride = 28      # 5 float32 (x, y, size, angle, response) + 2 int32
$script:ImfDescriptorStride = 512   # 128 float32

# <floor>.imf -> the keypoint positions the runtime loads, in the order it loads them. The order is
# what matters: the mask is indexed by descriptor position, so it has to be read from the same file
# the runtime reads rather than from the builder's features.yml, even though the two agree today
# (verified element-wise for jinzhou on 2026-09-26).
function Read-ImfKeypoints {
    [CmdletBinding()]
    param([Parameter(Mandatory = $true)][string]$Path)

    $bytes = [IO.File]::ReadAllBytes($Path)
    if ($bytes.Length -lt 8 -or [Text.Encoding]::ASCII.GetString($bytes, 0, 8) -ne 'IMAOFT01') {
        throw "not an IMF feature binary: $Path"
    }
    # fileLen = header + n*28 + n*512, and the header here is 116 bytes. Rather than trust that, find
    # the header length that makes the remainder divide exactly into whole keypoint+descriptor records
    # - an unaccounted-for byte would mean the mask is indexed against the wrong descriptors.
    $record = $script:ImfDescriptorStride + $script:ImfKeypointStride
    $header = -1
    $n = 0
    for ($probe = 8; $probe -lt $record; ++$probe) {
        $rest = $bytes.Length - $probe
        if ($rest -gt 0 -and $rest % $record -eq 0) {
            $header = $probe
            $n = $rest / $record
            break
        }
    }
    if ($header -lt 0 -or $n -le 0) { throw "cannot determine the keypoint count of $Path" }

    $points = New-Object 'System.Collections.Generic.List[double[]]' $n
    for ($i = 0; $i -lt $n; ++$i) {
        $offset = $header + $i * $script:ImfKeypointStride
        $points.Add(@([BitConverter]::ToSingle($bytes, $offset), [BitConverter]::ToSingle($bytes, $offset + 4)))
    }
    return , $points.ToArray()
}

# One overlay tile's alpha channel, flattened for fast lookup. Tiles are shared between floors, so the
# caller keeps these in a hashtable rather than loading one per floor.
function Read-OverlayAlpha {
    [CmdletBinding()]
    param([Parameter(Mandatory = $true)][string]$Path)

    Add-Type -AssemblyName System.Drawing -ErrorAction SilentlyContinue
    $bitmap = [System.Drawing.Bitmap]::FromFile($Path)
    try {
        $width = $bitmap.Width
        $height = $bitmap.Height
        $rect = New-Object System.Drawing.Rectangle(0, 0, $width, $height)
        $data = $bitmap.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly,
            [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
        try {
            $stride = [Math]::Abs($data.Stride)
            $pixels = New-Object byte[] ($stride * $height)
            [Runtime.InteropServices.Marshal]::Copy($data.Scan0, $pixels, 0, $pixels.Length)
        }
        finally { $bitmap.UnlockBits($data) }
        return [pscustomobject]@{ Alpha = $pixels; Stride = $stride; Width = $width; Height = $height }
    }
    finally { $bitmap.Dispose() }
}

# Map coordinate -> the tile and pixel the runtime's LocateCell would use. Kept as its own function so
# this stays the exact inverse of Feature/LayeredFloorIndex.cpp rather than a second opinion.
function Get-OverlayPixel {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][double]$MapX,
        [Parameter(Mandatory = $true)][double]$MapY,
        [Parameter(Mandatory = $true)][hashtable]$Transform)
    $gameX = ($MapX - $Transform.originX) / $Transform.scale
    $gameY = ($MapY - $Transform.originY) / $Transform.scale
    $tileX = [Math]::Floor($gameX / $Transform.virtualMapSize + 1.0)
    $tileY = [Math]::Ceiling(-$gameY / $Transform.virtualMapSize)
    $pixelX = $gameX * $Transform.tileSize / $Transform.virtualMapSize + $Transform.tileSize - $tileX * $Transform.tileSize
    $pixelY = $tileY * $Transform.tileSize + $gameY * $Transform.tileSize / $Transform.virtualMapSize
    return [pscustomobject]@{ TileX = [int]$tileX; TileY = [int]$tileY; PixelX = $pixelX; PixelY = $pixelY }
}

# The heart of it: one bit per keypoint, set where the floor's own overlay draws AND the spot is not
# ground the layer copied from the surface. Packed four bits to a hex nibble, first keypoint in the
# lowest bit - the same packing as the occupancy grid, and the same one DecodeOwnMask reads.
#
# The `shared` veto is not an optimisation. 下层金库's 1楼 is 27-32% a copy of the surface and 拉海洛's
# 星炬学院 floors are 58-84%, so for those floors most of what the overlay draws IS the surface's own
# picture; counting it as "the layer's own" put the whole-game scan's false-positive rate at 68% for
# 下层金库 and 19% for 拉海洛 (measured 2026-09-26, out/sweep-report.txt). What the identity vote has
# to be built on is the art the layer DREW.
function Get-OwnArtMask {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][double[][]]$Points,
        [Parameter(Mandatory = $true)][hashtable]$Transform,
        # "x,y" -> overlay path, exactly the tiles the floor's features were built from.
        [Parameter(Mandatory = $true)][hashtable]$TileOverlays,
        [Parameter(Mandatory = $true)][hashtable]$AlphaCache,
        # "x,y" -> that tile's `shared` grid, the cells the layer copied from the surface.
        [hashtable]$TileSharedGrids = @{},
        [int]$GridSize = 64,
        [int]$AlphaThreshold = 8,
        # Share of keypoints allowed to fall outside -TileOverlays before the coordinate frame is
        # declared wrong. Measured separation is 0.0% vs 100.0%, so this only has to be a sanity bar.
        [double]$FrameMismatchTolerance = 0.2)

    $count = $Points.Count
    $bits = New-Object byte[] $count
    $own = 0
    $copied = 0
    $outside = 0
    $cellSize = $Transform.tileSize / $GridSize
    for ($i = 0; $i -lt $count; ++$i) {
        $point = $Points[$i]
        $at = Get-OverlayPixel -MapX $point[0] -MapY $point[1] -Transform $Transform
        $key = "$($at.TileX),$($at.TileY)"
        if (-not $TileOverlays.ContainsKey($key)) { ++$outside; continue }
        $x = [int][Math]::Floor($at.PixelX)
        $y = [int][Math]::Floor($at.PixelY)
        if ($x -lt 0 -or $y -lt 0 -or $x -ge $Transform.tileSize -or $y -ge $Transform.tileSize) { ++$outside; continue }
        $path = $TileOverlays[$key]
        if (-not $AlphaCache.ContainsKey($path)) { $AlphaCache[$path] = Read-OverlayAlpha -Path $path }
        $tile = $AlphaCache[$path]
        if ($y -ge $tile.Height -or $x -ge $tile.Width) { ++$outside; continue }
        # 32bppArgb: BGRA in memory, alpha is the fourth byte.
        if ($tile.Alpha[$y * $tile.Stride + $x * 4 + 3] -le $AlphaThreshold) { continue }
        if ($TileSharedGrids.ContainsKey($key)) {
            $sharedHex = [string]$TileSharedGrids[$key]
            if ($sharedHex.Length * 4 -eq $GridSize * $GridSize) {
                $cellX = [int][Math]::Floor($x / $cellSize)
                $cellY = [int][Math]::Floor($y / $cellSize)
                if ($cellX -ge 0 -and $cellY -ge 0 -and $cellX -lt $GridSize -and $cellY -lt $GridSize) {
                    $bit = $cellY * $GridSize + $cellX
                    # [Math]::Floor, not `$bit / 4`: PowerShell's division gives a Double and the
                    # string indexer then ROUNDS it, so the last nibble of a 64x64 grid reads index
                    # 1024 of a 1024-character string and throws.
                    $nibbleIndex = [int][Math]::Floor($bit / 4)
                    $nibble = [Convert]::ToInt32([string]$sharedHex[$nibbleIndex], 16)
                    if ((($nibble -shr ($bit % 4)) -band 1) -eq 1) { ++$copied; continue }
                }
            }
        }
        $bits[$i] = 1
        ++$own
    }

    # A floor's keypoints were extracted from exactly the tiles in -TileOverlays, and written out in
    # the coordinate frame recorded beside them. Reading them back with a DIFFERENT frame puts them on
    # other tiles, where no overlay exists - and that is invisible: the alpha read is simply skipped
    # and the mask comes out all zeroes, which the runtime reads as "none of these descriptors is the
    # layer's own art" and quietly switches the own-art veto off for that floor.
    #
    # The separation is total, so this is a hard error rather than a warning. Every floor of all 90 in
    # the shipped packs puts 100.0% of its keypoints back on its own tiles; 隐海试验场's index, whose
    # transform said World while its .imf had been built in frame 905, put 0.0% there.
    if ($count -gt 0 -and $outside -gt $count * $FrameMismatchTolerance) {
        throw ("{0:N1}% of {1} keypoints landed outside the tiles they were built from; the .imf was not built with this coordinate transform. Rebuild the floor with scripts/New-LayeredFloorIndex.ps1 rather than retro-fitting a mask onto it." -f (100.0 * $outside / $count), $count)
    }

    # [int] is load-bearing: New-Object picks the StringBuilder(String) overload for a non-integral
    # capacity, which prefixes the whole mask with the decimal text of the capacity ("346.2500" for
    # 1381 keypoints) and shifts every bit against its descriptor. The runtime now rejects a mask of
    # the wrong length, but that guard is the second line of defence, not the first.
    $hex = New-Object Text.StringBuilder ([int]($count / 4 + 1))
    for ($i = 0; $i -lt $count; $i += 4) {
        $nibble = 0
        for ($b = 0; $b -lt 4; ++$b) {
            if ($i + $b -lt $count -and $bits[$i + $b] -ne 0) { $nibble = $nibble -bor (1 -shl $b) }
        }
        [void]$hex.Append('0123456789abcdef'[$nibble])
    }
    return [pscustomobject]@{ Hex = $hex.ToString(); Own = $own; Total = $count; Outside = $outside; Copied = $copied }
}

# The per-tile overlay paths a floor's index names, keyed the way Get-OverlayPixel returns a tile.
# floor-index.json does not carry the overlay path (only the composite's manifest does), so it is
# derived the way New-LayeredTileComposite names them: <archive>/<version>/<frame>/<layer>/<level>/<x>_<y>.png
function Get-FloorTileOverlays {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]$Floor,
        [Parameter(Mandatory = $true)][int]$Frame,
        [Parameter(Mandatory = $true)][int]$LayerId,
        [Parameter(Mandatory = $true)][int]$Level,
        [Parameter(Mandatory = $true)][string]$LayerArchiveRoot,
        [Parameter(Mandatory = $true)][string]$Version)
    $overlays = @{}
    $directory = Join-Path $LayerArchiveRoot (Join-Path $Version (Join-Path "$Frame" (Join-Path "$LayerId" "$Level")))
    foreach ($tile in $Floor.tiles) {
        $name = "$([int]$tile.x)_$([int]$tile.y).png"
        $path = Join-Path $directory $name
        if (Test-Path -LiteralPath $path) { $overlays["$([int]$tile.x),$([int]$tile.y)"] = $path }
    }
    return $overlays
}
