Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$producer = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../../../tooling/build_windows.py'))
& python $producer
if ($LASTEXITCODE -ne 0) {
    throw 'The native Guest Tools build or qualification failed.'
}
