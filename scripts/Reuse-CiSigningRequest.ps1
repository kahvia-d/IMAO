[CmdletBinding()]
param([Parameter(Mandatory)][string]$RequestRoot)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'CiReleaseMetadata.ps1')
$artifacts=(Invoke-CiGh @('api',"repos/kahvia-d/IMAO/actions/runs/$env:GITHUB_RUN_ID/artifacts?per_page=100") | ConvertFrom-Json).artifacts
$found=@($artifacts | Where-Object name -CEQ 'release-signing-request')
if ($found.Count -eq 0) { 'reused=false' >> $env:GITHUB_OUTPUT; exit 0 }
if ($found.Count -ne 1 -or $found[0].expired -or $found[0].size_in_bytes -gt 16MB) { throw 'Original signing request expired or ambiguous; do not create a replacement artifact.' }
$root=Join-Path (Split-Path $RequestRoot) 'original-request'
Save-CiArtifactArchive $found[0].id "$root.zip" 16MB
Assert-CiArtifactDigest "$root.zip" $found[0].digest
Expand-CiSmallRequest "$root.zip" $root
foreach ($file in Get-ChildItem $root -File) {
    if ((Get-FileHash $file.FullName -Algorithm SHA256).Hash -cne (Get-FileHash (Join-Path $RequestRoot $file.Name) -Algorithm SHA256).Hash) { throw 'Regenerated request differs from the original; inspect reservation instead of re-signing.' }
}
'reused=true' >> $env:GITHUB_OUTPUT
"artifact_id=$($found[0].id)" >> $env:GITHUB_OUTPUT
"artifact_digest=$($found[0].digest -replace '^sha256:','')" >> $env:GITHUB_OUTPUT
