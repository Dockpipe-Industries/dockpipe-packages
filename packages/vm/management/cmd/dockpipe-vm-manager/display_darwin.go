//go:build darwin

package main

import (
	"bytes"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"syscall"

	"golang.org/x/sys/unix"
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

func displayProcessToken(pid int) (string, error) {
	if pid <= 1 {
		return "", fmt.Errorf("invalid display process identity")
	}
	process, err := unix.SysctlKinfoProc("kern.proc.pid", pid)
	if err != nil || process.Proc.P_pid != int32(pid) || process.Eproc.Ucred.Uid != uint32(os.Getuid()) || process.Proc.P_stat == 5 {
		return "", fmt.Errorf("display process identity or owner is unavailable")
	}
	start := process.Proc.P_starttime
	return strconv.FormatInt(start.Sec, 10) + ":" + strconv.FormatInt(int64(start.Usec), 10), nil
}

func qemuExecutableIs(pid int, executable string) bool {
	if _, err := displayProcessToken(pid); err != nil {
		return false
	}
	arguments, err := unix.SysctlRaw("kern.procargs2", pid)
	if err != nil || len(arguments) < 5 || len(arguments) > 1024*1024 {
		return false
	}
	ending := bytes.IndexByte(arguments[4:], 0)
	if ending < 0 {
		return false
	}
	path, err := filepath.EvalSymlinks(string(arguments[4 : 4+ending]))
	return err == nil && path == executable
}

func configureDisplaySupervisor(command *exec.Cmd) {
	command.SysProcAttr = &syscall.SysProcAttr{Setsid: true}
}

func displayRuntimeRoot() (string, error) {
	cache, err := os.UserCacheDir()
	if err != nil || !filepath.IsAbs(cache) {
		return "", fmt.Errorf("per-user macOS cache is unavailable")
	}
	path := filepath.Join(cache, "DockpipeDisplay")
	if err := os.Mkdir(path, 0700); err != nil && !os.IsExist(err) {
		return "", err
	}
	if !privateOwnedPath(path, true, 0700) {
		return "", fmt.Errorf("macOS display runtime directory is not private")
	}
	return path, nil
}

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
