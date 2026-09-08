param([string]$OutputDirectory = 'out/overlay-ui-native')
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$outputPath = [IO.Path]::GetFullPath((Join-Path $repo $OutputDirectory))
if (-not $outputPath.StartsWith($repo + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) { throw 'Output must be inside the workspace.' }
[IO.Directory]::CreateDirectory($outputPath) | Out-Null
$utf8 = [Text.UTF8Encoding]::new($false)
$manifest = @()
function Export-ExactBody([string]$RelativePath, [string]$Start, [string]$End, [string]$Name) {
    $sourcePath = Join-Path $repo $RelativePath
    $source = [IO.File]::ReadAllText($sourcePath)
    $begin = $source.IndexOf($Start, [StringComparison]::Ordinal)
    if ($begin -lt 0) { throw "Start sentinel missing: $Start" }
    $finish = if ($End) { $source.IndexOf($End, $begin, [StringComparison]::Ordinal) } else { $source.Length }
    if ($finish -lt $begin) { throw "End sentinel missing: $End" }
    $targetPath = Join-Path $outputPath $Name
    [IO.File]::WriteAllText($targetPath, $source.Substring($begin, $finish - $begin), $utf8)
    $script:manifest += [ordered]@{source=$RelativePath;sourceSha256=(Get-FileHash -LiteralPath $sourcePath -Algorithm SHA256).Hash;extracted=$Name;extractedSha256=(Get-FileHash -LiteralPath $targetPath -Algorithm SHA256).Hash;start=$Start;end=$End}
}
Export-ExactBody 'IMao-Core/src/ImguiDraw/Items/DrawMarkerInteraction.cpp' 'void AddRegion(' 'void DrawGesturePreview(' 'ToolbarBody.inc'
Export-ExactBody 'IMao-Core/src/ImguiDraw/InteractiveInterface/RuntimeStatusBar.cpp' 'namespace {' '' 'StatusBody.inc'
Export-ExactBody 'IMao-Core/src/ImguiDraw/ImGuiOverWindows.cpp' '    std::string FontsPath =' '    //IM_ASSERT(font != nullptr);' 'FontsBody.inc'
$layoutPath = Join-Path $repo 'IMao-Core/src/Runtime/OverlayPanelLayout.h'
$manifest += [ordered]@{source='IMao-Core/src/Runtime/OverlayPanelLayout.h';sourceSha256=(Get-FileHash -LiteralPath $layoutPath -Algorithm SHA256).Hash}
$glyphPath = Join-Path $repo 'IMao-Core/src/ImguiDraw/UiFontGlyphs.h'
$manifest += [ordered]@{source='IMao-Core/src/ImguiDraw/UiFontGlyphs.h';sourceSha256=(Get-FileHash -LiteralPath $glyphPath -Algorithm SHA256).Hash}
$manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $outputPath 'source-manifest.json') -Encoding utf8
$sources = @('IMao-Core/tests/OverlayUiRenderHarness.cpp','IMao-Core/src/Base/imgui_dx11/imgui.cpp','IMao-Core/src/Base/imgui_dx11/imgui_draw.cpp','IMao-Core/src/Base/imgui_dx11/imgui_tables.cpp','IMao-Core/src/Base/imgui_dx11/imgui_widgets.cpp')
$quotedSources = ($sources | ForEach-Object { '"' + $_ + '"' }) -join ' '
$buildLog = Join-Path $outputPath 'build.log'
$command = 'call "C:\VSBuildTools-Current\VC\Auxiliary\Build\vcvars64.bat" >nul && cl /nologo /std:c++20 /EHsc /O2 /MT /utf-8 /DUNICODE /D_UNICODE /DIMGUI_DISABLE_OBSOLETE_FUNCTIONS /I "IMao-Core/src" /I "IMao-Core/packages/nlohmann" /I "' + $outputPath + '" ' + $quotedSources + ' /Fo"' + $outputPath + '\\" /Fe"' + (Join-Path $outputPath 'OverlayUiRenderHarness.exe') + '" > "' + $buildLog + '" 2>&1'
$start = [Diagnostics.ProcessStartInfo]::new()
$start.FileName = Join-Path $env:SystemRoot 'System32/cmd.exe'
$start.Arguments = '/d /c ' + $command
$start.UseShellExecute = $false; $start.CreateNoWindow = $true; $start.WorkingDirectory = $repo
[void]$start.EnvironmentVariables.Remove('Path');[void]$start.EnvironmentVariables.Remove('PATH')
$start.EnvironmentVariables['PATH'] = [Environment]::GetEnvironmentVariable('Path','Process')
$process = [Diagnostics.Process]::Start($start);$process.WaitForExit()
if ($process.ExitCode -ne 0) { Get-Content -LiteralPath $buildLog -Tail 40; throw "UI harness compilation failed: $($process.ExitCode)" }
Push-Location $repo
try { & (Join-Path $outputPath 'OverlayUiRenderHarness.exe') $outputPath; $result = $LASTEXITCODE } finally { Pop-Location }
Add-Type -AssemblyName System.Drawing
foreach ($bitmapPath in [IO.Directory]::EnumerateFiles($outputPath, '*.bmp')) {
    $bitmap = [Drawing.Bitmap]::new($bitmapPath)
    try { $bitmap.Save([IO.Path]::ChangeExtension($bitmapPath, '.png'), [Drawing.Imaging.ImageFormat]::Png) } finally { $bitmap.Dispose() }
}
Write-Output "UI fixture render complete: $outputPath (exit=$result)"
exit $result
