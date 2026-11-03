$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'MapTestModelPayload.ps1')
$root=Join-Path ([IO.Path]::GetTempPath()) ('IMao-model-sync-'+[guid]::NewGuid().ToString('N'))
$source=Join-Path $root 'source';$run=Join-Path $root 'run';$old=Join-Path $root 'old-diagnostic-payload'
[IO.Directory]::CreateDirectory((Join-Path $run 'Assets'))|Out-Null
[IO.Directory]::CreateDirectory($old)|Out-Null
[IO.File]::WriteAllText((Join-Path $old 'keep.txt'),'outside target sentinel')
foreach($name in @('coordinate-yolo/coordinate.onnx','coordinate-yolo/model.json','coordinate-yolo/onnxruntime.dll','coordinate-yolo/directml/onnxruntime.dll','coordinate-yolo/directml/onnxruntime_providers_shared.dll','coordinate-yolo/directml/DirectML.dll')) {
 $path=Join-Path $source $name;[IO.Directory]::CreateDirectory((Split-Path $path))|Out-Null
 [IO.File]::WriteAllText($path,('fixture '+$name))
}
$models=Join-Path $run 'Assets/models'
New-Item -ItemType Junction -Path $models -Target $old|Out-Null
Sync-MapTestModels $source $run -DryRun
if(-not (Get-Item $models).LinkType){throw 'Dry run detached link'}
Sync-MapTestModels $source $run
if((Get-Item $models).LinkType){throw 'Model payload still linked'}
if((Get-Content (Join-Path $old 'keep.txt')) -ne 'outside target sentinel'){throw 'Old target modified'}
if((Get-ChildItem $old -Force).Count -ne 1){throw 'Wrote through junction'}
Sync-MapTestModels $source $run
$rejected=$false
try {Assert-MapTestModelSource $old} catch {$rejected=$true}
if(-not $rejected){throw 'Missing model accepted'}
Write-Host 'PASS: dry run, junction materialization, outside-target preservation, full hashes, repeat sync, missing model rejection'
# No recursive removal: retain this tiny isolated fixture as test evidence.
