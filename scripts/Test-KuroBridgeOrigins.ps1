[CmdletBinding()]
param([string]$BridgeExecutable = (Join-Path $PSScriptRoot '../x64/Release/KuroSyncBridge.exe'))
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Enter-DevEnvironment.ps1')

# Run the self-contained host in an isolated directory. An invalid protocol
# request proves the origin gate was passed without reading/writing credentials.
$testRoot = Join-Path ([IO.Path]::GetTempPath()) ('imao-bridge-origins-' + [guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($testRoot) | Out-Null
$testExe = Join-Path $testRoot 'KuroSyncBridge.exe'
Copy-Item -LiteralPath $BridgeExecutable -Destination $testExe
$settingsPath = Join-Path $testRoot 'KuroSyncBridge.settings.json'
$edge = 'chrome-extension://ohmikfaeobbffhlhoocklplniobcfdbg/'
$chrome = 'chrome-extension://elhbnijeefmmmempajadehknbckaopge/'
$unknown = 'chrome-extension://aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa/'

function Assert-Origin([string]$Origin, [bool]$Allowed, [string]$Label) {
    $info = [Diagnostics.ProcessStartInfo]::new($testExe)
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardInput = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    if ($Origin) { $info.ArgumentList.Add($Origin) }
    $process = [Diagnostics.Process]::Start($info)
    try {
        $output = [IO.MemoryStream]::new()
        $copy = $process.StandardOutput.BaseStream.CopyToAsync($output)
        $errors = $process.StandardError.ReadToEndAsync()
        $payload = [Text.Encoding]::UTF8.GetBytes('{}')
        try {
            $process.StandardInput.BaseStream.Write([BitConverter]::GetBytes($payload.Length))
            $process.StandardInput.BaseStream.Write($payload)
            $process.StandardInput.Close()
        } catch [IO.IOException] {
            # A rejected origin can close its pipe before the parent writes.
        }
        if (-not $process.WaitForExit(15000)) { $process.Kill(); throw "$Label timed out" }
        $null = $copy.GetAwaiter().GetResult()
        if ($process.ExitCode -ne 0 -or $errors.GetAwaiter().GetResult()) { throw "$Label host failed" }
        $bytes = $output.ToArray()
        $output.Dispose()
        if ($Allowed) {
            if ($bytes.Length -lt 5 -or [BitConverter]::ToInt32($bytes, 0) -ne $bytes.Length - 4) { throw "$Label origin was rejected" }
            $response = [Text.Encoding]::UTF8.GetString($bytes, 4, $bytes.Length - 4) | ConvertFrom-Json
            if ($response.accepted -ne $false -or $response.error -ne 'unsupported-version') { throw "$Label did not validate the invalid request" }
        } elseif ($bytes.Length -ne 0) { throw "$Label unauthorized origin was accepted" }
        Write-Host "PASS $Label"
    } finally { $process.Dispose() }
}

try {
    # Missing settings must use the built-in store IDs and write default settings.
    Assert-Origin $chrome $true 'Chrome with missing settings'
    Assert-Origin $edge $true 'Edge with generated settings'
    Assert-Origin $chrome $true 'Chrome with generated settings'
    Assert-Origin $unknown $false 'unknown extension'
    Assert-Origin ($chrome + 'extra') $false 'origin prefix spoof'
    Assert-Origin '' $false 'missing origin'
    $settings = Get-Content -LiteralPath $settingsPath -Raw | ConvertFrom-Json
    if ($settings.AllowedOrigins.Count -ne 2 -or $edge -notin $settings.AllowedOrigins -or $chrome -notin $settings.AllowedOrigins) {
        throw 'Generated settings do not contain exactly the two store origins'
    }
    @{ AllowedOrigin = $edge } | ConvertTo-Json | Set-Content -LiteralPath $settingsPath
    Assert-Origin $edge $true 'legacy single-origin settings'
    Assert-Origin $chrome $false 'legacy settings remain explicit until re-registration'
    @{ AllowedOrigin = $edge; AllowedOrigins = @() } | ConvertTo-Json | Set-Content -LiteralPath $settingsPath
    Assert-Origin $edge $false 'explicit empty list overrides legacy field'
    Set-Content -LiteralPath $settingsPath -Value '{broken'
    Assert-Origin $chrome $true 'corrupt settings recover built-in Chrome origin'
    Write-Host 'All bridge origin checks passed.'
} finally {
    # Delete only the three known test files; no recursive directory removal.
    foreach ($path in @($testExe, $settingsPath)) {
        if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path -Force }
    }
    [IO.Directory]::Delete($testRoot)
}
