package main

import (
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"runtime"
	"testing"
	"time"
)

// The test executable also serves as a fake public CLI/SSH transport. No VM,
// credential store, external network or sibling source is used by these tests.
func init() {
	if os.Getenv("DOCKPIPE_VM_QEMU_LAUNCH_TEST") == "1" {
		os.Unsetenv("DOCKPIPE_VM_QEMU_LAUNCH_TEST")
		if err := runQemuLauncher(os.Args[1:]); err != nil {
			if exit, ok := err.(*displayProcessExitError); ok {
				os.Exit(exit.Code)
			}
			os.Exit(8)
		}
		os.Exit(0)
	}
	if os.Getenv("DOCKPIPE_VM_DISPLAY_CHILD_TEST") == "1" {
		if os.Getenv("DOCKPIPE_VM_DISPLAY_CHILD_EXIT") == "23" {
			os.Exit(23)
		}
		if err := os.WriteFile(os.Getenv("DOCKPIPE_VM_DISPLAY_CHILD_PID_FILE"), []byte(fmt.Sprint(os.Getpid())), 0600); err != nil {
			os.Exit(9)
		}
		time.Sleep(30 * time.Second)
		os.Exit(0)
	}
	if os.Getenv("DOCKPIPE_VM_DISPLAY_SUPERVISOR_TEST") == "1" {
		os.Unsetenv("DOCKPIPE_VM_DISPLAY_SUPERVISOR_TEST")
		if runDisplaySupervisor() != nil {
			os.Exit(7)
		}
		os.Exit(0)
	}
	if os.Getenv("DOCKPIPE_VM_MANAGER_TEST_HELPER") != "1" {
		return
	}
	call := map[string]any{"program": filepath.Base(os.Args[0]), "arguments": os.Args[1:], "run_id": os.Getenv("DOCKPIPE_RUN_ID"), "workdir": os.Getenv("DOCKPIPE_WORKDIR")}
	if filename := os.Getenv("MANAGER_TEST_CALLS"); filename != "" {
		file, err := os.OpenFile(filename, os.O_CREATE|os.O_WRONLY|os.O_APPEND, 0600)
		if err != nil {
			os.Exit(3)
		}
		data, _ := json.Marshal(call)
		file.Write(append(data, '\n'))
		file.Close()
	}
	if os.Getenv("MANAGER_TEST_BIG_OUTPUT") == "1" {
		os.Stdout.Write(make([]byte, 1024*1024+1))
		os.Exit(0)
	}
	if os.Getenv("MANAGER_TEST_DIAGNOSTIC") == "1" {
		fmt.Fprintln(os.Stderr, "synthetic-secret-diagnostic")
		os.Exit(9)
	}
	if os.Getenv("MANAGER_TEST_SLEEP") == "1" {
		time.Sleep(10 * time.Second)
	}
	name := filepath.Base(os.Args[0])
	if name == "ssh" || name == "ssh.exe" {
		fmt.Print(os.Getenv("MANAGER_TEST_GUEST_STATUS"))
		os.Exit(0)
	}
	if len(os.Args) >= 2 && os.Args[1] == "scope" {
		fmt.Println(os.Getenv("MANAGER_TEST_STATE"))
		os.Exit(0)
	}
	if len(os.Args) >= 3 && os.Args[1] == "runs" && os.Args[2] == "observe" {
		data, err := os.ReadFile(os.Getenv("MANAGER_TEST_OBSERVATION"))
		if err != nil {
			os.Exit(4)
		}
		os.Stdout.Write(data)
		os.Exit(0)
	}
	if len(os.Args) >= 3 && os.Args[1] == "runs" && os.Args[2] == "associate" {
		data := os.Getenv("MANAGER_TEST_ASSOCIATED_OBSERVATION")
		if err := os.WriteFile(os.Getenv("MANAGER_TEST_OBSERVATION"), []byte(data), 0600); err != nil {
			os.Exit(5)
		}
		os.Exit(0)
	}
	os.Exit(6)
}

func fixtureRecord(t *testing.T) sessionRecord {
	t.Helper()
	return sessionRecord{
		Schema: 1, RunID: "fixture-run", ResourceID: "dockpipe-vm-fixture-run",
		MachineUUID: "11111111-1111-4111-8111-111111111111", PID: 4321,
		StartToken: "kernel-fixture-token", Phase: "started", SSHHost: "127.0.0.1",
		SSHPort: 2222, SSHUser: "fixture-user", AgentPort: 47831, ExecMode: "powershell",
	}
}

func fixtureObservation(record sessionRecord) executionSnapshot {
	return executionSnapshot{Schema: "dockpipe.execution_snapshot.v1", Executions: []observedExecution{
		{ID: record.RunID, State: "running", Resource: &observedResource{
			ID: record.ResourceID, Runtime: "qemu", State: "running",
			Process: processIdentity{PID: record.PID, StartToken: record.StartToken},
		}},
	}}
}

func helperManager(t *testing.T) manager {
	t.Helper()
	root := t.TempDir()
	state := filepath.Join(root, "state")
	if err := os.Mkdir(state, 0700); err != nil {
		t.Fatal(err)
	}
	executable, err := os.Executable()
	if err != nil {
		t.Fatal(err)
	}
	observation := filepath.Join(root, "observation.json")
	setObservation(t, observation, executionSnapshot{Schema: "dockpipe.execution_snapshot.v1", Executions: []observedExecution{}})
	t.Setenv("DOCKPIPE_VM_MANAGER_TEST_HELPER", "1")
	t.Setenv("MANAGER_TEST_STATE", state)
	t.Setenv("MANAGER_TEST_OBSERVATION", observation)
	t.Setenv("MANAGER_TEST_CALLS", filepath.Join(root, "calls.jsonl"))
	t.Setenv("DOCKPIPE_BIN", executable)
	t.Setenv("DOCKPIPE_WORKDIR", root)
	t.Setenv("DOCKPIPE_VM_EVENT_RUN_ID", "")
	t.Setenv("DOCKPIPE_VM_EVENT_WORKDIR", "")
	return manager{program: executable, workspace: root, state: state}
}

func setObservation(t *testing.T, path string, observation executionSnapshot) {
	t.Helper()
	data, err := json.Marshal(observation)
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, data, 0600); err != nil {
		t.Fatal(err)
	}
}

func installSSHHelper(t *testing.T, manager manager) string {
	t.Helper()
	bin := filepath.Join(manager.workspace, "transport")
	if err := os.MkdirAll(bin, 0700); err != nil {
		t.Fatal(err)
	}
	name := "ssh"
	if runtime.GOOS == "windows" {
		name += ".exe"
	}
	destination := filepath.Join(bin, name)
	linked := false
	if runtime.GOOS != "windows" {
		linked = os.Link(manager.program, destination) == nil
	}
	if !linked {
		data, err := os.ReadFile(manager.program)
		if err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(destination, data, 0700); err != nil {
			t.Fatal(err)
		}
	}
	t.Setenv("PATH", bin+string(os.PathListSeparator)+os.Getenv("PATH"))
	key := filepath.Join(manager.workspace, "fixture-key")
	if err := os.WriteFile(key, []byte("synthetic transport fixture; not a private key"), 0600); err != nil {
		t.Fatal(err)
	}
	return key
}

func validStatusJSON() []byte {
	return []byte(`{"schema":1,"ssh":true,"agent":true,"repair_registered":true,"repair_running":false,"repair_result":0,"tools_state":"ready","tools_phase":"Complete","tools_message":"Ready","application_installed":true,"machine_name":"fixture-machine"}`)
}
