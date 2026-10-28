package cloudflare

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"time"

	contract "dockpipe/src/lib/domain/remote"
	"dockpipe/src/lib/infrastructure"
	"dockpipe/src/lib/infrastructure/process"
	remoteio "dockpipe/src/lib/infrastructure/remote"
)

const activationSchema = "dockpipe.remote.edge-activation/v1"

type brokerActivationResult struct {
	Schema        string    `json:"schema"`
	ID            string    `json:"id"`
	Status        string    `json:"status"`
	Message       string    `json:"message"`
	ActiveSHA256  string    `json:"active_sha256,omitempty"`
	CheckedAt     time.Time `json:"checked_at"`
	RecoveryUntil time.Time `json:"recovery_until"`
}

func updateApplyApproval(request changeRequest) string {
	return changeApproval(request, "apply")
}

func changeApproval(request changeRequest, operation string) string {
	data, _ := json.Marshal(struct {
		Operation string        `json:"operation"`
		Request   changeRequest `json:"request"`
	}{operation, request})
	digest := sha256.Sum256(data)
	return hex.EncodeToString(digest[:])
}

// prepareActivation keeps vendor launch/metrics and advisory semantics here. The
// broker supervises the returned generic process/health/recovery contract.
func prepareActivation(request changeRequest) (string, map[string]any, error) {
	if request.Schema != changeSchema || request.Resolver != LifecycleProviderID || request.ProviderVersion != LifecycleProviderVersion || request.Machine != "local" {
		return "", nil, errors.New("activation requires this versioned local resolver")
	}
	allowed := map[string]bool{"update_request": true, "update_approval": true, "broker_state": true, "expected_edge_digest": true, "tunnel_config_sha256": true, "metrics_address": true, "maintenance": true, "startup_seconds": true, "stable_seconds": true, "recovery_seconds": true, "emergency_recovery": true}
	values := request.Parameters
	for key := range values {
		if !allowed[key] {
			return "", nil, errors.New("unknown activation parameter")
		}
	}
	if !safeInventoryPath(values["update_request"]) || !safeInventoryPath(values["broker_state"]) || values["maintenance"] != "allow-connector-replacement" {
		return "", nil, errors.New("activation requires a private update request, explicit broker state and connector maintenance policy")
	}
	root := values["broker_state"]
	if err := infrastructure.ValidatePrivatePath(root, true); err != nil {
		return "", nil, err
	}
	var update changeRequest
	if err := remoteio.ReadPrivate(values["update_request"], &update); err != nil {
		return "", nil, err
	}
	// Activation understands the original v1 standalone 0.3.1 journal too.
	// Preserve its original version in the approval and transaction identity;
	// only validation of the known-compatible shape uses the current provider.
	validatedUpdate := update
	if update.ProviderVersion == "0.3.1" {
		validatedUpdate.ProviderVersion = LifecycleProviderVersion
	}
	if err := validateChangeRequest(validatedUpdate); err != nil {
		return "", nil, err
	}
	if values["update_approval"] != updateApplyApproval(update) {
		return "", nil, errors.New("update request no longer matches its exact approved identity")
	}
	updateValues := update.Parameters
	transaction := changeTransactionDigest(update)
	state := filepath.Join(filepath.Dir(updateValues["target"]), ".dockpipe-cloudflared-update")
	var active struct {
		Digest string `json:"digest"`
	}
	if err := remoteio.ReadPrivate(filepath.Join(state, "active.json"), &active); err != nil || active.Digest != transaction {
		return "", nil, errors.New("activation must reference the current installation transaction")
	}
	var journal updateJournal
	if err := remoteio.ReadPrivate(filepath.Join(state, transaction, "journal.json"), &journal); err != nil {
		return "", nil, err
	}
	if journal.RequestDigest != transaction || journal.Phase != "installed" || journal.CandidateSHA256 != updateValues["sha256"] || journal.PreviousSHA256 != updateValues["expected_sha256"] {
		return "", nil, errors.New("installation is not verified and ready for activation")
	}
	fallback := filepath.Join(state, transaction, "previous")
	for path, expected := range map[string]string{updateValues["target"]: updateValues["sha256"], fallback: updateValues["expected_sha256"]} {
		digest, err := binaryDigest(path)
		if err != nil || digest != expected {
			return "", nil, errors.New("candidate or recovery bytes changed")
		}
	}
	old, _ := parseCalendarVersion(updateValues["expected_version"])
	if values["emergency_recovery"] != "" && values["emergency_recovery"] != "allow-vulnerable-rollback" {
		return "", nil, errors.New("unknown activation recovery policy")
	}
	if compareCalendarVersion(old, [3]int{2026, 10, 2}) < 0 && values["emergency_recovery"] != "allow-vulnerable-rollback" {
		return "", nil, errors.New("activation recovery would start a vulnerable version; explicit emergency approval is required")
	}
	var edge contract.EdgeConfig
	if err := remoteio.ReadPrivate(filepath.Join(root, "edge.json"), &edge); err != nil {
		return "", nil, err
	}
	data, _ := json.Marshal(edge)
	digest := sha256.Sum256(data)
	if hex.EncodeToString(digest[:]) != values["expected_edge_digest"] || edge.Executable != updateValues["target"] || len(edge.Arguments) != 6 ||
		edge.Arguments[0] != "tunnel" || edge.Arguments[1] != "--config" || edge.Arguments[3] != "--no-autoupdate" || edge.Arguments[4] != "run" || !tunnelPattern.MatchString(edge.Arguments[5]) {
		return "", nil, errors.New("activation supports only the unchanged Dockpipe-managed standalone tunnel launch contract")
	}
	configPath := filepath.Join(root, "edge", "tunnel-config.json")
	if edge.Arguments[2] != configPath {
		return "", nil, errors.New("tunnel configuration is outside the selected broker state")
	}
	configDigest, err := binaryDigest(configPath)
	if err != nil || configDigest != values["tunnel_config_sha256"] {
		return "", nil, errors.New("tunnel configuration changed since approval")
	}
	host, portText, err := net.SplitHostPort(values["metrics_address"])
	port, portErr := strconv.Atoi(portText)
	if err != nil || portErr != nil || host != "127.0.0.1" || port < 1024 || port > 65535 {
		return "", nil, errors.New("choose an unprivileged explicit IPv4 loopback metrics address")
	}
	startup, stable, recovery := 10, 3, 120
	for key, target := range map[string]*int{"startup_seconds": &startup, "stable_seconds": &stable, "recovery_seconds": &recovery} {
		if values[key] != "" {
			number, err := strconv.Atoi(values[key])
			if err != nil {
				return "", nil, errors.New("invalid activation timing")
			}
			*target = number
		}
	}
	if startup < 2 || startup > 30 || stable < 2 || stable > 20 || recovery < 2*startup+stable+10 || recovery > 900 {
		return "", nil, errors.New("activation timing exceeds bounded policy")
	}
	arguments := append([]string{"tunnel", "--metrics", values["metrics_address"]}, edge.Arguments[1:]...)
	candidate := edge
	candidate.Arguments = arguments
	previous := candidate
	previous.Executable = fallback
	// The canonical JSON object identity is shared with the CLI contract without
	// importing an unreleased Core implementation into this package's pinned SDK.
	result := map[string]any{"inputs": map[string]string{configPath: configDigest}, "schema": activationSchema, "expected_config_digest": values["expected_edge_digest"],
		"candidate": candidate, "candidate_sha256": updateValues["sha256"], "recovery": previous, "recovery_sha256": updateValues["expected_sha256"],
		"readiness_url": "http://" + values["metrics_address"] + "/ready", "startup_seconds": startup, "stable_seconds": stable, "recovery_seconds": recovery, "maintenance": values["maintenance"]}
	return root, result, nil
}

