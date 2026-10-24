package main

import (
	"context"
	"encoding/json"
	"fmt"
	"net"
	"os"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"
)

type nativeDisplay struct {
	Kind              string         `json:"kind"`
	ProcessID         int            `json:"process_id,omitempty"`
	ProcessStartToken string         `json:"process_start_token,omitempty"`
	Options           map[string]any `json:"options,omitempty"`
}

type surfaceResource struct {
	ID       string            `json:"id"`
	LaunchID string            `json:"launch_id,omitempty"`
	Title    string            `json:"title"`
	State    string            `json:"state"`
	Details  map[string]string `json:"details"`
	Actions  []string          `json:"actions"`
	Display  *nativeDisplay    `json:"display,omitempty"`
}

func emitEvent(event any) error {
	data, err := json.Marshal(event)
	if err != nil || len(data) > 1024*1024 {
		return fmt.Errorf("surface event exceeds its contract")
	}
	fmt.Println("::dockpipe-surface::" + string(data))
	return nil
}

func replaceAgentPort(script string, port int) string {
	return strings.ReplaceAll(script, "__AGENT_PORT__", strconv.Itoa(port))
}

func (manager manager) snapshot(ctx context.Context) error {
	observation, err := manager.observe(ctx)
	if err != nil {
		return fmt.Errorf("VM process observations unavailable: %w", err)
	}
	entries, err := os.ReadDir(manager.state)
	if err != nil {
		return err
	}
	records := make([]sessionRecord, 0)
	for _, entry := range entries {
		if !strings.HasPrefix(entry.Name(), "vm-session-") || !strings.HasSuffix(entry.Name(), ".json") {
			continue
		}
		if len(records) >= 128 {
			return fmt.Errorf("VM inventory exceeds 128 sessions; archive old package records")
		}
		runID := strings.TrimSuffix(strings.TrimPrefix(entry.Name(), "vm-session-"), ".json")
		record, err := manager.readRecord(runID)
		if err != nil {
			return fmt.Errorf("VM inventory contains an invalid session record")
		}
		records = append(records, record)
	}
	sort.Slice(records, func(left, right int) bool { return records[left].RunID < records[right].RunID })
	resources := make([]surfaceResource, len(records))
	semaphore := make(chan struct{}, 4)
	var pending sync.WaitGroup
	for index, record := range records {
		resources[index] = resourceForRecord(record, observation)
		if resources[index].State == "running" {
			if options := manager.nativeDisplayOptions(record); options != nil {
				resources[index].Display = &nativeDisplay{Kind: "package-native", Options: options}
			}
		}
		if resources[index].State != "running" {
			continue
		}
		pending.Add(1)
		go func(index int, record sessionRecord) {
			defer pending.Done()
			select {
			case semaphore <- struct{}{}:
				defer func() { <-semaphore }()
			case <-ctx.Done():
				return
			}
			probeContext, cancel := context.WithTimeout(ctx, 10*time.Second)
			defer cancel()
			manager.probeGuest(probeContext, record, &resources[index])
		}(index, record)
	}
	pending.Wait()
	return emitEvent(map[string]any{"schema": 1, "kind": "snapshot", "resources": resources})
}

func resourceForRecord(record sessionRecord, observation executionSnapshot) surfaceResource {
	resource := surfaceResource{
		ID: record.RunID, LaunchID: record.LaunchID, Title: record.ResourceID, State: "unverified",
		Details: map[string]string{"Process": "Identity unavailable", "Guest Tools": "Not checked", "SSH": "Not checked"},
		Actions: []string{},
	}
	if record.Phase == "prepared" && record.PID == 0 {
		resource.State = "starting"
		resource.Details["Process"] = "Waiting for runtime process registration"
	} else if record.Phase == "exited" && observation.resource(record, true) == nil {
		resource.State = "stopped"
		resource.Details["Process"] = "Runtime session ended"
	} else if observation.resource(record, true) != nil {
		resource.State = "running"
		resource.Details["Process"] = "Running; kernel process identity verified"
		resource.Details["SSH"] = "Authentication not checked"
		if len(record.StartToken) <= 256 {
			resource.Display = &nativeDisplay{Kind: "native-window", ProcessID: record.PID, ProcessStartToken: record.StartToken}
		}
	}
	return resource
}

