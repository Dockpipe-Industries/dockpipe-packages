package main

import (
	"context"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"os"
	"os/signal"
	"path/filepath"
	"syscall"
	"time"
)

type Config struct {
	Capture           *CaptureProfile `json:"capture,omitempty"`
	Schema            string          `json:"schema"`
	Reference         Reference       `json:"reference"`
	Role              string          `json:"role"`
	Mode              string          `json:"mode"`
	LocalFingerprint  string          `json:"local_fingerprint"`
	PeerFingerprint   string          `json:"peer_fingerprint"`
	IdentityDirectory string          `json:"identity_directory"`
	CLI               string          `json:"cli"`
	StateDirectory    string          `json:"state_directory"`
	Deadline          time.Time       `json:"deadline"`
	LeaseExpiresAt    time.Time       `json:"lease_expires_at"`
}

func (config Config) hello() Hello {
	return Hello{Schema: channelVersion, Reference: config.Reference, Role: config.Role, Mode: config.Mode}
}

func (config Config) validate(now time.Time) error {
	if config.Role == "viewer" && (config.Capture == nil || config.Capture.validate() != nil) || config.Role == "worker" && config.Capture != nil {
		return errors.New("invalid native capture configuration")
	}
	if config.Schema != channelVersion || config.hello().validate() != nil || !digest(config.LocalFingerprint) || !digest(config.PeerFingerprint) ||
		config.LocalFingerprint == config.PeerFingerprint || !filepath.IsAbs(config.IdentityDirectory) || !filepath.IsAbs(config.CLI) ||
		!filepath.IsAbs(config.StateDirectory) || !validLease(config.LeaseExpiresAt, config.Deadline, now) || config.Deadline.After(now.Add(time.Hour)) {
		return errors.New("invalid native channel configuration")
	}
	info, err := os.Stat(config.CLI)
	if err != nil || !info.Mode().IsRegular() || info.Mode().Perm()&0111 == 0 {
		return errors.New("trusted channel CLI is unavailable")
	}
	return nil
}

func connect(ctx context.Context, configPath string, controlFD, statusFD int) error {
	input, err := adoptPipe(0, "desktop-native-input")
	if err != nil {
		return err
	}
	output, err := adoptPipe(1, "desktop-native-output")
	if err != nil {
		input.Close()
		return err
	}
	if statusFD < 3 || statusFD == controlFD {
		input.Close()
		output.Close()
		return errors.New("separate private status pipe required")
	}
	status, err := adoptPipe(statusFD, "desktop-native-status")
	if err != nil {
		input.Close()
		output.Close()
		return err
	}
	// This inherited descriptor has no os.File owner; only its private
	// duplicate remains, so closing readiness really produces one-shot EOF.
	syscall.Close(statusFD)
	defer status.Close()
	return connectWithPipes(ctx, configPath, controlFD, LocalPipes{Input: input, Output: output}, status)
}

func connectWithPipes(ctx context.Context, configPath string, controlFD int, pipes LocalPipes, status *os.File) error {
	defer pipes.close()
	data, err := privateFile(configPath, maximumJSON)
	if err != nil {
		return err
	}
	var config Config
	if decodeJSON(data, &config) != nil || config.validate(time.Now()) != nil {
		return errors.New("invalid native channel configuration")
	}
	identity, err := loadIdentity(config.IdentityDirectory, config.LocalFingerprint)
	if err != nil {
		return err
	}
	tlsConfig, err := tlsConfiguration(identity, config.PeerFingerprint, time.Now)
	if err != nil {
		return err
	}
	if controlFD < 3 {
		return errors.New("private authority control descriptor is required")
	}
	control, err := adoptControl(controlFD, "desktop-authority-control")
	if err != nil {
		return err
	}
	defer control.Close()
	pipes.Control = control
	// Burn before dispatch. Any failed attach requires new authorization and a
	// fresh identity; an uncertain relay effect is never silently replayed.
	if err := exclusiveFile(filepath.Join(config.IdentityDirectory, "consumed"), []byte("single-use\n")); err != nil {
		return errors.New("channel identity has already been consumed")
	}
	request := struct {
		Reference        Reference `json:"reference"`
		Role             string    `json:"role"`
		LocalFingerprint string    `json:"local_fingerprint"`
		PeerFingerprint  string    `json:"peer_fingerprint"`
	}{config.Reference, config.Role, config.LocalFingerprint, config.PeerFingerprint}
	encoded, err := json.Marshal(request)
	if err != nil {
		return errors.New("cannot prepare relay binding")
	}
	requestPath := filepath.Join(config.IdentityDirectory, "relay-request.json")
	if err := exclusiveFile(requestPath, encoded); err != nil {
		return err
	}
	defer os.Remove(requestPath)
	child, err := startRelay(config.CLI, config.StateDirectory, requestPath)
	if err != nil {
		return err
	}
	session := Session{Capture: config.Capture, Hello: config.hello(), TLS: tlsConfig, Deadline: config.Deadline, Lease: config.LeaseExpiresAt}
	if status != nil {
		session.Ready = func(profile CaptureProfile) error {
			value := struct {
				Capture   CaptureProfile `json:"capture"`
				Type      string         `json:"type"`
				Reference Reference      `json:"reference"`
				Role      string         `json:"role"`
				Mode      string         `json:"mode"`
			}{profile, "ready", config.Reference, config.Role, config.Mode}
			data, err := json.Marshal(value)
			if err != nil {
				return err
			}
			if err := status.SetWriteDeadline(minTime(time.Now().Add(time.Second), config.LeaseExpiresAt)); err != nil {
				return err
			}
			err = writeFrame(status, data, 4096)
			status.Close()
			return err
		}
	}
	sessionErr := session.Run(ctx, &chunkConnection{Conn: child.connection}, pipes)
	cleanupErr := child.close()
	if cleanupErr != nil {
		return cleanupErr
	}
	return sessionErr
}

func run(arguments []string, output io.Writer) error {
	if len(arguments) == 0 {
		return errors.New("channel mode required")
	}
	switch arguments[0] {
	case "identity":
		flags := flag.NewFlagSet("identity", flag.ContinueOnError)
		flags.SetOutput(io.Discard)
		directory := flags.String("directory", "", "fresh private identity directory")
		if flags.Parse(arguments[1:]) != nil || flags.NArg() != 0 || !filepath.IsAbs(*directory) {
			return errors.New("invalid identity arguments")
		}
		fingerprint, err := createIdentity(*directory, time.Now())
		if err != nil {
			return err
		}
		return json.NewEncoder(output).Encode(struct {
			Fingerprint string `json:"fingerprint"`
		}{fingerprint})
	case "connect":
		flags := flag.NewFlagSet("connect", flag.ContinueOnError)
		flags.SetOutput(io.Discard)
		config := flags.String("config", "", "private channel configuration file")
		controlFD := flags.Int("control-fd", 3, "private parent authority pipe")
		statusFD := flags.Int("status-fd", 4, "private native readiness pipe")
		if flags.Parse(arguments[1:]) != nil || flags.NArg() != 0 || !filepath.IsAbs(*config) {
			return errors.New("invalid connect arguments")
		}
		ctx, cancel := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
		defer cancel()
		return connect(ctx, *config, *controlFD, *statusFD)
	default:
		return errors.New("unknown channel mode")
	}
}

func main() {
	if err := run(os.Args[1:], os.Stdout); err != nil {
		// No certificate, private values, selected paths, record contents or
		// child stderr are printed. Connect stdout is binary IPC exclusively.
		fmt.Fprintln(os.Stderr, "desktop channel failed or closed")
		os.Exit(1)
	}
}
