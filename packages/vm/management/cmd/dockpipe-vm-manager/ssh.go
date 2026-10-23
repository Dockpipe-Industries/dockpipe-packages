package main

import (
	"bytes"
	"context"
	"encoding/base64"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"unicode/utf16"
	"unicode/utf8"
)

type guestStatus struct {
	Schema               int    `json:"schema"`
	SSH                  bool   `json:"ssh"`
	Agent                bool   `json:"agent"`
	RepairRegistered     bool   `json:"repair_registered"`
	RepairRunning        bool   `json:"repair_running"`
	RepairResult         int64  `json:"repair_result"`
	ToolsState           string `json:"tools_state"`
	ToolsPhase           string `json:"tools_phase"`
	ToolsMessage         string `json:"tools_message"`
	ApplicationInstalled bool   `json:"application_installed"`
	MachineName          string `json:"machine_name"`
}

const readGuestStatus = `
$ErrorActionPreference='Stop'
$ProgressPreference='SilentlyContinue'
[Console]::OutputEncoding=[Text.UTF8Encoding]::new($false)
$repair=Get-ScheduledTask -TaskName 'DockPipeGuestProvisioning' -ErrorAction SilentlyContinue
$repairResult=-1
if ($repair) { $repairResult=(Get-ScheduledTaskInfo -TaskName $repair.TaskName).LastTaskResult }
$agentHealthy=$false
try {
    $health=Invoke-RestMethod -Uri ('http://127.0.0.1:__AGENT_PORT__/health') -TimeoutSec 2
    $agentHealthy=($health.name -eq 'dockpipe-guest-agent' -and $health.status -eq 'ready')
} catch {}
$state='unavailable'
$phase='Guest Tools status is not installed'
$message='Install or repair Dockpipe Guest Tools inside Windows.'
$statusPath=Join-Path $env:ProgramData 'Dockpipe\GuestToolsStatus\status.json'
if (Test-Path -LiteralPath $statusPath) {
    $file=Get-Item -LiteralPath $statusPath
    if ($file.Length -le 65536 -and -not ($file.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        $status=Get-Content -Raw -LiteralPath $statusPath | ConvertFrom-Json
        $updated=[DateTimeOffset]::MinValue
        $validTime=([string]$status.updated_at).Length -le 64 -and [DateTimeOffset]::TryParse(
            [string]$status.updated_at,[Globalization.CultureInfo]::InvariantCulture,
            [Globalization.DateTimeStyles]::None,[ref]$updated)
        if ($status.schema -eq 1 -and $validTime -and $updated -le [DateTimeOffset]::UtcNow.AddMinutes(5)) {
            $state=$status.state; $phase=$status.phase; $message=$status.message
        } elseif ($status.schema -eq 1) {
            $phase='Guest Tools status needs refresh'
            $message='Guest services status has an invalid update time.'
        }
    }
}
$appTask=Get-ScheduledTask -TaskName 'DockPipeGuestToolsUI' -ErrorAction SilentlyContinue
$receipt=Join-Path $env:ProgramFiles 'Dockpipe\GuestTools\installation.json'
[pscustomobject]@{
    schema=1
    ssh=((Get-Service -Name sshd).Status -eq 'Running')
    agent=$agentHealthy
    repair_registered=($null -ne $repair)
    repair_running=($repair -and $repair.State -eq 'Running')
    repair_result=[long]$repairResult
    tools_state=[string]$state
    tools_phase=[string]$phase
    tools_message=[string]$message
    application_installed=($null -ne $appTask -and (Test-Path -LiteralPath $receipt))
    machine_name=$env:COMPUTERNAME
} | ConvertTo-Json -Compress
`

func encodePowerShell(script string) string {
	units := utf16.Encode([]rune(script))
	encoded := make([]byte, len(units)*2)
	for index, unit := range units {
		binary.LittleEndian.PutUint16(encoded[index*2:], unit)
	}
	return base64.StdEncoding.EncodeToString(encoded)
}

func (manager manager) guestCommand(ctx context.Context, record sessionRecord, script string) ([]byte, error) {
	if record.ExecMode != "powershell" || record.SSHUser == "" || record.SSHKeyReference == "" {
		return nil, fmt.Errorf("Windows key-authenticated guest connection is not configured")
	}
	info, err := os.Lstat(record.SSHKeyReference)
	if err != nil || !info.Mode().IsRegular() || info.Size() > 128*1024 {
		return nil, fmt.Errorf("SSH credential reference is unavailable")
	}
	knownHosts := filepath.Join(manager.state, "ssh-hosts-"+record.RunID)
	if info, err := os.Lstat(knownHosts); err == nil && (!info.Mode().IsRegular() || info.Size() > 64*1024) {
		return nil, fmt.Errorf("SSH host identity storage is invalid")
	} else if err != nil && !os.IsNotExist(err) {
		return nil, err
	}
	executable, err := exec.LookPath("ssh")
	if err != nil {
		return nil, fmt.Errorf("OpenSSH client is required")
	}
	arguments := []string{
		"-o", "BatchMode=yes", "-o", "IdentitiesOnly=yes",
		"-o", "StrictHostKeyChecking=accept-new", "-o", "UserKnownHostsFile=" + knownHosts,
		"-o", "ConnectTimeout=2", "-o", "ConnectionAttempts=1",
		"-i", record.SSHKeyReference, "-p", strconv.Itoa(record.SSHPort),
		"-l", record.SSHUser, record.SSHHost,
		"powershell.exe", "-NoProfile", "-NonInteractive", "-EncodedCommand", encodePowerShell(script),
	}
	return runCommand(ctx, executable, arguments, manager.workspace)
}

