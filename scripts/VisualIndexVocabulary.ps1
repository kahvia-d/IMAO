# Reading the identity words out of a visual index header.
#
# A visual index's words are shared: every pack shard and the base index have to name the
# same 4096 words, because a shard is merged into the base index by word id. Clustering is
# seeded and its sampling is fixed, so a rebuild is repeatable - but it is not insensitive
# to its input. Re-encoding descriptors moves them by up to half a quantization step, and
# that alone changes the clustered vocabulary, which is why a rebuild has to reuse an
# existing vocabulary rather than cluster a new one.
#
# When that goes wrong nothing fails loudly: the pack still loads, the shard still parses,
# and the runtime simply stops using the shard and drops visual locating. A pack that
# cannot be located is a silent, in-game-only failure, so the mismatch is caught here - in
# the release staging path, where the whole resource set is visible at once.
#
# Header: 8s magic, then the counts, the payload lengths and the tile geometry, then three
# 32-byte hashes. Only the vocabulary hash is needed here, and its offset is pinned by
# mutation: flipping one byte at 128 changes the value read below while flipping one at 96
# does not, so 128 is the vocabulary and 96 is a neighbouring field. Do not re-derive these
# offsets by adding up the struct - the serialized layout is not the field order.

$ImxMagic = 'IMAOIX01'
$ImxVocabularySha256Offset = 128
$ImxSourceImfSha256Offset = 96

function Read-VisualIndexIdentity([string]$Path) {
    $stream = [IO.File]::OpenRead($Path)
    try {
        if ($stream.Length -lt 192) { throw "Visual index is too small to carry a header: $Path" }
        $header = [byte[]]::new(192)
        $read = $stream.Read($header, 0, 192)
        if ($read -ne 192) { throw "Visual index header is truncated: $Path" }
    } finally { $stream.Dispose() }
    $magic = [Text.Encoding]::ASCII.GetString($header, 0, 8)
    if ($magic -ne $ImxMagic) { throw "Not an IMAOIX01 visual index: $Path" }
    function Read-Hex([int]$Offset) {
        $builder = [Text.StringBuilder]::new(64)
        for ($index = 0; $index -lt 32; ++$index) { [void]$builder.Append($header[$Offset + $index].ToString('x2')) }
        return $builder.ToString()
    }
    return [pscustomobject]@{
        path = $Path
        sourceImfSha256 = Read-Hex $ImxSourceImfSha256Offset
        vocabularySha256 = Read-Hex $ImxVocabularySha256Offset
    }
}

# Every index that ships has to agree on the vocabulary. Takes the index paths directly so
# the caller can assert over whatever set it is about to publish.
function Assert-VisualIndexVocabulary([string[]]$IndexPath) {
    $identities = @($IndexPath | ForEach-Object { Read-VisualIndexIdentity $_ })
    if ($identities.Count -eq 0) { throw 'No visual index was given to check a vocabulary for.' }
    $vocabularies = @($identities | ForEach-Object { $_.vocabularySha256 } | Sort-Object -Unique)
    if ($vocabularies.Count -ne 1) {
        $detail = ($identities | ForEach-Object {
            '  {0}  vocabulary {1}' -f ([IO.Path]::GetFileName((Split-Path -Parent $_.path)) + '/' + [IO.Path]::GetFileName($_.path)), $_.vocabularySha256
        }) -join "`n"
        throw ("The shipped visual indexes name $($vocabularies.Count) different vocabularies, but every index " +
            "that merges must share one. A shard built against another vocabulary is dropped at runtime without " +
            "an error, so this would ship as a silent loss of visual locating.`n$detail`n" +
            'Rebuild every index together, reusing one vocabulary: ' +
            'scripts/Build-VisualIndex.ps1 -VocabularySource <index.imx>')
    }
    return $vocabularies[0]
}
