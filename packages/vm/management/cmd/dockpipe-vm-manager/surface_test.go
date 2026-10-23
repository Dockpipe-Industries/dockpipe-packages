package main

import (
	"bytes"
	"context"
	"io"
	"net"
	"os"
	"path/filepath"
	"slices"
	"strconv"
	"testing"
	"time"
)

func TestProcessSnapshotStatesDoNotConfuseUnverifiedAndRunning(t *testing.T) {
	record := fixtureRecord(t)
	observed := fixtureObservation(record)
	running := resourceForRecord(record, observed)
	if running.Display == nil || running.Display.Kind != "native-window" || running.Display.ProcessID != record.PID || running.Display.ProcessStartToken != record.StartToken {
		t.Fatalf("verified process display identity missing: %+v", running.Display)
	}
	if running.State != "running" || running.Details["SSH"] != "Authentication not checked" || len(running.Actions) != 0 {
		t.Fatalf("process identity implied guest readiness: %+v", running)
	}
	observed.Executions[0].Resource.Process.StartToken = "replacement"
	unverified := resourceForRecord(record, observed)
	if unverified.State != "unverified" || len(unverified.Actions) != 0 || unverified.Display != nil {
		t.Fatalf("PID reuse implied live identity: %+v", unverified)
	}
	record.Phase = "exited"
	if result := resourceForRecord(record, observed); result.State != "stopped" || result.Display != nil {
		t.Fatalf("exited session not reconciled: %+v", result)
	}
	record.Phase = "prepared"
	record.PID = 0
	if result := resourceForRecord(record, observed); result.State != "starting" || len(result.Actions) != 0 || result.Display != nil {
		t.Fatalf("prepared session implied readiness: %+v", result)
	}
}

func TestProbeDistinguishesListeningAuthenticationAndGuestReadiness(t *testing.T) {
	manager := helperManager(t)
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer listener.Close()
	go func() {
		for {
			connection, err := listener.Accept()
			if err != nil {
				return
			}
			connection.Close()
		}
	}()
	record := fixtureRecord(t)
	_, port, err := net.SplitHostPort(listener.Addr().String())
	if err != nil {
		t.Fatal(err)
	}
	record.SSHPort, err = strconv.Atoi(port)
	if err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	resource := resourceForRecord(record, fixtureObservation(record))
	manager.probeGuest(ctx, record, &resource)
	if resource.Details["SSH"] != "Listening; authentication not confirmed" || len(resource.Actions) != 0 {
		t.Fatalf("listener implied authenticated access: %+v", resource)
	}
	record.SSHKeyReference = installSSHHelper(t, manager)
	t.Setenv("MANAGER_TEST_GUEST_STATUS", string(validStatusJSON()))
	manager.probeGuest(ctx, record, &resource)
	if resource.Details["SSH"] != "Key-authenticated" || resource.Details["Guest Tools"] != "ready" || resource.Details["Guest agent"] != "Healthy" {
		t.Fatalf("authenticated guest state missing: %+v", resource)
	}
	for _, action := range []string{"open", "repair", "shutdown"} {
		if !slices.Contains(resource.Actions, action) {
			t.Fatalf("authenticated guest action %q missing: %+v", action, resource)
		}
	}
	listener.Close()
	resource = resourceForRecord(record, fixtureObservation(record))
	manager.probeGuest(ctx, record, &resource)
	if resource.Details["SSH"] != "Waiting for guest SSH listener" || len(resource.Actions) != 0 {
		t.Fatalf("closed listener implied readiness: %+v", resource)
	}
}

func TestGuestCommandRejectsRedirectedOrOversizedCredentialReferences(t *testing.T) {
	manager := helperManager(t)
	record := fixtureRecord(t)
	record.SSHKeyReference = manager.state
	ctx := context.Background()
	if _, err := manager.guestCommand(ctx, record, "fixture"); err == nil {
		t.Fatal("credential directory accepted")
	}
	key := filepath.Join(manager.workspace, "large-key")
	if err := os.WriteFile(key, make([]byte, 128*1024+1), 0600); err != nil {
		t.Fatal(err)
	}
	record.SSHKeyReference = key
	if _, err := manager.guestCommand(ctx, record, "fixture"); err == nil {
		t.Fatal("oversized credential accepted")
	}
	reference := filepath.Join(manager.workspace, "linked-key")
	if err := os.Symlink(key, reference); err == nil {
		record.SSHKeyReference = reference
		if _, err := manager.guestCommand(ctx, record, "fixture"); err == nil {
			t.Fatal("linked credential accepted")
		}
	}
}

func TestBoundedCommandWriterCannotBypassCapacityThroughReaderFrom(t *testing.T) {
	buffer := &boundedBuffer{maximum: 4}
	_, err := io.Copy(buffer, bytes.NewReader([]byte("exceeds capacity")))
	if err == nil || len(buffer.Bytes()) > 4 {
		t.Fatal("io.Copy bypassed the command output capacity")
	}
}
