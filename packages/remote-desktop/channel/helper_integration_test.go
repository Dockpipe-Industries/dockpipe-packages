package main

import (
	"bytes"
	"encoding/json"
	"io"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"syscall"
	"testing"
	"time"
)

// Only the test executable handles this fake CLI mode. The helper process is
// the actual production binary built below, using real inherited descriptors
// and its normal identity, config, relay, TLS, HELLO and readiness code paths.
func TestMain(tests *testing.M) {
	if os.Getenv("DOCKPIPE_TEST_OPAQUE_RELAY") != "" && len(os.Args) > 2 && os.Args[1] == "remote" && os.Args[2] == "desktop-relay" {
		connection, err := net.DialTimeout("unix", os.Getenv("DOCKPIPE_TEST_OPAQUE_RELAY"), 2*time.Second)
		if err != nil {
			os.Exit(2)
		}
		go func() {
			io.Copy(connection, os.Stdin)
			connection.Close()
		}()
		_, err = io.Copy(os.Stdout, connection)
		connection.Close()
		if err != nil {
			os.Exit(3)
		}
		os.Exit(0)
	}
	os.Exit(tests.Run())
}

type actualHelper struct {
	command *exec.Cmd
	input   *os.File
	output  *os.File
	control *os.File
	status  *os.File
	wait    chan error
}

func startActualHelper(t *testing.T, binary, config string, socketControl bool) *actualHelper {
	t.Helper()
	inputRead, inputWrite, err := os.Pipe()
	if err != nil {
		t.Fatal(err)
	}
	outputRead, outputWrite, err := os.Pipe()
	if err != nil {
		t.Fatal(err)
	}
	statusRead, statusWrite, err := os.Pipe()
	if err != nil {
		t.Fatal(err)
	}
	var controlRead, controlWrite *os.File
	if socketControl {
		descriptor, peer := socketPairFixture(t, syscall.SOCK_STREAM)
		// Own a duplicate here; the fixture retains its original for cleanup.
		duplicate, err := syscall.Dup(descriptor)
		if err != nil {
			t.Fatal(err)
		}
		controlRead = os.NewFile(uintptr(duplicate), "helper-control-child")
		controlWrite = peer
	} else {
		controlRead, controlWrite, err = os.Pipe()
		if err != nil {
			t.Fatal(err)
		}
	}
	command := exec.Command(binary, "connect", "--config", config, "--control-fd", "3", "--status-fd", "4")
	command.Stdin = inputRead
	command.Stdout = outputWrite
	command.Stderr = io.Discard
	command.ExtraFiles = []*os.File{controlRead, statusWrite}
	if err := command.Start(); err != nil {
		t.Fatal(err)
	}
	inputRead.Close()
	outputWrite.Close()
	controlRead.Close()
	statusWrite.Close()
	helper := &actualHelper{command: command, input: inputWrite, output: outputRead, control: controlWrite, status: statusRead, wait: make(chan error, 1)}
	go func() { helper.wait <- command.Wait() }()
	t.Cleanup(func() {
		helper.control.Close()
		helper.input.Close()
		helper.output.Close()
		helper.status.Close()
		select {
		case <-helper.wait:
		case <-time.After(3 * time.Second):
			command.Process.Kill()
			<-helper.wait
		}
	})
	return helper
}

