package cloudflare

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"flag"
	"io"
	"os"
	"path/filepath"
	"runtime"

	"dockpipe/src/lib/infrastructure"
	remoteio "dockpipe/src/lib/infrastructure/remote"
)

const changeSchema = "dockpipe.dependency.change/v1"
const maximumBinaryBytes = 128 << 20

type changeRequest struct {
	Schema          string            `json:"schema"`
	Resolver        string            `json:"resolver"`
	ProviderVersion string            `json:"provider_version"`
	Machine         string            `json:"machine"`
	Parameters      map[string]string `json:"parameters"`
}

type changeResult struct {
	Schema  string `json:"schema"`
	Status  string `json:"status"`
	Message string `json:"message"`
}

type updateJournal struct {
	PreviousMode      uint32 `json:"previous_mode"`
	EmergencyRecovery bool   `json:"emergency_recovery"`
	RequestDigest     string `json:"request_digest"`
	Phase             string `json:"phase"`
	PreviousSHA256    string `json:"previous_sha256"`
	CandidateSHA256   string `json:"candidate_sha256"`
}

type binaryUpdater struct {
	probe func(context.Context, string) (string, error)
	// The test seam models validation failure after the durable replacement.
	validate func(context.Context, string, string) error
}

func ChangeCommand(ctx context.Context, arguments []string) error {
	flags := flag.NewFlagSet("lifecycle-change", flag.ContinueOnError)
	flags.SetOutput(io.Discard)
	input := flags.String("request", "", "private exact request")
	operation := flags.String("operation", "", "plan, apply or recover")
	output := flags.String("output", "", "new private result")
	if err := flags.Parse(arguments); err != nil {
		return err
	}
	if flags.NArg() != 0 || !safeInventoryPath(*input) || !safeInventoryPath(*output) {
		return errors.New("lifecycle-change requires absolute request and output paths")
	}
	var request changeRequest
	if err := remoteio.ReadPrivate(*input, &request); err != nil {
		return err
	}
	updater := binaryUpdater{probe: probeExecutableVersion, validate: validateInstalledVersion}
	result, err := updater.execute(ctx, request, *operation)
	if err != nil {
		return err
	}
	data, err := json.Marshal(result)
	if err != nil {
		return err
	}
	return writeInventoryReport(*output, data)
}

func validateChangeRequest(request changeRequest) error {
	if request.Schema != changeSchema || request.Resolver != LifecycleProviderID ||
		request.ProviderVersion != LifecycleProviderVersion || request.Machine != "local" {
		return errors.New("change request identity does not match this local provider")
	}
	allowed := map[string]bool{"target": true, "artifact": true, "sha256": true, "expected_sha256": true,
		"version": true, "expected_version": true, "pin": true, "owner": true, "emergency_recovery": true}
	for key := range request.Parameters {
		if !allowed[key] {
			return errors.New("unsupported change parameter")
		}
	}
	values := request.Parameters
	if values["owner"] != "private-standalone" {
		return errors.New("only explicitly owned private standalone binaries are supported; use the installation owner for managers, services, containers and bundled binaries")
	}
	for _, key := range []string{"target", "artifact"} {
		if !safeInventoryPath(values[key]) || filepath.Clean(values[key]) != values[key] {
			return errors.New("target and artifact must be clean absolute paths")
		}
	}
	for _, key := range []string{"sha256", "expected_sha256"} {
		digest, err := hex.DecodeString(values[key])
		if err != nil || len(digest) != 32 || hex.EncodeToString(digest) != values[key] {
			return errors.New("expected and candidate SHA-256 must be exact lowercase digests")
		}
	}
	old, oldValid := parseCalendarVersion(values["expected_version"])
	next, nextValid := parseCalendarVersion(values["version"])
	if !oldValid || !nextValid || compareCalendarVersion(next, old) <= 0 || compareCalendarVersion(next, [3]int{2026, 10, 2}) < 0 {
		return errors.New("candidate must be a stable strictly newer release meeting the advisory floor")
	}
	if values["pin"] != "" && values["pin"] != values["version"] {
		return errors.New("candidate conflicts with the exact pin")
	}
	if values["emergency_recovery"] != "" && values["emergency_recovery"] != "allow-vulnerable-rollback" {
		return errors.New("unknown emergency recovery policy")
	}
	return nil
}

