package main

import (
	"bytes"
	"context"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"errors"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"unicode/utf8"
)

const clipboardTextLimit = 1048576
const clipboardWireLimit = 1572864

var errClipboard = errors.New("clipboard unavailable")

type clipboardRequest struct {
	Schema    int     `json:"schema"`
	Operation string  `json:"operation"`
	Text      *string `json:"text_base64,omitempty"`
	Expected  *string `json:"expected_sha256,omitempty"`
}

type clipboardResponse struct {
	Schema  int     `json:"schema"`
	Success bool    `json:"success"`
	Text    *string `json:"text_base64,omitempty"`
	Error   string  `json:"error,omitempty"`
}

func clipboardFields(data []byte) (map[string]json.RawMessage, error) {
	if len(data) > clipboardWireLimit || !utf8.Valid(data) {
		return nil, errClipboard
	}
	decoder := json.NewDecoder(bytes.NewReader(data))
	token, err := decoder.Token()
	if err != nil || token != json.Delim('{') {
		return nil, errClipboard
	}
	fields := make(map[string]json.RawMessage)
	for decoder.More() {
		token, err = decoder.Token()
		if err != nil {
			return nil, errClipboard
		}
		name, ok := token.(string)
		if !ok {
			return nil, errClipboard
		}
		if _, exists := fields[name]; exists {
			return nil, errClipboard
		}
		var value json.RawMessage
		if decoder.Decode(&value) != nil {
			return nil, errClipboard
		}
		fields[name] = value
		if len(fields) > 4 {
			return nil, errClipboard
		}
	}
	token, err = decoder.Token()
	var extra any
	if err != nil || token != json.Delim('}') || decoder.Decode(&extra) != io.EOF {
		return nil, errClipboard
	}
	return fields, nil
}

func validateClipboardText(encoded string) error {
	if len(encoded) > base64.StdEncoding.EncodedLen(clipboardTextLimit) {
		return errClipboard
	}
	text, err := base64.StdEncoding.Strict().DecodeString(encoded)
	if err != nil || len(text) > clipboardTextLimit || !utf8.Valid(text) || bytes.IndexByte(text, 0) >= 0 || base64.StdEncoding.EncodeToString(text) != encoded {
		return errClipboard
	}
	return nil
}

func decodeClipboardRequest(data []byte) (clipboardRequest, error) {
	var request clipboardRequest
	fields, err := clipboardFields(data)
	if err != nil || string(fields["schema"]) != "1" {
		return request, errClipboard
	}
	for name := range fields {
		if name != "schema" && name != "operation" && name != "text_base64" && name != "expected_sha256" {
			return request, errClipboard
		}
	}
	if json.Unmarshal(data, &request) != nil {
		return request, errClipboard
	}
	switch request.Operation {
	case "read":
		if len(fields) != 2 {
			return request, errClipboard
		}
	case "write":
		if (len(fields) != 3 && len(fields) != 4) || request.Text == nil || validateClipboardText(*request.Text) != nil {
			return request, errClipboard
		}
	default:
		return request, errClipboard
	}
	if raw, exists := fields["expected_sha256"]; exists {
		if request.Operation != "write" || string(raw) == "null" || request.Expected == nil {
			return request, errClipboard
		}
		if *request.Expected != "unsupported-text" {
			value, err := hex.DecodeString(*request.Expected)
			if err != nil || len(value) != 32 || hex.EncodeToString(value) != *request.Expected {
				return request, errClipboard
			}
		}
	}
	return request, nil
}

func decodeClipboardResponse(data []byte, operation string) (clipboardResponse, error) {
	var response clipboardResponse
	fields, err := clipboardFields(data)
	if err != nil || string(fields["schema"]) != "1" || (string(fields["success"]) != "true" && string(fields["success"]) != "false") {
		return response, errClipboard
	}
	for name := range fields {
		if name != "schema" && name != "success" && name != "text_base64" && name != "error" {
			return response, errClipboard
		}
	}
	if json.Unmarshal(data, &response) != nil {
		return clipboardResponse{}, errClipboard
	}
	if !response.Success {
		if len(fields) == 3 && (response.Error == "unsupported-text" || response.Error == "conflict") {
			return response, nil
		}
		return clipboardResponse{}, errClipboard
	}
	if operation == "read" {
		if len(fields) != 3 || response.Text == nil || validateClipboardText(*response.Text) != nil {
			return clipboardResponse{}, errClipboard
		}
	} else if len(fields) != 2 {
		return clipboardResponse{}, errClipboard
	}
	return response, nil
}

