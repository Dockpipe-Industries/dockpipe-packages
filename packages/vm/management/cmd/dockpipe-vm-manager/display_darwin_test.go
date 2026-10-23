//go:build darwin

package main

import (
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"testing"
)

func TestDisplayDarwinProcessIdentityAndExecutable(t *testing.T) {
	first, err := displayProcessToken(os.Getpid())
	if err != nil {
		t.Fatal(err)
	}
	parts := strings.Split(first, ":")
	if len(parts) != 2 {
		t.Fatal("macOS process token must contain start seconds and microseconds")
	}
	seconds, secondsError := strconv.ParseInt(parts[0], 10, 64)
	microseconds, microsecondsError := strconv.ParseInt(parts[1], 10, 64)
	if secondsError != nil || microsecondsError != nil || seconds <= 0 || microseconds < 0 || microseconds >= 1000000 {
		t.Fatal("invalid macOS process start time")
	}
	second, err := displayProcessToken(os.Getpid())
	if err != nil || first != second {
		t.Fatal("macOS process creation identity changed")
	}
	executable, err := os.Executable()
	if err != nil {
		t.Fatal(err)
	}
	executable, err = filepath.EvalSymlinks(executable)
	if err != nil || !qemuExecutableIs(os.Getpid(), executable) {
		t.Fatal("macOS executable admission failed")
	}
	if qemuExecutableIs(os.Getpid(), "/not-the-running-executable") {
		t.Fatal("macOS executable mismatch admitted")
	}
	if _, err := displayProcessToken(-1); err == nil {
		t.Fatal("invalid macOS process admitted")
	}
}

func TestDisplayDarwinPrivateFilesAndRuntime(t *testing.T) {
	root := filepath.Join(t.TempDir(), "private")
	if err := createPrivateDisplayDirectory(root); err != nil {
		t.Fatal(err)
	}
	if !privateOwnedPath(root, true, 0700) {
		t.Fatal("macOS private directory failed ownership/mode admission")
	}
	file, err := createPrivateDisplayFile(root)
	if err != nil {
		t.Fatal(err)
	}
	path := file.Name()
	file.Close()
	if !privateOwnedPath(path, false, 0600) {
		t.Fatal("macOS private file failed ownership/mode admission")
	}
	if err := os.Chmod(path, 0644); err != nil {
		t.Fatal(err)
	}
	if privateOwnedPath(path, false, 0600) {
		t.Fatal("publicly readable macOS descriptor admitted")
	}
	link := filepath.Join(root, "redirected")
	if err := os.Symlink(path, link); err != nil {
		t.Fatal(err)
	}
	if privateOwnedPath(link, false, 0644) {
		t.Fatal("macOS descriptor symlink admitted")
	}
	runtime, err := displayRuntimeRoot()
	if err != nil || !filepath.IsAbs(runtime) || !privateOwnedPath(filepath.Clean(runtime), true, 0700) {
		t.Fatal("macOS runtime directory is unavailable or not private")
	}
}

func TestDisplayDarwinTerminalPassthrough(t *testing.T) {
	executable, err := os.Executable()
	if err != nil {
		t.Fatal(err)
	}
	command := exec.Command(executable, "-display", "cocoa", "-name", "fixture-terminal")
	command.Env = append(os.Environ(), "DOCKPIPE_VM_QEMU_LAUNCH_TEST=1", "DOCKPIPE_VM_QEMU_REAL_BIN=/bin/echo",
		"DOCKPIPE_VM_FRONTEND=off", "DOCKPIPE_SURFACE_LAUNCH_ID=")
	output, err := command.Output()
	if err != nil || string(output) != "-display cocoa -name fixture-terminal\n" {
		t.Fatal("macOS terminal arguments were changed")
	}
}
