function Resolve-MapTestPackageIdentity($Package, $Canonical, $Files) {
    if ($null -ne $Canonical -and ($Canonical.id -cne $Package.id -or $Canonical.kind -cne $Package.kind)) {
        throw 'Canonical resource identity does not match the test package.'
    }
    $matches = $null -ne $Canonical -and @($Canonical.files).Count -eq @($Files).Count
    $actual = [Collections.Generic.Dictionary[string,object]]::new([StringComparer]::Ordinal)
    foreach ($file in $Files) {
        if (-not $actual.TryAdd([string]$file.path, $file)) { throw "Duplicate test package file: $($file.path)" }
    }
    if ($matches) {
        foreach ($file in $Canonical.files) {
            $entry = $null
            if (-not $actual.TryGetValue([string]$file.path, [ref]$entry) -or
                [long]$entry.size -ne [long]$file.size -or
                -not ([string]$entry.sha256).Equals([string]$file.sha256, [StringComparison]::OrdinalIgnoreCase)) {
                $matches = $false; break
            }
        }
    }
    # Empty package hash is the existing local development-bundle convention.
    # Per-file hashes still describe the actual test bytes; never substitute a
    # manifest hash for the archive hash the signed publication names.
    $chosenFiles = @($Files)
    if ($matches) { $chosenFiles = @($Canonical.files) }
    [pscustomobject]@{
        version = if ($matches) { [string]$Canonical.version } else { [string]$Package.version }
        sha256 = if ($matches) { [string]$Canonical.sha256 } else { '' }
        files = $chosenFiles
        customized = -not $matches
    }
}
