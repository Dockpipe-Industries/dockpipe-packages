package main

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"net/url"
	"os"
	"os/exec"
	"path/filepath"
	"reflect"
	"runtime"
	"strings"
	"testing"
	"time"
)

func TestNativeDisplaySelectionPreservesTerminalAndExplicitDisplay(t *testing.T) {
	cases := []struct {
		name, mode, launchID, display string
		requested, required, invalid  bool
	}{
		{name: "terminal"},
		{name: "launcher", launchID: "launch-1", requested: true},
		{name: "explicit GTK", launchID: "launch-1", display: "gtk"},
		{name: "disabled", mode: "off", launchID: "launch-1"},
		{name: "explicit native CLI", mode: "native", requested: true, required: true},
		{name: "conflicting overrides", mode: "native", display: "gtk", required: true, invalid: true},
		{name: "unknown mode", mode: "Native", invalid: true},
	}
	for _, test := range cases {
		t.Run(test.name, func(t *testing.T) {
			requested, required, err := nativeDisplaySelection(test.mode, test.launchID, test.display)
			if requested != test.requested || required != test.required || (err != nil) != test.invalid {
				t.Fatalf("unexpected frontend selection: requested=%t required=%t error=%v", requested, required, err)
			}
		})
	}
}

func TestQemuDisplayRequiresQualifiedUnixMapVersion(t *testing.T) {
	for _, version := range []string{"7.2.0", "8.2.9", "9.2.4", "10.1.9", "10.2.3", "unknown", "10.2"} {
		if qemuVersionSupportsMap([]byte("QEMU emulator version " + version + "\n")) {
			t.Fatalf("unqualified QEMU version %s admitted", version)
		}
	}
	for _, version := range []string{"10.2.4", "10.2.5", "10.3.0", "11.0.0"} {
		if !qemuVersionSupportsMap([]byte("QEMU emulator version " + version + " (fixture)\n")) {
			t.Fatalf("qualified version floor rejected %s", version)
		}
	}
}

func TestDisplayMetadataRejectsDuplicateUnknownAndOversizedFields(t *testing.T) {
	path := filepath.Join(t.TempDir(), "display.json")
	for _, data := range []string{
		`{"schema":1,"schema":1}`,
		`{"schema":1,"unexpected":true}`,
		`{"schema":1} {"schema":1}`,
		strings.Repeat(" ", 4097),
		`[]`,
	} {
		if err := os.WriteFile(path, []byte(data), 0600); err != nil {
			t.Fatal(err)
		}
		var descriptor displayDescriptor
		if err := decodeDisplayJSON(path, 4096, &descriptor); err == nil {
			t.Fatal("invalid display metadata admitted")
		}
	}
}

func TestNativeDisplayArgumentsRetainNonDisplayQemuOptions(t *testing.T) {
	input := []string{"-name", "fixture", "-display", "gtk", "-m", "128M", "-daemonize", "-pidfile", "/fixture/pid"}
	result, err := nativeDisplayArguments(input, "unix:path=/private/bus")
	expected := []string{"-name", "fixture", "-m", "128M", "-daemonize", "-pidfile", "/fixture/pid", "-display", "dbus,addr=unix:path=/private/bus"}
	if err != nil || !reflect.DeepEqual(result, expected) {
		t.Fatalf("non-display arguments changed: %v (%v)", result, err)
	}
	for _, address := range []string{"tcp:host=localhost", "unix:path=/private/bus,p2p=on", "unix:path=/private/bus\n"} {
		if _, err := nativeDisplayArguments(input, address); err == nil {
			t.Fatal("unsafe address admitted")
		}
	}
	if _, err := nativeDisplayArguments([]string{"-display"}, "unix:path=/private/bus"); err == nil {
		t.Fatal("incomplete QEMU arguments admitted")
	}
}

func TestPrivateDisplayAddressEscapesTransportAndQemuSeparators(t *testing.T) {
	path := filepath.Join("/private", "path with space,percent%", "bus")
	address := privateDisplayBusAddress(path)
	if strings.ContainsAny(address, ",\r\n\x00") {
		t.Fatal("private bus address contains a QEMU option separator")
	}
	decoded, err := url.PathUnescape(strings.TrimPrefix(address, "unix:path="))
	if err != nil || decoded != filepath.ToSlash(path) {
		t.Fatal("private bus address did not preserve its owned filesystem path")
	}
}

func TestPrivateDisplayAddressEscapesWindowsDriveAndPlus(t *testing.T) {
	address := privateDisplayBusAddress("C:/Users/Name With Space+/bus")
	if address != "unix:path=C%3a/Users/Name%20With%20Space%2b/bus" {
		t.Fatal("Windows drive or D-Bus-reserved path characters were not escaped")
	}
}

func TestPrivateDisplayAddressMatchesCanonicalBrokerReadiness(t *testing.T) {
	address := privateDisplayBusAddress("C:/Users/é+/bus")
	brokerOutput := "unix:path=C%3a/Users/%c3%a9%2b/bus,guid=0123456789abcdef0123456789abcdef"
	if !strings.HasPrefix(brokerOutput, address+",") {
		t.Fatal("canonical broker address did not match the exact requested endpoint")
	}
	decoded, err := url.PathUnescape(strings.TrimPrefix(address, "unix:path="))
	if err != nil || decoded != "C:/Users/é+/bus" {
		t.Fatal("canonical escaping changed the owned endpoint")
	}
}