// The script and metadata arguments are fixed; clipboard bytes travel only on
// stdin and bounded private stdout. The user-owned pipe reaches the interactive
// Windows Qt application, never the SYSTEM agent's Session 0 clipboard.
const exchangeGuestClipboard = `
$ErrorActionPreference='Stop'
$ProgressPreference='SilentlyContinue'
[Console]::InputEncoding=[Text.UTF8Encoding]::new($false)
[Console]::OutputEncoding=[Text.UTF8Encoding]::new($false)
$pipe=$null
try {
    $line=[Console]::In.ReadLine()
    $encoding=[Text.UTF8Encoding]::new($false,$true)
    $request=$encoding.GetBytes($line+[char]10)
    if ($request.Length -gt 1572864) { throw 'invalid' }
    $sid=[Security.Principal.WindowsIdentity]::GetCurrent().User.Value
    $pipe=[IO.Pipes.NamedPipeClientStream]::new('.',('dockpipe-guest-clipboard-v1-'+$sid),[IO.Pipes.PipeDirection]::InOut,[IO.Pipes.PipeOptions]::Asynchronous)
    $pipe.Connect(1500)
    $write=$pipe.WriteAsync($request,0,$request.Length)
    if (-not $write.Wait(2500)) { throw 'timeout' }
    $null=$write.GetAwaiter().GetResult()
    $clock=[Diagnostics.Stopwatch]::StartNew()
    $output=[IO.MemoryStream]::new()
    $buffer=[byte[]]::new(8192)
    while ($output.Length -le 1572864) {
        $remaining=3000-[int]$clock.ElapsedMilliseconds
        if ($remaining -le 0) { throw 'timeout' }
        $read=$pipe.ReadAsync($buffer,0,$buffer.Length)
        if (-not $read.Wait($remaining)) { throw 'timeout' }
        $count=$read.GetAwaiter().GetResult()
        if ($count -eq 0) { throw 'closed' }
        $newline=[Array]::IndexOf($buffer,[byte]10,0,$count)
        $keep=if ($newline -ge 0) { $newline } else { $count }
        if ($output.Length+$keep -gt 1572864) { throw 'too large' }
        $output.Write($buffer,0,$keep)
        if ($newline -ge 0) {
            [Console]::Out.WriteLine($encoding.GetString($output.ToArray()))
            exit 0
        }
    }
    throw 'too large'
} catch {
    [Console]::Out.WriteLine('{"schema":1,"success":false,"error":"guest-clipboard-unavailable"}')
    exit 1
} finally {
    if ($null -ne $pipe) { $pipe.Dispose() }
}
`

