# Implements the public MirrorChyan upload protocol using .NET TLS verification and streaming multipart.
# Protocol references: MirrorChyan/uploading-action@ea9a0f6f516fafb6b0a3046a1fe4b0dccb29eca8
# and release-note-action@f392b50c411981ee6d33c8ed5e91d66d84d56202.
[CmdletBinding()]
param([Parameter(Mandatory)][ValidatePattern('^v\d+\.\d+\.\d+\.\d+$')][string]$Tag, [switch]$NotesOnly)
$ErrorActionPreference='Stop'
if (-not $env:MIRRORCHYAN_TOKEN) { throw 'MirrorChyan environment token is missing.' }
. (Join-Path $PSScriptRoot 'CiReleaseMetadata.ps1')
$repoRoot=Split-Path -Parent $PSScriptRoot
$file=Invoke-CiGh @('api','repos/kahvia-d/IMAO/contents/updates/stable.json?ref=main') | ConvertFrom-Json
$bytes=[Convert]::FromBase64String(($file.content -replace '\s',''))
$envelope=[Text.Encoding]::UTF8.GetString($bytes) | ConvertFrom-Json
$catalog=[Text.Encoding]::UTF8.GetString([Convert]::FromBase64String($envelope.payload)) | ConvertFrom-Json
if ($catalog.app.url -cne "https://github.com/kahvia-d/IMAO/releases/tag/$Tag") { Write-Host 'A newer stable exists; skipped stale MirrorChyan job.'; exit 0 }
$root=Join-Path $repoRoot 'out/mirrorchyan'; [IO.Directory]::CreateDirectory($root) | Out-Null
$stable=Join-Path $root 'stable.json'; [IO.File]::WriteAllBytes($stable,$bytes)
& dotnet build (Join-Path $repoRoot 'tools/UpdatePublisher/UpdatePublisher.csproj') -c Release
if ($LASTEXITCODE -ne 0) { throw 'Public verifier build failed.' }
$dll=Join-Path $repoRoot 'tools/UpdatePublisher/bin/Release/net8.0/UpdatePublisher.dll'
& dotnet $dll verify-manifest --input $stable --public-key (Join-Path $repoRoot 'Assets/Updates/trusted-keys.json')
if ($LASTEXITCODE -ne 0) { throw 'Stable signature cannot be verified.' }
$release=Invoke-CiGh @('api',"repos/kahvia-d/IMAO/releases/tags/$Tag") | ConvertFrom-Json
if ($release.draft) { throw 'Cannot mirror a draft.' }
$name="IMao-$Tag-windows-x64.zip"
$assets=@($release.assets | Where-Object name -CEQ $name)
if (-not $NotesOnly -and $assets.Count -ne 1) { throw 'A unique authorized first-install archive is required.' }
if (-not $NotesOnly) {
    Invoke-CiGh @('release','download',$Tag,'--repo','kahvia-d/IMAO','--pattern',$name,'--pattern','release-authorization.json','--dir',$root) | Out-Null
    $archive=Join-Path $root $name
    Assert-CiArtifactDigest $archive $assets[0].digest
    & dotnet $dll verify-install-authorization --input (Join-Path $root 'release-authorization.json') --program-zip $archive --manifest $stable --public-key (Join-Path $repoRoot 'Assets/Updates/trusted-keys.json')
    if ($LASTEXITCODE -ne 0) { throw 'Installation ZIP lacks matching production authorization.' }
}
$handler=[Net.Http.HttpClientHandler]::new(); $handler.AllowAutoRedirect=$false
$client=[Net.Http.HttpClient]::new($handler); $client.Timeout=[TimeSpan]::FromMinutes(45)
function Send-MirrorRequest([string]$Step, [string]$Url, [string]$Method, [Net.Http.HttpContent]$Content, [bool]$Authorized=$true) {
    $request=[Net.Http.HttpRequestMessage]::new([Net.Http.HttpMethod]::new($Method),$Url)
    if ($Authorized) { $request.Headers.TryAddWithoutValidation('Authorization',$env:MIRRORCHYAN_TOKEN.Trim()) | Out-Null }
    $request.Content=$Content; $response=$null
    try {
        $response=$client.SendAsync($request).GetAwaiter().GetResult()
        if ([int]$response.StatusCode -ne 200) {
            # A bare "request failed" cost a debugging round trip: the step and the status code are
            # what identify a stale token, a rejected parameter or an already uploaded version. The
            # body is truncated and has the upload token scrubbed out of it before it is reported.
            $detail=''
            try { $detail=$response.Content.ReadAsStringAsync().GetAwaiter().GetResult() } catch { }
            if ($env:MIRRORCHYAN_TOKEN) { $detail=$detail.Replace($env:MIRRORCHYAN_TOKEN.Trim(),'<token>') }
            $detail=($detail -replace '\s+',' ').Trim()
            if ($detail.Length -gt 300) { $detail=$detail.Substring(0,300) }
            throw "MirrorChyan step '$Step' failed with HTTP $([int]$response.StatusCode). $detail"
        }
        $text=$response.Content.ReadAsStringAsync().GetAwaiter().GetResult()
        if ($text.StartsWith('{')) { return $text | ConvertFrom-Json }
    } finally { if ($response) { $response.Dispose() }; $request.Dispose() }
}
function New-MirrorForm([hashtable]$Values) {
    $dictionary=[Collections.Generic.Dictionary[string,string]]::new()
    foreach ($k in $Values.Keys) { $dictionary.Add($k,[string]$Values[$k]) }
    return [Net.Http.FormUrlEncodedContent]::new($dictionary)
}
try {
    if (-not $NotesOnly) {
        $downloadName="IMAO-$Tag-win-x64.zip"
        $data=@{name=$Tag;os='win';arch='x64';channel='stable';filename=$downloadName}
        $reservation=Send-MirrorRequest 'reserve version' 'https://mirrorchyan.com/api/resources/IMAO/versions' 'POST' (New-MirrorForm $data)
        $upload=$reservation.data
        $uri=[Uri]$upload.host
        if ($uri.Scheme -cne 'https' -or -not $uri.Host.EndsWith('.aliyuncs.com',[StringComparison]::OrdinalIgnoreCase) -or $uri.UserInfo -or $uri.Query) { throw 'Unexpected MirrorChyan object storage destination.' }
        $multipart=[Net.Http.MultipartFormDataContent]::new()
        $fields=@{success_action_status='200';name=$upload.name;signature=$upload.signature;key=$upload.key;policy=$upload.policy;OSSAccessKeyId=$upload.access_key;'Content-Disposition'="attachment; filename=`"$downloadName`""}
        foreach ($k in $fields.Keys) { $multipart.Add([Net.Http.StringContent]::new([string]$fields[$k]),$k) }
        $multipart.Add([Net.Http.StreamContent]::new([IO.File]::OpenRead($archive)),'file',$name)
        Send-MirrorRequest 'upload object' $uri.AbsoluteUri 'POST' $multipart $false | Out-Null
        $data.key=$upload.key
        $callback=Send-MirrorRequest 'upload callback' 'https://mirrorchyan.com/api/resources/IMAO/versions/callback' 'POST' (New-MirrorForm $data)
        if ($callback.data.status_key) {
            $complete=$false
            for ($i=0; $i -lt 60; $i++) {
                Start-Sleep -Seconds 5
                $status=Send-MirrorRequest 'processing status' ("https://mirrorchyan.com/api/resources/IMAO/versions/status?key="+[Uri]::EscapeDataString($callback.data.status_key)) 'GET' $null
                if ($status.data.status -eq 2) { $complete=$true; break }
                if ($status.data.status -ne 1) { throw 'MirrorChyan processing failed.' }
            }
            if (-not $complete) { throw 'MirrorChyan processing timed out; retry this mirror job.' }
        }
    }
    $body=@{version_name=$Tag;channel='stable';content=[string]$release.body} | ConvertTo-Json
    Send-MirrorRequest 'release note' 'https://mirrorchyan.com/api/resources/IMAO/versions/release-note' 'PUT' ([Net.Http.StringContent]::new($body,[Text.Encoding]::UTF8,'application/json')) | Out-Null
    Write-Host "MirrorChyan synchronized $Tag."
} finally { $client.Dispose(); $handler.Dispose() }
