//go:build linux

package cloudflare

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"

	contract "dockpipe/src/lib/domain/remote"
	remoteio "dockpipe/src/lib/infrastructure/remote"
)

func activationFixture(t *testing.T) (changeRequest, changeRequest) {
	t.Helper()
	updater, update := updateFixture(t)
	result, err := updater.execute(context.Background(), update, "apply")
	if err != nil || result.Status != "installed-activation-pending" {
		t.Fatal(result, err)
	}
	directory := filepath.Dir(update.Parameters["target"])
	root := filepath.Join(directory, "broker")
	if err := os.MkdirAll(filepath.Join(root, "edge"), 0700); err != nil {
		t.Fatal(err)
	}
	configPath := filepath.Join(root, "edge", "tunnel-config.json")
	configData := []byte(`{"tunnel":"00000000-0000-0000-0000-000000000001","credentials-file":"/private/credential","ingress":[]}`)
	if err := os.WriteFile(configPath, configData, 0600); err != nil {
		t.Fatal(err)
	}
	configSum := sha256.Sum256(configData)
	edge := contract.EdgeConfig{Schema: contract.Version, Endpoint: "https://edge.example.test", Executable: update.Parameters["target"], Arguments: []string{"tunnel", "--config", configPath, "--no-autoupdate", "run", "00000000-0000-0000-0000-000000000001"}}
	if err := remoteio.WritePrivate(filepath.Join(root, "edge.json"), edge); err != nil {
		t.Fatal(err)
	}
	data, _ := json.Marshal(edge)
	sum := sha256.Sum256(data)
	updatePath := filepath.Join(directory, "update.json")
	if err := remoteio.WritePrivate(updatePath, update); err != nil {
		t.Fatal(err)
	}
	request := changeRequest{Schema: changeSchema, Resolver: LifecycleProviderID, ProviderVersion: LifecycleProviderVersion, Machine: "local", Parameters: map[string]string{
		"update_request": updatePath, "update_approval": updateApplyApproval(update), "broker_state": root, "expected_edge_digest": hex.EncodeToString(sum[:]), "tunnel_config_sha256": hex.EncodeToString(configSum[:]), "metrics_address": "127.0.0.1:45000", "maintenance": "allow-connector-replacement",
	}}
	return request, update
}

func TestActivationBindsInstalledTransactionAndPreservesProviderLaunch(t *testing.T) {
	request, update := activationFixture(t)
	root, result, err := prepareActivation(request)
	if err != nil || root != request.Parameters["broker_state"] {
		t.Fatal(result, err)
	}
	candidate := result["candidate"].(contract.EdgeConfig)
	previous := result["recovery"].(contract.EdgeConfig)
	if candidate.Arguments[1] != "--metrics" || candidate.Arguments[2] != "127.0.0.1:45000" || candidate.Executable != update.Parameters["target"] || previous.Executable == candidate.Executable {
		t.Fatal(candidate, previous)
	}
	if result["candidate_sha256"] != update.Parameters["sha256"] || result["recovery_sha256"] != update.Parameters["expected_sha256"] {
		t.Fatal("unbound executable identity")
	}
}

func TestActivationRejectsDriftAndMissingMaintenance(t *testing.T) {
	for _, scenario := range []string{"approval", "config", "edge", "metrics", "maintenance", "binary"} {
		t.Run(scenario, func(t *testing.T) {
			request, update := activationFixture(t)
			switch scenario {
			case "approval":
				request.Parameters["update_approval"] = "changed"
			case "config":
				request.Parameters["tunnel_config_sha256"] = "changed"
			case "edge":
				request.Parameters["expected_edge_digest"] = "changed"
			case "metrics":
				request.Parameters["metrics_address"] = "0.0.0.0:45000"
			case "maintenance":
				delete(request.Parameters, "maintenance")
			case "binary":
				os.WriteFile(update.Parameters["target"], []byte("unexpected"), 0700)
			}
			if _, _, err := prepareActivation(request); err == nil {
				t.Fatal("unapproved drift accepted")
			}
		})
	}
}

func TestActivationCLIEnvelopeMatchesCurrentCore(t *testing.T) {
	executable := os.Getenv("DOCKPIPE_TEST_CORE_CLI")
	if executable == "" {
		t.Skip("explicit current Core CLI not selected for cross-repository wire validation")
	}
	request, _ := activationFixture(t)
	root, activation, err := prepareActivation(request)
	if err != nil {
		t.Fatal(err)
	}
	data, err := json.Marshal(activation)
	if err != nil {
		t.Fatal(err)
	}
	var normalized map[string]any
	if err := json.Unmarshal(data, &normalized); err != nil {
		t.Fatal(err)
	}
	data, _ = json.Marshal(normalized)
	sum := sha256.Sum256(data)
	id := hex.EncodeToString(sum[:])
	path := filepath.Join(root, "activation-request.json")
	if err := os.WriteFile(path, data, 0600); err != nil {
		t.Fatal(err)
	}
	command := exec.Command(executable, "remote", "edge-activate", "--state", root, "--request", path, "--approve", id, "--auth", "never")
	command.Env = os.Environ()
	output, err := command.CombinedOutput()
	// The isolated fixture intentionally has no operator identity/service. Reaching
	// this error proves Core accepted the wire schema and identical approval ID,
	// without contacting or unlocking the installed broker.
	if err == nil || !strings.Contains(string(output), "broker management is not configured") {
		t.Fatalf("Core rejected provider activation envelope before operator admission: %v %s", err, output)
	}
	command = exec.Command(executable, "remote", "edge-activate", "--state", root, "--request", path, "--approve", strings.Repeat("0", 64), "--auth", "never")
	output, err = command.CombinedOutput()
	if err == nil || !strings.Contains(string(output), "approval does not match") {
		t.Fatal("mismatched activation approval reached operator admission", err, string(output))
	}
}

func TestActivationRecognizesOriginalV1StandaloneReceipt(t *testing.T) {
	request, update := activationFixture(t)
	currentTransaction := changeTransactionDigest(update)
	update.ProviderVersion = "0.3.1"
	legacyTransaction := changeTransactionDigest(update)
	root := filepath.Join(filepath.Dir(update.Parameters["target"]), ".dockpipe-cloudflared-update")
	oldDirectory := filepath.Join(root, currentTransaction)
	var journal updateJournal
	if err := remoteio.ReadPrivate(filepath.Join(oldDirectory, "journal.json"), &journal); err != nil {
		t.Fatal(err)
	}
	journal.RequestDigest = legacyTransaction
	if err := remoteio.WritePrivate(filepath.Join(oldDirectory, "journal.json"), journal); err != nil {
		t.Fatal(err)
	}
	if err := os.Rename(oldDirectory, filepath.Join(root, legacyTransaction)); err != nil {
		t.Fatal(err)
	}
	if err := remoteio.WritePrivate(filepath.Join(root, "active.json"), struct {
		Digest string `json:"digest"`
	}{legacyTransaction}); err != nil {
		t.Fatal(err)
	}
	if err := remoteio.WritePrivate(request.Parameters["update_request"], update); err != nil {
		t.Fatal(err)
	}
	request.Parameters["update_approval"] = updateApplyApproval(update)
	if _, _, err := prepareActivation(request); err != nil {
		t.Fatal("known original journal was rejected", err)
	}
}
