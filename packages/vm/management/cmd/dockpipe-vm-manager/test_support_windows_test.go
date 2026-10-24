//go:build windows

package main

import (
	"os"
	"path/filepath"
	"testing"
)

func TestSSHFixtureDoesNotAliasRunningWindowsImage(t *testing.T) {
	manager := helperManager(t)
	installSSHHelper(t, manager)
	original, err := os.Stat(manager.program)
	if err != nil {
		t.Fatal(err)
	}
	fixture, err := os.Stat(filepath.Join(manager.workspace, "transport", "ssh.exe"))
	if err != nil {
		t.Fatal(err)
	}
	if os.SameFile(original, fixture) {
		t.Fatal("SSH fixture aliases the running Windows test image and cannot be removed")
	}
}
