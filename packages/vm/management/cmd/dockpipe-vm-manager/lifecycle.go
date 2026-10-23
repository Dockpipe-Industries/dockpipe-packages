package main

import (
	"context"
	"fmt"
	"os"
	"strconv"
)

func eventInteger(name string) (int, error) {
	value, err := strconv.Atoi(os.Getenv("DOCKPIPE_VM_EVENT_" + name))
	if err != nil {
		return 0, fmt.Errorf("invalid session event field %s", name)
	}
	return value, nil
}

func (manager manager) report(ctx context.Context) error {
	if os.Getenv("DOCKPIPE_VM_EVENT_SCHEMA") != "1" {
		return fmt.Errorf("unsupported VM lifecycle contract")
	}
	processID, err := eventInteger("PID")
	if err != nil {
		return err
	}
	sshPort, err := eventInteger("SSH_PORT")
	if err != nil {
		return err
	}
	agentPort, err := eventInteger("AGENT_PORT")
	if err != nil {
		return err
	}
	record := sessionRecord{
		Schema:          1,
		RunID:           os.Getenv("DOCKPIPE_VM_EVENT_RUN_ID"),
		LaunchID:        os.Getenv("DOCKPIPE_VM_EVENT_LAUNCH_ID"),
		ResourceID:      os.Getenv("DOCKPIPE_VM_EVENT_RESOURCE_ID"),
		MachineUUID:     os.Getenv("DOCKPIPE_VM_EVENT_MACHINE_UUID"),
		PID:             processID,
		Phase:           os.Getenv("DOCKPIPE_VM_EVENT_PHASE"),
		SSHHost:         os.Getenv("DOCKPIPE_VM_EVENT_SSH_HOST"),
		SSHPort:         sshPort,
		SSHUser:         os.Getenv("DOCKPIPE_VM_EVENT_SSH_USER"),
		SSHKeyReference: os.Getenv("DOCKPIPE_VM_EVENT_SSH_KEY_REFERENCE"),
		AgentPort:       agentPort,
		ExecMode:        os.Getenv("DOCKPIPE_VM_EVENT_EXEC_MODE"),
	}
	previous, previousError := manager.readRecord(record.RunID)
	if previousError == nil {
		if previous.MachineUUID != record.MachineUUID || previous.ResourceID != record.ResourceID || previous.LaunchID != record.LaunchID {
			return fmt.Errorf("VM session identity changed")
		}
		if record.Phase == "exited" && record.PID == 0 {
			record.PID = previous.PID
		}
		if record.Phase == "prepared" && previous.Phase != "prepared" {
			return fmt.Errorf("VM session has already started")
		}
		record.StartToken = previous.StartToken
	} else if !os.IsNotExist(previousError) {
		return previousError
	}
	if err := record.validate(); err != nil {
		return err
	}
	if record.Phase == "started" {
		snapshot, err := manager.observe(ctx)
		if err != nil {
			return err
		}
		resource := snapshot.resource(record, false)
		if resource == nil {
			_, err := runCommand(ctx, manager.program, []string{"runs", "associate", "qemu", record.ResourceID, strconv.Itoa(record.PID)}, manager.workspace)
			if err != nil {
				return fmt.Errorf("register runtime process association: %w", err)
			}
			snapshot, err = manager.observe(ctx)
			if err != nil {
				return err
			}
			resource = snapshot.resource(record, false)
		}
		if resource == nil {
			return fmt.Errorf("live VM process association could not be verified")
		}
		if record.StartToken != "" && record.StartToken != resource.Process.StartToken {
			return fmt.Errorf("VM process identity changed")
		}
		record.StartToken = resource.Process.StartToken
	}
	return manager.writeRecord(record)
}
