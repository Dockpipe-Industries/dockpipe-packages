//go:build windows

package main

import (
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"testing"
	"time"

	"golang.org/x/sys/windows"
)

func windowsPrivateTestDirectory(t *testing.T) string {
	t.Helper()
	path := filepath.Join(t.TempDir(), "owned")
	if err := createPrivateDisplayDirectory(path); err != nil {
		t.Fatal(err)
	}
	return path
}

func TestDisplayWindowsPrivateOwnerAndACL(t *testing.T) {
	root := windowsPrivateTestDirectory(t)
	if !privateOwnedPath(root, true, 0700) {
		t.Fatal("new Windows display directory lacks its explicit current-user owner/private ACL")
	}
	file, err := createPrivateDisplayFile(root)
	if err != nil {
		t.Fatal(err)
	}
	path := file.Name()
	file.Close()
	if !privateOwnedPath(path, false, 0600) {
		t.Fatal("new Windows display file lacks its explicit current-user owner/private ACL")
	}
	sid, err := currentDisplaySID()
	if err != nil {
		t.Fatal(err)
	}
	public, err := windows.SecurityDescriptorFromString("O:" + sid.String() + "D:P(A;;FA;;;" + sid.String() + ")(A;;FR;;;WD)")
	if err != nil {
		t.Fatal(err)
	}
	acl, _, err := public.DACL()
	if err != nil {
		t.Fatal(err)
	}
	if err := windows.SetNamedSecurityInfo(path, windows.SE_FILE_OBJECT,
		windows.DACL_SECURITY_INFORMATION|windows.PROTECTED_DACL_SECURITY_INFORMATION, nil, nil, acl, nil); err != nil {
		t.Fatal(err)
	}
	if privateOwnedPath(path, false, 0600) {
		t.Fatal("Windows descriptor granting Everyone access was admitted")
	}
}

func TestDisplayWindowsProcessCreationIdentity(t *testing.T) {
	first, err := displayProcessToken(os.Getpid())
	if err != nil {
		t.Fatal(err)
	}
	creation, err := strconv.ParseUint(first, 10, 64)
	if err != nil || creation == 0 {
		t.Fatal("Windows process token must be decimal FILETIME creation ticks")
	}
	second, err := displayProcessToken(os.Getpid())
	if err != nil || second != first {
		t.Fatal("Windows process creation identity changed")
	}
	executable, err := os.Executable()
	if err != nil {
		t.Fatal(err)
	}
	executable, err = filepath.EvalSymlinks(executable)
	if err != nil || !qemuExecutableIs(os.Getpid(), executable) {
		t.Fatal("Windows executable identity was rejected")
	}
	if _, err := displayProcessToken(0); err == nil {
		t.Fatal("invalid Windows process admitted")
	}
}

func windowsFixtureExecutable(t *testing.T, directory string) string {
	t.Helper()
	executable, err := os.Executable()
	if err != nil {
		t.Fatal(err)
	}
	source, err := os.Open(executable)
	if err != nil {
		t.Fatal(err)
	}
	defer source.Close()
	destination, err := createPrivateDisplayFile(directory)
	if err != nil {
		t.Fatal(err)
	}
	temporary := destination.Name()
	if _, err := io.Copy(destination, source); err != nil {
		destination.Close()
		t.Fatal(err)
	}
	if err := destination.Close(); err != nil {
		t.Fatal(err)
	}
	path := filepath.Join(directory, "fixture-child.exe")
	if err := os.Rename(temporary, path); err != nil {
		t.Fatal(err)
	}
	return path
}

func waitWindowsFixturePID(t *testing.T, path string) int {
	t.Helper()
	deadline := time.Now().Add(5 * time.Second)
	for time.Now().Before(deadline) {
		if data, err := os.ReadFile(path); err == nil {
			pid, parseError := strconv.Atoi(string(data))
			if parseError == nil && pid > 1 {
				return pid
			}
		}
		time.Sleep(20 * time.Millisecond)
	}
	t.Fatal("owned Windows fixture did not publish its PID")
	return 0
}

func TestDisplayWindowsJobKillsOwnedChildAndPreservesUnrelatedProcess(t *testing.T) {
	root := windowsPrivateTestDirectory(t)
	child := windowsFixtureExecutable(t, root)
	executable, err := os.Executable()
	if err != nil {
		t.Fatal(err)
	}
	ownedPIDFile := filepath.Join(root, "owned.pid")
	wrapper := exec.Command(executable)
	wrapper.Env = append(os.Environ(), "DOCKPIPE_VM_QEMU_LAUNCH_TEST=1", "DOCKPIPE_VM_DISPLAY_CHILD_TEST=1",
		"DOCKPIPE_VM_DISPLAY_CHILD_PID_FILE="+ownedPIDFile, "DOCKPIPE_VM_QEMU_REAL_BIN="+child,
		"DOCKPIPE_VM_FRONTEND=off", "DOCKPIPE_SURFACE_LAUNCH_ID=")
	if err := wrapper.Start(); err != nil {
		t.Fatal(err)
	}
	defer wrapper.Process.Kill()
	ownedPID := waitWindowsFixturePID(t, ownedPIDFile)
	ownedToken, err := displayProcessToken(ownedPID)
	if err != nil {
		t.Fatal(err)
	}
	unrelated := exec.Command(child)
	unrelated.Env = append(os.Environ(), "DOCKPIPE_VM_DISPLAY_CHILD_TEST=1",
		"DOCKPIPE_VM_DISPLAY_CHILD_PID_FILE="+filepath.Join(root, "unrelated.pid"))
	if err := unrelated.Start(); err != nil {
		t.Fatal(err)
	}
	defer func() { unrelated.Process.Kill(); unrelated.Wait() }()
	waitWindowsFixturePID(t, filepath.Join(root, "unrelated.pid"))
	if err := wrapper.Process.Kill(); err != nil {
		t.Fatal(err)
	}
	wrapper.Wait()
	deadline := time.Now().Add(5 * time.Second)
	for time.Now().Before(deadline) {
		token, err := displayProcessToken(ownedPID)
		if err != nil || token != ownedToken {
			break
		}
		time.Sleep(20 * time.Millisecond)
	}
	if token, err := displayProcessToken(ownedPID); err == nil && token == ownedToken {
		t.Fatal("closing the lifecycle wrapper left its QEMU fixture child running")
	}
	if _, err := displayProcessToken(unrelated.Process.Pid); err != nil {
		t.Fatal("closing the lifecycle job affected an unrelated process")
	}
}

func TestDisplayWindowsPreservesNativeExitCode(t *testing.T) {
	root := windowsPrivateTestDirectory(t)
	child := windowsFixtureExecutable(t, root)
	executable, err := os.Executable()
	if err != nil {
		t.Fatal(err)
	}
	command := exec.Command(executable)
	command.Env = append(os.Environ(), "DOCKPIPE_VM_QEMU_LAUNCH_TEST=1", "DOCKPIPE_VM_DISPLAY_CHILD_TEST=1",
		"DOCKPIPE_VM_DISPLAY_CHILD_EXIT=23", "DOCKPIPE_VM_QEMU_REAL_BIN="+child, "DOCKPIPE_VM_FRONTEND=off")
	err = command.Run()
	exit, ok := err.(*exec.ExitError)
	if !ok || exit.ExitCode() != 23 {
		t.Fatal("native Windows QEMU exit code 23 was not preserved")
	}
}
