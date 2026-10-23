param(
    [Parameter(Mandatory = $true)][string]$BundlePath,
    [Parameter(Mandatory = $true)][string]$InteractiveUserName,
    [Parameter(Mandatory = $true)][string]$OutputDirectory
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if (Test-Path -LiteralPath $OutputDirectory) {
    throw 'Installer integrity tests require a fresh output directory.'
}
$tokens = $null
$errors = $null
[Management.Automation.Language.Parser]::ParseFile(
    (Join-Path $BundlePath 'install-guest-tools.ps1'), [ref]$tokens, [ref]$errors) | Out-Null
if ($errors.Count -ne 0) {
    throw ($errors | Out-String)
}
$inventoryPath = Join-Path $BundlePath 'inventory.json'
$inventory = Get-Content -Raw -LiteralPath $inventoryPath | ConvertFrom-Json
foreach ($entry in $inventory.files) {
    $path = Join-Path $BundlePath $entry.path
    if ((Get-Item -LiteralPath $path).Length -ne $entry.bytes -or
        (Get-FileHash -LiteralPath $path).Hash.ToLowerInvariant() -ne $entry.sha256) {
        throw ('Bundle integrity mismatch: ' + $entry.path)
    }
}
New-Item -ItemType Directory -Path $OutputDirectory | Out-Null
$corrupt = Join-Path $OutputDirectory 'corrupt-bundle'
Copy-Item -LiteralPath $BundlePath -Destination $corrupt -Recurse
$stream = [IO.File]::Open((Join-Path $corrupt 'dockpipe-guest-tools.exe'), [IO.FileMode]::Append)
try {
    $stream.WriteByte(1)
} finally {
    $stream.Dispose()
}
$before = ''
if ($null -ne (Get-ScheduledTask -TaskName 'DockPipeGuestToolsUI' -ErrorAction SilentlyContinue)) {
    $before = Export-ScheduledTask -TaskName 'DockPipeGuestToolsUI'
}
$arguments = @('-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File',
    ('"' + (Join-Path $corrupt 'install-guest-tools.ps1') + '"'),
    '-InteractiveUserName', ('"' + $InteractiveUserName + '"'))
$process = Start-Process -FilePath powershell.exe -ArgumentList $arguments -PassThru -Wait `
    -RedirectStandardOutput (Join-Path $OutputDirectory 'rejection.stdout.log') `
    -RedirectStandardError (Join-Path $OutputDirectory 'rejection.stderr.log')
$after = ''
if ($null -ne (Get-ScheduledTask -TaskName 'DockPipeGuestToolsUI' -ErrorAction SilentlyContinue)) {
    $after = Export-ScheduledTask -TaskName 'DockPipeGuestToolsUI'
}
$errorText = Get-Content -Raw -LiteralPath (Join-Path $OutputDirectory 'rejection.stderr.log')
if ($process.ExitCode -eq 0 -or $before -ne $after -or
    $errorText -notmatch 'Guest Tools file integrity failed: dockpipe-guest-tools.exe') {
    throw 'Corrupt executable was not rejected before installation/task registration.'
}
$result = [ordered]@{
    schema = 1
    platform = 'windows-amd64'
    installer_parse = 'passed'
    files_verified = @($inventory.files).Count
    inventory_sha256 = (Get-FileHash -LiteralPath $inventoryPath).Hash.ToLowerInvariant()
    corrupt_executable_rejected = $true
    ui_task_unchanged = $true
}
$result | ConvertTo-Json | Tee-Object -FilePath (Join-Path $OutputDirectory 'result.json')
