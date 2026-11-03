# Shared by the test-tree refresh and its isolated filesystem regression.
function Assert-MapTestModelSource([string]$ModelsRoot) {
    foreach ($name in @('coordinate-yolo/coordinate.onnx', 'coordinate-yolo/model.json',
        'coordinate-yolo/onnxruntime.dll', 'coordinate-yolo/directml/onnxruntime.dll',
        'coordinate-yolo/directml/onnxruntime_providers_shared.dll', 'coordinate-yolo/directml/DirectML.dll')) {
        $path = Join-Path $ModelsRoot $name
        if (-not (Test-Path -LiteralPath $path -PathType Leaf) -or (Get-Item -LiteralPath $path).Length -lt 1) {
            throw "Required OCR model/runtime missing: $path"
        }
    }
}

function Sync-MapTestModels([string]$ModelsRoot, [string]$RunRoot, [switch]$DryRun) {
    Assert-MapTestModelSource $ModelsRoot
    $RunRoot = [IO.Path]::GetFullPath($RunRoot).TrimEnd('\', '/')
    $assets = Join-Path $RunRoot 'Assets'
    $destination = Join-Path $assets 'models'
    if ((Get-Item -LiteralPath $assets -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) {
        throw 'Assets itself is linked; refusing to write models through an external tree.'
    }
    $linked = (Test-Path -LiteralPath $destination) -and
        ((Get-Item -LiteralPath $destination -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)
    if ($linked) {
        Write-Host 'Materializing test-tree models; the previous junction target remains untouched.'
        if (-not $DryRun) {
            # Delete only this verified directory entry, never recursively follow the junction.
            if ([IO.Path]::GetFullPath($destination) -ne ($RunRoot + [IO.Path]::DirectorySeparatorChar + 'Assets' + [IO.Path]::DirectorySeparatorChar + 'models')) {
                throw 'Unexpected model destination'
            }
            [IO.Directory]::Delete($destination)
        }
    }
    if (-not $DryRun) { [IO.Directory]::CreateDirectory($destination) | Out-Null }
    $changed = 0; $checked = 0
    foreach ($file in Get-ChildItem -LiteralPath $ModelsRoot -Recurse -File) {
        $relative = [IO.Path]::GetRelativePath($ModelsRoot, $file.FullName)
        $target = Join-Path $destination $relative
        # Refuse nested destination links instead of silently writing through them.
        if (-not ($DryRun -and $linked)) {
            $probe = $target
            while ($probe -ne $destination) {
                if ((Test-Path -LiteralPath $probe) -and
                    ((Get-Item -LiteralPath $probe -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
                    throw "Linked entry inside model destination: $relative"
                }
                $probe = Split-Path -Parent $probe
            }
        }
        $hash = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash
        $matches = -not ($DryRun -and $linked) -and (Test-Path -LiteralPath $target -PathType Leaf) -and
            ((Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash -eq $hash)
        if (-not $matches) {
            $changed++
            if (-not $DryRun) {
                [IO.Directory]::CreateDirectory((Split-Path -Parent $target)) | Out-Null
                Copy-Item -LiteralPath $file.FullName -Destination $target -Force
            }
        }
        if (-not $DryRun -and (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash -ne $hash) {
            throw "OCR model payload differs after sync: $relative"
        }
        $checked++
    }
    if (-not $DryRun) { Assert-MapTestModelSource $destination }
    Write-Host "OCR payload: checked=$checked changed=$changed dryRun=$([bool]$DryRun)"
}
