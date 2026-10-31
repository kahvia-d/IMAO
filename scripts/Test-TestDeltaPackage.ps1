[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Enter-DevEnvironment.ps1')
$work = Join-Path (Split-Path $PSScriptRoot) ('out/delta-tests-' + [guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($work)
function Put($root, $path, $text) {
    $p = Join-Path $root $path
    [void][IO.Directory]::CreateDirectory((Split-Path $p))
    [IO.File]::WriteAllText($p, $text)
}
function Assert($condition, $message) { if (-not $condition) { throw "ASSERT: $message" } }
. (Join-Path $PSScriptRoot 'TestDeltaPackage.Common.ps1')
$originalCulture = [Threading.Thread]::CurrentThread.CurrentCulture
try {
    $unicodeFiles = @(0x963f,0x827e,0x5f20,0x4e2d | ForEach-Object {
        [pscustomobject]@{path=([string][char]$_)+'.dat';length=1;sha256=('a'*64)}
    })
    [Threading.Thread]::CurrentThread.CurrentCulture = [Globalization.CultureInfo]::GetCultureInfo('zh-CN')
    $chineseIdentity = Get-DeltaIdentity $unicodeFiles
    [Threading.Thread]::CurrentThread.CurrentCulture = [Globalization.CultureInfo]::GetCultureInfo('en-US')
    Assert ((Get-DeltaIdentity $unicodeFiles) -eq $chineseIdentity) 'Delta identities must not depend on target Windows language'
} finally { [Threading.Thread]::CurrentThread.CurrentCulture = $originalCulture }
function Refuses([scriptblock]$action, $pattern) {
    $caught = $null
    try { & $action | Out-Null } catch { $caught = $_.Exception.Message }
    Assert ($caught -and $caught -match $pattern) "Expected refusal '$pattern', got '$caught'"
}
$base = Join-Path $work 'base'; $target = Join-Path $work 'target'; $delta = Join-Path $work 'delta'
foreach ($root in @($base, $target)) {
    Put $root 'build-info.json' '{"appVersion":"2026.10.31.102"}'
    Put $root 'IMao-WinUI.runtimeconfig.json' '{"runtimeOptions":{"includedFrameworks":[{"name":"Microsoft.NETCore.App","version":"8.0.0"}]}}'
    Put $root 'resources.pri' 'same-resource'
    Put $root 'unchanged.dat' 'unchanged'
    Put $root 'bin/changed.dll' 'old'
}
Put $target 'build-info.json' '{"appVersion":"2026.10.31.103"}'
Put $target 'bin/changed.dll' 'new'
Put $target 'added.dat' 'added'
$generator = Join-Path $PSScriptRoot 'New-TestDeltaPackage.ps1'
Assert (Test-Path -LiteralPath $generator) 'Delta generator must exist'
& $generator -BaseRoot $base -TargetRoot $target -OutputRoot $delta -BaseZipSha256 ('a' * 64)
Assert (Test-Path "$delta/resources.pri") 'Unchanged resources.pri must be shipped'
Assert (-not (Test-Path "$delta/unchanged.dat")) 'Unchanged data must be omitted'
Assert (Test-Path "$delta.zip") 'ZIP must be produced'
$apply = Join-Path $delta '_test-delta/Apply-TestDeltaPackage.ps1'
$install = Join-Path $work 'install'
Copy-Item -LiteralPath $base -Destination $install -Recurse
Put $install 'Logs/user.log' 'keep me'
Put $install 'settings.json' 'user preferences'
Put $install 'bin/changed.dll' 'tampered'
Refuses { & $apply -PackageRoot $install } 'digest|hash|mismatch'
Assert (-not (Test-Path "$install/added.dat")) 'Wrong base must refuse before writes'
Put $install 'bin/changed.dll' 'old'
$locked = [IO.File]::Open("$install/bin/changed.dll", 'Open', 'ReadWrite', 'None')
try { Refuses { & $apply -PackageRoot $install } 'used|process|access|sharing|locked|hash|digest' } finally { $locked.Dispose() }
Assert (-not (Test-Path "$install/added.dat")) 'Locked file must refuse before writes'
(Get-Item "$install/bin/changed.dll").IsReadOnly = $true
try { Refuses { & $apply -PackageRoot $install } 'read.only' } finally { (Get-Item "$install/bin/changed.dll").IsReadOnly = $false }
Assert (-not (Test-Path "$install/added.dat")) 'Readonly file must refuse before writes'
& $apply -PackageRoot $install
& $apply -PackageRoot $install -VerifyOnly
# The shipped CMD starts Windows PowerShell with -File, where script-scope
# automatic variables are not available while binding parameter defaults.
$windowsPowerShell = Join-Path $env:WINDIR 'System32/WindowsPowerShell/v1.0/powershell.exe'
& $windowsPowerShell -NoProfile -ExecutionPolicy Bypass -File $apply -PackageRoot $install -VerifyOnly
Assert ($LASTEXITCODE -eq 0) 'Windows PowerShell -File must infer DeltaRoot like Verify-Delta.cmd'
foreach ($file in Get-ChildItem $target -File -Recurse) {
    $relative = $file.FullName.Substring($target.Length + 1)
    Assert ((Get-FileHash $file.FullName).Hash -eq (Get-FileHash (Join-Path $install $relative)).Hash) "Target equality: $relative"
}
Assert ((Get-Content "$install/Logs/user.log" -Raw) -eq 'keep me') 'Preserve logs'
Assert ((Get-Content "$install/settings.json" -Raw) -eq 'user preferences') 'Preserve settings'
Remove-Item -LiteralPath "$target/unchanged.dat"
Refuses { & $generator -BaseRoot $base -TargetRoot $target -OutputRoot "$work/removal" -BaseZipSha256 ('a'*64) } 'remov'
Put $target 'unchanged.dat' 'unchanged'
$manifestPath = Join-Path $delta '_test-delta/manifest.json'
$original = Get-Content $manifestPath -Raw
$malicious = $original | ConvertFrom-Json
$malicious.targetFiles[0].path = '../escape'
$malicious | ConvertTo-Json -Depth 20 | Set-Content $manifestPath
Refuses { & $apply -PackageRoot $install -VerifyOnly } 'unsafe|path'
[IO.File]::WriteAllText($manifestPath, $original)
$link = Join-Path $target 'linked'
New-Item -ItemType Junction -Path $link -Target $base | Out-Null
try { Refuses { & $generator -BaseRoot $base -TargetRoot $target -OutputRoot "$work/junction" -BaseZipSha256 ('a'*64) } 'reparse|junction' }
finally { [IO.Directory]::Delete($link) }
Write-Host "PASS: delta generation, ZIP, mandatory PRI, full reconstruction, wrong base, locked/readonly files, extras, removals, traversal, junction. Evidence: $work"
