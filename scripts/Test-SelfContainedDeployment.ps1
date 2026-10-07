[CmdletBinding()]
param([Parameter(Mandatory)][string]$AppRoot, [Parameter(Mandatory)][string]$OutputRoot)
$ErrorActionPreference = 'Stop'
if (Test-Path -LiteralPath $OutputRoot) { throw 'Use a fresh test directory.' }
& (Join-Path $PSScriptRoot 'Assert-SelfContainedRuntime.ps1') -AppRoot $AppRoot
$cases = @{
    'framework-dependent' = 'IMao-WinUI.runtimeconfig.json'
    'missing-winui-sdk' = 'Microsoft.UI.Xaml.dll'
    'missing-dotnet-assembly' = 'System.Text.Json.dll'
}
foreach ($case in $cases.Keys) {
    $fixture = Join-Path $OutputRoot $case
    [IO.Directory]::CreateDirectory($fixture) | Out-Null
    foreach ($file in Get-ChildItem -LiteralPath $AppRoot -File | Where-Object Name -ne $cases[$case]) {
        New-Item -ItemType HardLink -Path (Join-Path $fixture $file.Name) -Target $file.FullName | Out-Null
    }
    if ($case -eq 'framework-dependent') {
        # Create a new file, never overwrite a hard-linked source runtimeconfig.
        '{"runtimeOptions":{"tfm":"net8.0","framework":{"name":"Microsoft.NETCore.App","version":"8.0.0"}}}' |
            Set-Content (Join-Path $fixture 'IMao-WinUI.runtimeconfig.json') -Encoding UTF8
    }
    $rejected = $false
    try { & (Join-Path $PSScriptRoot 'Assert-SelfContainedRuntime.ps1') -AppRoot $fixture }
    catch {
        if ($_.Exception.Message -notmatch 'Framework-dependent WinUI output rejected|Self-contained runtime file missing|Dependency runtime asset missing') { throw }
        $rejected = $true
    }
    if (-not $rejected) { throw "Invalid deployment accepted: $case" }
    Write-Host "PASS rejected $case"
}
