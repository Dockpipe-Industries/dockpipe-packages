package main

import (
	"bytes"
	"context"
	"fmt"
	"os"
	"os/exec"
)

type boundedBuffer struct {
	data    bytes.Buffer
	maximum int
}

func (buffer *boundedBuffer) Write(data []byte) (int, error) {
	if buffer.data.Len()+len(data) > buffer.maximum {
		return 0, fmt.Errorf("operation output exceeded its contract")
	}
	return buffer.data.Write(data)
}

func (buffer *boundedBuffer) Bytes() []byte {
	return buffer.data.Bytes()
}

func runCommand(ctx context.Context, program string, arguments []string, directory string, extraEnvironment ...string) ([]byte, error) {
	command := exec.CommandContext(ctx, program, arguments...)
	command.Dir = directory
	command.Env = append(os.Environ(), "DOCKPIPE_WORKDIR="+directory)
	command.Env = append(command.Env, extraEnvironment...)
	if runID := os.Getenv("DOCKPIPE_VM_EVENT_RUN_ID"); runID != "" {
		command.Env = append(command.Env, "DOCKPIPE_RUN_ID="+runID)
	}
	output := &boundedBuffer{maximum: 1024 * 1024}
	diagnostic := &boundedBuffer{maximum: 64 * 1024}
	command.Stdout = output
	command.Stderr = diagnostic
	if err := command.Run(); err != nil {
		if ctx.Err() != nil {
			return nil, ctx.Err()
		}
		// Child output can include transport details or credential references.
		// Keep it out of the public surface error stream.
		return nil, fmt.Errorf("operation failed: %w", err)
	}
	return output.Bytes(), nil
}
