package main

import (
	"bytes"
	"encoding/binary"
	"encoding/json"
	"net"
	"strings"
	"testing"
	"time"
)

func clipboardFixture(operation byte, epoch, revision uint64, text []byte) []byte {
	payload := make([]byte, clipboardHeaderBytes+len(text))
	payload[0] = 1
	payload[1] = operation
	binary.BigEndian.PutUint64(payload[2:10], epoch)
	binary.BigEndian.PutUint64(payload[10:18], revision)
	copy(payload[clipboardHeaderBytes:], text)
	return payload
}

func TestClipboardAdmissionEnforcesDirectionAuthorityAndText(t *testing.T) {
	for _, fixture := range []struct {
		operation byte
		role      string
		revision  uint64
		text      []byte
	}{
		{clipboardWatch, "viewer", 0, nil},
		{clipboardPause, "viewer", 0, nil},
		{clipboardWrite, "viewer", 1, []byte("λ🙂\ncommand")},
		{clipboardChanged, "worker", 1, bytes.Repeat([]byte{'x'}, maximumClipboardText)},
		{clipboardAcknowledged, "worker", 1, nil},
		{clipboardNotice, "worker", 0, []byte("too_large")},
	} {
		payload := clipboardFixture(fixture.operation, 1, fixture.revision, fixture.text)
		record := append([]byte{clipboardKind}, payload...)
		if err := admitNative(record, fixture.role, "control", true); err != nil {
			t.Fatalf("valid clipboard operation rejected: %d: %v", fixture.operation, err)
		}
		for _, mode := range []string{"view", "invalid"} {
			if admitNative(record, fixture.role, mode, true) == nil {
				t.Fatal("clipboard admitted without control authority")
			}
		}
		if admitNative(record, fixture.role, "control", false) == nil ||
			admitNative(record, opposite(fixture.role), "control", true) == nil {
			t.Fatal("clipboard admitted without capability or in the wrong direction")
		}
	}
	invalid := [][]byte{
		clipboardFixture(clipboardWrite, 0, 1, nil),
		clipboardFixture(clipboardWrite, 1, 0, nil),
		clipboardFixture(clipboardWrite, 1, 1, []byte{0}),
		clipboardFixture(clipboardWrite, 1, 1, []byte{0xc0, 0xaf}),
		clipboardFixture(clipboardWrite, 1, 1, []byte{0xed, 0xa0, 0x80}),
		clipboardFixture(clipboardWrite, 1, 1, []byte{0xf4, 0x90, 0x80, 0x80}),
		clipboardFixture(clipboardWrite, 1, 1, bytes.Repeat([]byte{'x'}, maximumClipboardText+1)),
		clipboardFixture(clipboardWatch, 1, 0, []byte("unexpected")),
		clipboardFixture(clipboardAcknowledged, 1, 1, []byte("unexpected")),
		clipboardFixture(clipboardNotice, 1, 0, []byte("arbitrary-text")),
		clipboardFixture(255, 1, 0, nil),
		{1, clipboardWrite},
	}
	wrongVersion := clipboardFixture(clipboardWrite, 1, 1, nil)
	wrongVersion[0] = 2
	invalid = append(invalid, wrongVersion)
	for _, payload := range invalid {
		if admitClipboard(payload, "viewer", "control", true) == nil ||
			admitClipboard(payload, "worker", "control", true) == nil {
			t.Fatal("malformed clipboard payload admitted")
		}
	}
}

func TestClipboardCaptureNegotiationIsExplicitAndControlOnly(t *testing.T) {
	for _, encoded := range []string{"false", "null", `"true"`, "1", "{}", "[]"} {
		var profile CaptureProfile
		payload := `{"quality":"high","frame_rate":30,"clipboard_text":` + encoded + `}`
		if json.Unmarshal([]byte(payload), &profile) == nil {
			t.Fatalf("noncanonical capability admitted: %s", encoded)
		}
	}
	profile := CaptureProfile{Quality: "high", FrameRate: 30, ClipboardText: true}
	if profile.validateForMode("view") == nil || profile.validateForMode("control") != nil {
		t.Fatal("clipboard capability mode check failed")
	}
	left, right := net.Pipe()
	left.SetDeadline(time.Now().Add(time.Second))
	right.SetDeadline(time.Now().Add(time.Second))
	defer left.Close()
	defer right.Close()
	result := make(chan error, 1)
	go func() {
		agreed, err := exchangeCapture(left, "worker", nil, "control")
		if err == nil && agreed != profile {
			t.Error("worker clipboard agreement changed")
		}
		result <- err
	}()
	agreed, err := exchangeCapture(right, "viewer", &profile, "control")
	if err != nil || agreed != profile {
		t.Fatalf("viewer clipboard agreement failed: %v", err)
	}
	if err := <-result; err != nil {
		t.Fatal(err)
	}
	profile.ClipboardText = false
	legacy, err := json.Marshal(profile)
	if err != nil || strings.Contains(string(legacy), "clipboard_text") {
		t.Fatal("legacy profile encoding changed")
	}
}

func TestClipboardViewWorkerRejectsBeforeCaptureEcho(t *testing.T) {
	left, right := net.Pipe()
	left.SetDeadline(time.Now().Add(time.Second))
	right.SetDeadline(time.Now().Add(time.Second))
	defer right.Close()
	result := make(chan error, 1)
	go func() {
		_, err := exchangeCapture(left, "worker", nil, "view")
		left.Close()
		result <- err
	}()
	payload := []byte(`{"quality":"high","frame_rate":30,"clipboard_text":true}`)
	if err := writeEnvelope(right, 2, captureKind, payload); err != nil {
		t.Fatal(err)
	}
	if err := <-result; err == nil {
		t.Fatal("view-only worker accepted clipboard capture")
	}
	if _, _, err := readEnvelope(right, 2); err == nil {
		t.Fatal("view-only worker echoed clipboard capability")
	}
}
