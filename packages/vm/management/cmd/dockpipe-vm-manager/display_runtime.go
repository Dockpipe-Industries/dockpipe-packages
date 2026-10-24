//go:build linux || darwin || windows

package main

import (
	"bufio"
	"context"
	"encoding/json"
	"encoding/xml"
	"fmt"
	"io"
	"os"
	"os/exec"
	"os/signal"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"
	"time"
)

type displaySupervisorConfiguration struct {
	Schema         int    `json:"schema"`
	RunID          string `json:"run_id"`
	StateRoot      string `json:"state_root"`
	QemuExecutable string `json:"qemu_executable"`
	LauncherPID    int    `json:"launcher_pid"`
	LauncherToken  string `json:"launcher_token"`
	PIDFile        string `json:"pid_file"`
}

func qemuSupportsDBus(ctx context.Context, executable string) bool {
	version := exec.CommandContext(ctx, executable, "--version")
	versionOutput := &boundedBuffer{maximum: 64 * 1024}
	version.Stdout = versionOutput
	version.Stderr = io.Discard
	if version.Run() != nil || !qemuVersionSupportsMap(versionOutput.Bytes()) {
		return false
	}
	command := exec.CommandContext(ctx, executable, "-display", "help")
	output := &boundedBuffer{maximum: 64 * 1024}
	command.Stdout = output
	command.Stderr = io.Discard
	if command.Run() != nil {
		return false
	}
	for _, field := range strings.Fields(string(output.Bytes())) {
		if field == "dbus" {
			return true
		}
	}
	return false
}

func resolveQemuExecutable(path string) (string, error) {
	if !filepath.IsAbs(path) {
		return "", fmt.Errorf("QEMU executable must be absolute")
	}
	resolved, err := filepath.EvalSymlinks(path)
	if err != nil {
		return "", fmt.Errorf("QEMU executable is unavailable")
	}
	info, err := os.Stat(resolved)
	if err != nil || !displayExecutable(info, resolved) {
		return "", fmt.Errorf("QEMU executable is unavailable")
	}
	return resolved, nil
}

func runQemuLauncher(arguments []string) error {
	configured, explicit := os.LookupEnv("DOCKPIPE_VM_QEMU_REAL_BIN")
	if !explicit {
		var err error
		configured, err = exec.LookPath("qemu-system-x86_64")
		if err != nil {
			return fmt.Errorf("stock QEMU is unavailable; set an explicit real QEMU executable")
		}
	}
	realQemu, err := resolveQemuExecutable(configured)
	if err != nil {
		return err
	}
	self, err := os.Executable()
	if err != nil {
		return err
	}
	self, _ = filepath.EvalSymlinks(self)
	if realQemu == self {
		return fmt.Errorf("the real QEMU executable must not resolve to its launcher")
	}
	requested, required, err := nativeDisplaySelection(os.Getenv("DOCKPIPE_VM_FRONTEND"), os.Getenv("DOCKPIPE_SURFACE_LAUNCH_ID"), os.Getenv("DOCKPIPE_VM_DISPLAY"))
	if err != nil {
		return err
	}
	// Capability and version probes must remain side-effect free.
	for index, argument := range arguments {
		if argument == "--version" || argument == "-version" || argument == "-help" || argument == "--help" {
			requested = false
		}
		if argument == "-display" && index+1 < len(arguments) && arguments[index+1] == "help" {
			requested = false
		}
	}
	if !requested {
		return executeDisplayQemu(realQemu, arguments, nil)
	}
	nativeQemu := realQemu
	if configured, present := os.LookupEnv("DOCKPIPE_VM_NATIVE_QEMU_BIN"); present {
		nativeQemu, err = resolveQemuExecutable(configured)
		if err != nil {
			return err
		}
	}
	packageRoot, err := installedPackageRoot()
	if err != nil {
		return err
	}
	capabilityContext, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	available := validateDisplayFactory(packageRoot) == nil && qemuSupportsDBus(capabilityContext, nativeQemu)
	cancel()
	if !available {
		if required {
			return fmt.Errorf("native VM display requires an installed compatible frontend and QEMU DBus support")
		}
		return executeDisplayQemu(realQemu, arguments, nil)
	}
	arguments, err = nativePointerArguments(arguments, os.Getenv("DOCKPIPE_VM_POINTER_MODE"))
	if err != nil {
		return err
	}
	ctx, cancel := context.WithTimeout(context.Background(), 8*time.Second)
	defer cancel()
	manager, err := newManager(ctx)
	if err != nil {
		return err
	}
	runID := os.Getenv("DOCKPIPE_RUN_ID")
	if !identifier.MatchString(runID) || !privateOwnedPath(manager.state, true, 0700) {
		return fmt.Errorf("native display requires an owned package state and valid resource identity")
	}
	configuration := displaySupervisorConfiguration{Schema: 1, RunID: runID, StateRoot: manager.state, QemuExecutable: nativeQemu, LauncherPID: os.Getpid()}
	configuration.LauncherToken, err = displayProcessToken(configuration.LauncherPID)
	if err != nil {
		return err
	}
	for index, argument := range arguments {
		if argument == "-pidfile" && index+1 < len(arguments) {
			configuration.PIDFile = arguments[index+1]
		}
	}
	if err := prepareDisplayLaunch(&configuration); err != nil {
		return err
	}
	defer releaseDisplayLaunch(configuration)
	address, err := startDisplaySupervisor(ctx, configuration)
	if err != nil {
		return err
	}
	arguments, err = nativeDisplayArguments(arguments, address)
	if err == nil {
		arguments, err = nativeAbsoluteInputArguments(arguments, address)
	}
	if err != nil {
		return err
	}
	return executeDisplayQemu(nativeQemu, arguments, &configuration)
}