func (updater binaryUpdater) execute(ctx context.Context, request changeRequest, operation string) (changeResult, error) {
	result := changeResult{Schema: changeSchema}
	if err := validateChangeRequest(request); err != nil {
		return result, err
	}
	if operation != "plan" && operation != "apply" && operation != "recover" && operation != "recover-plan" {
		return result, errors.New("unsupported change operation")
	}
	if runtime.GOOS != "linux" {
		result.Status = "manual-required"
		result.Message = "Atomic standalone replacement is currently qualified only on Linux; use the platform installation owner."
		return result, nil
	}
	values := request.Parameters
	target := values["target"]
	directory := filepath.Dir(target)
	if err := infrastructure.ValidatePrivatePath(directory, true); err != nil {
		return result, err
	}
	if filepath.Base(target) != "cloudflared" {
		return result, errors.New("target must be the explicit private cloudflared binary")
	}
	// A private directory is an explicit ownership boundary. System and manager
	// paths cannot be adopted by changing a request's owner label.
	state := filepath.Join(directory, ".dockpipe-cloudflared-update")
	if err := remoteio.PrivateDirectory(state); err != nil {
		return result, err
	}
	unlock, err := remoteio.Lock(filepath.Join(state, "update.lock"))
	if err != nil {
		return result, err
	}
	defer unlock()
	// Recovery policy can be approved after a failure without changing the
	// immutable installation transaction identity. Core approval still binds it.
	journalRequest := request
	journalRequest.Parameters = make(map[string]string, len(request.Parameters))
	for key, value := range request.Parameters {
		if key != "emergency_recovery" {
			journalRequest.Parameters[key] = value
		}
	}
	data, err := json.Marshal(journalRequest)
	if err != nil {
		return result, err
	}
	digest := sha256.Sum256(data)
	requestDigest := hex.EncodeToString(digest[:])
	activePath := filepath.Join(state, "active.json")
	var active struct {
		Digest string `json:"digest"`
	}
	activeErr := remoteio.ReadPrivate(activePath, &active)
	if activeErr != nil && !os.IsNotExist(activeErr) {
		return result, activeErr
	}
	if activeErr == nil && active.Digest != requestDigest {
		decoded, err := hex.DecodeString(active.Digest)
		if err != nil || len(decoded) != 32 || hex.EncodeToString(decoded) != active.Digest {
			return result, errors.New("invalid active journal identity")
		}
		var prior updateJournal
		if err := remoteio.ReadPrivate(filepath.Join(state, active.Digest, "journal.json"), &prior); err != nil {
			return result, err
		}
		if prior.Phase != "installed" && prior.Phase != "recovered" {
			return result, errors.New("another unfinished update requires recovery")
		}
		expected := prior.CandidateSHA256
		if prior.Phase == "recovered" {
			expected = prior.PreviousSHA256
		}
		current, err := binaryDigest(target)
		if err != nil || current != expected {
			return result, errors.New("previous transaction does not match installed bytes")
		}
	}
	transactionRoot := filepath.Join(state, requestDigest)
	if err := remoteio.PrivateDirectory(transactionRoot); err != nil {
		return result, err
	}
	journalPath := filepath.Join(transactionRoot, "journal.json")
	backup := filepath.Join(transactionRoot, "previous")
	candidate := filepath.Join(transactionRoot, "candidate")
	var journal updateJournal
	journalErr := remoteio.ReadPrivate(journalPath, &journal)
	if journalErr != nil && !os.IsNotExist(journalErr) {
		return result, journalErr
	}
	if journalErr == nil && journal.RequestDigest != requestDigest {
		return result, errors.New("a different update journal exists; reconcile it before starting another update")
	}
	if journalErr == nil && (journal.PreviousSHA256 != values["expected_sha256"] || journal.CandidateSHA256 != values["sha256"]) {
		return result, errors.New("journal bytes do not match the approved request")
	}
	if operation == "recover" || operation == "recover-plan" {
		if journalErr != nil {
			return result, errors.New("there is no update journal to recover")
		}
		return updater.recover(ctx, request, journal, journalPath, backup, operation == "recover-plan")
	}
	current, err := binaryDigest(target)
	if err != nil {
		return result, err
	}
	if journalErr == nil && current == values["sha256"] {
		if err := updater.validateCandidate(ctx, target, values["version"], values["sha256"]); err != nil {
			return attention("Installed candidate failed validation; use the explicit recovery operation."), nil
		}
		if operation == "plan" {
			return changeResult{Schema: changeSchema, Status: "ready", Message: "Candidate bytes are already installed and verified. Apply will reconcile the journal without replacing bytes or restarting a process."}, nil
		}
		journal.Phase = "installed"
		if err := remoteio.WritePrivate(journalPath, journal); err != nil {
			return attention("Installed candidate verified but journal reconciliation failed."), nil
		}
		return installedResult(), nil
	}
	if current != values["expected_sha256"] {
		return result, errors.New("installed bytes changed; discover and approve a fresh request")
	}
	version, err := updater.probe(ctx, target)
	if err != nil || version != values["expected_version"] {
		return result, errors.New("installed version differs from the request")
	}
	if journalErr == nil && journal.Phase != "prepared" {
		return result, errors.New("previous attempt needs explicit recovery; it will not be replayed")
	}
	// Never execute candidate bytes until the copied bytes match the explicit
	// operator digest. The source can change; the staged snapshot is authoritative.
	if err := copyVerified(values["artifact"], candidate, values["sha256"]); err != nil {
		return result, err
	}
	defer os.Remove(candidate)
	version, err = updater.probe(ctx, candidate)
	if err != nil || version != values["version"] {
		return result, errors.New("verified candidate has the wrong version or architecture")
	}
	if operation == "plan" {
		result.Status = "ready"
		result.Message = "Verified private standalone candidate; apply replaces installed bytes only. Running connectors are not restarted; tunnel activation and health remain unproven. Recovery to a vulnerable version requires an explicitly bound emergency exception."
		return result, nil
	}
	if err := ctx.Err(); err != nil {
		return result, err
	}
	if err := copyVerified(target, backup, values["expected_sha256"]); err != nil {
		return result, err
	}
	targetInfo, err := os.Stat(target)
	if err != nil {
		return result, err
	}
	if targetInfo.Mode().Perm() != 0500 && targetInfo.Mode().Perm() != 0700 {
		return result, errors.New("standalone executable must have owner-only read/execute permissions (0500 or 0700)")
	}
	if err := os.Chmod(candidate, targetInfo.Mode().Perm()); err != nil {
		return result, err
	}
	journal = updateJournal{PreviousMode: uint32(targetInfo.Mode().Perm()), RequestDigest: requestDigest, Phase: "prepared", PreviousSHA256: values["expected_sha256"], CandidateSHA256: values["sha256"]}
	if err := remoteio.WritePrivate(journalPath, journal); err != nil {
		return result, err
	}
	active.Digest = requestDigest
	if err := remoteio.WritePrivate(activePath, active); err != nil {
		return result, err
	}
	// Recheck after staging and journalling; never replace an unexpected target.
	current, err = binaryDigest(target)
	if err != nil || current != values["expected_sha256"] {
		return result, errors.New("target changed before replacement; journal preserved")
	}
	journal.Phase = "replacing"
	if err := remoteio.WritePrivate(journalPath, journal); err != nil {
		return result, err
	}
	if err := syncBinary(candidate); err != nil {
		return result, err
	}
	if err := replaceBinary(candidate, target); err != nil {
		return attention("Replacement outcome is uncertain; use recovery to reconcile the durable journal."), nil
	}
	if err := updater.validateCandidate(ctx, target, values["version"], values["sha256"]); err != nil {
		// Recovery must survive caller cancellation but never runs without a deadline.
		recoveryContext, cancel := context.WithTimeout(context.Background(), recoveryTimeout)
		defer cancel()
		recovered, recoveryErr := updater.recover(recoveryContext, request, journal, journalPath, backup, false)
		if recoveryErr != nil {
			return result, recoveryErr
		}
		if recovered.Status == "recovered-activation-pending" {
			return attention("Candidate validation failed; previous binary was restored. Update did not succeed; tunnel activation and health remain unproven."), nil
		}
		return recovered, nil
	}
	journal.Phase = "installed"
	if err := remoteio.WritePrivate(journalPath, journal); err != nil {
		return attention("Candidate installed but journal completion failed; reconcile before further changes."), nil
	}
	return installedResult(), nil
}

func installedResult() changeResult {
	return changeResult{Schema: changeSchema, Status: "installed-activation-pending", Message: "Candidate bytes and installed version verified. Existing processes, tunnel configuration and credentials were preserved. Running tunnel version, TLS path and connector health remain unproven; no service or broker was restarted."}
}

func attention(message string) changeResult {
	return changeResult{Schema: changeSchema, Status: "recovery-required", Message: message}
}

func (updater binaryUpdater) validateCandidate(ctx context.Context, target, version, expectedDigest string) error {
	if err := updater.validate(ctx, target, version); err != nil {
		return err
	}
	digest, err := binaryDigest(target)
	if err != nil || digest != expectedDigest {
		return errors.New("installed candidate digest changed during validation")
	}
	return nil
}
