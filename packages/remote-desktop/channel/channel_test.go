package main

import (
	"bytes"
	"context"
	"crypto/tls"
	"encoding/binary"
	"encoding/json"
	"io"
	"net"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"syscall"
	"testing"
	"time"
)

func referenceFixture() Reference {
	return Reference{Schema: "dockpipe.remote.desktop/v1", ID: strings.Repeat("1", 64), Epoch: strings.Repeat("2", 64), Target: Target{Node: "fixture", Fingerprint: strings.Repeat("3", 64), Generation: strings.Repeat("4", 64)}}
}
func identityFixture(t *testing.T) (tls.Certificate, string, string) {
	t.Helper()
	directory := filepath.Join(t.TempDir(), "identity")
	pin, err := createIdentity(directory, time.Now())
	if err != nil {
		t.Fatal(err)
	}
	identity, err := loadIdentity(directory, pin)
	if err != nil {
		t.Fatal(err)
	}
	return identity, pin, directory
}
func configFixture(t *testing.T, identity tls.Certificate, pin string) *tls.Config {
	t.Helper()
	config, err := tlsConfiguration(identity, pin, time.Now)
	if err != nil {
		t.Fatal(err)
	}
	return config
}
func waitResult(t *testing.T, result <-chan error) error {
	t.Helper()
	select {
	case err := <-result:
		return err
	case <-time.After(3 * time.Second):
		t.Fatal("channel failed to stop within fixture deadline")
		return nil
	}
}

type observedConnection struct {
	net.Conn
	lock sync.Mutex
	wire bytes.Buffer
}

func (connection *observedConnection) Write(data []byte) (int, error) {
	connection.lock.Lock()
	connection.wire.Write(data)
	connection.lock.Unlock()
	return connection.Conn.Write(data)
}
func (connection *observedConnection) contains(value []byte) bool {
	connection.lock.Lock()
	defer connection.lock.Unlock()
	return bytes.Contains(connection.wire.Bytes(), value)
}

func TestPinnedTLSBidirectionalEncryptedRecords(t *testing.T) {
	workerIdentity, workerPin, _ := identityFixture(t)
	viewerIdentity, viewerPin, _ := identityFixture(t)
	rawWorker, rawViewer := net.Pipe()
	observed := &observedConnection{Conn: rawWorker}
	nativeWorker, workerPeer := net.Pipe()
	nativeViewer, viewerPeer := net.Pipe()
	workerControl, workerLease := io.Pipe()
	viewerControl, viewerLease := io.Pipe()
	defer workerLease.Close()
	defer viewerLease.Close()
	defer workerPeer.Close()
	defer viewerPeer.Close()
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	reference := referenceFixture()
	now := time.Now()
	worker := Session{Hello: Hello{channelVersion, reference, "worker", "control"}, TLS: configFixture(t, workerIdentity, viewerPin), Deadline: now.Add(time.Minute), Lease: now.Add(5 * time.Second)}
	viewer := Session{Capture: &CaptureProfile{Quality: "high", FrameRate: 30, ClipboardText: true}, Hello: Hello{channelVersion, reference, "viewer", "control"}, TLS: configFixture(t, viewerIdentity, workerPin), Deadline: now.Add(time.Minute), Lease: now.Add(5 * time.Second)}
	workerResult := make(chan error, 1)
	viewerResult := make(chan error, 1)
	go func() {
		workerResult <- worker.Run(ctx, &chunkConnection{Conn: observed}, LocalPipes{nativeWorker, nativeWorker, workerControl})
	}()
	go func() {
		viewerResult <- viewer.Run(ctx, &chunkConnection{Conn: rawViewer}, LocalPipes{nativeViewer, nativeViewer, viewerControl})
	}()
	workerPeer.SetDeadline(now.Add(2 * time.Second))
	viewerPeer.SetDeadline(now.Add(2 * time.Second))
	payload := []byte("fixture-private-video-not-readable-by-relay")
	media := append([]byte{mediaKind}, payload...)
	if err := writeFrame(workerPeer, media, maximumRecord); err != nil {
		t.Fatal(err)
	}
	got, err := readFrame(viewerPeer, maximumRecord)
	if err != nil || !bytes.Equal(got, media) {
		t.Fatalf("media forwarding failed: %v", err)
	}
	input := []byte{inputKind, 0x71, 0x22, 0x01}
	if err := writeFrame(viewerPeer, input, maximumRecord); err != nil {
		t.Fatal(err)
	}
	got, err = readFrame(workerPeer, maximumRecord)
	if err != nil || !bytes.Equal(got, input) {
		t.Fatalf("input forwarding failed: %v", err)
	}
	clipboardText := []byte("fixture-private-clipboard-λ🙂")
	for _, transfer := range []struct {
		sender    net.Conn
		receiver  net.Conn
		operation byte
	}{
		{viewerPeer, workerPeer, clipboardWrite},
		{workerPeer, viewerPeer, clipboardChanged},
		{workerPeer, viewerPeer, clipboardAcknowledged},
	} {
		text := clipboardText
		if transfer.operation == clipboardAcknowledged {
			text = nil
		}
		record := append([]byte{clipboardKind}, clipboardFixture(transfer.operation, 1, 1, text)...)
		if err := writeFrame(transfer.sender, record, maximumRecord); err != nil {
			t.Fatal(err)
		}
		received, err := readFrame(transfer.receiver, maximumRecord)
		if err != nil || !bytes.Equal(received, record) {
			t.Fatalf("encrypted clipboard forwarding failed: %v", err)
		}
	}
	if observed.contains(clipboardText) {
		t.Fatal("relay observed clipboard plaintext")
	}
	if observed.contains(payload) || observed.contains([]byte(reference.ID)) {
		t.Fatal("relay observed plaintext")
	}
	workerLease.Close()
	if waitResult(t, workerResult) == nil {
		t.Fatal("control EOF accepted as successful continued session")
	}
	if waitResult(t, viewerResult) == nil {
		t.Fatal("peer detach accepted as continued session")
	}
}

