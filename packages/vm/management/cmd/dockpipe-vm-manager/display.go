package main

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"runtime"
	"strconv"
	"strings"
)

type displayDescriptor struct {
	Schema         int    `json:"schema"`
	ResourceID     string `json:"resource_id"`
	Backend        string `json:"backend"`
	BusAddress     string `json:"bus_address"`
	ConsoleID      int    `json:"console_id"`
	QemuPID        int    `json:"qemu_pid"`
	QemuStartToken string `json:"qemu_start_token"`
}

type displayProcessExitError struct{ Code int }

func (err *displayProcessExitError) Error() string { return "QEMU process exited unsuccessfully" }

func displayLibraryName() string {
	switch runtime.GOOS {
	case "windows":
		return "dockpipe-vm-display.dll"
	case "darwin":
		return "libdockpipe-vm-display.dylib"
	default:
		return "libdockpipe-vm-display.so"
	}
}

func displaySurfacePlatform() string {
	if runtime.GOOS == "darwin" {
		return "macos"
	}
	return runtime.GOOS
}

func privateDisplayBusAddress(path string) string {
	var result strings.Builder
	result.WriteString("unix:path=")
	// Match the canonical escaping printed by dbus-daemon for readiness admission.
	const hexadecimal = "0123456789abcdef"
	for _, value := range []byte(filepath.ToSlash(path)) {
		if value >= 'a' && value <= 'z' || value >= 'A' && value <= 'Z' ||
			value >= '0' && value <= '9' || strings.ContainsRune("/_-.", rune(value)) {
			result.WriteByte(value)
		} else {
			result.WriteByte('%')
			result.WriteByte(hexadecimal[value>>4])
			result.WriteByte(hexadecimal[value&15])
		}
	}
	return result.String()
}

func displayBusExecutable() (string, error) {
	if configured, present := os.LookupEnv("DOCKPIPE_VM_DBUS_DAEMON_BIN"); present {
		return resolveQemuExecutable(configured)
	}
	name := "dbus-daemon"
	if runtime.GOOS == "windows" {
		name += ".exe"
	}
	if root, err := installedPackageRoot(); err == nil {
		candidate := filepath.Join(root, "assets", "display", "tooling", "bin", name)
		if _, err := os.Lstat(candidate); err == nil {
			return resolveQemuExecutable(candidate)
		}
	}
	return exec.LookPath(name)
}

func displayExecutable(info os.FileInfo, path string) bool {
	if !info.Mode().IsRegular() {
		return false
	}
	if runtime.GOOS == "windows" {
		return strings.EqualFold(filepath.Ext(path), ".exe")
	}
	return info.Mode().Perm()&0111 != 0
}

type displayFactory struct {
	Schema          int    `json:"schema"`
	Interface       string `json:"interface"`
	DisplayABI      int    `json:"display_abi"`
	QtVersion       string `json:"qt_version"`
	Platform        string `json:"platform"`
	Library         string `json:"library"`
	SHA256          string `json:"sha256"`
	SizeBytes       int64  `json:"size_bytes"`
	BuildABI        string `json:"build_abi"`
	CompilerID      string `json:"compiler_id"`
	CompilerVersion string `json:"compiler_version"`
}

var qemuVersionPattern = regexp.MustCompile(`(?m)^QEMU emulator version ([0-9]+)\.([0-9]+)\.([0-9]+)(?:[\s(-]|$)`)

func qemuVersionSupportsMap(output []byte) bool {
	match := qemuVersionPattern.FindSubmatch(output)
	if len(match) != 4 {
		return false
	}
	version := [3]int{}
	minimum := [3]int{10, 2, 4}
	for index := range version {
		value, err := strconv.Atoi(string(match[index+1]))
		if err != nil {
			return false
		}
		version[index] = value
	}
	for index := range version {
		if version[index] != minimum[index] {
			return version[index] > minimum[index]
		}
	}
	return true
}

func readDisplayFile(path string, maximum int64) ([]byte, error) {
	info, err := os.Lstat(path)
	if err != nil || !info.Mode().IsRegular() || info.Size() > maximum {
		return nil, fmt.Errorf("invalid display metadata")
	}
	file, err := os.Open(path)
	if err != nil {
		return nil, fmt.Errorf("display metadata unavailable")
	}
	defer file.Close()
	opened, err := file.Stat()
	if err != nil || !os.SameFile(info, opened) {
		return nil, fmt.Errorf("display metadata changed")
	}
	data, err := io.ReadAll(io.LimitReader(file, maximum+1))
	if err != nil || int64(len(data)) > maximum {
		return nil, fmt.Errorf("display metadata exceeds its contract")
	}
	return data, nil
}

