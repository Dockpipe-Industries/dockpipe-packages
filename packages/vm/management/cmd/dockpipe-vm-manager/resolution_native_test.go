//go:build linux || darwin || windows

package main

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
	"time"
)

func TestResolutionRequestPreservesOrdinaryQemuRange(t *testing.T) {
	manager := manager{state: t.TempDir()}
	if _, err := manager.resolutionRequest(sessionRecord{RunID: "test-run"}, "8192", "8192"); err != nil {
		t.Fatalf("ordinary QEMU request lost the generic driver-dependent range: %v", err)
	}
}

func TestResolutionRequestRefusesUnverifiedNativeState(t *testing.T) {
	manager := manager{state: t.TempDir()}
	record := sessionRecord{RunID: "test-run"}
	if err := os.Mkdir(filepath.Join(manager.state, "display-"+record.RunID), 0700); err != nil {
		t.Fatal(err)
	}
	if _, err := manager.resolutionRequest(record, "1920", "1080"); err == nil {
		t.Fatal("unverified native state silently fell back to ordinary QEMU")
	}
}

func TestNativeResolutionWithinLimits(t *testing.T) {
	cases := []struct {
		name    string
		width   int
		height  int
		allowed bool
	}{
		{"full-HD", 1920, 1080, true},
		{"exact-frame-budget", 4096, 2048, true},
		{"one-row-over-budget", 4096, 2049, false},
		{"square-within-axis-but-over-memory", 3072, 3072, false},
		{"over-axis-within-memory", 5120, 1440, false},
		{"UHD-within-renderer-only", 3840, 2160, true},
		{"empty-frame", 0, 1080, false},
		{"negative-frame", -1920, 1080, false},
	}
	for _, test := range cases {
		t.Run(test.name, func(t *testing.T) {
			if actual := nativeResolutionWithinLimits(test.width, test.height); actual != test.allowed {
				t.Fatalf("native renderer admits=%v, expected=%v", actual, test.allowed)
			}
		})
	}
}