func TestPinMismatchNoPlaintextFallback(t *testing.T) {
	workerIdentity, _, _ := identityFixture(t)
	viewerIdentity, viewerPin, _ := identityFixture(t)
	left, right := net.Pipe()
	worker := tls.Server(&chunkConnection{Conn: left}, configFixture(t, workerIdentity, viewerPin))
	viewer := tls.Client(&chunkConnection{Conn: right}, configFixture(t, viewerIdentity, strings.Repeat("a", 64)))
	results := make(chan error, 2)
	ctx, cancel := context.WithTimeout(context.Background(), time.Second)
	defer cancel()
	go func() { defer worker.Close(); results <- worker.HandshakeContext(ctx) }()
	go func() { defer viewer.Close(); results <- viewer.HandshakeContext(ctx) }()
	for range 2 {
		if waitResult(t, results) == nil {
			t.Fatal("wrong leaf pin admitted")
		}
	}
}

func TestEncryptedHelloRejectsCrossSessionRoleAndMode(t *testing.T) {
	for _, mutation := range []string{"session", "target", "role", "mode", "legacy"} {
		t.Run(mutation, func(t *testing.T) {
			workerIdentity, workerPin, _ := identityFixture(t)
			viewerIdentity, viewerPin, _ := identityFixture(t)
			left, right := net.Pipe()
			worker := tls.Server(left, configFixture(t, workerIdentity, viewerPin))
			viewer := tls.Client(right, configFixture(t, viewerIdentity, workerPin))
			defer left.Close()
			defer right.Close()
			worker.SetDeadline(time.Now().Add(time.Second))
			viewer.SetDeadline(time.Now().Add(time.Second))
			local := Hello{channelVersion, referenceFixture(), "worker", "control"}
			peer := Hello{channelVersion, referenceFixture(), "viewer", "control"}
			switch mutation {
			case "session":
				peer.Reference.ID = strings.Repeat("5", 64)
			case "target":
				peer.Reference.Target.Generation = strings.Repeat("6", 64)
			case "role":
				peer.Role = "worker"
			case "mode":
				peer.Mode = "view"
			case "legacy":
				peer.Schema = "dockpipe.desktop.channel/v1"
			}
			result := make(chan error, 1)
			go func() { result <- exchangeHello(worker, local); worker.Close() }()
			data, _ := json.Marshal(peer)
			if err := writeEnvelope(viewer, 1, helloKind, data); err != nil {
				t.Fatal(err)
			}
			if waitResult(t, result) == nil {
				t.Fatal("mismatched binding admitted")
			}
		})
	}
}

