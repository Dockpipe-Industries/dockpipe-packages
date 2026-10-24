package main

import (
	"encoding/json"
	"os"
	"path/filepath"
	"reflect"
	"runtime"
	"strings"
	"testing"
)

func TestNativeAbsoluteInputPreservesManagementMonitor(t *testing.T) {
	directory, err := createPrivateDisplayTempDirectory(os.TempDir())
	if err != nil {
		t.Fatal(err)
	}
	defer os.RemoveAll(directory)
	address := privateDisplayBusAddress(filepath.Join(directory, "bus"))
	old := filepath.Join(directory, "old.input")
	management := []string{"-qmp", "unix:management.qmp,server=on,wait=off"}
	original := append([]string{}, management...)
	original = append(original,
		"-chardev", "socket,id="+displayInputID+",path="+old+",server=on,wait=off",
		"-mon", "chardev="+displayInputID+",mode=control",
	)
	result, err := nativeAbsoluteInputArguments(original, address)
	if err != nil {
		t.Fatal(err)
	}
	if !reflect.DeepEqual(result[:2], management) || strings.Contains(strings.Join(result, " "), old) {
		t.Fatal("management monitor changed or stale input endpoint retained")
	}
	if len(result) != 6 || result[3] != "socket,id="+displayInputID+",path="+filepath.Join(directory, "input")+",server=on,wait=off" {
		t.Fatalf("dedicated channel not private/unique: %q", result)
	}
	if !reflect.DeepEqual(original[:2], management) {
		t.Fatal("caller arguments mutated")
	}
	if err := os.WriteFile(filepath.Join(directory, "input"), []byte("collision"), 0600); err != nil {
		t.Fatal(err)
	}
	if _, err := nativeAbsoluteInputArguments(nil, address); err == nil {
		t.Fatal("existing endpoint accepted")
	}
}

func TestNativeAbsoluteInputRejectsConflictingReservedChannel(t *testing.T) {
	directory, err := createPrivateDisplayTempDirectory(os.TempDir())
	if err != nil {
		t.Fatal(err)
	}
	defer os.RemoveAll(directory)
	address := privateDisplayBusAddress(filepath.Join(directory, "bus"))
	for _, arguments := range [][]string{
		{"-mon", "chardev=" + displayInputID + ",mode=readline"},
		{"-mon", "chardev=" + displayInputID + ",mode=control"},
		{"-chardev", "socket,id=" + displayInputID + ",path=relative,server=on,wait=off"},
		{"-chardev", "stdio,id=" + displayInputID},
	} {
		if _, err := nativeAbsoluteInputArguments(arguments, address); err == nil {
			t.Fatalf("conflict accepted: %q", arguments)
		}
	}
	if runtime.GOOS != "windows" {
		if err := os.Chmod(directory, 0755); err != nil {
			t.Fatal(err)
		}
		if _, err := nativeAbsoluteInputArguments(nil, address); err == nil {
			t.Fatal("public parent accepted")
		}
	}
}

func TestDisplayInputDescriptorVersionsBindExactSocket(t *testing.T) {
	bus := filepath.Join(string(filepath.Separator), "private", "bus")
	descriptor := displayDescriptor{Schema: 1, BusAddress: privateDisplayBusAddress(bus)}
	if runtime.GOOS == "windows" {
		bus = `C:\private\bus`
		descriptor.BusAddress = privateDisplayBusAddress(bus)
	}
	if !validDisplayInputDescriptor(descriptor) {
		t.Fatal("legacy descriptor rejected")
	}
	descriptor.Schema = 2
	if validDisplayInputDescriptor(descriptor) {
		t.Fatal("v2 without input accepted")
	}
	descriptor.InputAddress = privateDisplayBusAddress(filepath.Join(filepath.Dir(bus), "input"))
	if !validDisplayInputDescriptor(descriptor) {
		t.Fatal("exact dedicated socket rejected")
	}
	descriptor.InputAddress = descriptor.BusAddress
	if validDisplayInputDescriptor(descriptor) {
		t.Fatal("broker socket accepted as input monitor")
	}
	descriptor.Schema = 1
	if validDisplayInputDescriptor(descriptor) {
		t.Fatal("v1 widened with new field")
	}
}

func TestDisplayInputWireDoesNotWidenLegacySchema(t *testing.T) {
	path := filepath.Join(t.TempDir(), "display.json")
	descriptor := displayDescriptor{Schema: 1, ResourceID: "fixture", Backend: "qemu-dbus",
		BusAddress: privateDisplayBusAddress(filepath.Join(os.TempDir(), "bus")),
		QemuPID:    1, QemuStartToken: "1"}
	data, err := json.Marshal(descriptor)
	if err != nil {
		t.Fatal(err)
	}
	var wire map[string]any
	if err := json.Unmarshal(data, &wire); err != nil {
		t.Fatal(err)
	}
	wire["input_address"] = ""
	data, err = json.Marshal(wire)
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, data, 0600); err != nil {
		t.Fatal(err)
	}
	var decoded displayDescriptor
	if decodeDisplayJSON(path, 4096, &decoded) == nil {
		t.Fatal("schema 1 accepted an eighth field")
	}
	delete(wire, "input_address")
	wire["schema"] = 2
	data, err = json.Marshal(wire)
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, data, 0600); err != nil {
		t.Fatal(err)
	}
	if decodeDisplayJSON(path, 4096, &decoded) == nil {
		t.Fatal("schema 2 accepted missing input address")
	}
}
