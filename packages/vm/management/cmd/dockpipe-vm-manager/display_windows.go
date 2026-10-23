//go:build windows

package main

import (
	"crypto/rand"
	"encoding/hex"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"
	"unsafe"

	"golang.org/x/sys/windows"
)

func currentDisplaySID() (*windows.SID, error) {
	user, err := windows.GetCurrentProcessToken().GetTokenUser()
	if err != nil {
		return nil, err
	}
	return user.User.Sid, nil
}

func privateOwnedPath(path string, directory bool, mode os.FileMode) bool {
	for ancestor := filepath.Clean(path); ; ancestor = filepath.Dir(ancestor) {
		name, err := windows.UTF16PtrFromString(ancestor)
		if err != nil {
			return false
		}
		attributes, err := windows.GetFileAttributes(name)
		if err != nil || attributes&windows.FILE_ATTRIBUTE_REPARSE_POINT != 0 {
			return false
		}
		if parent := filepath.Dir(ancestor); parent == ancestor {
			break
		}
	}
	info, err := os.Lstat(path)
	if err != nil || info.IsDir() != directory || info.Mode()&os.ModeSymlink != 0 || (!directory && !info.Mode().IsRegular()) {
		return false
	}
	name, err := windows.UTF16PtrFromString(path)
	if err != nil {
		return false
	}
	attributes, err := windows.GetFileAttributes(name)
	if err != nil || attributes&windows.FILE_ATTRIBUTE_REPARSE_POINT != 0 {
		return false
	}
	sid, err := currentDisplaySID()
	if err != nil {
		return false
	}
	security, err := windows.GetNamedSecurityInfo(path, windows.SE_FILE_OBJECT,
		windows.OWNER_SECURITY_INFORMATION|windows.DACL_SECURITY_INFORMATION)
	if err != nil {
		return false
	}
	owner, _, err := security.Owner()
	if err != nil || owner == nil || !windows.EqualSid(owner, sid) {
		return false
	}
	acl, _, err := security.DACL()
	if err != nil || acl == nil || acl.AceCount == 0 {
		return false
	}
	allowed := map[string]bool{sid.String(): true, "S-1-5-18": true, "S-1-5-32-544": true}
	hasUser := false
	for index := uint32(0); index < uint32(acl.AceCount); index++ {
		var ace *windows.ACCESS_ALLOWED_ACE
		if windows.GetAce(acl, index, &ace) != nil || ace.Header.AceType != windows.ACCESS_ALLOWED_ACE_TYPE {
			return false
		}
		principal := (*windows.SID)(unsafe.Pointer(&ace.SidStart)).String()
		if !allowed[principal] {
			return false
		}
		if principal == sid.String() && ace.Header.AceFlags&windows.INHERIT_ONLY_ACE == 0 {
			hasUser = true
		}
	}
	return hasUser
}

func displaySecurityAttributes(directory bool) (*windows.SecurityAttributes, error) {
	sid, err := currentDisplaySID()
	if err != nil {
		return nil, err
	}
	inheritance := ""
	if directory {
		inheritance = "OICI"
	}
	descriptor, err := windows.SecurityDescriptorFromString("O:" + sid.String() + "D:P" +
		"(A;" + inheritance + ";FA;;;" + sid.String() + ")(A;" + inheritance + ";FA;;;SY)(A;" + inheritance + ";FA;;;BA)")
	if err != nil {
		return nil, err
	}
	return &windows.SecurityAttributes{Length: uint32(unsafe.Sizeof(windows.SecurityAttributes{})), SecurityDescriptor: descriptor}, nil
}

func createPrivateDisplayDirectory(path string) error {
	attributes, err := displaySecurityAttributes(true)
	if err != nil {
		return err
	}
	name, err := windows.UTF16PtrFromString(path)
	if err != nil {
		return err
	}
	return windows.CreateDirectory(name, attributes)
}

func privateDisplayRandomName(root, prefix string) (string, error) {
	var random [8]byte
	if _, err := rand.Read(random[:]); err != nil {
		return "", err
	}
	return filepath.Join(root, prefix+hex.EncodeToString(random[:])), nil
}

func createPrivateDisplayTempDirectory(root string) (string, error) {
	path, err := privateDisplayRandomName(root, "dp-")
	if err != nil {
		return "", err
	}
	return path, createPrivateDisplayDirectory(path)
}

