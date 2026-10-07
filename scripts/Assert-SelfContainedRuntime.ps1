[CmdletBinding()]
param([Parameter(Mandatory)][string]$AppRoot)
$ErrorActionPreference = 'Stop'
$options = (Get-Content -LiteralPath (Join-Path $AppRoot 'IMao-WinUI.runtimeconfig.json') -Raw | ConvertFrom-Json).runtimeOptions
if (-not ($options.PSObject.Properties.Name -contains 'includedFrameworks') -or
    @($options.includedFrameworks).Count -eq 0 -or
    ($options.PSObject.Properties.Name -contains 'framework') -or
    ($options.PSObject.Properties.Name -contains 'frameworks')) {
    throw 'Framework-dependent WinUI output rejected before deployment. Rebuild with -r win-x64 --self-contained true.'
}
foreach ($name in @('hostpolicy.dll','hostfxr.dll','coreclr.dll','System.Private.CoreLib.dll',
    'Microsoft.WindowsAppRuntime.Bootstrap.dll','Microsoft.UI.Xaml.dll','Microsoft.WindowsAppRuntime.dll','DWriteCore.dll')) {
    if (-not (Test-Path -LiteralPath (Join-Path $AppRoot $name) -PathType Leaf)) { throw "Self-contained runtime file missing: $name" }
}
$deps = Get-Content -LiteralPath (Join-Path $AppRoot 'IMao-WinUI.deps.json') -Raw | ConvertFrom-Json
$target = @($deps.targets.PSObject.Properties | Where-Object Name -Like '*/win-x64')
if ($target.Count -ne 1) { throw 'Exactly one win-x64 dependency target is required.' }
$checked = 0
foreach ($library in $target[0].Value.PSObject.Properties) {
    foreach ($kind in @('runtime','native')) {
        $group = $library.Value.PSObject.Properties[$kind]
        if ($null -eq $group) { continue }
        foreach ($asset in $group.Value.PSObject.Properties) {
            if ($asset.Name.EndsWith('/_._')) { continue }
            $relative = $asset.Name.Replace('/', '\')
            $leaf = [IO.Path]::GetFileName($relative)
            if (-not (Test-Path -LiteralPath (Join-Path $AppRoot $relative) -PathType Leaf) -and
                -not (Test-Path -LiteralPath (Join-Path $AppRoot $leaf) -PathType Leaf)) {
                throw "Dependency runtime asset missing: $($asset.Name)"
            }
            $checked++
        }
    }
}
Write-Host "Self-contained WinUI verified: $(@($options.includedFrameworks).Count) frameworks, $checked dependency assets, hostpolicy/coreclr/CoreLib/WindowsAppSDK bootstrap present."
