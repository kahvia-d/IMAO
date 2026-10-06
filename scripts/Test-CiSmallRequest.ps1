[CmdletBinding()]
param([string]$OutputRoot)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'CiReleaseMetadata.ps1')
if (-not $OutputRoot) { $OutputRoot=Join-Path (Split-Path $PSScriptRoot) ('out/small-request-'+[guid]::NewGuid().ToString('N')) }
[IO.Directory]::CreateDirectory($OutputRoot) | Out-Null
$names=@('request.json','catalog-payload.json','approval-payload.json','asset-inventory.json','preparation-report.json','previous-stable.json')
function Make-Zip([string]$Path,[string[]]$Names,[bool]$Oversized=$false) {
    $zip=[IO.Compression.ZipArchive]::new([IO.File]::Create($Path),[IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($name in $Names) {
            $stream=$zip.CreateEntry($name).Open()
            try { if ($Oversized -and $name -eq 'catalog-payload.json') { $buffer=[byte[]]::new(1MB); for ($i=0;$i -lt 17;$i++) { $stream.Write($buffer,0,$buffer.Length) } } else { $stream.Write([Text.Encoding]::UTF8.GetBytes('{}')) } } finally { $stream.Dispose() }
        }
    } finally { $zip.Dispose() }
}
function Reject([scriptblock]$Action) { try { & $Action } catch { return }; throw 'Expected bounded request rejection.' }
$valid=Join-Path $OutputRoot 'valid.zip'; Make-Zip $valid $names
Expand-CiSmallRequest $valid (Join-Path $OutputRoot 'valid')
if (@(Get-ChildItem (Join-Path $OutputRoot 'valid') -File).Count -ne 6) { throw 'Valid small request extraction failed.' }
$traversal=Join-Path $OutputRoot 'traversal.zip'; Make-Zip $traversal (@('request.json','catalog-payload.json','approval-payload.json','asset-inventory.json','preparation-report.json','../previous-stable.json'))
Reject { Expand-CiSmallRequest $traversal (Join-Path $OutputRoot 'traversal') }
$duplicate=Join-Path $OutputRoot 'duplicate.zip'; Make-Zip $duplicate (@('request.json','request.json')+$names[2..5])
Reject { Expand-CiSmallRequest $duplicate (Join-Path $OutputRoot 'duplicate') }
$large=Join-Path $OutputRoot 'large.zip'; Make-Zip $large $names $true
Reject { Expand-CiSmallRequest $large (Join-Path $OutputRoot 'large') }
Reject { Assert-CiArtifactDigest $valid ('a'*64) }
Assert-CiArtifactDigest $valid ((Get-FileHash $valid -Algorithm SHA256).Hash.ToLowerInvariant())
Write-Host 'PASS bounded archive extraction, duplicate/path traversal/ZIP bomb rejection and service digest checks.'