func TestControlEOFCancelsBlockedHandshake(t *testing.T) {
	identity, pin, _ := identityFixture(t)
	raw, peer := net.Pipe()
	defer peer.Close()
	native, nativePeer := net.Pipe()
	defer nativePeer.Close()
	control, parent := io.Pipe()
	now := time.Now()
	session := Session{Hello: Hello{channelVersion, referenceFixture(), "worker", "view"}, TLS: configFixture(t, identity, pin), Deadline: now.Add(time.Minute), Lease: now.Add(9 * time.Second)}
	result := make(chan error, 1)
	go func() {
		result <- session.Run(context.Background(), &chunkConnection{Conn: raw}, LocalPipes{native, native, control})
	}()
	parent.Close()
	if waitResult(t, result) == nil {
		t.Fatal("authority EOF did not reject handshake")
	}
}

func TestFramePartialTruncationAndLimits(t *testing.T) {
	for _, data := range [][]byte{{}, {0}, {0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 2, 1}, {0, 0, 1, 0}} {
		if _, err := readFrame(bytes.NewReader(data), 64); err == nil {
			t.Fatalf("accepted invalid frame %v", data)
		}
	}
	var buffer bytes.Buffer
	if err := writeFrame(&buffer, []byte{1, 2, 3}, 3); err != nil {
		t.Fatal(err)
	}
	got, err := readFrame(&oneByteReader{reader: &buffer}, 3)
	if err != nil || !bytes.Equal(got, []byte{1, 2, 3}) {
		t.Fatal("partial reads lost data")
	}
	var short shortWriter
	if err := writeFrame(&short, []byte{1, 2, 3}, 3); err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(short.data, []byte{0, 0, 0, 3, 1, 2, 3}) {
		t.Fatal("partial writes lost data")
	}
	if err := writeFrame(&buffer, make([]byte, 65), 64); err == nil {
		t.Fatal("oversize write accepted")
	}
	var header [4]byte
	binary.BigEndian.PutUint32(header[:], uint32(maximumRecord+1))
	if _, err := readFrame(bytes.NewReader(header[:]), maximumRecord); err == nil {
		t.Fatal("oversize accepted before allocation")
	}
}

type oneByteReader struct{ reader io.Reader }

func (reader *oneByteReader) Read(value []byte) (int, error) {
	return reader.reader.Read(value[:min(1, len(value))])
}

type shortWriter struct{ data []byte }

func (writer *shortWriter) Write(value []byte) (int, error) {
	writer.data = append(writer.data, value[0])
	return 1, nil
}

func TestEnvelopeReplayAndNativeAuthority(t *testing.T) {
	var stream bytes.Buffer
	writeEnvelope(&stream, 2, inputKind, []byte{1})
	writeEnvelope(&stream, 2, inputKind, []byte{1})
	if _, _, err := readEnvelope(&stream, 2); err != nil {
		t.Fatal(err)
	}
	if _, _, err := readEnvelope(&stream, 3); err == nil {
		t.Fatal("replay accepted")
	}
	if admitNative([]byte{inputKind, 1}, "viewer", "view", false) == nil {
		t.Fatal("view-only input admitted")
	}
	if admitNative([]byte{mediaKind, 1}, "viewer", "control", false) == nil {
		t.Fatal("viewer media admitted")
	}
	if admitNative([]byte{inputKind, 1}, "worker", "control", false) == nil {
		t.Fatal("worker input admitted")
	}
	if admitNative(append([]byte{inputKind}, make([]byte, 4097)...), "viewer", "control", false) == nil {
		t.Fatal("oversize input admitted")
	}
	if admitNative([]byte{mediaControlKind, 1}, "viewer", "view", false) != nil {
		t.Fatal("view-only keyframe request rejected")
	}
	for _, record := range [][]byte{{mediaControlKind}, {mediaControlKind, 2}, {mediaControlKind, 1, 0}} {
		if admitNative(record, "viewer", "view", false) == nil {
			t.Fatal("invalid media control accepted")
		}
	}
	if admitNative([]byte{mediaControlKind, 1}, "worker", "control", false) == nil {
		t.Fatal("worker media control admitted")
	}
	if admitNative([]byte{heartbeatKind}, "viewer", "view", false) == nil {
		t.Fatal("native injected heartbeat admitted")
	}
}

