[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [ValidateSet('LowerVault', 'Darkplain', 'TimeRiftRuins')]
    [string]$Scene,

    [Parameter(Mandatory)]
    [ValidatePattern('^[A-Za-z0-9_-]+$')]
    [string]$Sample,

    [ValidateRange(1, 60)]
    [int]$MaxSeconds = 10
)

$repositoryRoot = Split-Path -Parent $PSScriptRoot
$assistantPath = Join-Path $repositoryRoot 'x64\Release\IMaoCaptureAssistant.exe'
$outputRoot = Join-Path $repositoryRoot 'Assets\KuroMap\captures'

if (-not (Test-Path -LiteralPath $assistantPath -PathType Leaf)) {
    throw "未找到采集助手：$assistantPath。请先构建 IMaoCaptureAssistant。"
}

& $assistantPath --scene $Scene --sample $Sample --output $outputRoot --max-seconds $MaxSeconds
exit $LASTEXITCODE