func startDisplaySupervisor(ctx context.Context, configuration displaySupervisorConfiguration) (string, error) {
	executable, err := os.Executable()
	if err != nil {
		return "", err
	}
	command := exec.Command(executable, "display-supervise")
	configureDisplaySupervisor(command)
	input, err := command.StdinPipe()
	if err != nil {
		return "", err
	}
	output, err := command.StdoutPipe()
	if err != nil {
		return "", err
	}
	command.Stderr = io.Discard
	if err := command.Start(); err != nil {
		return "", fmt.Errorf("private display supervisor could not start")
	}
	if json.NewEncoder(input).Encode(configuration) != nil {
		input.Close()
		command.Process.Kill()
		return "", fmt.Errorf("private display supervisor configuration failed")
	}
	input.Close()
	response := make(chan string, 1)
	go func() {
		line, _ := bufio.NewReader(io.LimitReader(output, 4096)).ReadString('\n')
		response <- strings.TrimSpace(line)
		output.Close()
	}()
	select {
	case address := <-response:
		if !strings.HasPrefix(address, "unix:path=") || strings.ContainsAny(address, "\x00\r\n,") {
			command.Process.Kill()
			return "", fmt.Errorf("private display supervisor failed")
		}
		command.Process.Release()
		return address, nil
	case <-ctx.Done():
		command.Process.Kill()
		return "", fmt.Errorf("private display supervisor timed out")
	}
}

// The broker must not read a system session.conf or discover activation services.
// EXTERNAL authentication is admitted only through this resource's owner-private
// socket directory; the frontend separately verifies the exact QEMU identity.
func writePrivateDisplayBusConfiguration(directory, address string) (string, error) {
	if !privateOwnedPath(directory, true, 0700) || len(address) > 1024 ||
		address != privateDisplayBusAddress(filepath.Join(directory, "bus")) {
		return "", fmt.Errorf("invalid private display bus configuration")
	}
	var configuration strings.Builder
	configuration.WriteString("<busconfig><type>session</type><auth>EXTERNAL</auth><listen>")
	if err := xml.EscapeText(&configuration, []byte(address)); err != nil {
		return "", err
	}
	configuration.WriteString(`</listen><policy context="default"><allow send_destination="*"/><allow receive_sender="*"/><allow own="*"/></policy></busconfig>`)
	file, err := createPrivateDisplayFile(directory)
	if err != nil {
		return "", err
	}
	if _, err := file.WriteString(configuration.String()); err != nil {
		file.Close()
		os.Remove(file.Name())
		return "", err
	}
	if err := file.Close(); err != nil {
		os.Remove(file.Name())
		return "", err
	}
	return file.Name(), nil
}