func decodeDisplayJSON(path string, maximum int64, target any) error {
	data, err := readDisplayFile(path, maximum)
	if err != nil {
		return err
	}
	fields := json.NewDecoder(bytes.NewReader(data))
	opening, err := fields.Token()
	if err != nil || opening != json.Delim('{') {
		return fmt.Errorf("display metadata must be an object")
	}
	seen := make(map[string]bool)
	for fields.More() {
		token, err := fields.Token()
		name, ok := token.(string)
		if err != nil || !ok || seen[name] {
			return fmt.Errorf("duplicate or invalid display metadata field")
		}
		seen[name] = true
		var value json.RawMessage
		if fields.Decode(&value) != nil {
			return fmt.Errorf("invalid display metadata field")
		}
	}
	if _, descriptor := target.(*displayDescriptor); descriptor && len(seen) != 7 {
		return fmt.Errorf("incomplete display resource metadata")
	}
	decoder := json.NewDecoder(bytes.NewReader(data))
	decoder.DisallowUnknownFields()
	if decoder.Decode(target) != nil {
		return fmt.Errorf("invalid display metadata")
	}
	var extra any
	if decoder.Decode(&extra) != io.EOF {
		return fmt.Errorf("trailing display metadata")
	}
	return nil
}

func validateDisplayFactory(packageRoot string) error {
	surfaceData, err := readDisplayFile(filepath.Join(packageRoot, "assets", "ui", "surface.json"), 64*1024)
	if err != nil {
		return fmt.Errorf("native display is not declared by its owning surface")
	}
	var surface struct {
		NativeDisplay struct {
			Interface int               `json:"interface"`
			Libraries map[string]string `json:"libraries"`
		} `json:"native_display"`
	}
	if json.Unmarshal(surfaceData, &surface) != nil || surface.NativeDisplay.Interface != 1 ||
		surface.NativeDisplay.Libraries[displaySurfacePlatform()] != "assets/display/"+displayLibraryName() {
		return fmt.Errorf("native display factory is not declared for this platform")
	}
	directory := filepath.Join(packageRoot, "assets", "display")
	var factory displayFactory
	if err := decodeDisplayJSON(filepath.Join(directory, "display-factory.json"), 4096, &factory); err != nil {
		return err
	}
	if factory.Schema != 1 || factory.Interface != "com.dockpipe.launcher.DisplayPackage/1.0" ||
		factory.DisplayABI != 1 || factory.QtVersion != "6.10.3" || factory.Platform != runtime.GOOS+"-"+runtime.GOARCH ||
		factory.Library != displayLibraryName() || factory.SizeBytes <= 0 || factory.SizeBytes > 64*1024*1024 ||
		factory.BuildABI == "" || factory.CompilerID == "" || factory.CompilerVersion == "" {
		return fmt.Errorf("installed native display factory is incompatible")
	}
	expected, err := hex.DecodeString(factory.SHA256)
	if err != nil || len(expected) != sha256.Size || hex.EncodeToString(expected) != factory.SHA256 {
		return fmt.Errorf("invalid display factory digest")
	}
	path := filepath.Join(directory, factory.Library)
	info, err := os.Lstat(path)
	if err != nil || !info.Mode().IsRegular() || info.Size() != factory.SizeBytes {
		return fmt.Errorf("installed native display factory is unavailable")
	}
	file, err := os.Open(path)
	if err != nil {
		return fmt.Errorf("installed native display factory is unavailable")
	}
	defer file.Close()
	opened, err := file.Stat()
	if err != nil || !os.SameFile(info, opened) {
		return fmt.Errorf("display factory changed")
	}
	hash := sha256.New()
	if _, err := io.Copy(hash, io.LimitReader(file, factory.SizeBytes+1)); err != nil || hex.EncodeToString(hash.Sum(nil)) != factory.SHA256 {
		return fmt.Errorf("display factory digest mismatch")
	}
	return nil
}

func nativeDisplaySelection(mode, launchID, authoredDisplay string) (requested, required bool, err error) {
	switch mode {
	case "", "auto":
		return launchID != "" && authoredDisplay == "", false, nil
	case "off":
		return false, false, nil
	case "native":
		if authoredDisplay != "" {
			return false, true, fmt.Errorf("native frontend conflicts with an explicit QEMU display")
		}
		return true, true, nil
	default:
		return false, false, fmt.Errorf("unsupported VM frontend selection")
	}
}

func nativeDisplayArguments(arguments []string, address string) ([]string, error) {
	if !strings.HasPrefix(address, "unix:path=") || strings.ContainsAny(address, "\x00\r\n,") {
		return nil, fmt.Errorf("invalid private display address")
	}
	result := make([]string, 0, len(arguments)+2)
	for index := 0; index < len(arguments); index++ {
		if arguments[index] == "-display" {
			if index+1 >= len(arguments) {
				return nil, fmt.Errorf("missing QEMU display value")
			}
			index++
			continue
		}
		result = append(result, arguments[index])
	}
	return append(result, "-display", "dbus,addr="+address), nil
}

func installedPackageRoot() (string, error) {
	executable, err := os.Executable()
	if err != nil {
		return "", err
	}
	return filepath.Clean(filepath.Join(filepath.Dir(executable), "..", "..", "..")), nil
}
