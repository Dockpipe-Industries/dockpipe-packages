package cloudflare

import (
	"context"
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
	"time"
)

func inventoryFixture(t *testing.T, directory, name string) string {
	t.Helper()
	path := filepath.Join(directory, name)
	if err := os.WriteFile(path, []byte("fixture executable\n"), 0700); err != nil {
		t.Fatal(err)
	}
	return path
}

func fixtureInspector(paths []string, versions map[string]string, active []inspectedExecutable) inventoryInspector {
	return inventoryInspector{
		platform: "linux",
		paths:    paths,
		probe: func(_ context.Context, path string) (string, error) {
			return versions[path], nil
		},
		active: func(context.Context) ([]inspectedExecutable, []string) {
			return active, []string{"Fixture process coverage is intentionally partial."}
		},
		now: func() time.Time { return time.Date(2026, 10, 27, 12, 0, 0, 0, time.UTC) },
	}
}

func TestLifecycleInventoryMultipleInstallationsAndUnprovenActiveImage(t *testing.T) {
	directory := t.TempDir()
	old := inventoryFixture(t, directory, "old-cloudflared")
	fixed := inventoryFixture(t, directory, "new-cloudflared")
	malformed := inventoryFixture(t, directory, "vendor-cloudflared")
	inspector := fixtureInspector([]string{old, fixed, malformed}, map[string]string{
		old: "2026.9.2", fixed: "2026.10.2", malformed: "2026.10.3-vendor",
	}, nil)
	report, err := inspector.inspect(context.Background(), "machine-01", nil)
	if err != nil {
		t.Fatal(err)
	}
	if report.Schema != InventorySchema || report.Provider.ID != LifecycleProviderID || report.Provider.Version != "0.3.2" || report.Machine != "machine-01" {
		t.Fatalf("incorrect report binding: %+v", report)
	}
	if len(report.Observations) != 3 || len(report.Gaps) == 0 {
		t.Fatalf("incorrect coverage: %+v", report)
	}
	for _, observation := range report.Observations {
		want := "unknown"
		if observation.Location == old {
			want = "affected"
		}
		if observation.Assessment.Status != want || observation.ActiveState != "unknown" || observation.ActiveVersion != "" {
			t.Fatalf("PATH candidate became active proof: %+v", observation)
		}
	}
	encoded, err := json.Marshal(report)
	if err != nil || len(encoded) > maximumInventoryBytes {
		t.Fatalf("invalid bounded JSON: %v", err)
	}
}

func TestLifecycleInventoryDeduplicatesSymlinksAndHardLinks(t *testing.T) {
	if runtime.GOOS == "windows" {
		t.Skip("Native Windows link permissions are qualified separately")
	}
	directory := t.TempDir()
	path := inventoryFixture(t, directory, "cloudflared")
	link := filepath.Join(directory, "symlink")
	hard := filepath.Join(directory, "hardlink")
	if err := os.Symlink(path, link); err != nil {
		t.Fatal(err)
	}
	if err := os.Link(path, hard); err != nil {
		t.Fatal(err)
	}
	inspector := fixtureInspector([]string{path, link, hard, path}, map[string]string{path: "2026.10.2"}, nil)
	report, err := inspector.inspect(context.Background(), "machine", nil)
	if err != nil || len(report.Observations) != 1 {
		t.Fatalf("deduplication failed: %+v / %v", report, err)
	}
}

func TestLifecycleActiveImageMustMatchInstalledIdentity(t *testing.T) {
	directory := t.TempDir()
	installed := inventoryFixture(t, directory, "installed")
	oldRunning := inventoryFixture(t, directory, "old-running")
	installedInfo, _ := os.Stat(installed)
	oldInfo, _ := os.Stat(oldRunning)
	active := []inspectedExecutable{
		{path: "/proc/12/exe", info: oldInfo, version: "2026.9.2"},
		{path: "/proc/13/exe", info: installedInfo, version: "2026.10.2"},
	}
	inspector := fixtureInspector([]string{installed}, map[string]string{installed: "2026.10.2"}, active)
	report, err := inspector.inspect(context.Background(), "machine", nil)
	if err != nil {
		t.Fatal(err)
	}
	if len(report.Observations) != 3 {
		t.Fatalf("lost separate active image: %+v", report)
	}
	for _, observation := range report.Observations {
		if observation.ActiveState != "running" {
			t.Fatalf("known image should be running: %+v", observation)
		}
		if observation.Location == "/proc/12/exe" && observation.Assessment.Status != "affected" {
			t.Fatalf("old active image was hidden by fixed installation: %+v", observation)
		}
		if observation.Location == installed && (observation.Assessment.Status != "not_affected" || observation.ActiveVersion != "2026.10.2") {
			t.Fatalf("matching active image was not proven: %+v", observation)
		}
	}
	inspector.active = func(context.Context) ([]inspectedExecutable, []string) { return active[:1], nil }
	report, err = inspector.inspect(context.Background(), "machine", nil)
	if err != nil {
		t.Fatal(err)
	}
	for _, observation := range report.Observations {
		if observation.Location == installed && observation.Assessment.Status != "unknown" {
			t.Fatalf("different old image cleared installed candidate: %+v", observation)
		}
	}
}

