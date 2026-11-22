# 构建并校验浏览器扩展的双商店发布包（Edge/手动版保留 key，Chrome 商店版自动剥离 key）。
# 本机与 GitHub Actions 共用同一入口，只产出打包文件与校验报告，不创建 GitHub Release。
[CmdletBinding()]
param(
    [string]$SourceRoot,
    [string]$OutputRoot
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if ($PSVersionTable.PSVersion.Major -lt 7) { throw '需要 PowerShell 7 或更高版本。' }
if (-not $SourceRoot) { $SourceRoot = Split-Path -Parent $PSScriptRoot }
$SourceRoot = [IO.Path]::GetFullPath($SourceRoot)
if (-not $OutputRoot) { $OutputRoot = Join-Path $SourceRoot 'out/browser-extension' }
$OutputRoot = [IO.Path]::GetFullPath($OutputRoot, $SourceRoot)
$extensionRoot = Join-Path $SourceRoot 'BrowserExtensions/KuroMapSync'

# 1. 若存在 Node.js，先做全部扩展脚本语法检查与 WebDAV 加解密/PROPFIND 离线自测
$node = Get-Command node -ErrorAction SilentlyContinue
if (-not $node) { throw '未找到 node，无法执行浏览器扩展脚本语法与加解密校验。' }

$jsFiles = Get-ChildItem -LiteralPath $extensionRoot -Filter '*.js' -File | Sort-Object Name
foreach ($js in $jsFiles) {
    & $node.Source --check $js.FullName
    if ($LASTEXITCODE -ne 0) { throw "JS 语法校验失败：$($js.Name)" }
}

$cryptoTest = @'
import * as cryptoMod from './BrowserExtensions/KuroMapSync/webdav-crypto.js';
import { parseAccountIdsFromPropfind } from './BrowserExtensions/KuroMapSync/webdav-client.js';

const salt = new Uint8Array([1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16]);
const nonce = new Uint8Array([21,22,23,24,25,26,27,28,29,30,31,32]);
const env = await cryptoMod.encryptEnvelopeWithMaterial({
  accountId: '10146974',
  version: 5,
  token: 'cross-platform-token-xyz',
  passphrase: 'shared-passphrase-2026',
  salt,
  nonce,
  iterations: 210000,
  sourceDevice: 'browser',
  displayName: '测试大号',
  userInfoJson: JSON.stringify({ userId: '10146974' }),
  timestampUtc: '2026-11-22T09:00:00.000Z'
});
const expectedCipher = '9MguaWAyovtiDyTcom+m4yKCBAmnyg57icxNOZdqoJUk/fMhmyLuo4Bo99nF6X9MXVniGhQ3N9F6ge9ogoSPEV+1qcN4ktzCoByebsUMMMNmydEN/3Kb2CmgmuIt/reaJ0r0mssiF14E8PNt33pPmRk+1uHsKWBSETTC1jLbzKwgi6pCk2i1S6VUsTWWtyY+G4I9VHd8Lrx6xA+fUe0=';
if (env.cipher.ciphertext !== expectedCipher) throw new Error('跨端测试向量密文不一致');
const dec = await cryptoMod.decryptEnvelope(env, 'shared-passphrase-2026');
if (dec.token !== 'cross-platform-token-xyz') throw new Error('解密结果不一致');
const ids = parseAccountIdsFromPropfind('<d:multistatus xmlns:d="DAV:"><d:response><d:href>/dav/imao-kuro-sync/v1/kuro_10146974.json</d:href></d:response></d:multistatus>');
if (ids.join(',') !== '10146974') throw new Error('PROPFIND 解析异常');
console.log('Extension JS checks passed.');
'@
Push-Location $SourceRoot
try {
    $cryptoTest | & $node.Source --input-type=module
    if ($LASTEXITCODE -ne 0) { throw '浏览器扩展 WebDAV 加密离线自测失败。' }
}
finally {
    Pop-Location
}

# 2. 调用既有确定性打包脚本生成保留 key 的 Edge/手动解压版归档
& (Join-Path $PSScriptRoot 'Publish-BrowserExtension.ps1') -SourceRoot $SourceRoot -OutputRoot $OutputRoot
if ($LASTEXITCODE -ne 0) { throw 'Publish-BrowserExtension.ps1 执行失败。' }

$manifest = Get-Content -LiteralPath (Join-Path $extensionRoot 'manifest.json') -Raw -Encoding UTF8 | ConvertFrom-Json
$version = [string]$manifest.version
$edgeZip = Join-Path $OutputRoot "IMao-KuroMapSync-$version.zip"
$chromeZip = Join-Path $OutputRoot "IMao-KuroMapSync-$version-chrome-store.zip"
$chromeSha = "$chromeZip.sha256"

foreach ($path in @($chromeZip, $chromeSha)) {
    if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path -Force }
}

