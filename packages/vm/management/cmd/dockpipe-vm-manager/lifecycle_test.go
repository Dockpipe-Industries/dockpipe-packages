package main

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"os"
	"strconv"
	"strings"
	"testing"
	"time"
)

func setLifecycleEvent(t *testing.T, record sessionRecord) {
	t.Helper()
	values := map[string]string{
		"SCHEMA": "1", "PHASE": record.Phase, "RUN_ID": record.RunID, "LAUNCH_ID": record.LaunchID,
		"RESOURCE_ID": record.ResourceID, "MACHINE_UUID": record.MachineUUID,
		"PID": strconv.Itoa(record.PID), "SSH_HOST": record.SSHHost,
		"SSH_PORT": strconv.Itoa(record.SSHPort), "SSH_USER": record.SSHUser,
		"SSH_KEY_REFERENCE": record.SSHKeyReference, "AGENT_PORT": strconv.Itoa(record.AgentPort),
		"EXEC_MODE": record.ExecMode,
	}
	for name, value := range values {
		t.Setenv("DOCKPIPE_VM_EVENT_"+name, value)
	}
}

func TestLifecycleRegistersPublicAssociationAndRefusesPreparedReplay(t *testing.T) {
	manager := helperManager(t)
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	record := fixtureRecord(t)
	associated, _ := json.Marshal(fixtureObservation(record))
	t.Setenv("MANAGER_TEST_ASSOCIATED_OBSERVATION", string(associated))
	record.Phase = "prepared"
	record.PID = 0
	record.StartToken = ""
	setLifecycleEvent(t, record)
	if err := manager.report(ctx); err != nil {
		t.Fatal(err)
	}
	started := fixtureRecord(t)
	setLifecycleEvent(t, started)
	if err := manager.report(ctx); err != nil {
		t.Fatal(err)
	}
	saved, err := manager.readRecord(record.RunID)
	if err != nil || saved.Phase != "started" || saved.StartToken != started.StartToken {
		t.Fatalf("registered record missing kernel identity: %+v %v", saved, err)
	}
	path, _ := manager.recordPath(record.RunID)
	before, _ := os.ReadFile(path)
	setLifecycleEvent(t, record)
	if err := manager.report(ctx); err == nil {
		t.Fatal("prepared replay downgraded a started session")
	}
	after, _ := os.ReadFile(path)
	if !bytes.Equal(before, after) {
		t.Fatal("rejected replay changed the durable record")
	}
	reused := fixtureObservation(started)
	reused.Executions[0].Resource.Process.StartToken = "reused-pid-token"
	setObservation(t, os.Getenv("MANAGER_TEST_OBSERVATION"), reused)
	setLifecycleEvent(t, started)
	if err := manager.report(ctx); err == nil {
		t.Fatal("PID reuse replaced the recorded kernel identity")
	}
	after, _ = os.ReadFile(path)
	if !bytes.Equal(before, after) {
		t.Fatal("PID reuse changed the durable record")
	}
}

func TestActionsRefuseUnverifiedOrReusedProcessBeforeSSH(t *testing.T) {
	manager := helperManager(t)
	record := fixtureRecord(t)
	if err := manager.writeRecord(record); err != nil {
		t.Fatal(err)
	}
	variants := map[string]func(*executionSnapshot){
		"PID reuse":      func(value *executionSnapshot) { value.Executions[0].Resource.Process.StartToken = "replacement" },
		"missing token":  func(value *executionSnapshot) { value.Executions[0].Resource.Process.StartToken = "" },
		"wrong PID":      func(value *executionSnapshot) { value.Executions[0].Resource.Process.PID++ },
		"wrong run":      func(value *executionSnapshot) { value.Executions[0].ID = "other-run" },
		"wrong resource": func(value *executionSnapshot) { value.Executions[0].Resource.ID = "other-resource" },
		"wrong runtime":  func(value *executionSnapshot) { value.Executions[0].Resource.Runtime = "other" },
		"stopped":        func(value *executionSnapshot) { value.Executions[0].State = "stopped" },
	}
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	for name, mutate := range variants {
		t.Run(name, func(t *testing.T) {
			observed := fixtureObservation(record)
			mutate(&observed)
			setObservation(t, os.Getenv("MANAGER_TEST_OBSERVATION"), observed)
			if err := manager.action(ctx, "shutdown", record.RunID); err == nil || !strings.Contains(err.Error(), "identity") {
				t.Fatalf("unverified action accepted or failed after identity gate: %v", err)
			}
		})
	}
	if err := manager.action(ctx, "shutdown", "../outside"); err == nil {
		t.Fatal("action selection escaped package records")
	}
}