func TestLifecycleInventoryCancellationAndInternalBudget(t *testing.T) {
	path := inventoryFixture(t, t.TempDir(), "cloudflared")
	inspector := fixtureInspector([]string{path}, nil, nil)
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	if _, err := inspector.inspect(ctx, "machine", nil); !errors.Is(err, context.Canceled) {
		t.Fatalf("cancellation was lost: %v", err)
	}
	inspector.budget = time.Millisecond
	inspector.probe = func(ctx context.Context, _ string) (string, error) {
		<-ctx.Done()
		return "", ctx.Err()
	}
	report, err := inspector.inspect(context.Background(), "machine", nil)
	if err != nil || !strings.Contains(strings.Join(report.Gaps, " "), "budget expired") {
		t.Fatalf("bounded partial scan was not explicit: %+v / %v", report, err)
	}
}

func TestLifecycleRejectsInvalidMachineAndOutputWithoutProbing(t *testing.T) {
	inspector := fixtureInspector(nil, nil, nil)
	inspector.active = func(context.Context) ([]inspectedExecutable, []string) {
		t.Fatal("invalid input reached process inspection")
		return nil, nil
	}
	for _, machine := range []string{"", "bad machine", "../escape", strings.Repeat("a", 129)} {
		if _, err := inspector.inspect(context.Background(), machine, nil); err == nil {
			t.Fatalf("accepted machine %q", machine)
		}
	}
	for _, arguments := range [][]string{
		{"--output", "relative.json", "--machine", "machine"},
		{"--output", "/tmp/result.json", "--machine", "bad machine"},
		{"--output", "/tmp/result.json", "--machine", "machine", "unexpected"},
		{"--state", "not-a-setup-mode"},
	} {
		if err := InspectCommand(context.Background(), arguments); err == nil {
			t.Fatalf("accepted invalid arguments: %q", arguments)
		}
	}
}

func TestLifecycleOutputIsPrivateAndNeverOverwrites(t *testing.T) {
	path := filepath.Join(t.TempDir(), "report.json")
	if err := writeInventoryReport(path, []byte(`{"test":true}`)); err != nil {
		t.Fatal(err)
	}
	info, err := os.Stat(path)
	if err != nil {
		t.Fatal(err)
	}
	if runtime.GOOS != "windows" && info.Mode().Perm() != 0600 {
		t.Fatalf("report permissions = %o", info.Mode().Perm())
	}
	if err := writeInventoryReport(path, []byte("replacement")); err == nil {
		t.Fatal("existing report was overwritten")
	}
	data, _ := os.ReadFile(path)
	if string(data) != "{\"test\":true}\n" {
		t.Fatalf("existing output changed: %q", data)
	}
}

func TestLifecycleVersionProbeUsesOnlyReadOnlyArgumentAndCleanEnvironment(t *testing.T) {
	if runtime.GOOS == "windows" {
		t.Skip("POSIX executable fixture; native Windows validation pending")
	}
	directory := t.TempDir()
	path := filepath.Join(directory, "cloudflared")
	script := `#!/bin/sh
[ "$#" = 1 ] && [ "$1" = --version ] || exit 21
[ -z "${TUNNEL_TOKEN+x}" ] || exit 22
[ -z "${CLOUDFLARE_API_TOKEN+x}" ] || exit 23
[ -z "${LD_PRELOAD+x}" ] || exit 24
[ -z "${DYLD_INSERT_LIBRARIES+x}" ] || exit 25
[ "$PWD" = / ] || exit 26
printf '%s\n' 'cloudflared version 2026.10.2 (fixture)'
`
	if err := os.WriteFile(path, []byte(script), 0700); err != nil {
		t.Fatal(err)
	}
	t.Setenv("TUNNEL_TOKEN", "private-fixture-value")
	t.Setenv("CLOUDFLARE_API_TOKEN", "private-fixture-value")
	t.Setenv("LD_PRELOAD", "private-fixture-path")
	t.Setenv("DYLD_INSERT_LIBRARIES", "private-fixture-path")
	version, err := probeExecutableVersion(context.Background(), path)
	if err != nil || version != "2026.10.2" {
		t.Fatalf("read-only clean-environment probe failed: %q / %v", version, err)
	}
	if err := os.WriteFile(path, []byte("#!/bin/sh\nprintf '%05000d' 0\n"), 0700); err != nil {
		t.Fatal(err)
	}
	if version, err := probeExecutableVersion(context.Background(), path); err == nil || version != "" {
		t.Fatalf("unbounded output accepted: %q / %v", version, err)
	}
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	if _, err := probeExecutableVersion(ctx, path); err == nil {
		t.Fatal("cancelled probe succeeded")
	}
}

func TestLifecycleInstallationChangedDuringProbeRemainsUnknown(t *testing.T) {
	path := inventoryFixture(t, t.TempDir(), "cloudflared")
	inspector := fixtureInspector([]string{path}, nil, nil)
	inspector.probe = func(context.Context, string) (string, error) {
		if err := os.WriteFile(path, []byte("changed installation during probe with different length\n"), 0700); err != nil {
			t.Fatal(err)
		}
		return "2026.10.2", nil
	}
	report, err := inspector.inspect(context.Background(), "machine", nil)
	if err != nil || len(report.Observations) != 1 {
		t.Fatalf("unexpected report: %+v / %v", report, err)
	}
	if observation := report.Observations[0]; observation.InstalledVersion != "" || observation.Assessment.Status != "unknown" {
		t.Fatalf("raced installation became fixed proof: %+v", observation)
	}
}
