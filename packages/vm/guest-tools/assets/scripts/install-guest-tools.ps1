param(
    [Parameter(Mandatory = $true)][string]$InteractiveUserName,
    [string]$RuntimeHelper = ''
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = New-Object Security.Principal.WindowsPrincipal($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Guest Tools installation requires administrator approval.'
}
if ([string]::IsNullOrWhiteSpace($InteractiveUserName) -or $InteractiveUserName.Length -gt 256) {
    throw 'The original interactive Windows username is required.'
}
$user = Get-LocalUser -Name $InteractiveUserName

function Assert-RegularPath {
    param([string]$Path)
    $current = [IO.Path]::GetFullPath($Path)
    while (-not [string]::IsNullOrWhiteSpace($current)) {
        if (Test-Path -LiteralPath $current) {
            $item = Get-Item -LiteralPath $current -Force
            if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) {
                throw 'Guest Tools paths must not contain redirected files or directories.'
            }
        }
        $parent = [IO.Directory]::GetParent($current)
        if ($null -eq $parent) {
            break
        }
        $current = $parent.FullName
    }
}

function Protect-ApplicationDirectory {
    param([string]$Path)
    Assert-RegularPath -Path $Path
    New-Item -ItemType Directory -Path $Path -Force | Out-Null
    $acl = New-Object Security.AccessControl.DirectorySecurity
    $acl.SetAccessRuleProtection($true, $false)
    $inheritance = [Security.AccessControl.InheritanceFlags]'ContainerInherit, ObjectInherit'
    $propagation = [Security.AccessControl.PropagationFlags]::None
    foreach ($sid in @('S-1-5-18', 'S-1-5-32-544')) {
        $rule = New-Object Security.AccessControl.FileSystemAccessRule(
            (New-Object Security.Principal.SecurityIdentifier($sid)),
            [Security.AccessControl.FileSystemRights]::FullControl,
            $inheritance, $propagation, [Security.AccessControl.AccessControlType]::Allow)
        $acl.AddAccessRule($rule)
    }
    $users = New-Object Security.AccessControl.FileSystemAccessRule(
        (New-Object Security.Principal.SecurityIdentifier('S-1-5-32-545')),
        [Security.AccessControl.FileSystemRights]::ReadAndExecute,
        $inheritance, $propagation, [Security.AccessControl.AccessControlType]::Allow)
    $acl.AddAccessRule($users)
    $acl.SetOwner((New-Object Security.Principal.SecurityIdentifier('S-1-5-32-544')))
    Set-Acl -LiteralPath $Path -AclObject $acl
}

