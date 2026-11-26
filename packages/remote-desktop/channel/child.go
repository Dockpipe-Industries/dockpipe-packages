package main

import (
	"errors"
	"io"
	"os"
	"os/exec"
	"sync"
	"syscall"
	"time"
)

// Child diagnostics are discarded synchronously with no retained data. Keeping
// only a count avoids ever journaling accidentally sensitive CLI diagnostics.
type discardDiagnostics struct{}

func (discardDiagnostics) Write(value []byte) (int, error) { return len(value), nil }

type relayChild struct {
	command      *exec.Cmd
	connection   *pipeConnection
	wait         chan error
	once         sync.Once
	cleanupError error
}

func startRelay(cli, state, request string) (*relayChild, error) {
	inputRead, inputWrite, err := os.Pipe()
	if err != nil {
		return nil, errors.New("cannot create relay pipes")
	}
	outputRead, outputWrite, err := os.Pipe()
	if err != nil {
		inputRead.Close()
		inputWrite.Close()
		return nil, errors.New("cannot create relay pipes")
	}
	command := exec.Command(cli, "remote", "desktop-relay", "--stdio", "--state", state, "--request", request, "--auth", "never")
	command.Stdin = inputRead
	command.Stdout = outputWrite
	command.Stderr = discardDiagnostics{}
	command.WaitDelay = 2 * time.Second
	if err := command.Start(); err != nil {
		inputRead.Close()
		inputWrite.Close()
		outputRead.Close()
		outputWrite.Close()
		return nil, errors.New("cannot start trusted relay")
	}
	inputRead.Close()
	outputWrite.Close()
	child := &relayChild{
		command:    command,
		connection: &pipeConnection{input: inputWrite, output: outputRead},
		wait:       make(chan error, 1),
	}
	go func() { child.wait <- command.Wait() }()
	return child, nil
}

func (child *relayChild) close() error {
	child.once.Do(func() {
		child.connection.Close()
		// The pinned relay CLI owns no descendants by contract. Use os.Process
		// signaling, whose lifetime lock rejects a reused/reaped PID. SIGTERM
		// permits authenticated WS detach; SIGKILL follows a short grace.
		// The native controller owns a dedicated helper/relay process group
		// for forced shutdown if this helper itself cannot run cleanup.
		child.command.Process.Signal(syscall.SIGTERM)
		select {
		case <-child.wait:
			return
		case <-time.After(200 * time.Millisecond):
		}
		child.command.Process.Kill()
		select {
		case <-child.wait:
		case <-time.After(2 * time.Second):
			child.cleanupError = errors.New("relay child cleanup remains unconfirmed")
		}
	})
	return child.cleanupError
}

var _ io.Writer = discardDiagnostics{}