func activateBroker(ctx context.Context, request changeRequest, preview bool) (changeResult, error) {
	root, activation, err := prepareActivation(request)
	if err != nil {
		return changeResult{}, err
	}
	executable := os.Getenv("DOCKPIPE_BIN")
	if !filepath.IsAbs(executable) {
		return changeResult{}, errors.New("activation requires the explicit operator CLI")
	}
	// Normalize nested objects identically to Core's canonical activation ID.
	data, err := json.Marshal(activation)
	if err != nil {
		return changeResult{}, err
	}
	var canonical map[string]any
	if err := json.Unmarshal(data, &canonical); err != nil {
		return changeResult{}, err
	}
	data, err = json.Marshal(canonical)
	if err != nil {
		return changeResult{}, err
	}
	digest := sha256.Sum256(data)
	id := hex.EncodeToString(digest[:])
	if err := saveActivationReceipt(request, id); err != nil {
		return changeResult{}, err
	}
	directory, err := os.MkdirTemp(filepath.Dir(os.Getenv("DOCKPIPE_DEPENDENCY_OUTPUT")), "activation-")
	if err != nil {
		return changeResult{}, err
	}
	defer os.RemoveAll(directory)
	path := filepath.Join(directory, "request.json")
	if err := os.WriteFile(path, data, 0600); err != nil {
		return changeResult{}, err
	}
	arguments := []string{"remote", "edge-activate", "--state", root, "--request", path}
	if preview {
		arguments = append(arguments, "--dry-run")
	} else {
		arguments = append(arguments, "--approve", id)
	}
	command := exec.CommandContext(ctx, executable, arguments...)
	command.Env = os.Environ()
	command.Stderr = os.Stderr
	output := &activationOutput{}
	command.Stdout = output
	command.WaitDelay = time.Second
	runErr := process.RunNested(command)
	var status brokerActivationResult
	if err := remoteio.Decode(output.data, &status); err != nil || status.ID != id || status.Schema != activationSchema {
		return changeResult{Schema: changeSchema, Status: "recovery-required", Message: "Broker activation outcome is unknown. Do not replay; inspect dockpipe remote edge-status --id " + id + " using the selected broker state. Its bounded recovery continues independently."}, nil
	}
	if preview && (status.Status == "ready" || status.Status == "healthy") && runErr == nil {
		return changeResult{Schema: changeSchema, Status: "ready", Message: "Broker accepted the exact activation contract. Candidate readiness is checked before old connector shutdown; active digest and signed HTTPS identity are then verified. New jobs pause through the bounded recovery window."}, nil
	}
	if status.Status == "healthy" && runErr == nil {
		return changeResult{Schema: changeSchema, Status: "active-healthy", Message: status.Message + " Activation ID: " + id + " Checked: " + status.CheckedAt.Format(time.RFC3339) + " Recovery until: " + status.RecoveryUntil.Format(time.RFC3339)}, nil
	}
	if status.Status == "recovered" {
		return changeResult{Schema: changeSchema, Status: "activation-recovered", Message: status.Message + " Activation ID: " + id}, nil
	}
	return changeResult{Schema: changeSchema, Status: "recovery-required", Message: status.Message + " Inspect activation ID " + id + "; do not retry automatically."}, nil
}

