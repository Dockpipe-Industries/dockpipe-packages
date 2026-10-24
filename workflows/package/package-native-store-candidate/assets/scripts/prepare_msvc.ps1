param(
    [Parameter(Mandatory = $true)][string]$WorkDirectory,
    [Parameter(Mandatory = $true)][string]$AcquisitionFile
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Get-ContainedFile([string]$Path, [string]$Root) {
    $fullRoot = [IO.Path]::GetFullPath($Root).TrimEnd('\', '/')
    $fullPath = [IO.Path]::GetFullPath($Path)
    if (-not $fullPath.StartsWith($fullRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Hosted tool must be contained in the explicit installation root.'
    }
    $item = Get-Item -LiteralPath $fullPath
    if ($item -isnot [IO.FileInfo]) {
        throw 'Hosted tool must be a regular file.'
    }
    $ancestor = $item
    while ($null -ne $ancestor) {
        if ($ancestor.Attributes -band [IO.FileAttributes]::ReparsePoint) {
            throw 'Hosted tool path contains a reparse point.'
        }
        if ($ancestor.FullName -eq $fullRoot) {
            break
        }
        if ($ancestor -is [IO.DirectoryInfo]) {
            $ancestor = $ancestor.Parent
        } else {
            $ancestor = $ancestor.Directory
        }
    }
    return $item
}

function Assert-MicrosoftBinary([string]$Path, [string]$Root, [string]$ExpectedHash = '', [string]$ExpectedVersion = '') {
    $item = Get-ContainedFile $Path $Root
    if ($item.Length -gt 128MB) {
        throw 'Hosted native binary exceeds its bound.'
    }
    $hash = (Get-FileHash -LiteralPath $item.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($ExpectedHash -and $hash -ne $ExpectedHash) {
        throw 'Hosted binary differs from the official payload hash.'
    }
    $version = [Diagnostics.FileVersionInfo]::GetVersionInfo($item.FullName)
    $fileVersion = '{0}.{1}.{2}.{3}' -f $version.FileMajorPart, $version.FileMinorPart, $version.FileBuildPart, $version.FilePrivatePart
    if ($ExpectedVersion -and $fileVersion -ne $ExpectedVersion) {
        throw 'Hosted PE file version differs.'
    }
    $stream = [IO.File]::OpenRead($item.FullName)
    $reader = [IO.BinaryReader]::new($stream)
    try {
        $stream.Position = 0x3c
        $offset = $reader.ReadUInt32()
        if ($offset -gt $item.Length - 6) {
            throw 'Invalid hosted PE header offset.'
        }
        $stream.Position = $offset
        if ($reader.ReadUInt32() -ne 0x00004550 -or $reader.ReadUInt16() -ne 0x8664) {
            throw 'Hosted native binary must be an x64 PE file.'
        }
    } finally {
        $reader.Dispose()
    }
    $signature = Get-AuthenticodeSignature -LiteralPath $item.FullName
    if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch 'Microsoft Corporation') {
        throw 'Hosted native binary Microsoft Authenticode admission failed.'
    }
    return $hash
}

if ($env:GITHUB_ACTIONS -ne 'true' -or $env:RUNNER_ENVIRONMENT -ne 'github-hosted' -or
    $env:RUNNER_OS -ne 'Windows' -or $env:RUNNER_ARCH -ne 'X64' -or
    $env:GITHUB_REPOSITORY -ne 'Dockpipe-Industries/dockpipe-packages') {
    throw 'Compiler admission is restricted to the owning GitHub-hosted Windows job.'
}
if (-not [IO.Path]::IsPathRooted($WorkDirectory) -or (Test-Path -LiteralPath $WorkDirectory)) {
    throw 'Use a fresh explicit hosted compiler receipt workspace.'
}
$source = [IO.Path]::GetFullPath($env:GITHUB_WORKSPACE)
$expectedFile = Join-Path $source 'release/native-compiler/windows-hosted-19.51.json'
if ([IO.Path]::GetFullPath($AcquisitionFile) -ne [IO.Path]::GetFullPath($expectedFile)) {
    throw 'Select the source-bound hosted compiler descriptor.'
}
$descriptor = Get-Content -Raw -LiteralPath $AcquisitionFile | ConvertFrom-Json
if ($descriptor.schema -ne 1 -or $descriptor.acquisition_mode -ne 'hosted-exact' -or
    $descriptor.runner_label -ne 'windows-2025-vs2026' -or
    $descriptor.toolset_version -ne '14.51.36231' -or
    $descriptor.compiler_version -ne '19.51.36260.0' -or
    $descriptor.windows_sdk_version -ne '10.0.26100.0') {
    throw 'Unsupported exact hosted Microsoft compiler profile.'
}
$installation = Join-Path $env:ProgramFiles 'Microsoft Visual Studio/18/Enterprise'
$toolset = Join-Path $installation 'VC/Tools/MSVC/14.51.36231'
$directory = Join-Path $toolset 'bin/Hostx64/x64'
$compiler = Join-Path $directory 'cl.exe'
$compilerHash = Assert-MicrosoftBinary $compiler $installation $descriptor.compiler_payload.compiler_sha256 $descriptor.compiler_version
$runtimeDirectory = Join-Path $installation $descriptor.desktop_runtime.directory
$runtimeFiles = [ordered]@{}
foreach ($record in $descriptor.desktop_runtime.files) {
    $file = Join-Path $runtimeDirectory $record.name
    $hash = Assert-MicrosoftBinary $file $installation $record.sha256 $record.version
    if ((Get-Item -LiteralPath $file).Length -ne $record.size_bytes) {
        throw 'Hosted desktop CRT size differs.'
    }
    $runtimeFiles[$record.name] = $record
}
# Initialize from a clean compiler include/library environment. This does not
# modify machine settings or install a toolchain.
$previousCompilerVariables = @('INCLUDE', 'LIB', 'LIBPATH', 'VSINSTALLDIR', 'VCINSTALLDIR',
    'VisualStudioVersion', 'VCToolsInstallDir', 'VCToolsVersion', 'VCToolsRedistDir',
    'WindowsSdkDir', 'WindowsSDKVersion')
$previousCompilerVariables += [Environment]::GetEnvironmentVariables('Process').Keys | Where-Object {
    $_ -like 'VSCMD_*' -or $_ -like '__VSCMD_*'
}
foreach ($name in $previousCompilerVariables) {
    [Environment]::SetEnvironmentVariable($name, $null, 'Process')
}
$vcvars = (Get-ContainedFile (Join-Path $installation 'VC/Auxiliary/Build/vcvars64.bat') $installation).FullName
$environment = & $env:ComSpec /d /c ('call "' + $vcvars + '" 10.0.26100.0 -vcvars_ver=14.51.36231 >nul && set')
if ($LASTEXITCODE -ne 0) {
    throw 'Exact hosted MSVC environment initialization failed.'
}
$allowed = @('PATH', 'INCLUDE', 'LIB', 'LIBPATH', 'VCToolsInstallDir', 'VCToolsVersion',
    'WindowsSdkDir', 'WindowsSDKVersion', 'VCToolsRedistDir')
foreach ($line in $environment) {
    if ($line -match '^([^=]+)=(.*)$' -and $allowed -contains $matches[1]) {
        if ($matches[2] -match '[\r\n]') {
            throw 'Invalid hosted compiler environment value.'
        }
        [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process')
    }
}
$sdk = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits/10'
if ([IO.Path]::GetFullPath($env:VCToolsInstallDir).TrimEnd('\', '/') -ne [IO.Path]::GetFullPath($toolset) -or
    $env:VCToolsVersion.TrimEnd('\', '/') -ne '14.51.36231' -or
    [IO.Path]::GetFullPath($env:WindowsSdkDir).TrimEnd('\', '/') -ne [IO.Path]::GetFullPath($sdk) -or
    $env:WindowsSDKVersion.TrimEnd('\', '/') -ne '10.0.26100.0' -or
    [IO.Path]::GetFullPath($env:VCToolsRedistDir).TrimEnd('\', '/') -ne
        [IO.Path]::GetFullPath((Split-Path (Split-Path $runtimeDirectory -Parent) -Parent))) {
    throw 'vcvars selected a different toolset or Windows SDK; no latest fallback is allowed.'
}
$tools = [ordered]@{}
foreach ($name in @('cl', 'dumpbin', 'lib', 'link')) {
    $file = Join-Path $directory ($name + '.exe')
    $hash = Assert-MicrosoftBinary $file $installation
    if ((Get-Command ($name + '.exe') -CommandType Application).Source -ne [IO.Path]::GetFullPath($file)) {
        throw 'Hosted compiler command resolves outside the selected toolset.'
    }
    $tools[$name] = @{
        path = $file.Substring($installation.Length + 1).Replace('\', '/')
        sha256 = $hash
    }
}
$sdkTools = [ordered]@{}
foreach ($name in @('rc', 'mt')) {
    $file = Join-Path $sdk ('bin/10.0.26100.0/x64/' + $name + '.exe')
    $hash = Assert-MicrosoftBinary $file $sdk
    $sdkTools[$name] = @{
        path = 'bin/10.0.26100.0/x64/' + $name + '.exe'
        sha256 = $hash
    }
    if ((Get-Command ($name + '.exe') -CommandType Application).Source -ne [IO.Path]::GetFullPath($file)) {
        throw 'Hosted Windows SDK command resolves outside the selected SDK.'
    }
}
$start = [Diagnostics.ProcessStartInfo]::new()
$start.FileName = $compiler
$start.Arguments = '/Bv'
$start.UseShellExecute = $false
$start.RedirectStandardOutput = $true
$start.RedirectStandardError = $true
$observation = [Diagnostics.Process]::Start($start)
$stdout = $observation.StandardOutput.ReadToEndAsync()
$stderr = $observation.StandardError.ReadToEndAsync()
if (-not $observation.WaitForExit(15000)) {
    $observation.Kill()
    throw 'Native compiler version observation timed out.'
}
$observed = $stdout.GetAwaiter().GetResult() + $stderr.GetAwaiter().GetResult()
$observation.Dispose()
if ($observed.Length -gt 65536 -or $observed -notmatch 'Compiler Version 19\.51\.36260\b') {
    throw 'Hosted compiler execution differs from the exact admitted ABI.'
}
New-Item -ItemType Directory -Path $WorkDirectory | Out-Null
$hostRuntime = @{
    version = $descriptor.desktop_runtime.version
    source_sha256 = $descriptor.desktop_runtime.sha256
    directory = $descriptor.desktop_runtime.directory
    files = $runtimeFiles
}
$receipt = Join-Path $WorkDirectory 'compiler-receipt.json'
$receiptData = @{
    schema = 1
    acquisition_mode = 'hosted-exact'
    compiler_id = 'MSVC'
    compiler_version = '19.51.36260.0'
    windows_sdk_version = '10.0.26100.0'
    acquisition_sha256 = (Get-FileHash -LiteralPath $AcquisitionFile -Algorithm SHA256).Hash.ToLowerInvariant()
    installation = $installation
    observed_image = @{
        image_os = [string]$env:ImageOS
        image_version = [string]$env:ImageVersion
    }
    tools = $tools
    sdk_tools = $sdkTools
    host_runtime = $hostRuntime
}
$receiptData | ConvertTo-Json -Depth 8 | Out-File -LiteralPath $receipt -Encoding utf8
foreach ($name in $allowed) {
    $value = [Environment]::GetEnvironmentVariable($name, 'Process')
    "$name=$value" | Out-File -LiteralPath $env:GITHUB_ENV -Append -Encoding utf8
}
"DOCKPIPE_NATIVE_MSVC_ROOT=$installation" | Out-File -LiteralPath $env:GITHUB_ENV -Append -Encoding utf8
"DOCKPIPE_NATIVE_MSVC_RECEIPT=$receipt" | Out-File -LiteralPath $env:GITHUB_ENV -Append -Encoding utf8
"DOCKPIPE_NATIVE_CRT_DIR=$runtimeDirectory" | Out-File -LiteralPath $env:GITHUB_ENV -Append -Encoding utf8
