package main

import (
	"bytes"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"time"
)

var identifier = regexp.MustCompile(`^[a-zA-Z0-9][a-zA-Z0-9._-]{0,127}$`)
var machineIdentity = regexp.MustCompile(`(?i)^[0-9a-f]{8}-[0-9a-f]{4}-[1-5][0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$`)

type sessionRecord struct {
	Schema          int       `json:"schema"`
	RunID           string    `json:"run_id"`
	LaunchID        string    `json:"launch_id,omitempty"`
	ResourceID      string    `json:"resource_id"`
	MachineUUID     string    `json:"machine_uuid"`
	PID             int       `json:"pid"`
	StartToken      string    `json:"start_token"`
	Phase           string    `json:"phase"`
	SSHHost         string    `json:"ssh_host"`
	SSHPort         int       `json:"ssh_port"`
	SSHUser         string    `json:"ssh_user"`
	SSHKeyReference string    `json:"ssh_key_reference"`
	AgentPort       int       `json:"agent_port"`
	ExecMode        string    `json:"exec_mode"`
	UpdatedAt       time.Time `json:"updated_at"`
}

func (record sessionRecord) validate() error {
	if record.LaunchID != "" && !identifier.MatchString(record.LaunchID) {
		return fmt.Errorf("invalid VM launch correlation")
	}
	if record.Schema != 1 || !identifier.MatchString(record.RunID) || record.ResourceID != "dockpipe-vm-"+record.RunID {
		return fmt.Errorf("invalid VM session identity")
	}
	if !machineIdentity.MatchString(record.MachineUUID) {
		return fmt.Errorf("invalid VM machine identity")
	}
	if record.ExecMode != "powershell" && record.ExecMode != "bash" && record.ExecMode != "raw" {
		return fmt.Errorf("unsupported guest execution mode")
	}
	if record.Phase != "prepared" && record.Phase != "started" && record.Phase != "exited" {
		return fmt.Errorf("unsupported VM lifecycle phase")
	}
	if record.PID < 0 || (record.Phase == "started" && record.PID == 0) || len(record.StartToken) > 512 {
		return fmt.Errorf("invalid VM process identity")
	}
	if record.SSHPort < 1 || record.SSHPort > 65535 || record.AgentPort < 1 || record.AgentPort > 65535 {
		return fmt.Errorf("invalid guest connection port")
	}
	if record.SSHHost != "127.0.0.1" && record.SSHHost != "localhost" && record.SSHHost != "::1" {
		return fmt.Errorf("this provider manages local QEMU guest connections only")
	}
	for _, value := range []string{record.MachineUUID, record.SSHUser, record.ExecMode} {
		if len(value) > 256 || strings.ContainsAny(value, "\x00\r\n") {
			return fmt.Errorf("invalid VM session metadata")
		}
	}
	if record.SSHKeyReference != "" && (!filepath.IsAbs(record.SSHKeyReference) || len(record.SSHKeyReference) > 4096 || strings.ContainsAny(record.SSHKeyReference, "\x00\r\n")) {
		return fmt.Errorf("invalid SSH credential reference")
	}
	return nil
}

func (manager manager) recordPath(runID string) (string, error) {
	if !identifier.MatchString(runID) {
		return "", fmt.Errorf("invalid VM resource selection")
	}
	return filepath.Join(manager.state, "vm-session-"+runID+".json"), nil
}

func (manager manager) readRecord(runID string) (sessionRecord, error) {
	var record sessionRecord
	path, err := manager.recordPath(runID)
	if err != nil {
		return record, err
	}
	info, err := os.Lstat(path)
	if err != nil {
		return record, err
	}
	if !info.Mode().IsRegular() || info.Size() > 64*1024 {
		return record, fmt.Errorf("invalid VM record file")
	}
	file, err := os.Open(path)
	if err != nil {
		return record, err
	}
	defer file.Close()
	opened, err := file.Stat()
	if err != nil || !opened.Mode().IsRegular() || !os.SameFile(info, opened) {
		return record, fmt.Errorf("VM record changed while opening")
	}
	data, err := io.ReadAll(io.LimitReader(file, 64*1024+1))
	if err != nil {
		return record, err
	}
	if len(data) > 64*1024 {
		return record, fmt.Errorf("VM record exceeds its contract")
	}
	decoder := json.NewDecoder(bytes.NewReader(data))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(&record); err != nil {
		return record, err
	}
	var extra any
	if decoder.Decode(&extra) != io.EOF || record.RunID != runID {
		return record, fmt.Errorf("VM record identity mismatch or trailing data")
	}
	return record, record.validate()
}

func (manager manager) writeRecord(record sessionRecord) error {
	if err := record.validate(); err != nil {
		return err
	}
	path, err := manager.recordPath(record.RunID)
	if err != nil {
		return err
	}
	if info, err := os.Lstat(path); err == nil && !info.Mode().IsRegular() {
		return fmt.Errorf("VM record must not be redirected")
	} else if err != nil && !os.IsNotExist(err) {
		return err
	}
	record.UpdatedAt = time.Now().UTC()
	data, err := json.MarshalIndent(record, "", "  ")
	if err != nil || len(data) > 64*1024 {
		return fmt.Errorf("VM record exceeds its contract")
	}
	file, err := os.CreateTemp(manager.state, ".vm-session-*")
	if err != nil {
		return err
	}
	defer os.Remove(file.Name())
	if _, err := file.Write(data); err != nil {
		file.Close()
		return err
	}
	if err := file.Sync(); err != nil {
		file.Close()
		return err
	}
	if err := file.Close(); err != nil {
		return err
	}
	return os.Rename(file.Name(), path)
}