// Re-execution uses the real installed-path, factory, private metadata and native
// process identity admission. The synthetic library is hashed, never loaded.
func TestResolutionVerifiedNativeSession(t *testing.T) {
	if root := os.Getenv("DOCKPIPE_VM_RESOLUTION_TEST_ROOT"); root != "" {
		testResolutionVerifiedNativeSession(t, root)
		return
	}

	original, err := os.Executable()
	if err != nil {
		t.Fatal(err)
	}
	executable, err := os.ReadFile(original)
	if err != nil {
		t.Fatal(err)
	}
	root := t.TempDir()
	name := "dockpipe-vm-manager"
	if runtime.GOOS == "windows" {
		name += ".exe"
	}
	program := filepath.Join(root, "assets", "tooling", "bin", name)
	if err := os.MkdirAll(filepath.Dir(program), 0700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(program, executable, 0700); err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	command := exec.CommandContext(ctx, program, "-test.run=^TestResolutionVerifiedNativeSession$", "-test.v")
	for _, entry := range os.Environ() {
		if !strings.HasPrefix(entry, "DOCKPIPE_VM_RESOLUTION_TEST_ROOT=") {
			command.Env = append(command.Env, entry)
		}
	}
	command.Env = append(command.Env, "DOCKPIPE_VM_RESOLUTION_TEST_ROOT="+root)
	if output, err := command.CombinedOutput(); err != nil {
		t.Fatalf("real native resolution admission failed: %v\n%s", err, output)
	}
}

func testResolutionVerifiedNativeSession(t *testing.T, root string) {
	directory := filepath.Join(root, "assets", "display")
	if err := os.MkdirAll(directory, 0700); err != nil {
		t.Fatal(err)
	}
	if err := os.MkdirAll(filepath.Join(root, "assets", "ui"), 0700); err != nil {
		t.Fatal(err)
	}
	surface, err := json.Marshal(map[string]any{"native_display": map[string]any{
		"interface": 1, "libraries": map[string]string{displaySurfacePlatform(): "assets/display/" + displayLibraryName()}}})
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(root, "assets", "ui", "surface.json"), surface, 0600); err != nil {
		t.Fatal(err)
	}
	library := []byte("synthetic renderer artifact for resolution admission")
	digest := sha256.Sum256(library)
	factory := displayFactory{Schema: 1, Interface: "com.dockpipe.launcher.DisplayPackage/1.0", DisplayABI: 1,
		QtVersion: "6.10.3", Platform: runtime.GOOS + "-" + runtime.GOARCH, Library: displayLibraryName(),
		SHA256: hex.EncodeToString(digest[:]), SizeBytes: int64(len(library)), BuildABI: "fixture-abi",
		CompilerID: "fixture", CompilerVersion: "1"}
	metadata, err := json.Marshal(factory)
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(directory, "display-factory.json"), metadata, 0600); err != nil {
		t.Fatal(err)
	}
	libraryPath := filepath.Join(directory, factory.Library)
	if err := os.WriteFile(libraryPath, library, 0600); err != nil {
		t.Fatal(err)
	}

	state, err := createPrivateDisplayTempDirectory(os.TempDir())
	if err != nil {
		t.Fatal(err)
	}
	defer os.RemoveAll(state)
	state, err = filepath.EvalSymlinks(state)
	if err != nil {
		t.Fatal(err)
	}
	token, err := displayProcessToken(os.Getpid())
	if err != nil {
		t.Fatal(err)
	}
	record := sessionRecord{RunID: "resolution-fixture", PID: os.Getpid(), StartToken: token}
	privateDirectory := filepath.Join(state, "display-"+record.RunID)
	if err := createPrivateDisplayDirectory(privateDirectory); err != nil {
		t.Fatal(err)
	}
	descriptor := displayDescriptor{Schema: 2, ResourceID: record.RunID, Backend: "qemu-dbus",
		BusAddress:   privateDisplayBusAddress(filepath.Join(privateDirectory, "bus")),
		InputAddress: privateDisplayBusAddress(filepath.Join(privateDirectory, "input")),
		QemuPID:      os.Getpid(), QemuStartToken: token}
	descriptorPath := filepath.Join(privateDirectory, "display.json")
	if err := writeDisplayDescriptor(descriptorPath, descriptor); err != nil {
		t.Fatal(err)
	}
	manager := manager{state: state}
	if manager.nativeDisplayOptions(record) == nil {
		t.Fatal("real valid factory/descriptor/current-process fixture was not admitted")
	}
	for _, dimensions := range [][2]string{{"1920", "1080"}, {"4096", "2048"}} {
		if _, err := manager.resolutionRequest(record, dimensions[0], dimensions[1]); err != nil {
			t.Fatalf("verified native mode %v rejected: %v", dimensions, err)
		}
	}
	for _, dimensions := range [][2]string{{"3072", "3072"}, {"4096", "2049"}, {"5120", "1440"}} {
		if _, err := manager.resolutionRequest(record, dimensions[0], dimensions[1]); err == nil {
			t.Fatalf("verified native renderer-overflow mode %v accepted", dimensions)
		}
	}
	descriptor.QemuStartToken += "0"
	if err := os.Remove(descriptorPath); err != nil {
		t.Fatal(err)
	}
	if err := writeDisplayDescriptor(descriptorPath, descriptor); err != nil {
		t.Fatal(err)
	}
	if _, err := manager.resolutionRequest(record, "1920", "1080"); err == nil {
		t.Fatal("stale descriptor token silently fell back to ordinary QEMU")
	}
	descriptor.QemuStartToken = token
	if err := os.Remove(descriptorPath); err != nil {
		t.Fatal(err)
	}
	if err := writeDisplayDescriptor(descriptorPath, descriptor); err != nil {
		t.Fatal(err)
	}
	library[0] ^= 1
	if err := os.WriteFile(libraryPath, library, 0600); err != nil {
		t.Fatal(err)
	}
	if _, err := manager.resolutionRequest(record, "1920", "1080"); err == nil {
		t.Fatal("missing or changed renderer silently widened native-session limits")
	}
}