type activationOutput struct{ data []byte }

func (output *activationOutput) Write(data []byte) (int, error) {
	if len(output.data)+len(data) > 16384 {
		return 0, errors.New("activation response exceeded 16 KiB")
	}
	output.data = append(output.data, data...)
	return len(data), nil
}

type activationReceipt struct {
	Schema   string `json:"schema"`
	Approval string `json:"approval"`
	ID       string `json:"id"`
}

func activationReceiptPath(request changeRequest) (string, error) {
	if request.Schema != changeSchema || request.Resolver != LifecycleProviderID || request.ProviderVersion != LifecycleProviderVersion || request.Machine != "local" || !safeInventoryPath(request.Parameters["broker_state"]) {
		return "", errors.New("invalid local activation status request")
	}
	root := request.Parameters["broker_state"]
	if err := infrastructure.ValidatePrivatePath(root, true); err != nil {
		return "", err
	}
	directory := filepath.Join(root, "edge-activation-requests")
	if err := remoteio.PrivateDirectory(directory); err != nil {
		return "", err
	}
	return filepath.Join(directory, changeApproval(request, "activate")+".json"), nil
}

func saveActivationReceipt(request changeRequest, id string) error {
	path, err := activationReceiptPath(request)
	if err != nil {
		return err
	}
	return remoteio.WritePrivate(path, activationReceipt{Schema: activationSchema, Approval: changeApproval(request, "activate"), ID: id})
}

func activationStatus(ctx context.Context, request changeRequest) (changeResult, error) {
	path, err := activationReceiptPath(request)
	if err != nil {
		return changeResult{}, err
	}
	var receipt activationReceipt
	if err := remoteio.ReadPrivate(path, &receipt); err != nil {
		return changeResult{}, err
	}
	digest, decodeErr := hex.DecodeString(receipt.ID)
	if receipt.Schema != activationSchema || receipt.Approval != changeApproval(request, "activate") || decodeErr != nil || len(digest) != 32 || hex.EncodeToString(digest) != receipt.ID {
		return changeResult{}, errors.New("invalid activation receipt")
	}
	executable := os.Getenv("DOCKPIPE_BIN")
	if !filepath.IsAbs(executable) {
		return changeResult{}, errors.New("activation status requires the operator CLI")
	}
	command := exec.CommandContext(ctx, executable, "remote", "edge-status", "--state", request.Parameters["broker_state"], "--id", receipt.ID)
	command.Env = os.Environ()
	command.Stderr = os.Stderr
	command.WaitDelay = time.Second
	output := &activationOutput{}
	command.Stdout = output
	if err := process.RunNested(command); err != nil {
		return changeResult{}, errors.New("activation health could not be read; no activation was replayed")
	}
	var status brokerActivationResult
	if err := remoteio.Decode(output.data, &status); err != nil || status.Schema != activationSchema || status.ID != receipt.ID {
		return changeResult{}, errors.New("invalid broker activation status")
	}
	resultStatus := "recovery-required"
	if status.Status == "healthy" {
		resultStatus = "active-healthy"
	}
	if status.Status == "recovered" {
		resultStatus = "activation-recovered"
	}
	return changeResult{Schema: changeSchema, Status: resultStatus, Message: status.Message + " Activation ID: " + status.ID + " Checked: " + status.CheckedAt.Format(time.RFC3339) + " Recovery until: " + status.RecoveryUntil.Format(time.RFC3339)}, nil
}
