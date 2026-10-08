[CmdletBinding()]
param(
    # The two cloud release build runs to compare. The defaults are the 2026-10-07 pair that exposed
    # the three reproducibility bugs below; pass any two artifact IDs to compare a new pair.
    [long]$LeftArtifactId = 11460916069,
    [long]$RightArtifactId = 11461663480,
    [string]$OutputRoot
)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'CiReleaseMetadata.ps1')
if (-not $OutputRoot) { $OutputRoot = Join-Path $repoRoot 'out/compare' }
[IO.Directory]::CreateDirectory($OutputRoot) | Out-Null

# What two cloud builds of the same commit differing by a few hundred bytes actually differ IN.
#
# Two artifacts of the SAME commit were once byte-different and the release was not reproducible. The
# answers were not visible in a size or a hash, and the whole point of this script is to get at them
# without downloading a package set that is hundreds of megabytes: it pulls the two run artifacts,
# compares the per-file digests their catalog already carries, and then digs only into the files that
# disagree - the length, the text of a small JSON, the PE timestamp of a binary, and the byte ranges
# that changed with readable context on both sides.
#
# It found two of the three:
#   * `/Brepro` was not enough. MSVC names an anonymous namespace after a hash of the RESOLVED SOURCE
#     DIRECTORY (`?A0x78f5c21b@` becoming `?A0xb2c44639@` on the other runner), so the same code built
#     from the same relative path on two machines differs. The ASCII snippets beside each changed
#     range are what named it; the PE timestamp column is what pointed at the file. Fixed with
#     `/d1trimfile:<repo root>` on all three native builds.
#   * `-DCMAKE_C_FLAGS=/Brepro` REPLACES CMake's MSVC defaults (`/DWIN32 /D_WINDOWS /W3 /GR /EHsc`)
#     instead of adding to them, which silently changed what OpenCV was compiled as. Found by reading
#     the cloud configuration summary this comparison pointed at.
#
# It does NOT find the third, and the header says so because that is the trap: the CsWinRT AOT
# optimizer generated a DIFFERENT SET OF TYPES per run - `…_MainWindowWinRTTypeDetails` on one and
# `…_Views_ShellPageWinRTTypeDetails` on the other - and a byte-range diff of a managed assembly
# cannot show that. Comparing type and method sets out of the metadata is a different tool, and the
# one used at the time was never committed and no longer exists anywhere. If a managed assembly
# differs and this report's byte ranges look meaningless, that is why: read the metadata
# (`System.Reflection.Metadata` is enough) rather than the bytes.
#
# Read-only apart from $OutputRoot, which is inside the gitignored `out/`.
$sides = @()
foreach ($id in @($LeftArtifactId, $RightArtifactId)) {
    $meta = Invoke-CiGh @('api', "repos/kahvia-d/IMAO/actions/artifacts/$id") | ConvertFrom-Json
    $archive = Join-Path $OutputRoot "$id.zip"
    Save-CiArtifactArchive $id $archive
    Assert-CiArtifactDigest $archive $meta.digest
    $zip = [IO.Compression.ZipFile]::OpenRead($archive)
    try {
        $catalogEntry = @($zip.Entries | Where-Object FullName -CEQ 'catalog-draft.json')
        if ($catalogEntry.Count -ne 1 -or $catalogEntry[0].Length -gt 16MB) { throw 'Unexpected catalog' }
        $reader = [IO.StreamReader]::new($catalogEntry[0].Open())
        try { $catalog = $reader.ReadToEnd() | ConvertFrom-Json -Depth 100 } finally { $reader.Dispose() }
        $manualEntry = @($zip.Entries | Where-Object { $_.FullName -like 'manual/*.zip' })
        if ($manualEntry.Count -ne 1) { throw 'Unexpected manual archive' }
        $manualPath = Join-Path $OutputRoot "manual-$id.zip"
        $stream = $manualEntry[0].Open(); $out = [IO.File]::Create($manualPath)
        try { $stream.CopyTo($out) } finally { $stream.Dispose(); $out.Dispose() }
        $sides += @{ id = $id; catalog = $catalog; manual = $manualPath }
    }
    finally { $zip.Dispose() }
}
$left = @{}; foreach ($f in $sides[0].catalog.app.package.files) { $left[$f.path] = $f }
$right = @{}; foreach ($f in $sides[1].catalog.app.package.files) { $right[$f.path] = $f }
$paths = @(@($left.Keys) + @($right.Keys) | Sort-Object -Unique)
$diff = @(foreach ($p in $paths) { if ($left[$p].sha256 -cne $right[$p].sha256) { [ordered]@{ path = $p; left = $left[$p]; right = $right[$p] } } })
$details = @()
$binarySides = @{}
foreach ($side in $sides) {
    $zip = [IO.Compression.ZipFile]::OpenRead($side.manual)
    try {
        foreach ($d in $diff) {
            $entry = @($zip.Entries | Where-Object { $_.FullName -ceq $d.path -or $_.FullName.EndsWith('/' + $d.path, [StringComparison]::Ordinal) })
            if ($entry.Count -ne 1) { continue }
            $stream = $entry[0].Open(); $mem = [IO.MemoryStream]::new()
            try { $stream.CopyTo($mem); $bytes = $mem.ToArray() } finally { $stream.Dispose(); $mem.Dispose() }
            $binarySides["$($side.id)/$($d.path)"] = $bytes
            $item = [ordered]@{ artifactId = $side.id; path = $d.path; length = $bytes.Length }
            if ($d.path.EndsWith('.json') -and $bytes.Length -lt 64KB) { $item.text = [Text.Encoding]::UTF8.GetString($bytes) }
            if ($bytes.Length -gt 256 -and $bytes[0] -eq 77 -and $bytes[1] -eq 90) {
                $peOffset = [BitConverter]::ToInt32($bytes, 60)
                $item.peTimestamp = [BitConverter]::ToUInt32($bytes, $peOffset + 8)
            }
            $details += $item
        }
    }
    finally { $zip.Dispose() }
}
# Guarded: a second run in the same session would otherwise fail on a duplicate type name.
if (-not ('ByteDifferences' -as [type])) {
    Add-Type -TypeDefinition @'
public static class ByteDifferences {
    public static long[] Ranges(byte[] a, byte[] b) {
        var values = new System.Collections.Generic.List<long>(); long count=0; int start=-1;
        for(int i=0;i<System.Math.Min(a.Length,b.Length);i++) {
            if(a[i]!=b[i]) { count++; if(start<0) start=i; }
            else if(start>=0) { if(values.Count<100) { values.Add(start); values.Add(i-start); } start=-1; }
        }
        if(start>=0 && values.Count<100) { values.Add(start); values.Add(System.Math.Min(a.Length,b.Length)-start); }
        values.Insert(0,count); return values.ToArray();
    }
}
'@
}
$binaryDiffs = @()
foreach ($d in $diff) {
    $a = $binarySides["$($sides[0].id)/$($d.path)"]; $b = $binarySides["$($sides[1].id)/$($d.path)"]
    $ranges = [ByteDifferences]::Ranges($a, $b); $snippets = @()
    for ($i = 1; $i -lt $ranges.Length; $i += 2) {
        $offset = [int]$ranges[$i]; $length = [int]$ranges[$i + 1]; $start = [Math]::Max(0, $offset - 60); $span = [Math]::Min(220, [Math]::Min($a.Length, $b.Length) - $start)
        $snippets += @{ offset = $offset; length = $length; leftText = [Text.Encoding]::ASCII.GetString($a, $start, $span); rightText = [Text.Encoding]::ASCII.GetString($b, $start, $span) }
    }
    $binaryDiffs += @{ path = $d.path; changedBytes = $ranges[0]; snippets = $snippets }
}
[ordered]@{ leftId = $sides[0].id; rightId = $sides[1].id; differences = $diff; details = $details; binaryDiffs = $binaryDiffs } |
    ConvertTo-Json -Depth 100 | Set-Content (Join-Path $OutputRoot 'report.json') -Encoding utf8
Write-Host "Differing files: $($diff.Count)"
$diff | ForEach-Object { Write-Host $_.path }
Write-Host "Report: $(Join-Path $OutputRoot 'report.json')"
