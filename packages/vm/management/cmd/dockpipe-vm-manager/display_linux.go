//go:build linux

package main

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"
)

func privateOwnedPath(path string, directory bool, mode os.FileMode) bool {
	info, err := os.Lstat(path)
	if err != nil || info.Mode().Perm() != mode || info.IsDir() != directory || info.Mode()&os.ModeSymlink != 0 {
		return false
	}
	if !directory && !info.Mode().IsRegular() {
		return false
	}
	stat, ok := info.Sys().(*syscall.Stat_t)
	return ok && int(stat.Uid) == os.Getuid()
}

func linuxProcessToken(pid int) (string, error) {
	if pid <= 1 {
		return "", fmt.Errorf("invalid display process identity")
	}
	path := filepath.Join("/proc", strconv.Itoa(pid))
	info, err := os.Stat(path)
	if err != nil {
		return "", err
	}
	stat, ok := info.Sys().(*syscall.Stat_t)
	if !ok || int(stat.Uid) != os.Getuid() {
		return "", fmt.Errorf("display process owner mismatch")
	}
	data, err := os.ReadFile(filepath.Join(path, "stat"))
	if err != nil || len(data) > 4096 {
		return "", fmt.Errorf("display process identity unavailable")
	}
	closing := strings.LastIndexByte(string(data), ')')
	if closing < 0 {
		return "", fmt.Errorf("invalid display process identity")
	}
	fields := strings.Fields(string(data[closing+1:]))
	if len(fields) < 20 || fields[0] == "Z" {
		return "", fmt.Errorf("display process is not running")
	}
	if _, err := strconv.ParseUint(fields[19], 10, 64); err != nil {
		return "", fmt.Errorf("invalid display process token")
	}
	return fields[19], nil
}

func qemuExecutableIs(pid int, executable string) bool {
	actual, err := os.Readlink(filepath.Join("/proc", strconv.Itoa(pid), "exe"))
	return err == nil && actual == executable
}

func displayProcessToken(pid int) (string, error) { return linuxProcessToken(pid) }
func configureDisplaySupervisor(command *exec.Cmd) {
	command.SysProcAttr = &syscall.SysProcAttr{Setsid: true}
}
func displayRuntimeRoot() (string, error)             { return os.Getenv("XDG_RUNTIME_DIR"), nil }
func createPrivateDisplayDirectory(path string) error { return os.Mkdir(path, 0700) }
func createPrivateDisplayTempDirectory(root string) (string, error) {
	return os.MkdirTemp(root, "dockpipe-vm-")
}
func createPrivateDisplayFile(directory string) (*os.File, error) {
	return os.CreateTemp(directory, ".display-")
}
func prepareDisplayLaunch(configuration *displaySupervisorConfiguration) error { return nil }
func releaseDisplayLaunch(configuration displaySupervisorConfiguration)        {}
func executeDisplayQemu(executable string, arguments []string, configuration *displaySupervisorConfiguration) error {
	return syscall.Exec(executable, append([]string{executable}, arguments...), os.Environ())
}
func displayResourceMatches(record sessionRecord, descriptor displayDescriptor) bool {
	return descriptor.QemuPID == record.PID
}
