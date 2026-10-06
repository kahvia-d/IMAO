# Administrative bootstrap; this does not publish or read the production signing key.
[CmdletBinding()]
param([string]$GiteeTokenFile=(Join-Path $env:LOCALAPPDATA 'WWMAP-TOOLS-Publisher/gitee-token.txt'), [string]$MirrorChyanTokenFile)
$ErrorActionPreference='Stop'
$repo='kahvia-d/IMAO'
$owner=& gh api users/kahvia-d --jq '.id'
if ($LASTEXITCODE -ne 0) { throw 'Cannot resolve production reviewer.' }
$root=Join-Path $env:LOCALAPPDATA ('WWMAP-TOOLS-Publisher/ci-setup/'+[guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($root) | Out-Null
foreach ($name in @('production','gitee','mirrorchyan')) {
    $policy=@{deployment_branch_policy=@{protected_branches=$false;custom_branch_policies=$true}}
    if ($name -eq 'production') { $policy.reviewers=@(@{type='User';id=[long]$owner}); $policy.prevent_self_review=$false; $policy.can_admins_bypass=$false }
    $file=Join-Path $root "$name.json"; [IO.File]::WriteAllText($file,($policy | ConvertTo-Json -Depth 10),[Text.UTF8Encoding]::new($false))
    & gh api "repos/$repo/environments/$name" --method PUT --input $file | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "Cannot configure $name Environment." }
    $rules=(& gh api "repos/$repo/environments/$name/deployment-branch-policies" | ConvertFrom-Json).branch_policies
    if ($LASTEXITCODE -ne 0) { throw 'Cannot inspect Environment branch restriction.' }
    if (-not @($rules | Where-Object { $_.name -ceq 'main' -and $_.type -ceq 'branch' }).Count) {
        & gh api "repos/$repo/environments/$name/deployment-branch-policies" --method POST -f name=main -f type=branch | Out-Null
        if ($LASTEXITCODE -ne 0) { throw 'Cannot restrict Environment to main.' }
    }
}
function Set-CiEnvironmentSecret([string]$Name,[string]$Environment,[string]$TokenFile) {
    if (-not (Test-Path $TokenFile)) { throw "Token file not found: $TokenFile" }
    $start=[Diagnostics.ProcessStartInfo]::new('gh'); $start.UseShellExecute=$false; $start.CreateNoWindow=$true; $start.RedirectStandardInput=$true; $start.RedirectStandardOutput=$true; $start.RedirectStandardError=$true
    foreach ($arg in @('secret','set',$Name,'--repo',$repo,'--env',$Environment)) { $start.ArgumentList.Add($arg) }
    $p=[Diagnostics.Process]::Start($start)
    try {
        $p.StandardInput.Write([IO.File]::ReadAllText($TokenFile).Trim()); $p.StandardInput.Close()
        $stdout=$p.StandardOutput.ReadToEndAsync(); $stderr=$p.StandardError.ReadToEndAsync(); $p.WaitForExit()
        if ($p.ExitCode -ne 0) { throw 'Environment secret upload failed; diagnostics suppressed.' }
    } finally { $p.Dispose() }
}
if ($GiteeTokenFile) { Set-CiEnvironmentSecret 'GITEE_TOKEN' 'gitee' $GiteeTokenFile }
if ($MirrorChyanTokenFile) {
    Set-CiEnvironmentSecret 'MIRRORCHYANUPLOADTOKEN' 'mirrorchyan' $MirrorChyanTokenFile
    & gh secret delete MIRRORCHYANUPLOADTOKEN --repo $repo
    if ($LASTEXITCODE -ne 0) { throw 'New Environment secret exists, but legacy repository secret removal failed.' }
} else { Write-Warning 'MirrorChyan upload token path is required to migrate the existing repository Secret; never substitute a consumer CDK.' }
& gh api "repos/$repo/immutable-releases" --method PUT | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'Cannot enable immutable releases.' }
$immutability=& gh api "repos/$repo/immutable-releases" | ConvertFrom-Json
if ($LASTEXITCODE -ne 0 -or -not $immutability.enabled) { throw 'Release immutability not confirmed.' }
Write-Host 'Environment branch restrictions, production manual reviewer and release immutability configured. Production signing key was not accessed.'
