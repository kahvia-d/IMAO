[CmdletBinding()]
param(
    [ValidatePattern('^[A-Za-z0-9_-]+$')]
    [string]$Configuration = 'Release',
    [ValidateSet('x64', 'x86', 'arm64')]
    [string]$Platform = 'x64'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$taskRepo = Split-Path -Parent $PSScriptRoot
$taskProject = Join-Path $taskRepo 'IMao-WinUI'
$taskPageServicePath = Join-Path $taskProject 'Services\PageService.cs'
$taskMetadataPath = Join-Path $taskProject "obj\$Platform\$Configuration\XamlTypeInfo.g.cs"

if (-not (Test-Path -LiteralPath $taskMetadataPath -PathType Leaf)) {
    throw "Generated WinUI navigation metadata is missing: $taskMetadataPath. Build $Platform/$Configuration first."
}

$taskPageService = Get-Content -LiteralPath $taskPageServicePath -Raw
$taskMetadata = Get-Content -LiteralPath $taskMetadataPath -Raw
if ([string]::IsNullOrWhiteSpace($taskMetadata)) {
    throw "Generated WinUI metadata is empty. Wait for the $Platform/$Configuration build to finish."
}

# Match actual registration statements, not the generic Configure<VM, V> method declaration.
$taskRegistrations = [regex]::Matches($taskPageService,
    '(?m)^\s*(?:this\.)?Configure\s*<\s*[\w.:]+\s*,\s*(?<page>[\w.:]+)\s*>\s*\(\s*\)\s*;')
if ($taskRegistrations.Count -eq 0) { throw 'No navigation page registrations were found in PageService.cs.' }

[xml]$taskProjectXml = Get-Content -LiteralPath (Join-Path $taskProject 'IMao-WinUI.csproj') -Raw
$taskRootNamespace = [string]$taskProjectXml.SelectSingleNode('/Project/PropertyGroup/RootNamespace').InnerText
if ([string]::IsNullOrWhiteSpace($taskRootNamespace)) { throw 'The WinUI project RootNamespace is missing.' }
$taskPageNames = @($taskRegistrations | ForEach-Object { $_.Groups['page'].Value } | Select-Object -Unique)
$taskFailures = [Collections.Generic.List[string]]::new()

foreach ($taskPageName in $taskPageNames) {
    $taskType = $taskPageName -replace '^global::', ''
    if (-not $taskType.Contains('.')) { $taskType = "$taskRootNamespace.Views.$taskType" }
    $taskEscapedType = [regex]::Escape($taskType)

    # A source class or a DI registration is insufficient. Frame.Navigate needs a generated
    # type entry with a constructible factory that is actually assigned to its IXamlType.
    $taskFactory = [regex]::Match($taskMetadata,
        '(?m)^\s*private\s+object\s+(?<method>Activate_\w+)\s*\(\s*\)\s*\{\s*return\s+new\s+global::' +
        $taskEscapedType + '\s*\(\s*\)\s*;\s*\}')
    $taskCase = [regex]::Match($taskMetadata,
        '(?ms)^\s*case\s+\d+\s*:\s*//\s*' + $taskEscapedType + '\s*\r?\n(?<body>.*?^\s*break\s*;)')
    $taskTypeEntry = [regex]::IsMatch($taskMetadata,
        '_typeTable\[\d+\]\s*=\s*typeof\s*\(\s*global::' + $taskEscapedType + '\s*\)\s*;')
    $taskActivatorBound = $taskFactory.Success -and $taskCase.Success -and [regex]::IsMatch(
        $taskCase.Groups['body'].Value, '\buserType\.Activator\s*=\s*' +
        [regex]::Escape($taskFactory.Groups['method'].Value) + '\s*;')

    if (-not $taskTypeEntry -or -not $taskFactory.Success -or -not $taskActivatorBound) {
        $taskReason = @(
            if (-not $taskTypeEntry) { 'type table entry' }
            if (-not $taskFactory.Success) { 'generated constructor factory' }
            if (-not $taskActivatorBound) { 'IXamlType Activator assignment' }
        ) -join ', '
        $taskFailures.Add($taskType)
        Write-Host "FAIL navigation metadata: $taskType is missing $taskReason."
    }
    else {
        Write-Host "PASS navigation metadata: $taskType -> $($taskFactory.Groups['method'].Value)"
    }
}

if ($taskFailures.Count -gt 0) {
    throw "WinUI navigation metadata validation failed for: $($taskFailures -join ', '). Add the page to XAML compilation and rebuild."
}
Write-Host "WinUI navigation metadata validation passed: $($taskPageNames.Count) registered pages ($Platform/$Configuration)."