# 3. 生成剥离 manifest.json 中 "key" 字段的 Chrome 商店专用确定性归档
$files = @(Get-ChildItem -LiteralPath $extensionRoot -Recurse -File |
    ForEach-Object { [IO.Path]::GetRelativePath($extensionRoot, $_.FullName).Replace('\', '/') } |
    Sort-Object -CaseSensitive)

$chromeManifest = Get-Content -LiteralPath (Join-Path $extensionRoot 'manifest.json') -Raw -Encoding UTF8 | ConvertFrom-Json -AsHashtable
$chromeManifest.Remove('key')
$chromeManifestBytes = [Text.UTF8Encoding]::new($false).GetBytes(($chromeManifest | ConvertTo-Json -Depth 10) + "`n")

$fixedTimestamp = [DateTimeOffset]::new([DateTime]::new(2026, 1, 1, 0, 0, 0, [DateTimeKind]::Utc))
$stream = [IO.File]::Open($chromeZip, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
try {
    $archive = [IO.Compression.ZipArchive]::new($stream, [IO.Compression.ZipArchiveMode]::Create, $false)
    try {
        foreach ($relative in $files) {
            $entry = $archive.CreateEntry($relative, [IO.Compression.CompressionLevel]::Optimal)
            $entry.LastWriteTime = $fixedTimestamp
            $target = $entry.Open()
            try {
                if ($relative -eq 'manifest.json') {
                    $target.Write($chromeManifestBytes, 0, $chromeManifestBytes.Length)
                } else {
                    $source = [IO.File]::OpenRead((Join-Path $extensionRoot $relative))
                    try { $source.CopyTo($target) } finally { $source.Dispose() }
                }
            } finally { $target.Dispose() }
        }
    } finally { $archive.Dispose() }
} finally { $stream.Dispose() }

# 复核 Chrome 商店包确实移除了 "key"
$verifyArchive = [IO.Compression.ZipFile]::OpenRead($chromeZip)
try {
    $manifestEntry = $verifyArchive.GetEntry('manifest.json')
    if (-not $manifestEntry) { throw 'Chrome 商店包缺少 manifest.json。' }
    $reader = [IO.StreamReader]::new($manifestEntry.Open(), [Text.UTF8Encoding]::new($false))
    try { $verifyJson = $reader.ReadToEnd() | ConvertFrom-Json -AsHashtable } finally { $reader.Dispose() }
    if ($verifyJson.ContainsKey('key')) { throw 'Chrome 商店包中不应包含 manifest key 字段。' }
} finally { $verifyArchive.Dispose() }

$chromeHash = (Get-FileHash -LiteralPath $chromeZip -Algorithm SHA256).Hash.ToLowerInvariant()
[IO.File]::WriteAllText($chromeSha, "$chromeHash  IMao-KuroMapSync-$version-chrome-store.zip`n", [Text.UTF8Encoding]::new($false))

Write-Host "Edge / Unpacked archive: $edgeZip"
Write-Host "Chrome Store archive:    $chromeZip"
Write-Host "  Chrome SHA256 $chromeHash"
