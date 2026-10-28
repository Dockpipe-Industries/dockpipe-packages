package cloudflare

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"io"
	"os"
	"path/filepath"
	"time"

	"dockpipe/src/lib/infrastructure"
	remoteio "dockpipe/src/lib/infrastructure/remote"
)

const recoveryTimeout = 10 * time.Second

// openBinary requires private, unlinked, operator-owned state before opening.
// It rechecks inode identity; the directory must itself pass private validation.
func openBinary(path string) (*os.File, error) {
	if err := infrastructure.ValidatePrivatePath(filepath.Dir(path), true); err != nil {
		return nil, err
	}
	if err := infrastructure.ValidateUnlinkedPath(path); err != nil {
		return nil, err
	}
	before, err := os.Lstat(path)
	if err != nil {
		return nil, err
	}
	if !before.Mode().IsRegular() || before.Size() < 1 || before.Size() > maximumBinaryBytes {
		return nil, errors.New("binary must be a bounded private regular file")
	}
	file, err := openBinaryFile(path)
	if err != nil {
		return nil, err
	}
	after, err := file.Stat()
	if err != nil || !os.SameFile(before, after) || validateBinaryAccess(after) != nil {
		file.Close()
		return nil, errors.New("binary changed while opening")
	}
	return file, nil
}

func binaryDigest(path string) (string, error) {
	file, err := openBinary(path)
	if err != nil {
		return "", err
	}
	defer file.Close()
	hash := sha256.New()
	size, err := io.Copy(hash, io.LimitReader(file, maximumBinaryBytes+1))
	if err != nil || size > maximumBinaryBytes {
		return "", errors.New("binary read failed or exceeded limit")
	}
	return hex.EncodeToString(hash.Sum(nil)), nil
}

func copyVerified(source, destination, expected string) error {
	input, err := openBinary(source)
	if err != nil {
		return err
	}
	defer input.Close()
	file, err := os.CreateTemp(filepath.Dir(destination), "stage-")
	if err != nil {
		return err
	}
	defer os.Remove(file.Name())
	defer file.Close()
	hash := sha256.New()
	size, err := io.Copy(io.MultiWriter(file, hash), io.LimitReader(input, maximumBinaryBytes+1))
	if err != nil || size > maximumBinaryBytes || hex.EncodeToString(hash.Sum(nil)) != expected {
		return errors.New("binary checksum verification failed")
	}
	if err := file.Chmod(0700); err != nil {
		return err
	}
	if err := file.Sync(); err != nil {
		return err
	}
	if err := file.Close(); err != nil {
		return err
	}
	return replaceBinary(file.Name(), destination)
}

func replaceBinary(source, destination string) error {
	if err := os.Rename(source, destination); err != nil {
		return err
	}
	directories := []string{filepath.Dir(destination)}
	if filepath.Dir(source) != filepath.Dir(destination) {
		directories = append(directories, filepath.Dir(source))
	}
	for _, path := range directories {
		directory, err := os.Open(path)
		if err != nil {
			return err
		}
		syncErr := directory.Sync()
		closeErr := directory.Close()
		if syncErr != nil {
			return syncErr
		}
		if closeErr != nil {
			return closeErr
		}
	}
	return nil
}

func validateInstalledVersion(ctx context.Context, path, expected string) error {
	version, err := probeExecutableVersion(ctx, path)
	if err != nil || version != expected {
		return errors.New("installed version validation failed")
	}
	return nil
}

func (updater binaryUpdater) recover(ctx context.Context, request changeRequest, journal updateJournal, journalPath, backup string, dryRun bool) (changeResult, error) {
	values := request.Parameters
	target := values["target"]
	current, err := binaryDigest(target)
	if err != nil {
		return attention("Cannot read the installed target; preserve journal and inspect manually."), nil
	}
	if current != values["expected_sha256"] && current != values["sha256"] {
		return attention("Installed bytes match neither approved version; recovery refuses to overwrite unrelated changes."), nil
	}
	if current == values["sha256"] {
		digest, err := binaryDigest(backup)
		if err != nil || digest != values["expected_sha256"] {
			return attention("Recovery backup failed verification; target preserved."), nil
		}
		previous, _ := parseCalendarVersion(values["expected_version"])
		if compareCalendarVersion(previous, [3]int{2026, 10, 2}) < 0 && values["emergency_recovery"] != "allow-vulnerable-rollback" {
			return attention("Rollback would restore a vulnerable version and is blocked. Preserve the journal and prepare a verified forward fix or explicitly approve the emergency exception."), nil
		}
		if dryRun {
			return changeResult{Schema: changeSchema, Status: "ready", Message: "Recovery backup and installed bytes verified; restore does not restart any process."}, nil
		}
		if err := ctx.Err(); err != nil {
			return attention("Recovery cancelled before mutation; journal preserved."), nil
		}
		replacement := filepath.Join(filepath.Dir(backup), "restore")
		if err := copyVerified(backup, replacement, values["expected_sha256"]); err != nil {
			return attention("Recovery backup failed verification; target was preserved."), nil
		}
		defer os.Remove(replacement)
		if journal.PreviousMode != 0500 && journal.PreviousMode != 0700 {
			return attention("Recovery journal has an invalid previous mode; manual inspection required."), nil
		}
		if err := os.Chmod(replacement, os.FileMode(journal.PreviousMode)); err != nil {
			return attention("Recovery permissions could not be restored."), nil
		}
		if err := syncBinary(replacement); err != nil {
			return attention("Recovery metadata could not be synchronized."), nil
		}
		// Journal recovery intent before the atomic transition.
		journal.Phase = "recovering"
		journal.EmergencyRecovery = values["emergency_recovery"] == "allow-vulnerable-rollback"
		if err := remoteio.WritePrivate(journalPath, journal); err != nil {
			return changeResult{}, err
		}
		again, err := binaryDigest(target)
		if err != nil || again != current {
			return attention("Target changed before recovery; no replacement attempted."), nil
		}
		if err := replaceBinary(replacement, target); err != nil {
			return attention("Recovery replacement outcome is uncertain; reconcile again."), nil
		}
	}
	if dryRun {
		return changeResult{Schema: changeSchema, Status: "ready", Message: "Previous bytes are already installed; recovery will validate and reconcile the journal."}, nil
	}
	if err := updater.validate(ctx, target, values["expected_version"]); err != nil {
		return attention("Previous bytes restored but version validation failed; manual inspection required."), nil
	}
	journal.Phase = "recovered"
	if err := remoteio.WritePrivate(journalPath, journal); err != nil {
		return attention("Previous bytes restored but recovery journal failed to synchronize."), nil
	}
	return changeResult{Schema: changeSchema, Status: "recovered-activation-pending", Message: "Previous approved binary restored and validated. No process was restarted. Running version and tunnel health remain unproven."}, nil
}

func syncBinary(path string) error {
	file, err := os.Open(path)
	if err != nil {
		return err
	}
	defer file.Close()
	return file.Sync()
}
