//go:build linux || darwin

package main

import (
	"encoding/json"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func TestDisplaySupervisorOwnsPrivateBusUntilExactProcessExit(t *testing.T) {
	if _, err := exec.LookPath("dbus-daemon"); err != nil {
		t.Skip("dbus-daemon is required for private bus lifecycle proof")
	}
	manager := helperManager(t)
	runtimeDirectory, err := os.MkdirTemp("", "dp-")
	if err != nil {
		t.Fatal(err)
	}
	defer os.RemoveAll(runtimeDirectory)
	t.Setenv("XDG_RUNTIME_DIR", runtimeDirectory)
	t.Setenv("DOCKPIPE_RUN_ID", "fixture-run")
	sleeper, err := exec.LookPath("sleep")
	if err != nil {
		t.Fatal(err)
	}
	sleeper, err = filepath.EvalSymlinks(sleeper)
	if err != nil {
		t.Fatal(err)
	}
	process := exec.Command(sleeper, "30")
	if err := process.Start(); err != nil {
		t.Fatal(err)
	}
	defer process.Process.Kill()
	token, err := displayProcessToken(process.Process.Pid)
	if err != nil {
		t.Fatal(err)
	}
	configuration := displaySupervisorConfiguration{Schema: 1, RunID: "fixture-run", StateRoot: manager.state,
		QemuExecutable: sleeper, LauncherPID: process.Process.Pid, LauncherToken: token}
	data, _ := json.Marshal(configuration)
	supervisor := exec.Command(manager.program)
	supervisor.Env = append(os.Environ(), "DOCKPIPE_VM_DISPLAY_SUPERVISOR_TEST=1")
	supervisor.Stdin = strings.NewReader(string(data))
	supervisor.Stdout = io.Discard
	supervisor.Stderr = io.Discard
	if err := supervisor.Start(); err != nil {
		t.Fatal(err)
	}
	defer supervisor.Process.Kill()
	done := make(chan error, 1)
	go func() { done <- supervisor.Wait() }()
	path := filepath.Join(manager.state, "display-fixture-run", "display.json")
	var descriptor displayDescriptor
	deadline := time.Now().Add(3 * time.Second)
	for time.Now().Before(deadline) {
		if decodeDisplayJSON(path, 4096, &descriptor) == nil {
			break
		}
		time.Sleep(20 * time.Millisecond)
	}
	if descriptor.QemuPID != process.Process.Pid || descriptor.QemuStartToken != token ||
		!privateOwnedPath(filepath.Dir(path), true, 0700) || !privateOwnedPath(path, false, 0600) {
		t.Fatal("private descriptor did not bind the live owned process")
	}
	socket := strings.TrimPrefix(descriptor.BusAddress, "unix:path=")
	if !privateOwnedPath(filepath.Dir(socket), true, 0700) {
		t.Fatal("private bus parent is not owner-only")
	}
	canonicalSocket, err := filepath.EvalSymlinks(socket)
	if err != nil || canonicalSocket != socket {
		t.Fatal("published private bus socket path is not canonical")
	}
	if info, err := os.Lstat(socket); err != nil || info.Mode()&os.ModeSocket == 0 {
		t.Fatal("private bus socket was not created")
	}
	process.Process.Kill()
	process.Wait()
	select {
	case err := <-done:
		if err != nil {
			t.Fatal("supervisor failed during process-exit cleanup")
		}
	case <-time.After(3 * time.Second):
		t.Fatal("supervisor retained a dead process's bus")
	}
	for _, removed := range []string{path, filepath.Dir(path), socket, filepath.Dir(socket)} {
		if _, err := os.Lstat(removed); !os.IsNotExist(err) {
			t.Fatal("supervisor retained private display state after process exit")
		}
	}
}