func createPrivateDisplayFile(directory string) (*os.File, error) {
	path, err := privateDisplayRandomName(directory, ".display-")
	if err != nil {
		return nil, err
	}
	attributes, err := displaySecurityAttributes(false)
	if err != nil {
		return nil, err
	}
	name, err := windows.UTF16PtrFromString(path)
	if err != nil {
		return nil, err
	}
	handle, err := windows.CreateFile(name, windows.GENERIC_READ|windows.GENERIC_WRITE, 0, attributes,
		windows.CREATE_NEW, windows.FILE_ATTRIBUTE_NORMAL, 0)
	if err != nil {
		return nil, err
	}
	return os.NewFile(uintptr(handle), path), nil
}

func displayRuntimeRoot() (string, error) {
	root, err := os.UserCacheDir()
	if err != nil || !filepath.IsAbs(root) {
		return "", fmt.Errorf("per-user Windows cache is unavailable")
	}
	path := filepath.Join(root, "DockpipeDisplay")
	if err := createPrivateDisplayDirectory(path); err != nil && !os.IsExist(err) {
		return "", err
	}
	if !privateOwnedPath(path, true, 0700) {
		return "", fmt.Errorf("Windows display runtime directory is not private")
	}
	return path, nil
}

func displayProcessToken(pid int) (string, error) {
	handle, err := windows.OpenProcess(windows.PROCESS_QUERY_LIMITED_INFORMATION|windows.SYNCHRONIZE, false, uint32(pid))
	if err != nil {
		return "", fmt.Errorf("display process is unavailable")
	}
	defer windows.CloseHandle(handle)
	state, err := windows.WaitForSingleObject(handle, 0)
	if err != nil || state != uint32(windows.WAIT_TIMEOUT) {
		return "", fmt.Errorf("display process is not running")
	}
	var token windows.Token
	if windows.OpenProcessToken(handle, windows.TOKEN_QUERY, &token) != nil {
		return "", fmt.Errorf("display process owner is unavailable")
	}
	defer token.Close()
	user, err := token.GetTokenUser()
	sid, sidError := currentDisplaySID()
	if err != nil || sidError != nil || !windows.EqualSid(user.User.Sid, sid) {
		return "", fmt.Errorf("display process owner mismatch")
	}
	var creation, exit, kernel, userTime windows.Filetime
	if windows.GetProcessTimes(handle, &creation, &exit, &kernel, &userTime) != nil {
		return "", fmt.Errorf("display process creation time unavailable")
	}
	ticks := uint64(creation.HighDateTime)<<32 | uint64(creation.LowDateTime)
	return strconv.FormatUint(ticks, 10), nil
}

func qemuExecutableIs(pid int, executable string) bool {
	handle, err := windows.OpenProcess(windows.PROCESS_QUERY_LIMITED_INFORMATION, false, uint32(pid))
	if err != nil {
		return false
	}
	defer windows.CloseHandle(handle)
	buffer := make([]uint16, 32768)
	size := uint32(len(buffer))
	if windows.QueryFullProcessImageName(handle, 0, &buffer[0], &size) != nil {
		return false
	}
	actual, err := filepath.EvalSymlinks(windows.UTF16ToString(buffer[:size]))
	return err == nil && strings.EqualFold(actual, executable)
}

func configureDisplaySupervisor(command *exec.Cmd) {
	command.SysProcAttr = &syscall.SysProcAttr{CreationFlags: windows.CREATE_NEW_PROCESS_GROUP | windows.CREATE_NO_WINDOW}
}

func prepareDisplayLaunch(configuration *displaySupervisorConfiguration) error {
	configuration.PIDFile = filepath.Join(configuration.StateRoot, ".display-child-"+configuration.RunID+".pid")
	if _, err := os.Lstat(configuration.PIDFile); !os.IsNotExist(err) {
		return fmt.Errorf("native display child identity already exists or is unavailable")
	}
	return nil
}

func releaseDisplayLaunch(configuration displaySupervisorConfiguration) {
	if privateOwnedPath(configuration.PIDFile, false, 0600) {
		os.Remove(configuration.PIDFile)
	}
}