func TestIdentityPrivateFreshAndSingleUse(t *testing.T) {
	_, pin, directory := identityFixture(t)
	if _, err := createIdentity(directory, time.Now()); err == nil {
		t.Fatal("existing identity reused")
	}
	if err := exclusiveFile(filepath.Join(directory, "consumed"), []byte("used")); err != nil {
		t.Fatal(err)
	}
	if err := exclusiveFile(filepath.Join(directory, "consumed"), []byte("used")); err == nil {
		t.Fatal("identity replay admitted")
	}
	if _, err := loadIdentity(directory, strings.Repeat("a", 64)); err == nil {
		t.Fatal("wrong own pin accepted")
	}
	os.Chmod(filepath.Join(directory, "key.pem"), 0644)
	if _, err := loadIdentity(directory, pin); err == nil {
		t.Fatal("public private-key file accepted")
	}
}

func TestLeaseCannotResurrectOrOutliveAuthority(t *testing.T) {
	now := time.Now()
	if validLease(now.Add(11*time.Second), now.Add(time.Hour), now) {
		t.Fatal("lease beyond ten seconds accepted")
	}
	if validLease(now.Add(time.Second), now, now) {
		t.Fatal("lease after deadline accepted")
	}
	ctx, cancel := context.WithCancel(context.Background())
	reader, writer := io.Pipe()
	guard := watchAuthority(ctx, cancel, reader, now.Add(100*time.Millisecond), now.Add(time.Minute))
	payload, _ := json.Marshal(map[string]any{"type": "lease", "expires_at": now.Add(50 * time.Millisecond)})
	writeFrame(writer, payload, 4096)
	select {
	case <-ctx.Done():
	case <-time.After(time.Second):
		t.Fatal("shortened lease did not revoke")
	}
	reader.Close()
	writer.Close()
	guard.Wait()
}

func TestOuterChunkSegmentationAndTruncation(t *testing.T) {
	left, right := net.Pipe()
	defer left.Close()
	defer right.Close()
	stream := &chunkConnection{Conn: left}
	data := bytes.Repeat([]byte{0x19}, maximumChunk*2+17)
	result := make(chan error, 1)
	go func() { _, err := stream.Write(data); result <- err }()
	var recovered []byte
	for _, expected := range []int{maximumChunk, maximumChunk, 17} {
		chunk, err := readFrame(right, maximumChunk)
		if err != nil || len(chunk) != expected {
			t.Fatalf("outer chunk contract: %v", err)
		}
		recovered = append(recovered, chunk...)
	}
	if err := waitResult(t, result); err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(data, recovered) {
		t.Fatal("TLS byte stream changed across chunks")
	}
	go func() { right.Write([]byte{0, 0, 0, 2, 1}); right.Close() }()
	if _, err := stream.Read(make([]byte, 1)); err == nil {
		t.Fatal("truncated outer chunk accepted")
	}
}