func TestActualHelperUnixControlAndPipePeerEndToEnd(t *testing.T) {
	binary := filepath.Join(t.TempDir(), "channel")
	build := exec.Command(filepath.Join(runtime.GOROOT(), "bin/go"), "build", "-buildvcs=false", "-trimpath", "-o", binary, ".")
	build.Env = append(os.Environ(), "CGO_ENABLED=0", "GOTOOLCHAIN=local")
	if output, err := build.CombinedOutput(); err != nil {
		t.Fatalf("production helper build: %v %s", err, output)
	}
	relayDirectory, err := os.MkdirTemp("/tmp", "dp-channel-relay-")
	if err != nil {
		t.Fatal(err)
	}
	defer os.RemoveAll(relayDirectory)
	relayPath := filepath.Join(relayDirectory, "relay.sock")
	listener, err := net.Listen("unix", relayPath)
	if err != nil {
		t.Fatal(err)
	}
	defer listener.Close()
	t.Setenv("DOCKPIPE_TEST_OPAQUE_RELAY", relayPath)
	relayDone := make(chan struct{})
	go func() {
		defer close(relayDone)
		first, err := listener.Accept()
		if err != nil {
			return
		}
		defer first.Close()
		second, err := listener.Accept()
		if err != nil {
			return
		}
		defer second.Close()
		copied := make(chan struct{})
		go func() { io.Copy(first, second); first.Close(); close(copied) }()
		io.Copy(second, first)
		second.Close()
		<-copied
	}()
	_, workerPin, workerDirectory := identityFixture(t)
	_, viewerPin, viewerDirectory := identityFixture(t)
	cli, err := os.Executable()
	if err != nil {
		t.Fatal(err)
	}
	now := time.Now()
	createConfig := func(directory, role, localPin, peerPin string) string {
		config := Config{Schema: channelVersion, Reference: referenceFixture(), Role: role, Mode: "control", LocalFingerprint: localPin, PeerFingerprint: peerPin, IdentityDirectory: directory, CLI: cli, StateDirectory: relayDirectory, Deadline: now.Add(time.Minute), LeaseExpiresAt: now.Add(9 * time.Second)}
		if role == "viewer" {
			config.Capture = &CaptureProfile{Quality: "balanced", FrameRate: 15}
		}
		data, _ := json.Marshal(config)
		path := filepath.Join(directory, "connect.json")
		if err := exclusiveFile(path, data); err != nil {
			t.Fatal(err)
		}
		return path
	}
	worker := startActualHelper(t, binary, createConfig(workerDirectory, "worker", workerPin, viewerPin), false)
	viewer := startActualHelper(t, binary, createConfig(viewerDirectory, "viewer", viewerPin, workerPin), true)
	for _, helper := range []*actualHelper{worker, viewer} {
		helper.status.SetReadDeadline(time.Now().Add(5 * time.Second))
		data, err := readFrame(helper.status, 4096)
		if err != nil {
			t.Fatalf("actual pinned helper readiness failed: %v", err)
		}
		var ready struct {
			Capture   CaptureProfile `json:"capture"`
			Type      string         `json:"type"`
			Reference Reference      `json:"reference"`
			Role      string         `json:"role"`
			Mode      string         `json:"mode"`
		}
		if decodeJSON(data, &ready) != nil || ready.Type != "ready" || ready.Reference != referenceFixture() || ready.Mode != "control" || ready.Capture != (CaptureProfile{Quality: "balanced", FrameRate: 15}) {
			t.Fatal("actual HELLO readiness mismatch")
		}
		var extra [1]byte
		if count, err := helper.status.Read(extra[:]); count != 0 || err != io.EOF {
			t.Fatal("readiness pipe did not close after exact record")
		}
	}
	media := append([]byte{mediaKind}, []byte("original synthetic encrypted fixture")...)
	viewer.output.SetReadDeadline(time.Now().Add(time.Second))
	if err := writeFrame(worker.input, media, maximumRecord); err != nil {
		t.Fatal(err)
	}
	got, err := readFrame(viewer.output, maximumRecord)
	if err != nil || !bytes.Equal(got, media) {
		t.Fatalf("actual helper media: %v", err)
	}
	input := []byte{inputKind, 0x41}
	worker.output.SetReadDeadline(time.Now().Add(time.Second))
	if err := writeFrame(viewer.input, input, maximumRecord); err != nil {
		t.Fatal(err)
	}
	got, err = readFrame(worker.output, maximumRecord)
	if err != nil || !bytes.Equal(got, input) {
		t.Fatalf("actual helper input: %v", err)
	}
	viewer.control.Close()
	for _, helper := range []*actualHelper{viewer, worker} {
		select {
		case err := <-helper.wait:
			if err == nil {
				t.Fatal("authority close did not end actual helper")
			}
			helper.wait <- err // Leave deterministic cleanup ownership to t.Cleanup.
		case <-time.After(3 * time.Second):
			t.Fatal("actual helper/relay did not stop after socket EOF")
		}
	}
	select {
	case <-relayDone:
	case <-time.After(time.Second):
		t.Fatal("opaque relay fixture did not join")
	}
}