func writeDisplayChildPID(configuration displaySupervisorConfiguration, pid int) error {
	file, err := createPrivateDisplayFile(configuration.StateRoot)
	if err != nil {
		return err
	}
	defer os.Remove(file.Name())
	if _, err := fmt.Fprintln(file, pid); err != nil {
		file.Close()
		return err
	}
	if err := file.Close(); err != nil {
		return err
	}
	return os.Rename(file.Name(), configuration.PIDFile)
}

func executeDisplayQemu(executable string, arguments []string, configuration *displaySupervisorConfiguration) error {
	job, err := windows.CreateJobObject(nil, nil)
	if err != nil {
		return fmt.Errorf("QEMU lifecycle job could not be created")
	}
	defer windows.CloseHandle(job)
	limits := windows.JOBOBJECT_EXTENDED_LIMIT_INFORMATION{}
	limits.BasicLimitInformation.LimitFlags = windows.JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
	if _, err := windows.SetInformationJobObject(job, windows.JobObjectExtendedLimitInformation,
		uintptr(unsafe.Pointer(&limits)), uint32(unsafe.Sizeof(limits))); err != nil {
		return fmt.Errorf("QEMU lifecycle job limits could not be applied")
	}
	quoted := make([]string, 0, len(arguments)+1)
	for _, argument := range append([]string{executable}, arguments...) {
		quoted = append(quoted, syscall.EscapeArg(argument))
	}
	commandLine, err := windows.UTF16PtrFromString(strings.Join(quoted, " "))
	if err != nil {
		return err
	}
	application, err := windows.UTF16PtrFromString(executable)
	if err != nil {
		return err
	}
	startup := windows.StartupInfo{Cb: uint32(unsafe.Sizeof(windows.StartupInfo{}))}
	var process windows.ProcessInformation
	if windows.CreateProcess(application, commandLine, nil, nil, false,
		windows.CREATE_SUSPENDED|windows.CREATE_NEW_PROCESS_GROUP|windows.CREATE_NO_WINDOW, nil, nil, &startup, &process) != nil {
		return fmt.Errorf("QEMU child process could not be created")
	}
	defer windows.CloseHandle(process.Process)
	defer windows.CloseHandle(process.Thread)
	if err := windows.AssignProcessToJobObject(job, process.Process); err != nil {
		windows.TerminateProcess(process.Process, 1)
		return fmt.Errorf("QEMU child could not join its lifecycle job")
	}
	if configuration != nil {
		if err := writeDisplayChildPID(*configuration, int(process.ProcessId)); err != nil {
			return fmt.Errorf("QEMU child identity could not be published")
		}
	}
	if _, err := windows.ResumeThread(process.Thread); err != nil {
		return fmt.Errorf("QEMU child could not be resumed")
	}
	if _, err := windows.WaitForSingleObject(process.Process, windows.INFINITE); err != nil {
		return fmt.Errorf("QEMU child lifecycle wait failed")
	}
	var exitCode uint32
	if windows.GetExitCodeProcess(process.Process, &exitCode) != nil {
		return fmt.Errorf("QEMU child exit status unavailable")
	}
	if exitCode != 0 {
		return &displayProcessExitError{Code: int(exitCode)}
	}
	return nil
}

func displayResourceMatches(record sessionRecord, descriptor displayDescriptor) bool {
	if descriptor.QemuPID == record.PID {
		return true
	}
	root, err := installedPackageRoot()
	if err != nil || !qemuExecutableIs(record.PID, filepath.Join(root, "assets", "tooling", "bin", "dockpipe-qemu-launch.exe")) {
		return false
	}
	if _, err := displayProcessToken(record.PID); err != nil {
		return false
	}
	snapshot, err := windows.CreateToolhelp32Snapshot(windows.TH32CS_SNAPPROCESS, 0)
	if err != nil {
		return false
	}
	defer windows.CloseHandle(snapshot)
	entry := windows.ProcessEntry32{Size: uint32(unsafe.Sizeof(windows.ProcessEntry32{}))}
	for err = windows.Process32First(snapshot, &entry); err == nil; err = windows.Process32Next(snapshot, &entry) {
		if int(entry.ProcessID) == descriptor.QemuPID {
			return int(entry.ParentProcessID) == record.PID
		}
	}
	return false
}