func runDisplaySupervisor() error {
	lifetime, stop := signal.NotifyContext(context.Background(), syscall.SIGTERM, syscall.SIGINT)
	defer stop()
	var configuration displaySupervisorConfiguration
	decoder := json.NewDecoder(io.LimitReader(os.Stdin, 4096))
	decoder.DisallowUnknownFields()
	if decoder.Decode(&configuration) != nil || configuration.Schema != 1 || !identifier.MatchString(configuration.RunID) ||
		!privateOwnedPath(configuration.StateRoot, true, 0700) {
		return fmt.Errorf("invalid private display supervisor configuration")
	}
	var extra any
	if decoder.Decode(&extra) != io.EOF {
		return fmt.Errorf("invalid private display supervisor configuration")
	}
	launcherToken, err := displayProcessToken(configuration.LauncherPID)
	if err != nil || launcherToken != configuration.LauncherToken {
		return fmt.Errorf("private display launcher identity changed")
	}
	qemu, err := resolveQemuExecutable(configuration.QemuExecutable)
	if err != nil || qemu != configuration.QemuExecutable || configuration.RunID != os.Getenv("DOCKPIPE_RUN_ID") {
		return fmt.Errorf("invalid private display executable or resource identity")
	}
	if configuration.PIDFile != "" && (!filepath.IsAbs(configuration.PIDFile) || len(configuration.PIDFile) > 4096 || strings.ContainsAny(configuration.PIDFile, "\x00\r\n")) {
		return fmt.Errorf("invalid QEMU identity file reference")
	}
	ctx, cancel := context.WithTimeout(context.Background(), 8*time.Second)
	manager, err := newManager(ctx)
	cancel()
	if err != nil || manager.state != configuration.StateRoot {
		return fmt.Errorf("private display state does not match its SDK scope")
	}
	stateDirectory := filepath.Join(manager.state, "display-"+configuration.RunID)
	if createPrivateDisplayDirectory(stateDirectory) != nil {
		return fmt.Errorf("private display resource already exists or is unavailable")
	}
	defer os.Remove(stateDirectory)
	runtimeRoot, err := displayRuntimeRoot()
	if err == nil {
		runtimeRoot, err = filepath.EvalSymlinks(runtimeRoot)
	}
	if err != nil || !filepath.IsAbs(runtimeRoot) || !privateOwnedPath(runtimeRoot, true, 0700) {
		return fmt.Errorf("native display requires an owned platform runtime directory")
	}
	busDirectory, err := createPrivateDisplayTempDirectory(runtimeRoot)
	if err != nil {
		return fmt.Errorf("private display socket directory unavailable")
	}
	defer os.RemoveAll(busDirectory)
	address := privateDisplayBusAddress(filepath.Join(busDirectory, "bus"))
	if len(filepath.Join(busDirectory, "bus")) > 100 || strings.ContainsAny(address, ",\x00\r\n") {
		return fmt.Errorf("private display socket path exceeds its contract")
	}
	dbus, err := displayBusExecutable()
	if err != nil {
		return fmt.Errorf("native display requires dbus-daemon")
	}
	configurationPath, err := writePrivateDisplayBusConfiguration(busDirectory, address)
	if err != nil {
		return fmt.Errorf("private display bus configuration unavailable")
	}
	bus := exec.Command(dbus, "--config-file="+configurationPath, "--nofork", "--nopidfile", "--address="+address, "--print-address=1")
	bus.Stderr = io.Discard
	busOutput, err := bus.StdoutPipe()
	if err != nil || bus.Start() != nil {
		return fmt.Errorf("private display bus could not start")
	}
	busDone := make(chan struct{})
	go func() {
		bus.Wait()
		close(busDone)
	}()
	defer func() {
		bus.Process.Kill()
		<-busDone
	}()
	ready := make(chan bool, 1)
	go func() {
		line, err := bufio.NewReader(io.LimitReader(busOutput, 4096)).ReadString('\n')
		ready <- err == nil && strings.HasPrefix(strings.TrimSpace(line), address)
		busOutput.Close()
	}()
	select {
	case success := <-ready:
		if !success {
			return fmt.Errorf("private display bus did not become ready")
		}
	case <-time.After(3 * time.Second):
		return fmt.Errorf("private display bus readiness timed out")
	}
	fmt.Fprintln(os.Stdout, address)
	os.Stdout.Close()
	pid, token, err := waitForDisplayQemu(lifetime, configuration)
	if err != nil {
		return err
	}
	descriptor := displayDescriptor{Schema: 2, ResourceID: configuration.RunID, Backend: "qemu-dbus", BusAddress: address, InputAddress: privateDisplayBusAddress(filepath.Join(busDirectory, "input")), ConsoleID: 0, QemuPID: pid, QemuStartToken: token}
	path := filepath.Join(stateDirectory, "display.json")
	if err := writeDisplayDescriptor(path, descriptor); err != nil {
		return err
	}
	defer os.Remove(path)
	for {
		select {
		case <-lifetime.Done():
			return nil
		case <-busDone:
			return fmt.Errorf("private display bus stopped")
		case <-time.After(250 * time.Millisecond):
		}
		current, err := displayProcessToken(pid)
		if err != nil || current != token {
			return nil
		}
	}
}