function Read-VerifiedBundle {
    param([string]$Directory)
    Assert-RegularPath -Path $Directory
    $inventoryPath = Join-Path $Directory 'inventory.json'
    Assert-RegularPath -Path $inventoryPath
    if ((Get-Item -LiteralPath $inventoryPath).Length -gt 1048576) {
        throw 'Guest Tools inventory exceeds its supported size.'
    }
    $inventory = Get-Content -Raw -LiteralPath $inventoryPath | ConvertFrom-Json
    if ($inventory.schema -ne 1 -or $inventory.product -ne 'DockpipeGuestTools' -or
        $inventory.version -ne '1.0.0' -or $inventory.qt_version -ne '6.10.3') {
        throw 'Guest Tools inventory uses an unsupported contract.'
    }
    $files = @($inventory.files)
    if ($files.Count -eq 0 -or $files.Count -gt 5000) {
        throw 'Guest Tools inventory has an unsupported file count.'
    }
    $seen = New-Object 'Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
    [long]$total = 0
    foreach ($entry in $files) {
        $relative = [string]$entry.path
        if ([string]::IsNullOrWhiteSpace($relative) -or $relative.Contains('\') -or
            $relative.Contains(':') -or [IO.Path]::IsPathRooted($relative) -or
            @($relative.Split('/') | Where-Object { $_ -eq '..' -or $_ -eq '.' -or $_ -eq '' }).Count -gt 0 -or
            -not $seen.Add($relative) -or $entry.sha256 -notmatch '^[0-9a-f]{64}$' -or
            $entry.bytes -isnot [long] -and $entry.bytes -isnot [int]) {
            throw 'Guest Tools inventory contains an invalid file entry.'
        }
        [long]$size = $entry.bytes
        $total += $size
        if ($size -lt 0 -or $total -gt 268435456) {
            throw 'Guest Tools payload exceeds its supported size.'
        }
        $file = Join-Path $Directory $relative
        Assert-RegularPath -Path $file
        $item = Get-Item -LiteralPath $file -Force
        if ($item.PSIsContainer -or $item.Length -ne $size -or
            (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant() -ne $entry.sha256) {
            throw ('Guest Tools file integrity failed: ' + $relative)
        }
    }
    foreach ($required in @('dockpipe-guest-tools.exe', 'install-guest-tools.ps1', 'licenses/THIRD-PARTY.txt')) {
        if (-not $seen.Contains($required)) {
            throw ('Guest Tools bundle is missing ' + $required)
        }
    }
    return $inventory
}

$source = [IO.Path]::GetFullPath($PSScriptRoot)
Write-Output 'Verifying the Guest Tools application bundle.'
$inventory = Read-VerifiedBundle -Directory $source
$inventoryPath = Join-Path $source 'inventory.json'
$bundleId = (Get-FileHash -LiteralPath $inventoryPath -Algorithm SHA256).Hash.ToLowerInvariant().Substring(0, 16)
$applicationRoot = Join-Path $env:ProgramFiles 'Dockpipe\GuestTools'
Protect-ApplicationDirectory -Path $applicationRoot
$versions = Join-Path $applicationRoot 'versions'
Protect-ApplicationDirectory -Path $versions
$destination = Join-Path $versions $bundleId
if (Test-Path -LiteralPath $destination) {
    Read-VerifiedBundle -Directory $destination | Out-Null
} else {
    Write-Output 'Installing the verified application in Program Files.'
    $staging = Join-Path $versions ('pending-' + [guid]::NewGuid().ToString('N'))
    Protect-ApplicationDirectory -Path $staging
    foreach ($entry in $inventory.files) {
        $file = Join-Path $staging $entry.path
        $parent = Split-Path -Parent $file
        New-Item -ItemType Directory -Path $parent -Force | Out-Null
        Copy-Item -LiteralPath (Join-Path $source $entry.path) -Destination $file
    }
    Copy-Item -LiteralPath $inventoryPath -Destination (Join-Path $staging 'inventory.json')
    Read-VerifiedBundle -Directory $staging | Out-Null
    Move-Item -LiteralPath $staging -Destination $destination
}

$installedExecutable = Join-Path $destination 'dockpipe-guest-tools.exe'
$encodedUser = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($InteractiveUserName))
$action = New-ScheduledTaskAction -Execute $installedExecutable -Argument (
    '--startup --interactive-user-b64 ' + $encodedUser)
$trigger = New-ScheduledTaskTrigger -AtLogOn -User $user.SID.Value
$taskPrincipal = New-ScheduledTaskPrincipal -UserId $user.SID.Value -LogonType Interactive -RunLevel Limited
$settings = New-ScheduledTaskSettingsSet -StartWhenAvailable -AllowStartIfOnBatteries `
    -DontStopIfGoingOnBatteries -MultipleInstances IgnoreNew -ExecutionTimeLimit ([TimeSpan]::Zero)
Register-ScheduledTask -TaskName 'DockPipeGuestToolsUI' -Action $action -Trigger $trigger `
    -Principal $taskPrincipal -Settings $settings -Force | Out-Null

$repairRoot = Join-Path $env:ProgramData 'Dockpipe\GuestProvisioning'
$configuration = Join-Path $repairRoot 'configuration.json'
$repairTask = Get-ScheduledTask -TaskName 'DockPipeGuestProvisioning' -ErrorAction SilentlyContinue
if ((Test-Path -LiteralPath $configuration) -and $null -ne $repairTask) {
    Write-Output 'Starting the independently registered SYSTEM repair task.'
    Start-ScheduledTask -TaskName 'DockPipeGuestProvisioning'
} else {
    if ([string]::IsNullOrWhiteSpace($RuntimeHelper)) {
        $RuntimeHelper = Join-Path $repairRoot 'provision-windows-ssh.ps1'
    }
    Assert-RegularPath -Path $RuntimeHelper
    if (-not (Test-Path -LiteralPath $RuntimeHelper -PathType Leaf)) {
        throw 'The runtime-owned guest provisioning helper is required from the original Guest Tools media.'
    }
    $helperLiteral = $RuntimeHelper.Replace("'", "''")
    $userLiteral = $InteractiveUserName.Replace("'", "''")
    $command = "`$ErrorActionPreference='Stop'; & '$helperLiteral' -RegisterOnly -UserName '$userLiteral' -CreateUser:`$false -GrantAdministrators:`$false -EnablePasswordAuth:`$false -EnablePublicKeyAuth:`$true -InstallDockPipeAgent:`$true"
    $encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($command))
    $powershell = Join-Path $env:WINDIR 'System32\WindowsPowerShell\v1.0\powershell.exe'
    Write-Output 'Registering independent SYSTEM provisioning through the runtime helper.'
    & $powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -EncodedCommand $encoded
    if ($LASTEXITCODE -ne 0) {
        throw 'The runtime helper could not register durable guest provisioning.'
    }
}
$receipt = [ordered]@{
    schema = 1
    product = 'DockpipeGuestTools'
    version = '1.0.0'
    bundle = $bundleId
    executable = $installedExecutable
    ui_task = 'DockPipeGuestToolsUI'
    provisioning_task = 'DockPipeGuestProvisioning'
    provisioning_runs_as = 'SYSTEM'
}
$receipt | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $applicationRoot 'installation.json') -Encoding UTF8
Write-Output 'Guest Tools installed. SYSTEM provisioning continues independently of this window.'
