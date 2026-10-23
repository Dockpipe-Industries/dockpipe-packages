//go:build linux

package main

import (
	"encoding/json"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

func TestDisplaySupervisorRejectsWrongProcessTokenBeforeCreatingState(t *testing.T) {
	manager := helperManager(t)
	t.Setenv("DOCKPIPE_RUN_ID", "fixture-run")
	executable, err := os.Executable()
	if err != nil {
		t.Fatal(err)
	}
	configuration := displaySupervisorConfiguration{Schema: 1, RunID: "fixture-run", StateRoot: manager.state,
		QemuExecutable: executable, LauncherPID: os.Getpid(), LauncherToken: "wrong-token"}
	data, _ := json.Marshal(configuration)
	supervisor := exec.Command(manager.program)
	supervisor.Env = append(os.Environ(), "DOCKPIPE_VM_DISPLAY_SUPERVISOR_TEST=1")
	supervisor.Stdin = strings.NewReader(string(data))
	supervisor.Stdout = io.Discard
	supervisor.Stderr = io.Discard
	if err := supervisor.Run(); err == nil {
		t.Fatal("supervisor accepted an invalid kernel process token")
	}
	if _, err := os.Lstat(filepath.Join(manager.state, "display-fixture-run")); !os.IsNotExist(err) {
		t.Fatal("invalid process identity created private bus state")
	}
}

func TestPrivateDisplayPathsRejectSymlinksAndOpenPermissions(t *testing.T) {
	directory := t.TempDir()
	if err := os.Chmod(directory, 0700); err != nil {
		t.Fatal(err)
	}
	path := filepath.Join(directory, "display.json")
	if err := os.WriteFile(path, []byte("{}"), 0600); err != nil {
		t.Fatal(err)
	}
	if !privateOwnedPath(path, false, 0600) {
		t.Fatal("owned private file rejected")
	}
	if err := os.Chmod(path, 0644); err != nil {
		t.Fatal(err)
	}
	if privateOwnedPath(path, false, 0600) {
		t.Fatal("publicly readable descriptor admitted")
	}
	link := filepath.Join(directory, "link")
	if err := os.Symlink(path, link); err != nil {
		t.Fatal(err)
	}
	if privateOwnedPath(link, false, 0644) {
		t.Fatal("redirected descriptor admitted")
	}
}

func TestQemuLauncherExecutesUnchangedTerminalArgumentsWithoutFrontend(t *testing.T) {
	executable, err := os.Executable()
	if err != nil {
		t.Fatal(err)
	}
	echo, err := exec.LookPath("echo")
	if err != nil {
		t.Fatal(err)
	}
	arguments := []string{"-display", "gtk", "-name", "fixture-terminal"}
	command := exec.Command(executable, arguments...)
	command.Env = append(os.Environ(), "DOCKPIPE_VM_QEMU_LAUNCH_TEST=1", "DOCKPIPE_VM_QEMU_REAL_BIN="+echo,
		"DOCKPIPE_VM_FRONTEND=off", "DOCKPIPE_SURFACE_LAUNCH_ID=")
	output, err := command.Output()
	if err != nil || string(output) != strings.Join(arguments, " ")+"\n" {
		t.Fatal("terminal QEMU arguments were changed or required an installed frontend")
	}

	command = exec.Command(executable, "--version")
	command.Env = append(os.Environ(), "DOCKPIPE_VM_QEMU_LAUNCH_TEST=1", "DOCKPIPE_VM_QEMU_REAL_BIN="+echo,
		"DOCKPIPE_VM_FRONTEND=native", "DOCKPIPE_VM_DISPLAY=", "DOCKPIPE_SURFACE_LAUNCH_ID=fixture")
	if _, err := command.Output(); err != nil {
		t.Fatal("a QEMU version probe tried to prepare a native display")
	}
}
