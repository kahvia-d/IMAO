[CmdletBinding()]
param([string]$OutputRoot)
$ErrorActionPreference='Stop'
$policy=Join-Path $PSScriptRoot 'ReleasePublishPolicy.ps1'
if (Test-Path $policy) { . $policy }
if (-not $OutputRoot) { $OutputRoot=Join-Path (Split-Path $PSScriptRoot) ('out/publish-policy-'+[guid]::NewGuid().ToString('N')) }
[IO.Directory]::CreateDirectory((Join-Path $OutputRoot 'prepared/manual')) | Out-Null
$root=Join-Path $OutputRoot 'prepared'; $version='2026.10.6.2'; $name="IMao-v$version-windows-x64.zip"
$file=Join-Path $root "manual/$name"; [IO.File]::WriteAllText($file,'approved fixture bytes')
$inventory=@(@{name=$name;path="manual/$name";size=(Get-Item $file).Length;sha256=(Get-FileHash $file -Algorithm SHA256).Hash.ToLowerInvariant()})
function Assert($Condition,$Message) { if (-not $Condition) { throw $Message } }
function Reject([scriptblock]$Action) { try { & $Action } catch { return }; throw 'Expected release policy rejection.' }
$actual=Get-AuthorizedManualInstallArchive $inventory $root $version $file
Assert ($actual.path -ceq [IO.Path]::GetFullPath($file)) 'Approved installer not selected.'
$external=Join-Path $OutputRoot $name; Copy-Item $file $external
Reject { Get-AuthorizedManualInstallArchive $inventory $root $version $external }
$derived=Get-AuthorizedManualInstallArchive $inventory $root $version ''
Assert ($derived.path -ceq $actual.path) 'Omitted argument did not derive the required approved installer.'
[IO.File]::AppendAllText($file,'changed'); Reject { Get-AuthorizedManualInstallArchive $inventory $root $version $file }
Reject { Get-AuthorizedManualInstallArchive @() $root $version $file }
$expected=@(@{name=$name},@{name='update.json'})
Assert-ExactReleaseAssetSet $expected @(@{name='update.json'}) $false
Reject { Assert-ExactReleaseAssetSet $expected @(@{name='setup.exe'}) $false }
Reject { Assert-ExactReleaseAssetSet $expected @(@{name=$name}) $true }
Reject { Assert-ExactReleaseAssetSet $expected @(@{name=$name},@{name='update.json'},@{name='setup.exe'}) $true }
Reject { Assert-ExactReleaseAssetSet $expected @(@{name=$name},@{name=$name},@{name='update.json'}) $true }
Assert-ExactReleaseAssetSet $expected @(@{name='update.json'},@{name=$name}) $true
Assert-ReleaseSourceVersion '2026.10.6.2' '2026.10.6.2' # original source Version.props, regardless of newer main
Reject { Assert-ReleaseSourceVersion '2026.10.6.2' '2026.10.6.3' }
Write-Host 'PASS authorized installer selection, omitted installer derivation, external/swapped installer rejection, exact draft asset set and frozen source version confirmation.'