func TestDisplayFactoryRequiresExactInstalledArtifact(t *testing.T) {
	root := t.TempDir()
	directory := filepath.Join(root, "assets", "display")
	if err := os.MkdirAll(directory, 0700); err != nil {
		t.Fatal(err)
	}
	if err := os.MkdirAll(filepath.Join(root, "assets", "ui"), 0700); err != nil {
		t.Fatal(err)
	}
	surface, _ := json.Marshal(map[string]any{"native_display": map[string]any{
		"interface": 1, "libraries": map[string]string{displaySurfacePlatform(): "assets/display/" + displayLibraryName()}}})
	if err := os.WriteFile(filepath.Join(root, "assets", "ui", "surface.json"), surface, 0600); err != nil {
		t.Fatal(err)
	}
	library := []byte("synthetic display factory artifact")
	digest := sha256.Sum256(library)
	factory := displayFactory{Schema: 1, Interface: "com.dockpipe.launcher.DisplayPackage/1.0", DisplayABI: 1,
		QtVersion: "6.10.3", Platform: runtime.GOOS + "-" + runtime.GOARCH, Library: displayLibraryName(),
		SHA256: hex.EncodeToString(digest[:]), SizeBytes: int64(len(library)), BuildABI: "fixture-abi", CompilerID: "fixture", CompilerVersion: "1"}
	metadata, _ := json.Marshal(factory)
	if err := os.WriteFile(filepath.Join(directory, "display-factory.json"), metadata, 0600); err != nil {
		t.Fatal(err)
	}
	path := filepath.Join(directory, factory.Library)
	if err := os.WriteFile(path, library, 0600); err != nil {
		t.Fatal(err)
	}
	if err := validateDisplayFactory(root); err != nil {
		t.Fatal(err)
	}
	library[0] ^= 1
	if err := os.WriteFile(path, library, 0600); err != nil {
		t.Fatal(err)
	}
	if err := validateDisplayFactory(root); err == nil {
		t.Fatal("changed factory bytes admitted")
	}
	factory.QtVersion = "6.9.0"
	metadata, _ = json.Marshal(factory)
	if err := os.WriteFile(filepath.Join(directory, "display-factory.json"), metadata, 0600); err != nil {
		t.Fatal(err)
	}
	if err := validateDisplayFactory(root); err == nil {
		t.Fatal("incompatible Qt factory admitted")
	}
}

func TestDisplayBrokerPrefersInstalledExecutableAndRejectsBrokenClosure(t *testing.T) {
	if root := os.Getenv("DOCKPIPE_VM_BROKER_TEST_ROOT"); root != "" {
		selected, err := displayBusExecutable()
		if os.Getenv("DOCKPIPE_VM_BROKER_TEST_INVALID") == "1" {
			if err == nil {
				t.Fatal("broken installed broker silently fell back to PATH")
			}
			return
		}
		name := "dbus-daemon"
		if runtime.GOOS == "windows" {
			name += ".exe"
		}
		expected := filepath.Join(root, "assets", "display", "tooling", "bin", name)
		if err != nil || selected != expected {
			t.Fatalf("installed mode-preserving broker seam was not selected: %v", err)
		}
		return
	}
	original, err := os.Executable()
	if err != nil {
		t.Fatal(err)
	}
	data, err := os.ReadFile(original)
	if err != nil {
		t.Fatal(err)
	}
	for _, broken := range []bool{false, true} {
		name := "installed"
		if broken {
			name = "broken installed closure"
		}
		t.Run(name, func(t *testing.T) {
			root := t.TempDir()
			programName := "dockpipe-vm-manager"
			brokerName := "dbus-daemon"
			if runtime.GOOS == "windows" {
				programName += ".exe"
				brokerName += ".exe"
			}
			program := filepath.Join(root, "assets", "tooling", "bin", programName)
			broker := filepath.Join(root, "assets", "display", "tooling", "bin", brokerName)
			pathBroker := filepath.Join(root, "fallback", brokerName)
			for _, directory := range []string{filepath.Dir(program), filepath.Dir(broker), filepath.Dir(pathBroker)} {
				if err := os.MkdirAll(directory, 0700); err != nil {
					t.Fatal(err)
				}
			}
			if err := os.WriteFile(program, data, 0700); err != nil {
				t.Fatal(err)
			}
			if err := os.WriteFile(pathBroker, []byte("fixture"), 0700); err != nil {
				t.Fatal(err)
			}
			if broken {
				if err := os.Mkdir(broker, 0700); err != nil {
					t.Fatal(err)
				}
			} else if err := os.WriteFile(broker, []byte("fixture"), 0700); err != nil {
				t.Fatal(err)
			}
			ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
			defer cancel()
			command := exec.CommandContext(ctx, program, "-test.run=^TestDisplayBrokerPrefersInstalledExecutableAndRejectsBrokenClosure$")
			for _, entry := range os.Environ() {
				key := strings.SplitN(entry, "=", 2)[0]
				if !strings.EqualFold(key, "DOCKPIPE_VM_DBUS_DAEMON_BIN") && !strings.EqualFold(key, "PATH") {
					command.Env = append(command.Env, entry)
				}
			}
			command.Env = append(command.Env, "PATH="+filepath.Dir(pathBroker), "DOCKPIPE_VM_BROKER_TEST_ROOT="+root)
			if broken {
				command.Env = append(command.Env, "DOCKPIPE_VM_BROKER_TEST_INVALID=1")
			}
			if output, err := command.CombinedOutput(); err != nil {
				t.Fatalf("installed broker selection fixture failed: %v\n%s", err, output)
			}
		})
	}
}
