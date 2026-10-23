package main

import (
	"context"
	"encoding/json"
	"fmt"
)

type processIdentity struct {
	PID        int    `json:"pid"`
	StartToken string `json:"start_token"`
}

type observedResource struct {
	ID      string          `json:"id"`
	Runtime string          `json:"runtime"`
	State   string          `json:"state"`
	Process processIdentity `json:"process"`
}

type observedExecution struct {
	ID       string            `json:"id"`
	State    string            `json:"state"`
	Resource *observedResource `json:"resource"`
}

type executionSnapshot struct {
	Schema     string              `json:"schema"`
	Executions []observedExecution `json:"executions"`
}

func (manager manager) observe(ctx context.Context) (executionSnapshot, error) {
	var snapshot executionSnapshot
	data, err := runCommand(ctx, manager.program, []string{"runs", "observe", "--workdir", manager.workspace}, manager.workspace)
	if err != nil {
		return snapshot, err
	}
	if json.Unmarshal(data, &snapshot) != nil || snapshot.Schema != "dockpipe.execution_snapshot.v1" || len(snapshot.Executions) > 256 {
		return snapshot, fmt.Errorf("unsupported execution observation contract")
	}
	return snapshot, nil
}

func (snapshot executionSnapshot) resource(record sessionRecord, requireToken bool) *observedResource {
	for _, execution := range snapshot.Executions {
		resource := execution.Resource
		if execution.ID != record.RunID || execution.State != "running" || resource == nil {
			continue
		}
		if resource.Runtime != "qemu" || resource.ID != record.ResourceID || resource.State != "running" || resource.Process.PID != record.PID || resource.Process.StartToken == "" {
			continue
		}
		if requireToken && resource.Process.StartToken != record.StartToken {
			continue
		}
		return resource
	}
	return nil
}
