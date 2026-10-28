//go:build linux

package cloudflare

import (
	"context"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"
	"testing"
	"time"
)

func processFixture(t *testing.T, root, pid, executable string) string {
	t.Helper()
	directory := filepath.Join(root, pid)
	if err := os.Mkdir(directory, 0700); err != nil {
		t.Fatal(err)
	}
	handle := filepath.Join(directory, "exe")
	if err := os.Symlink(executable, handle); err != nil {
		t.Fatal(err)
	}
	// These are deliberately unreadable. The inspector must never need command
	// lines or environments, which can contain tunnel tokens and credentials.
	for _, name := range []string{"cmdline", "environ"} {
		if err := os.WriteFile(filepath.Join(directory, name), []byte("private-fixture-token"), 0000); err != nil {
			t.Fatal(err)
		}
	}
	return handle
}

func TestLifecycleLinuxInspectsExactProcessImageHandles(t *testing.T) {
	directory := t.TempDir()
	oldDirectory := filepath.Join(directory, "old")
	fixedDirectory := filepath.Join(directory, "fixed")
	proc := filepath.Join(directory, "proc")
	for _, path := range []string{oldDirectory, fixedDirectory, proc} {
		if err := os.Mkdir(path, 0700); err != nil {
			t.Fatal(err)
		}
	}
	old := inventoryFixture(t, oldDirectory, "cloudflared")
	fixed := inventoryFixture(t, fixedDirectory, "cloudflared")
	oldHandle := processFixture(t, proc, "41", old)
	fixedHandle := processFixture(t, proc, "42", fixed)
	other := inventoryFixture(t, directory, "not-cloudflared")
	processFixture(t, proc, "43", other)
	versions := map[string]string{oldHandle: "2026.9.9", fixedHandle: "2026.10.2"}
	probes := map[string]int{}
	images, gaps := inspectLinuxProcesses(context.Background(), proc, func(_ context.Context, path string) (string, error) {
		probes[path]++
		version, known := versions[path]
		if !known {
			t.Fatalf("probed unrelated executable %q", path)
		}
		return version, nil
	})
	if len(images) != 2 || len(gaps) == 0 || probes[oldHandle] != 1 || probes[fixedHandle] != 1 {
		t.Fatalf("incorrect process inspection: %+v / %+v / %+v", images, gaps, probes)
	}
	for _, image := range images {
		if image.version != versions[image.path] || !strings.HasPrefix(image.path, proc+"/") {
			t.Fatalf("process version was taken from PATH: %+v", image)
		}
	}
}

func TestLifecycleLinuxUnknownVersionAndChangedImageRemainUnproven(t *testing.T) {
	directory := t.TempDir()
	executable := inventoryFixture(t, directory, "cloudflared")
	proc := filepath.Join(directory, "proc")
	if err := os.Mkdir(proc, 0700); err != nil {
		t.Fatal(err)
	}
	handle := processFixture(t, proc, "51", executable)
	images, _ := inspectLinuxProcesses(context.Background(), proc, func(context.Context, string) (string, error) {
		return "", errors.New("unavailable")
	})
	if len(images) != 1 || images[0].version != "" {
		t.Fatalf("unknown active version must remain explicit: %+v", images)
	}
	replacementDirectory := filepath.Join(directory, "replacement")
	if err := os.Mkdir(replacementDirectory, 0700); err != nil {
		t.Fatal(err)
	}
	replacement := inventoryFixture(t, replacementDirectory, "cloudflared")
	images, gaps := inspectLinuxProcesses(context.Background(), proc, func(_ context.Context, path string) (string, error) {
		if path != handle {
			t.Fatalf("unexpected image handle: %q", path)
		}
		if err := os.Remove(handle); err != nil {
			t.Fatal(err)
		}
		if err := os.Symlink(replacement, handle); err != nil {
			t.Fatal(err)
		}
		return "2026.10.2", nil
	})
	if len(images) != 0 || !strings.Contains(strings.Join(gaps, " "), "changed during inspection") {
		t.Fatalf("raced image became fixed proof: %+v / %+v", images, gaps)
	}
}

func TestLifecycleLinuxUnavailableProcessCoverageIsExplicit(t *testing.T) {
	images, gaps := inspectLinuxProcesses(context.Background(), filepath.Join(t.TempDir(), "missing"), func(context.Context, string) (string, error) {
		t.Fatal("missing process inventory invoked an executable")
		return "", nil
	})
	if len(images) != 0 || !strings.Contains(strings.Join(gaps, " "), "unavailable") {
		t.Fatalf("missing process inventory became proof: %+v / %+v", images, gaps)
	}
}

func TestLifecycleLinuxActiveProbeAttemptsAreBoundedEvenWhenImagesChange(t *testing.T) {
	directory := t.TempDir()
	executable := inventoryFixture(t, directory, "cloudflared")
	proc := filepath.Join(directory, "proc")
	if err := os.Mkdir(proc, 0700); err != nil {
		t.Fatal(err)
	}
	for index := 0; index <= maximumActiveImages; index++ {
		processFixture(t, proc, fmt.Sprint(index+100), executable)
	}
	probes := 0
	images, gaps := inspectLinuxProcesses(context.Background(), proc, func(_ context.Context, path string) (string, error) {
		probes++
		if err := os.Remove(path); err != nil {
			t.Fatal(err)
		}
		return "2026.10.2", nil
	})
	if probes != maximumActiveImages || len(images) != 0 {
		t.Fatalf("active attempts are not bounded: %d probes / %+v", probes, images)
	}
	if len(gaps) > 64 || !strings.Contains(strings.Join(gaps, " "), "64 image limit") {
		t.Fatalf("bounded coverage gap missing: %+v", gaps)
	}
}

func TestLifecycleStandaloneProbeCancellationStopsDescendants(t *testing.T) {
	// A standalone inspector owns a tree; inside a workflow RunNested preserves
	// the workflow owner's tree instead of allowing an escaping process group.
	t.Setenv("DOCKPIPE_PROCESS_TREE", "")
	directory := t.TempDir()
	childPID := filepath.Join(directory, "child.pid")
	path := filepath.Join(directory, "cloudflared")
	script := "#!/bin/sh\n/bin/sleep 30 &\nprintf '%s\\n' \"$!\" > '" + childPID + "'\nwait\n"
	if err := os.WriteFile(path, []byte(script), 0700); err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithTimeout(context.Background(), 100*time.Millisecond)
	defer cancel()
	started := time.Now()
	if _, err := probeExecutableVersion(ctx, path); err == nil {
		t.Fatal("cancelled process tree unexpectedly succeeded")
	}
	if time.Since(started) > time.Second {
		t.Fatal("version probe did not honor bounded cancellation")
	}
	data, err := os.ReadFile(childPID)
	if err != nil {
		t.Fatal(err)
	}
	pid, err := strconv.Atoi(strings.TrimSpace(string(data)))
	if err != nil || pid <= 1 {
		t.Fatalf("invalid fixture child PID: %q / %v", data, err)
	}
	defer syscall.Kill(pid, syscall.SIGKILL)
	deadline := time.Now().Add(time.Second)
	for {
		state, err := os.ReadFile(fmt.Sprintf("/proc/%d/stat", pid))
		if errors.Is(err, os.ErrNotExist) {
			return
		}
		if err == nil {
			fields := strings.Fields(string(state))
			if len(fields) > 2 && fields[2] == "Z" {
				return
			}
		}
		if time.Now().After(deadline) {
			t.Fatal("version probe descendant survived cancellation")
		}
		time.Sleep(10 * time.Millisecond)
	}
}
