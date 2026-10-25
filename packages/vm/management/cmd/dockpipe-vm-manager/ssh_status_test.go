package main

import (
	"context"
	"encoding/json"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func TestGuestStatusRejectsFutureOrInvalidUpdateTime(t *testing.T) {
	powerShell, err := exec.LookPath("powershell.exe")
	if err != nil {
		powerShell, err = exec.LookPath("pwsh")
	}
	if err != nil {
		t.Skip("PowerShell is required for the actual guest status script fixture")
	}
	cases := []struct {
		name    string
		updated string
		ready   bool
	}{
		{"current", time.Now().UTC().Format(time.RFC3339Nano), true},
		{"past", time.Now().UTC().Add(-24 * time.Hour).Format(time.RFC3339Nano), true},
		{"allowed clock skew", time.Now().UTC().Add(4 * time.Minute).Format(time.RFC3339Nano), true},
		{"future", time.Now().UTC().Add(10 * time.Minute).Format(time.RFC3339Nano), false},
		{"malformed", "not-a-timestamp", false},
		{"missing", "", false},
		{"oversized", strings.Repeat("2", 65), false},
	}
	for _, test := range cases {
		t.Run(test.name, func(t *testing.T) {
			root := t.TempDir()
			directory := filepath.Join(root, "Dockpipe", "GuestToolsStatus")
			if err := os.MkdirAll(directory, 0700); err != nil {
				t.Fatal(err)
			}
			status := map[string]any{
				"schema": 1, "state": "ready", "phase": "fixture", "message": "fixture",
				"updated_at": test.updated,
			}
			data, err := json.Marshal(status)
			if err != nil {
				t.Fatal(err)
			}
			if err := os.WriteFile(filepath.Join(directory, "status.json"), data, 0600); err != nil {
				t.Fatal(err)
			}
			// Replace every Windows service/task/network boundary. Only the
			// temporary status file is real; execute the exact production script.
			fixture := `
[Console]::Error.WriteLine('::dockpipe-guest-status-fixture-start::')
[Console]::Error.WriteLine('updatecheck='+$env:POWERSHELL_UPDATECHECK+' telemetry_optout='+$env:POWERSHELL_TELEMETRY_OPTOUT)
function Get-ScheduledTask { param($TaskName,$ErrorAction) return $null }
function Get-Service { param($Name) [pscustomobject]@{Status='Running'} }
function Invoke-RestMethod { param($Uri,$TimeoutSec) [pscustomobject]@{name='dockpipe-guest-agent';status='ready'} }
`
			ctx, cancel := context.WithTimeout(context.Background(), 8*time.Second)
			defer cancel()
			script := fixture + readGuestStatus + "\n[Console]::Error.WriteLine('::dockpipe-guest-status-fixture-end::')\n"
			command := exec.CommandContext(ctx, powerShell, "-NoProfile", "-NonInteractive", "-EncodedCommand", encodePowerShell(script))
			// This offline unit fixture must not initiate PowerShell telemetry
			// or update requests while evaluating the local status script.
			// Set documented controls before interpreter startup; retain the
			// same per-case and production deadlines.
			command.Env = append(os.Environ(),
				"ProgramData="+root,
				"ProgramFiles="+root,
				"COMPUTERNAME=fixture",
				"POWERSHELL_UPDATECHECK=Off",
				"POWERSHELL_TELEMETRY_OPTOUT=1",
			)
			outputBuffer := &boundedBuffer{maximum: 64 * 1024}
			diagnosticBuffer := &boundedBuffer{maximum: 8 * 1024}
			command.Stdout = outputBuffer
			command.Stderr = diagnosticBuffer
			started := time.Now()
			err = command.Run()
			elapsed := time.Since(started)
			var userTime, systemTime time.Duration
			if command.ProcessState != nil {
				userTime = command.ProcessState.UserTime()
				systemTime = command.ProcessState.SystemTime()
			}
			t.Logf("guest status fixture interpreter=%s elapsed=%s user=%s system=%s context=%v updatecheck=%q telemetry_optout=%q stderr=%q",
				powerShell, elapsed, userTime, systemTime, ctx.Err(), "Off",
				"1", diagnosticBuffer.Bytes())
			if err != nil {
				t.Fatalf("guest status fixture failed: %v", err)
			}
			if !strings.Contains(string(diagnosticBuffer.Bytes()), "updatecheck=Off telemetry_optout=1") {
				t.Fatal("PowerShell fixture did not observe its explicit offline environment")
			}
			output := outputBuffer.Bytes()
			var result guestStatus
			if err := json.Unmarshal(output, &result); err != nil {
				t.Fatal(err)
			}
			if (result.ToolsState == "ready") != test.ready || !result.SSH || !result.Agent {
				t.Fatal("status freshness or mocked live service contract failed")
			}
			if !test.ready && result.ToolsPhase != "Guest Tools status needs refresh" {
				t.Fatal("rejected update time did not produce an honest refresh status")
			}
		})
	}
}
