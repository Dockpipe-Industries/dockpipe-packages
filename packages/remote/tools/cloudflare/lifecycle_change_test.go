//go:build linux

package cloudflare

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"os"
	"path/filepath"
	"testing"

	remoteio "dockpipe/src/lib/infrastructure/remote"
)

func updateFixture(t *testing.T) (binaryUpdater, changeRequest) {
	t.Helper()
	directory := t.TempDir()
	if err := os.Chmod(directory, 0700); err != nil {
		t.Fatal(err)
	}
	target := filepath.Join(directory, "cloudflared")
	artifact := filepath.Join(directory, "download")
	old := []byte("2026.10.2")
	next := []byte("2026.10.4")
	if err := os.WriteFile(target, old, 0700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(artifact, next, 0600); err != nil {
		t.Fatal(err)
	}
	digest := func(data []byte) string { sum := sha256.Sum256(data); return hex.EncodeToString(sum[:]) }
	request := changeRequest{Schema: changeSchema, Resolver: LifecycleProviderID, ProviderVersion: LifecycleProviderVersion, Machine: "local", Parameters: map[string]string{
		"target": target, "artifact": artifact, "sha256": digest(next), "expected_sha256": digest(old), "expected_version": string(old), "version": string(next), "owner": "private-standalone",
	}}
	probe := func(ctx context.Context, path string) (string, error) {
		data, err := os.ReadFile(path)
		return string(data), err
	}
	updater := binaryUpdater{probe: probe, validate: func(ctx context.Context, path, expected string) error {
		actual, err := probe(ctx, path)
		if err != nil || actual != expected {
			return errors.New("version mismatch")
		}
		return nil
	}}
	return updater, request
}

func TestChangeApplyPlanRecoveryAndReplay(t *testing.T) {
	updater, request := updateFixture(t)
	ctx := context.Background()
	result, err := updater.execute(ctx, request, "plan")
	if err != nil || result.Status != "ready" {
		t.Fatalf("plan: %+v %v", result, err)
	}
	data, _ := os.ReadFile(request.Parameters["target"])
	if string(data) != request.Parameters["expected_version"] {
		t.Fatal("plan changed installed bytes")
	}
	result, err = updater.execute(ctx, request, "apply")
	if err != nil || result.Status != "installed-activation-pending" {
		t.Fatalf("apply: %+v %v", result, err)
	}
	result, err = updater.execute(ctx, request, "plan")
	if err != nil || result.Status != "ready" {
		t.Fatalf("installed review: %+v %v", result, err)
	}
	result, err = updater.execute(ctx, request, "apply")
	if err != nil || result.Status != "installed-activation-pending" {
		t.Fatalf("reconcile: %+v %v", result, err)
	}
	os.Remove(request.Parameters["artifact"])
	result, err = updater.execute(ctx, request, "recover-plan")
	if err != nil || result.Status != "ready" {
		t.Fatalf("recovery plan without artifact: %+v %v", result, err)
	}
	result, err = updater.execute(ctx, request, "recover")
	if err != nil || result.Status != "recovered-activation-pending" {
		t.Fatalf("recovery: %+v %v", result, err)
	}
	if _, err = updater.execute(ctx, request, "apply"); err == nil {
		t.Fatal("recovered transaction reapplied")
	}
	result, err = updater.execute(ctx, request, "recover")
	if err != nil || result.Status != "recovered-activation-pending" {
		t.Fatalf("recovery replay: %+v %v", result, err)
	}
}

func TestChangeRejectsInvalidInputsWithoutMutation(t *testing.T) {
	for _, test := range []string{"pin", "checksum", "stale", "owner", "symlink", "cancelled"} {
		t.Run(test, func(t *testing.T) {
			updater, request := updateFixture(t)
			ctx := context.Background()
			switch test {
			case "pin":
				request.Parameters["pin"] = "2026.10.3"
			case "checksum":
				request.Parameters["sha256"] = request.Parameters["expected_sha256"]
			case "stale":
				os.WriteFile(request.Parameters["target"], []byte("unexpected"), 0700)
			case "owner":
				request.Parameters["owner"] = "apt"
			case "symlink":
				os.Remove(request.Parameters["target"])
				os.Symlink(request.Parameters["artifact"], request.Parameters["target"])
			case "cancelled":
				var cancel context.CancelFunc
				ctx, cancel = context.WithCancel(ctx)
				cancel()
			}
			before, _ := os.ReadFile(request.Parameters["target"])
			if _, err := updater.execute(ctx, request, "apply"); err == nil {
				t.Fatal("invalid change accepted")
			}
			after, _ := os.ReadFile(request.Parameters["target"])
			if string(before) != string(after) {
				t.Fatal("rejected change mutated target")
			}
		})
	}
}

func TestChangeValidationFailureRestoresPrevious(t *testing.T) {
	updater, request := updateFixture(t)
	original := updater.validate
	updater.validate = func(ctx context.Context, path, version string) error {
		if version == request.Parameters["version"] {
			return errors.New("failed validation")
		}
		return original(ctx, path, version)
	}
	result, err := updater.execute(context.Background(), request, "apply")
	if err != nil || result.Status != "recovery-required" {
		t.Fatalf("%+v %v", result, err)
	}
	data, _ := os.ReadFile(request.Parameters["target"])
	if string(data) != request.Parameters["expected_version"] {
		t.Fatal("failed update not restored")
	}
}

func TestChangeCorruptBackupAndUnrelatedTargetArePreserved(t *testing.T) {
	for _, corrupt := range []bool{false, true} {
		updater, request := updateFixture(t)
		result, err := updater.execute(context.Background(), request, "apply")
		if err != nil || result.Status != "installed-activation-pending" {
			t.Fatal(result, err)
		}
		if corrupt {
			matches, _ := filepath.Glob(filepath.Join(filepath.Dir(request.Parameters["target"]), ".dockpipe-cloudflared-update", "*", "previous"))
			os.WriteFile(matches[0], []byte("corrupted"), 0700)
		} else {
			os.WriteFile(request.Parameters["target"], []byte("external change"), 0700)
		}
		before, _ := os.ReadFile(request.Parameters["target"])
		result, err = updater.execute(context.Background(), request, "recover")
		if err != nil || result.Status != "recovery-required" {
			t.Fatal(result, err)
		}
		after, _ := os.ReadFile(request.Parameters["target"])
		if string(before) != string(after) {
			t.Fatal("unsafe recovery overwrote target")
		}
	}
}

func TestChangeConcurrentLockAndInterruptedReplacement(t *testing.T) {
	updater, request := updateFixture(t)
	state := filepath.Join(filepath.Dir(request.Parameters["target"]), ".dockpipe-cloudflared-update")
	if err := remoteio.PrivateDirectory(state); err != nil {
		t.Fatal(err)
	}
	unlock, err := remoteio.Lock(filepath.Join(state, "update.lock"))
	if err != nil {
		t.Fatal(err)
	}
	if _, err := updater.execute(context.Background(), request, "apply"); err == nil {
		t.Fatal("concurrent change accepted")
	}
	unlock()
	result, err := updater.execute(context.Background(), request, "apply")
	if err != nil {
		t.Fatal(err)
	}
	journals, _ := filepath.Glob(filepath.Join(state, "*", "journal.json"))
	var journal updateJournal
	if err := remoteio.ReadPrivate(journals[0], &journal); err != nil {
		t.Fatal(err)
	}
	journal.Phase = "replacing"
	if err := remoteio.WritePrivate(journals[0], journal); err != nil {
		t.Fatal(err)
	}
	result, err = updater.execute(context.Background(), request, "recover")
	if err != nil || result.Status != "recovered-activation-pending" {
		t.Fatal(result, err)
	}
}

func TestChangeVulnerableRollbackRequiresNewExplicitException(t *testing.T) {
	updater, request := updateFixture(t)
	old := []byte("2026.10.1")
	sum := sha256.Sum256(old)
	request.Parameters["expected_version"] = string(old)
	request.Parameters["expected_sha256"] = hex.EncodeToString(sum[:])
	os.WriteFile(request.Parameters["target"], old, 0700)
	result, err := updater.execute(context.Background(), request, "apply")
	if err != nil || result.Status != "installed-activation-pending" {
		t.Fatal(result, err)
	}
	result, err = updater.execute(context.Background(), request, "recover")
	if err != nil || result.Status != "recovery-required" {
		t.Fatal(result, err)
	}
	request.Parameters["emergency_recovery"] = "allow-vulnerable-rollback"
	result, err = updater.execute(context.Background(), request, "recover-plan")
	if err != nil || result.Status != "ready" {
		t.Fatal(result, err)
	}
	result, err = updater.execute(context.Background(), request, "recover")
	if err != nil || result.Status != "recovered-activation-pending" {
		t.Fatal(result, err)
	}
}

func TestChangePreservesExecutablePermissions(t *testing.T) {
	updater, request := updateFixture(t)
	if err := os.Chmod(request.Parameters["target"], 0500); err != nil {
		t.Fatal(err)
	}
	result, err := updater.execute(context.Background(), request, "apply")
	if err != nil || result.Status != "installed-activation-pending" {
		t.Fatal(result, err)
	}
	info, _ := os.Stat(request.Parameters["target"])
	if info.Mode().Perm() != 0500 {
		t.Fatal("update changed executable mode")
	}
	result, err = updater.execute(context.Background(), request, "recover")
	if err != nil || result.Status != "recovered-activation-pending" {
		t.Fatal(result, err)
	}
	info, _ = os.Stat(request.Parameters["target"])
	if info.Mode().Perm() != 0500 {
		t.Fatal("recovery changed executable mode")
	}
}

func TestChangeInterruptedBeforeReplacementDoesNotReplay(t *testing.T) {
	updater, request := updateFixture(t)
	result, err := updater.execute(context.Background(), request, "apply")
	if err != nil || result.Status != "installed-activation-pending" {
		t.Fatal(result, err)
	}
	state := filepath.Join(filepath.Dir(request.Parameters["target"]), ".dockpipe-cloudflared-update")
	journals, _ := filepath.Glob(filepath.Join(state, "*", "journal.json"))
	var journal updateJournal
	if err := remoteio.ReadPrivate(journals[0], &journal); err != nil {
		t.Fatal(err)
	}
	journal.Phase = "replacing"
	if err := remoteio.WritePrivate(journals[0], journal); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(request.Parameters["target"], []byte(request.Parameters["expected_version"]), 0700); err != nil {
		t.Fatal(err)
	}
	if _, err := updater.execute(context.Background(), request, "apply"); err == nil {
		t.Fatal("ambiguous transaction blindly replayed")
	}
	result, err = updater.execute(context.Background(), request, "recover")
	if err != nil || result.Status != "recovered-activation-pending" {
		t.Fatal(result, err)
	}
}