func (manager manager) guestStatus(ctx context.Context, record sessionRecord) (guestStatus, error) {
	var status guestStatus
	script := replaceAgentPort(readGuestStatus, record.AgentPort)
	data, err := manager.guestCommand(ctx, record, script)
	if err != nil {
		return status, err
	}
	return decodeGuestStatus(data)
}

func decodeGuestStatus(data []byte) (guestStatus, error) {
	var status guestStatus
	if len(data) > 64*1024 || !utf8.Valid(data) {
		return status, fmt.Errorf("invalid guest health response size or encoding")
	}
	decoder := json.NewDecoder(bytes.NewReader(data))
	token, err := decoder.Token()
	if err != nil || token != json.Delim('{') {
		return status, fmt.Errorf("guest health response must be an object")
	}
	fields := make(map[string]json.RawMessage)
	for decoder.More() {
		token, err := decoder.Token()
		if err != nil {
			return status, err
		}
		name, ok := token.(string)
		if !ok {
			return status, fmt.Errorf("invalid guest health field")
		}
		if _, exists := fields[name]; exists {
			return status, fmt.Errorf("duplicate guest health field")
		}
		var value json.RawMessage
		if err := decoder.Decode(&value); err != nil {
			return status, err
		}
		fields[name] = value
	}
	token, err = decoder.Token()
	if err != nil || token != json.Delim('}') {
		return status, fmt.Errorf("invalid guest health object")
	}
	var extra any
	if decoder.Decode(&extra) != io.EOF || len(fields) != 11 {
		return status, fmt.Errorf("guest health fields or trailing data changed")
	}
	for _, name := range []string{"ssh", "agent", "repair_registered", "repair_running", "application_installed"} {
		value := string(bytes.TrimSpace(fields[name]))
		if value != "true" && value != "false" {
			return status, fmt.Errorf("guest health boolean field is missing or invalid")
		}
	}
	for name, maximum := range map[string]int{"tools_state": 32, "tools_phase": 4096, "tools_message": 4096, "machine_name": 128} {
		var value string
		raw, exists := fields[name]
		if !exists || string(raw) == "null" || json.Unmarshal(raw, &value) != nil || len(value) > maximum || strings.ContainsRune(value, 0) {
			return status, fmt.Errorf("guest health string field is missing or invalid")
		}
	}
	repairResult, hasRepairResult := fields["repair_result"]
	if !hasRepairResult || string(repairResult) == "null" {
		return status, fmt.Errorf("guest repair result is missing or invalid")
	}
	if string(fields["schema"]) != "1" || string(fields["repair_result"]) == "null" || json.Unmarshal(data, &status) != nil {
		return status, fmt.Errorf("unsupported guest health schema")
	}
	if status.RepairResult < -1 || status.RepairResult > 4294967295 {
		return status, fmt.Errorf("invalid guest repair result")
	}
	switch status.ToolsState {
	case "ready", "provisioning", "failed", "unavailable":
		return status, nil
	default:
		return status, fmt.Errorf("unsupported guest provisioning state")
	}
}

// Showing an already-running limited application goes through its user-owned
// pipe; the logon task alone deliberately keeps a ready application hidden.
const showGuestTools = `
Start-ScheduledTask -TaskName 'DockPipeGuestToolsUI'
$sid=[Security.Principal.WindowsIdentity]::GetCurrent().User.Value
$pipe=[IO.Pipes.NamedPipeClientStream]::new('.',('dockpipe-guest-clipboard-v1-'+$sid),[IO.Pipes.PipeDirection]::InOut,[IO.Pipes.PipeOptions]::Asynchronous)
try {
    $pipe.Connect(4000)
    $request=[Text.Encoding]::UTF8.GetBytes('{"schema":1,"operation":"show"}'+[char]10)
    $write=$pipe.WriteAsync($request,0,$request.Length)
    if (-not $write.Wait(1000)) { throw 'Guest Tools request timed out' }
    $null=$write.GetAwaiter().GetResult()
    $buffer=[byte[]]::new(4096)
    $output=[IO.MemoryStream]::new()
    $clock=[Diagnostics.Stopwatch]::StartNew()
    while ($true) {
        $remaining=1000-[int]$clock.ElapsedMilliseconds
        if ($remaining -le 0) { throw 'Guest Tools reply timed out' }
        $read=$pipe.ReadAsync($buffer,0,$buffer.Length)
        if (-not $read.Wait($remaining)) { throw 'Guest Tools reply timed out' }
        $count=$read.GetAwaiter().GetResult()
        if ($count -le 0 -or $output.Length+$count -gt 4096) { throw 'Guest Tools reply invalid' }
        $newline=[Array]::IndexOf($buffer,[byte]10,0,$count)
        $keep=if ($newline -ge 0) { $newline } else { $count }
        $output.Write($buffer,0,$keep)
        if ($newline -ge 0) { break }
    }
    $reply=[Text.Encoding]::UTF8.GetString($output.ToArray()) | ConvertFrom-Json
    if ($reply.schema -ne 1 -or $reply.success -ne $true) { throw 'Guest Tools request failed' }
} finally {
    $pipe.Dispose()
}
`