func (manager manager) clipboardExchange(ctx context.Context, input io.Reader, runID string) (clipboardResponse, error) {
	type inputResult struct {
		data []byte
		err  error
	}
	received := make(chan inputResult, 1)
	go func() {
		data, err := io.ReadAll(io.LimitReader(input, clipboardWireLimit+1))
		received <- inputResult{data, err}
	}()
	var data []byte
	var err error
	select {
	case result := <-received:
		data, err = result.data, result.err
	case <-ctx.Done():
		return clipboardResponse{}, errClipboard
	}
	if err != nil {
		return clipboardResponse{}, errClipboard
	}
	request, err := decodeClipboardRequest(data)
	if err != nil {
		return clipboardResponse{}, errClipboard
	}
	record, err := manager.readRecord(runID)
	if err != nil || record.Phase != "started" || record.StartToken == "" || record.ExecMode != "powershell" {
		return clipboardResponse{}, errClipboard
	}
	observation, err := manager.observe(ctx)
	if err != nil || observation.resource(record, true) == nil {
		return clipboardResponse{}, errClipboard
	}
	key, err := os.Lstat(record.SSHKeyReference)
	if err != nil || !key.Mode().IsRegular() || key.Size() > 128*1024 {
		return clipboardResponse{}, errClipboard
	}
	knownHosts := filepath.Join(manager.state, "ssh-hosts-"+record.RunID)
	identity, err := os.Lstat(knownHosts)
	if err != nil || !identity.Mode().IsRegular() || identity.Size() > 65536 {
		return clipboardResponse{}, errClipboard
	}
	executable, err := exec.LookPath("ssh")
	if err != nil {
		return clipboardResponse{}, errClipboard
	}
	arguments := []string{"-T", "-o", "BatchMode=yes", "-o", "IdentitiesOnly=yes", "-o", "StrictHostKeyChecking=yes", "-o", "UserKnownHostsFile=" + knownHosts,
		"-o", "ConnectTimeout=2", "-o", "ConnectionAttempts=1", "-i", record.SSHKeyReference, "-p", strconv.Itoa(record.SSHPort), "-l", record.SSHUser, record.SSHHost,
		"powershell.exe", "-NoProfile", "-NonInteractive", "-EncodedCommand", encodePowerShell(exchangeGuestClipboard)}
	payload, _ := json.Marshal(request)
	command := exec.CommandContext(ctx, executable, arguments...)
	command.Dir = manager.workspace
	command.Stdin = bytes.NewReader(append(payload, '\n'))
	output := &boundedBuffer{maximum: clipboardWireLimit}
	command.Stdout = output
	command.Stderr = io.Discard
	if command.Run() != nil {
		return clipboardResponse{}, errClipboard
	}
	response, err := decodeClipboardResponse(output.Bytes(), request.Operation)
	if err != nil {
		return clipboardResponse{}, errClipboard
	}
	observation, err = manager.observe(ctx)
	if err != nil || observation.resource(record, true) == nil {
		return clipboardResponse{}, errClipboard
	}
	return response, nil
}

func runClipboardExchange(ctx context.Context) {
	response := clipboardResponse{Schema: 1, Success: false, Error: "clipboard-unavailable"}
	manager, err := newManager(ctx)
	if err == nil {
		if result, exchangeError := manager.clipboardExchange(ctx, os.Stdin, os.Getenv("DOCKPIPE_SURFACE_RESOURCE_ID")); exchangeError == nil {
			response = result
		}
	}
	_ = json.NewEncoder(os.Stdout).Encode(response)
	if !response.Success {
		os.Exit(1)
	}
}

func (manager manager) clipboardBridge(ctx context.Context, runID, operation string) ([]byte, error) {
	executable, err := os.Executable()
	if err != nil {
		return nil, errClipboard
	}
	bridge := filepath.Join(filepath.Dir(executable), "dockpipe-vm-clipboard")
	info, err := os.Lstat(bridge)
	if err != nil || !info.Mode().IsRegular() || info.Mode().Perm()&0111 == 0 {
		return nil, errClipboard
	}
	return runCommand(ctx, bridge, []string{operation, "--resource", runID, "--manager", executable}, manager.workspace,
		"DOCKPIPE_VM_CLIPBOARD_SCOPE="+manager.state)
}

func (manager manager) clipboardStatus(ctx context.Context, runID string) string {
	data, err := manager.clipboardBridge(ctx, runID, "--status")
	if err != nil {
		return "Unavailable"
	}
	var status struct {
		Schema int    `json:"schema"`
		State  string `json:"state"`
	}
	if json.Unmarshal(data, &status) != nil || status.Schema != 1 {
		return "Unavailable"
	}
	switch status.State {
	case "active":
		return "Enabled (text only)"
	case "paused":
		return "Paused; start again to reconnect"
	case "conflict":
		return "Both sides changed; waiting for the next copy"
	case "unsupported-text":
		return "Enabled; waiting for text"
	default:
		return "Disabled"
	}
}

func (manager manager) clipboardAction(ctx context.Context, runID, operation string) error {
	if _, err := manager.clipboardBridge(ctx, runID, operation); err != nil {
		return errClipboard
	}
	message := "Text clipboard bridge enabled; the next copy will be shared"
	if operation == "--stop" {
		message = "Text clipboard bridge stopped"
	}
	return emitEvent(map[string]any{"schema": 1, "kind": "progress", "resource_id": runID, "phase": "clipboard", "message": message})
}
