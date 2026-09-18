[CmdletBinding()]
param(
    # Comma-separated, because -File invocation cannot bind an array.
    [Parameter(Mandatory = $true)][string]$State,
    [string]$ResourceVersion = 'B50F4135DCCC4D8DA87ED33CE95EA31D',
    [int]$MinX = -40,
    [int]$MaxX = 40,
    [int]$MinY = -40,
    [int]$MaxY = 40,
    # A coarse step locates the map first; a fine sweep then covers the box it was found in.
    [ValidateRange(1, 64)][int]$Step = 1,
    [ValidateRange(1, 32)][int]$ThrottleLimit = 16
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# A frame's compiled origin can be a placeholder, in which case the derived tile window
# lands where the map is not and every tile 404s. Probing the public tile index directly
# locates the map without needing the origin at all: each state's tiles are addressed by
# (state, tileX, tileY), so a coarse sweep finds the extent the host actually serves.
$host_ = 'web-static.kurobbs.com'

$states = @($State -split ',' | ForEach-Object { [int]$_.Trim() } | Where-Object { $_ -gt 0 })
if ($states.Count -eq 0) { throw 'State is empty.' }

foreach ($frame in $states) {
    $probes = [Collections.Generic.List[object]]::new()
    for ($x = $MinX; $x -le $MaxX; $x += $Step) {
        for ($y = $MinY; $y -le $MaxY; $y += $Step) {
            $name = "${frame}_${x}_${y}.png"
            $probes.Add([pscustomobject]@{
                X = $x; Y = $y; Name = $name
                Url = "https://$host_/mcmap/tiles/$ResourceVersion/$frame/$name"
            })
        }
    }
    Write-Host "state $frame : probing $($probes.Count) tiles over x $MinX..$MaxX, y $MinY..$MaxY (step $Step)"
    $results = $probes | ForEach-Object -Parallel {
        $target = $_
        $code = & curl.exe -s -o NUL -w '%{http_code}' --max-time 25 --retry 3 --retry-delay 1 --retry-all-errors --head $target.Url 2>&1
        [pscustomobject]@{ X = $target.X; Y = $target.Y; Code = "$code" }
    } -ThrottleLimit $ThrottleLimit

    $present = @($results | Where-Object { $_.Code -eq '200' })
    $other = @($results | Where-Object { $_.Code -ne '200' -and $_.Code -ne '404' })
    $histogram = ($results | Group-Object Code | Sort-Object Count -Descending | ForEach-Object { "$($_.Name)x$($_.Count)" }) -join ' '
    if ($present.Count -eq 0) {
        Write-Host "  no tiles found in this box   codes: $histogram" -ForegroundColor Yellow
        continue
    }
    $xs = @($present | ForEach-Object { $_.X })
    $ys = @($present | ForEach-Object { $_.Y })
    $bx = ($xs | Measure-Object -Minimum).Minimum; $bX = ($xs | Measure-Object -Maximum).Maximum
    $by = ($ys | Measure-Object -Minimum).Minimum; $bY = ($ys | Measure-Object -Maximum).Maximum
    Write-Host ("  found {0} tiles, grid box x {1}..{2} y {3}..{4} (step {5} sampling)" -f `
        $present.Count, $bx, $bX, $by, $bY, $Step) -ForegroundColor Green
    $present | Sort-Object X, Y | ForEach-Object { Write-Host "    ($($_.X),$($_.Y))" }
}
