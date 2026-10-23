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
function Get-ScheduledTask { param($TaskName,$ErrorAction) return $null }
function Get-Service { param($Name) [pscustomobject]@{Status='Running'} }
function Invoke-RestMethod { param($Uri,$TimeoutSec) [pscustomobject]@{name='dockpipe-guest-agent';status='ready'} }
`
			ctx, cancel := context.WithTimeout(context.Background(), 8*time.Second)
			defer cancel()
			command := exec.CommandContext(ctx, powerShell, "-NoProfile", "-NonInteractive", "-EncodedCommand", encodePowerShell(fixture+readGuestStatus))
			command.Env = append(os.Environ(), "ProgramData="+root, "ProgramFiles="+root, "COMPUTERNAME=fixture")
			output, err := command.Output()
			if err != nil {
				t.Fatalf("guest status fixture failed: %v", err)
			}
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
