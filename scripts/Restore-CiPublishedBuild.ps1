# Cloud-only recovery when the frozen build artifact has expired after public release publication.
function Restore-CiPublishedBuild($Approval, $Release, [string]$RequestRoot, [string]$PreparedRoot) {
    if ($env:GITHUB_ACTIONS -ne 'true' -or $Release.draft -or $Release.tag_name -cne $Approval.tag) { throw 'Expired artifacts may only be recovered in cloud from the original public release.' }
    $source=Invoke-CiGh @('api',"repos/kahvia-d/IMAO/commits/$($Approval.tag)",'--jq','.sha')
    if ($source -cne $Approval.sourceCommit) { throw 'Original release tag differs from approved source.' }
    $assets=Get-Content (Join-Path $RequestRoot 'asset-inventory.json') -Raw | ConvertFrom-Json
    [IO.Directory]::CreateDirectory($PreparedRoot) | Out-Null
    foreach ($asset in @($assets | Where-Object { $null -ne $_.path })) {
        if ($asset.path -notmatch '^(packages|program|manual)/[^/\\:]+$' -or $asset.name -cne [IO.Path]::GetFileName($asset.path)) { throw 'Unsafe recovery path.' }
        $uri=[Uri]$asset.url
        if ($uri.Scheme -cne 'https' -or $uri.Host -cne 'github.com' -or $uri.UserInfo -or $uri.Query -or $uri.AbsolutePath -notmatch '^/(kahvia-d/(?:IMAO|WWMAP-TOOLS))/releases/download/([^/]+)/([^/]+)$') { throw 'Unsafe recovery URL.' }
        $repo=$Matches[1]; $tag=$Matches[2]; $name=[Uri]::UnescapeDataString($Matches[3])
        if ($name -cne $asset.name) { throw 'Recovery attachment name differs.' }
        $root=Join-Path $PreparedRoot ([IO.Path]::GetDirectoryName($asset.path)); [IO.Directory]::CreateDirectory($root) | Out-Null
        # Some unchanged packages were built and verified in cloud but retain their original release URL.
        Invoke-CiGh @('release','download',$tag,'--repo',$repo,'--pattern',$name,'--dir',$root) | Out-Null
        $file=Join-Path $PreparedRoot $asset.path
        if ((Get-Item $file).Length -ne $asset.size -or (Get-FileHash $file -Algorithm SHA256).Hash.ToLowerInvariant() -cne $asset.sha256) { throw 'Published recovery bytes differ from signed inventory.' }
    }
    Copy-Item (Join-Path $RequestRoot 'preparation-report.json') (Join-Path $PreparedRoot 'release-report.json')
    Copy-Item (Join-Path $RequestRoot 'catalog-payload.json') (Join-Path $PreparedRoot 'catalog-draft.json')
}
