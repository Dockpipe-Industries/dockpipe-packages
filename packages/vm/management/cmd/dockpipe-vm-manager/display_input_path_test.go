package main

import (
	"path/filepath"
	"runtime"
	"strings"
	"testing"
)

func TestDisplayInputPathCanonicalRoundTrip(t *testing.T) {
	bus := filepath.Join(t.TempDir(), "bus")
	address := privateDisplayBusAddress(bus)
	input, err := displayInputPath(address)
	if err != nil {
		t.Fatalf("canonical native bus address rejected on %s: %v", runtime.GOOS, err)
	}
	if expected := filepath.Join(filepath.Dir(bus), "input"); input != expected {
		t.Fatalf("input path = %q, want native path %q", input, expected)
	}
	if privateDisplayBusAddress(filepath.Join(filepath.Dir(input), "bus")) != address {
		t.Fatal("canonical address identity changed after native path conversion")
	}
}

func TestDisplayInputPathRejectsNoncanonicalAliases(t *testing.T) {
	address := privateDisplayBusAddress(filepath.Join(t.TempDir(), "bus"))
	variants := []string{
		strings.TrimSuffix(address, "bus") + "%62us",
		strings.TrimSuffix(address, "/bus") + "/./bus",
		strings.TrimSuffix(address, "/bus") + "/sub/../bus",
		address + ",guid=unrelated",
		address + "%00",
	}
	if runtime.GOOS == "windows" {
		variants = append(variants, strings.Replace(address, "%3a", "%3A", 1))
		variants = append(variants, strings.ReplaceAll(address, "/", `\`))
		variants = append(variants, privateDisplayBusAddress(`\\server\share\bus`))
		variants = append(variants, privateDisplayBusAddress(`\\?\C:\private\bus`))
	}
	for _, variant := range variants {
		if _, err := displayInputPath(variant); err == nil {
			t.Errorf("noncanonical address accepted: %q", variant)
		}
	}
}