func TestPublicCLIContextUsesEventRunAndActualWorkspace(t *testing.T) {
	fixture := helperManager(t)
	t.Setenv("DOCKPIPE_VM_EVENT_RUN_ID", "lifecycle-owner")
	t.Setenv("DOCKPIPE_RUN_ID", "inherited-wrong-run")
	t.Setenv("DOCKPIPE_WORKDIR", "inherited-wrong-workspace")
	t.Setenv("DOCKPIPE_VM_EVENT_WORKDIR", fixture.workspace)
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	manager, err := newManager(ctx)
	if err != nil {
		t.Fatal(err)
	}
	if manager.workspace != fixture.workspace || manager.state != fixture.state {
		t.Fatalf("wrong workspace or SDK state: %+v", manager)
	}
	data, err := os.ReadFile(os.Getenv("MANAGER_TEST_CALLS"))
	if err != nil {
		t.Fatal(err)
	}
	var call struct {
		Arguments []string `json:"arguments"`
		RunID     string   `json:"run_id"`
		Workdir   string   `json:"workdir"`
	}
	if err := json.Unmarshal(bytes.TrimSpace(data), &call); err != nil {
		t.Fatal(err)
	}
	if call.RunID != "lifecycle-owner" || call.Workdir != fixture.workspace || len(call.Arguments) < 3 || call.Arguments[0] != "scope" || call.Arguments[2] != packageOwner {
		t.Fatalf("public CLI received the wrong lifecycle owner: %+v", call)
	}
}

func TestRunCommandBoundsOutputCancellationAndDiagnosticExposure(t *testing.T) {
	manager := helperManager(t)
	t.Setenv("MANAGER_TEST_DIAGNOSTIC", "1")
	_, err := runCommand(context.Background(), manager.program, []string{"scope"}, manager.workspace)
	if err == nil || strings.Contains(err.Error(), "synthetic-secret-diagnostic") {
		t.Fatalf("child diagnostic exposed or failure lost: %v", err)
	}
	t.Setenv("MANAGER_TEST_DIAGNOSTIC", "")
	t.Setenv("MANAGER_TEST_BIG_OUTPUT", "1")
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	if _, err := runCommand(ctx, manager.program, []string{"scope"}, manager.workspace); err == nil {
		t.Fatal("oversized child output accepted")
	}
	t.Setenv("MANAGER_TEST_BIG_OUTPUT", "")
	t.Setenv("MANAGER_TEST_SLEEP", "1")
	short, stop := context.WithTimeout(context.Background(), 50*time.Millisecond)
	defer stop()
	if _, err := runCommand(short, manager.program, []string{"scope"}, manager.workspace); !errors.Is(err, context.DeadlineExceeded) {
		t.Fatalf("cancellation lost: %v", err)
	}
}

func TestLaunchCorrelationSurvivesReportingAndCannotBeReassigned(t *testing.T) {
	manager := helperManager(t)
	record := fixtureRecord(t)
	record.Phase = "prepared"
	record.PID = 0
	record.StartToken = ""
	record.LaunchID = "launcher-fixture"
	setLifecycleEvent(t, record)
	if err := manager.report(context.Background()); err != nil {
		t.Fatal(err)
	}
	saved, err := manager.readRecord(record.RunID)
	if err != nil || saved.LaunchID != record.LaunchID {
		t.Fatalf("launch correlation lost: %v", err)
	}
	resource := resourceForRecord(saved, executionSnapshot{})
	if resource.LaunchID != record.LaunchID || resource.Display != nil {
		t.Fatal("prepared resource must retain correlation without claiming a native display")
	}
	record.LaunchID = "different-launch"
	setLifecycleEvent(t, record)
	if err := manager.report(context.Background()); err == nil {
		t.Fatal("existing session was assigned to another launcher request")
	}
}
