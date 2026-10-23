package main

import (
	"bytes"
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestRecordRejectsInvalidIdentityConnectionAndCredentialReferences(t *testing.T) {
	mutations := map[string]func(*sessionRecord){
		"run traversal":            func(record *sessionRecord) { record.RunID = "../other" },
		"resource mismatch":        func(record *sessionRecord) { record.ResourceID = "other" },
		"machine UUID":             func(record *sessionRecord) { record.MachineUUID = "not-a-uuid" },
		"empty machine":            func(record *sessionRecord) { record.MachineUUID = "" },
		"phase":                    func(record *sessionRecord) { record.Phase = "unknown" },
		"missing started PID":      func(record *sessionRecord) { record.PID = 0 },
		"negative PID":             func(record *sessionRecord) { record.PID = -1 },
		"token capacity":           func(record *sessionRecord) { record.StartToken = strings.Repeat("x", 513) },
		"SSH port":                 func(record *sessionRecord) { record.SSHPort = 65536 },
		"agent port":               func(record *sessionRecord) { record.AgentPort = 0 },
		"nonlocal SSH":             func(record *sessionRecord) { record.SSHHost = "example.invalid" },
		"credential relative path": func(record *sessionRecord) { record.SSHKeyReference = "relative-key" },
		"credential line break":    func(record *sessionRecord) { record.SSHKeyReference = filepath.Join(t.TempDir(), "key\nother") },
		"execution mode":           func(record *sessionRecord) { record.ExecMode = "shell-fragment" },
		"metadata line break":      func(record *sessionRecord) { record.SSHUser = "user\nother" },
	}
	for name, mutate := range mutations {
		t.Run(name, func(t *testing.T) {
			record := fixtureRecord(t)
			mutate(&record)
			if err := record.validate(); err == nil {
				t.Fatal("invalid record accepted")
			}
		})
	}
}

func TestRecordReaderRejectsOversizeTrailingFieldsAndIdentitySubstitution(t *testing.T) {
	manager := helperManager(t)
	record := fixtureRecord(t)
	encoded, err := json.Marshal(record)
	if err != nil {
		t.Fatal(err)
	}
	path, err := manager.recordPath(record.RunID)
	if err != nil {
		t.Fatal(err)
	}
	cases := map[string][]byte{
		"oversized":                  bytes.Repeat([]byte(" "), 64*1024+1),
		"trailing document":          append(append([]byte{}, encoded...), []byte("{}")...),
		"plaintext credential field": bytes.Replace(encoded, []byte(`"schema":1`), []byte(`"schema":1,"password":"fixture-not-real"`), 1),
		"run substitution":           bytes.ReplaceAll(encoded, []byte(record.RunID), []byte("other-run")),
	}
	for name, data := range cases {
		t.Run(name, func(t *testing.T) {
			if err := os.WriteFile(path, data, 0600); err != nil {
				t.Fatal(err)
			}
			if _, err := manager.readRecord(record.RunID); err == nil {
				t.Fatal("invalid record file accepted")
			}
		})
	}
}

func TestRecordRefusesLinkedFilesAndPreservesPreviousRecord(t *testing.T) {
	manager := helperManager(t)
	record := fixtureRecord(t)
	if err := manager.writeRecord(record); err != nil {
		t.Fatal(err)
	}
	path, _ := manager.recordPath(record.RunID)
	before, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	changed := record
	changed.SSHHost = "example.invalid"
	if err := manager.writeRecord(changed); err == nil {
		t.Fatal("invalid replacement accepted")
	}
	after, _ := os.ReadFile(path)
	if !bytes.Equal(before, after) {
		t.Fatal("failed replacement changed the previous record")
	}
	if err := os.Remove(path); err != nil {
		t.Fatal(err)
	}
	target := filepath.Join(manager.workspace, "outside-record")
	if err := os.WriteFile(target, before, 0600); err != nil {
		t.Fatal(err)
	}
	if err := os.Symlink(target, path); err != nil {
		t.Log("Host does not permit symlink creation; replacement preservation was verified")
		return
	}
	if _, err := manager.readRecord(record.RunID); err == nil {
		t.Fatal("linked record read accepted")
	}
	if err := manager.writeRecord(record); err == nil {
		t.Fatal("linked record replacement accepted")
	}
}
