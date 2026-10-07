# Validate the actual linker result, rather than trusting a build receipt.
function Assert-ReproducibleNativeImage([string]$Path) {
    Add-Type -AssemblyName System.Reflection.Metadata
    $stream = [IO.File]::OpenRead($Path)
    $image = [Reflection.PortableExecutable.PEReader]::new($stream)
    try {
        if (-not @($image.ReadDebugDirectory() | Where-Object Type -EQ Reproducible).Count) {
            throw "Native image lacks the deterministic-link marker: $Path"
        }
    } finally { $image.Dispose(); $stream.Dispose() }
}