func waitForDisplayQemu(ctx context.Context, configuration displaySupervisorConfiguration) (int, string, error) {
	deadline := time.Now().Add(20 * time.Second)
	for time.Now().Before(deadline) {
		if ctx.Err() != nil {
			return 0, "", ctx.Err()
		}
		pid := configuration.LauncherPID
		if configuration.PIDFile != "" {
			info, err := os.Lstat(configuration.PIDFile)
			if err != nil || !info.Mode().IsRegular() || info.Size() > 32 {
				time.Sleep(25 * time.Millisecond)
				continue
			}
			data, err := readDisplayFile(configuration.PIDFile, 32)
			if err != nil {
				return 0, "", fmt.Errorf("QEMU identity file unavailable")
			}
			pid, _ = strconv.Atoi(strings.TrimSpace(string(data)))
		}
		token, err := displayProcessToken(pid)
		if configuration.PIDFile == "" && (err != nil || token != configuration.LauncherToken) {
			return 0, "", fmt.Errorf("QEMU launcher exited or changed identity")
		}
		if err == nil && qemuExecutableIs(pid, configuration.QemuExecutable) &&
			(configuration.PIDFile != "" || token == configuration.LauncherToken) {
			return pid, token, nil
		}
		time.Sleep(25 * time.Millisecond)
	}
	return 0, "", fmt.Errorf("QEMU display identity did not become ready")
}

func writeDisplayDescriptor(path string, descriptor displayDescriptor) error {
	data, err := json.Marshal(descriptor)
	if err != nil || len(data) > 4096 {
		return fmt.Errorf("private display descriptor exceeds its contract")
	}
	file, err := createPrivateDisplayFile(filepath.Dir(path))
	if err != nil {
		return err
	}
	defer os.Remove(file.Name())
	if _, err := file.Write(data); err != nil {
		file.Close()
		return err
	}
	if err := file.Close(); err != nil {
		return err
	}
	return os.Rename(file.Name(), path)
}

func (manager manager) nativeDisplayOptions(record sessionRecord) map[string]any {
	packageRoot, err := installedPackageRoot()
	if err != nil || validateDisplayFactory(packageRoot) != nil {
		return nil
	}
	directory := filepath.Join(manager.state, "display-"+record.RunID)
	if !privateOwnedPath(directory, true, 0700) {
		return nil
	}
	canonicalDirectory, err := filepath.EvalSymlinks(directory)
	if err != nil {
		return nil
	}
	originalInfo, originalErr := os.Stat(directory)
	canonicalInfo, canonicalErr := os.Stat(canonicalDirectory)
	if originalErr != nil || canonicalErr != nil || !os.SameFile(originalInfo, canonicalInfo) {
		return nil
	}
	directory = canonicalDirectory
	path := filepath.Join(directory, "display.json")
	if !privateOwnedPath(directory, true, 0700) || !privateOwnedPath(path, false, 0600) {
		return nil
	}
	var descriptor displayDescriptor
	if decodeDisplayJSON(path, 4096, &descriptor) != nil || !validDisplayInputDescriptor(descriptor) || descriptor.ResourceID != record.RunID ||
		descriptor.Backend != "qemu-dbus" || descriptor.ConsoleID != 0 || !displayResourceMatches(record, descriptor) ||
		!strings.HasPrefix(descriptor.BusAddress, "unix:path=") {
		return nil
	}
	token, err := displayProcessToken(descriptor.QemuPID)
	if err != nil || token != descriptor.QemuStartToken {
		return nil
	}
	return map[string]any{"schema": 1, "resource_id": record.RunID, "state_ref": directory}
}