func TestPlaintextCannotBecomeNativeOutput(t *testing.T) {
	identity, pin, _ := identityFixture(t)
	raw, peer := net.Pipe()
	native, nativePeer := net.Pipe()
	control, parent := io.Pipe()
	defer parent.Close()
	defer nativePeer.Close()
	defer peer.Close()
	now := time.Now()
	session := Session{Hello: Hello{channelVersion, referenceFixture(), "worker", "control"}, TLS: configFixture(t, identity, pin), Deadline: now.Add(time.Minute), Lease: now.Add(time.Second)}
	result := make(chan error, 1)
	go func() {
		result <- session.Run(context.Background(), &chunkConnection{Conn: raw}, LocalPipes{native, native, control})
	}()
	peer.SetDeadline(time.Now().Add(time.Second))
	// A framed native input record is not a TLS ClientHello and must never be
	// retried through an unencrypted compatibility path.
	writeFrame(peer, []byte{inputKind, 1, 2, 3, 4, 5, 6}, maximumChunk)
	peer.Close()
	if waitResult(t, result) == nil {
		t.Fatal("plaintext admitted")
	}
	nativePeer.SetReadDeadline(time.Now().Add(time.Second))
	var data [1]byte
	if count, _ := nativePeer.Read(data[:]); count != 0 {
		t.Fatal("plaintext escaped to native endpoint")
	}
}

func TestLeaseExpiryAndRenewal(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	reader, writer := io.Pipe()
	defer reader.Close()
	defer writer.Close()
	now := time.Now()
	guard := watchAuthority(ctx, cancel, reader, now.Add(100*time.Millisecond), now.Add(time.Minute))
	update, _ := json.Marshal(map[string]any{"type": "lease", "expires_at": now.Add(300 * time.Millisecond)})
	if err := writeFrame(writer, update, 4096); err != nil {
		t.Fatal(err)
	}
	select {
	case <-ctx.Done():
		t.Fatal("renewed lease expired before extension")
	case <-time.After(150 * time.Millisecond):
	}
	select {
	case <-ctx.Done():
	case <-time.After(time.Second):
		t.Fatal("lease never expired")
	}
	reader.Close()
	guard.Wait()
}

func TestRelayChildTerminatedAndReaped(t *testing.T) {
	script := filepath.Join(t.TempDir(), "relay-fixture")
	if err := os.WriteFile(script, []byte("#!/bin/sh\ntrap '' TERM\nwhile :; do :; done\n"), 0700); err != nil {
		t.Fatal(err)
	}
	child, err := startRelay(script, t.TempDir(), filepath.Join(t.TempDir(), "request"))
	if err != nil {
		t.Fatal(err)
	}
	// Ensure SIGTERM-ignore is installed, proving escalation/reaping rather
	// than relying on fixture startup scheduling.
	time.Sleep(30 * time.Millisecond)
	if err := child.close(); err != nil {
		t.Fatal(err)
	}
	if child.command.ProcessState == nil {
		t.Fatal("child was not reaped")
	}
	if err := child.command.Process.Signal(os.Signal(syscall.Signal(0))); err == nil {
		t.Fatal("child remains alive")
	}
	if err := child.close(); err != nil {
		t.Fatal("cleanup is not idempotent")
	}
}

func TestConnectBurnsIdentityBeforeFailedAttach(t *testing.T) {
	_, pin, directory := identityFixture(t)
	_, peerPin, _ := identityFixture(t)
	now := time.Now()
	config := Config{Capture: &CaptureProfile{Quality: "high", FrameRate: 30}, Schema: channelVersion, Reference: referenceFixture(), Role: "viewer", Mode: "view", LocalFingerprint: pin, PeerFingerprint: peerPin, IdentityDirectory: directory, CLI: "/usr/bin/true", StateDirectory: t.TempDir(), Deadline: now.Add(time.Minute), LeaseExpiresAt: now.Add(5 * time.Second)}
	data, _ := json.Marshal(config)
	configPath := filepath.Join(directory, "connect.json")
	if err := exclusiveFile(configPath, data); err != nil {
		t.Fatal(err)
	}
	for attempt := 0; attempt < 2; attempt++ {
		reader, writer, err := os.Pipe()
		if err != nil {
			t.Fatal(err)
		}
		native, nativePeer := net.Pipe()
		err = connectWithPipes(context.Background(), configPath, int(reader.Fd()), LocalPipes{Input: native, Output: native}, nil)
		nativePeer.Close()
		reader.Close()
		writer.Close()
		if err == nil {
			t.Fatal("dead fixture relay accepted")
		}
		if attempt == 1 && !strings.Contains(err.Error(), "already been consumed") {
			t.Fatalf("failed attach was replayed: %v", err)
		}
	}
}