func (manager manager) probeGuest(ctx context.Context, record sessionRecord, resource *surfaceResource) {
	connection, err := (&net.Dialer{Timeout: time.Second}).DialContext(ctx, "tcp", net.JoinHostPort(record.SSHHost, strconv.Itoa(record.SSHPort)))
	if err != nil {
		resource.Details["SSH"] = "Waiting for guest SSH listener"
		return
	}
	connection.Close()
	resource.Details["SSH"] = "Listening; authentication not confirmed"
	status, err := manager.guestStatus(ctx, record)
	if err != nil {
		resource.Details["Guest Tools"] = "Status not confirmed"
		return
	}
	resource.Details["SSH"] = "Key-authenticated"
	resource.Details["Guest Tools"] = status.ToolsState
	resource.Details["Provisioning"] = status.ToolsPhase
	resource.Details["Message"] = status.ToolsMessage
	resource.Details["Startup repair"] = "Missing"
	if status.RepairRegistered {
		resource.Details["Startup repair"] = "Registered; last result " + strconv.FormatInt(status.RepairResult, 10)
	}
	if status.RepairRunning {
		resource.Details["Startup repair"] = "Provisioning in background"
	}
	resource.Details["Guest agent"] = "Not ready"
	if status.Agent {
		resource.Details["Guest agent"] = "Healthy"
	}
	resource.Details["Guest Tools app"] = "Not installed"
	if status.ApplicationInstalled {
		resource.Details["Guest Tools app"] = "Installed"
		resource.Actions = append(resource.Actions, "open", "resolution")
		clipboardState := manager.clipboardStatus(ctx, record.RunID)
		resource.Details["Clipboard"] = clipboardState
		if clipboardState != "Unavailable" {
			resource.Actions = append(resource.Actions, "clipboard-start", "clipboard-stop")
		}

	}
	if status.MachineName != "" && len(status.MachineName) <= 128 {
		resource.Title = status.MachineName + " (" + record.RunID + ")"
	}
	resource.Actions = append(resource.Actions, "repair", "shutdown")
}

func (manager manager) action(ctx context.Context, action, runID string) error {
	if action == "clipboard-stop" && identifier.MatchString(runID) {
		return manager.clipboardAction(ctx, runID, "--stop")
	}
	record, err := manager.readRecord(runID)
	if err != nil {
		return err
	}
	observation, err := manager.observe(ctx)
	if err != nil || observation.resource(record, true) == nil {
		return fmt.Errorf("VM process identity is not verified; action refused")
	}
	if action == "clipboard-start" {
		return manager.clipboardAction(ctx, runID, "--start")
	}
	script := "$ErrorActionPreference='Stop'; $ProgressPreference='SilentlyContinue'; "
	message := ""
	switch action {
	case "resolution":
		request, err := manager.resolutionRequest(record, os.Getenv("DOCKPIPE_VM_DISPLAY_WIDTH"), os.Getenv("DOCKPIPE_VM_DISPLAY_HEIGHT"))
		if err != nil {
			return err
		}
		script += request
		message = "Windows applied and saved the requested guest display resolution"
	case "repair":
		script += "Start-ScheduledTask -TaskName 'DockPipeGuestProvisioning'"
		message = "Background guest provisioning requested"
	case "open":
		script += showGuestTools
		message = "Guest Tools window requested in the logged-in Windows session"
	case "shutdown":
		script += "& shutdown.exe /s /t 0; if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }"
		message = "Guest shutdown requested; Windows may require attention before closing applications"
	default:
		return fmt.Errorf("unsupported VM action")
	}
	if _, err := manager.guestCommand(ctx, record, script); err != nil {
		return err
	}
	return emitEvent(map[string]any{"schema": 1, "kind": "progress", "resource_id": runID, "phase": action, "message": message})
}
