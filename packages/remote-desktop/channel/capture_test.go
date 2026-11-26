package main

import (
	"encoding/json"
	"net"
	"testing"
	"time"
)

func TestCaptureAgreementPreservesEverySupportedProfile(t *testing.T) {
	for _, displaySource := range []string{"", "virtual"} {
		for _, quality := range []string{"low", "balanced", "high", "ultra"} {
			for _, rate := range []int{10, 15, 24, 30, 60} {
				profile := CaptureProfile{Quality: quality, FrameRate: rate, DisplaySource: displaySource}
				left, right := net.Pipe()
				left.SetDeadline(time.Now().Add(time.Second))
				right.SetDeadline(time.Now().Add(time.Second))
				result := make(chan error, 1)
				go func() {
					agreed, err := exchangeCapture(left, "worker", nil, "control")
					if err == nil && agreed != profile {
						t.Errorf("worker profile changed: %+v", agreed)
					}
					left.Close()
					result <- err
				}()
				agreed, err := exchangeCapture(right, "viewer", &profile, "control")
				right.Close()
				if err != nil || agreed != profile {
					t.Fatalf("viewer profile changed: %+v %v", agreed, err)
				}
				if err := <-result; err != nil {
					t.Fatal(err)
				}
			}
		}
	}
}

func TestCaptureAgreementRejectsMalformedRequestsBeforeReadiness(t *testing.T) {
	cases := []string{
		`{}`, `{"quality":"high"}`, `{"quality":"high","frame_rate":0}`,
		`{"quality":"cinema","frame_rate":30}`, `{"quality":"high","frame_rate":120}`,
		`{"quality":"high","frame_rate":15.5}`, `{"quality":"high","frame_rate":true}`,
		`{"quality":"high","frame_rate":30,"width":3840}`, `null`,
	}
	for _, payload := range cases {
		left, right := net.Pipe()
		left.SetDeadline(time.Now().Add(time.Second))
		right.SetDeadline(time.Now().Add(time.Second))
		result := make(chan error, 1)
		go func() {
			_, err := exchangeCapture(left, "worker", nil, "control")
			left.Close()
			result <- err
		}()
		if err := writeEnvelope(right, 2, captureKind, []byte(payload)); err != nil {
			t.Fatal(err)
		}
		if err := <-result; err == nil {
			t.Fatalf("accepted malformed capture: %s", payload)
		}
		right.Close()
	}
}

func TestCaptureAgreementRejectsChangedEchoAndConfirmation(t *testing.T) {
	for _, role := range []string{"viewer", "worker"} {
		t.Run(role, func(t *testing.T) {
			left, right := net.Pipe()
			defer left.Close()
			defer right.Close()
			left.SetDeadline(time.Now().Add(time.Second))
			right.SetDeadline(time.Now().Add(time.Second))
			original := CaptureProfile{Quality: "low", FrameRate: 10}
			changed, _ := json.Marshal(CaptureProfile{Quality: "high", FrameRate: 30})
			result := make(chan error, 1)
			go func() {
				var requested *CaptureProfile
				if role == "viewer" {
					requested = &original
				}
				_, err := exchangeCapture(left, role, requested, "control")
				left.Close()
				result <- err
			}()
			if role == "viewer" {
				if _, _, err := readEnvelopeBounded(right, 2, maximumJSON); err != nil {
					t.Fatal(err)
				}
				if err := writeEnvelope(right, 2, captureKind, changed); err != nil {
					t.Fatal(err)
				}
			} else {
				data, _ := json.Marshal(original)
				if err := writeEnvelope(right, 2, captureKind, data); err != nil {
					t.Fatal(err)
				}
				if _, _, err := readEnvelopeBounded(right, 2, maximumJSON); err != nil {
					t.Fatal(err)
				}
				if err := writeEnvelope(right, 3, captureKind, changed); err != nil {
					t.Fatal(err)
				}
			}
			if err := <-result; err == nil {
				t.Fatal("changed capture agreement accepted")
			}
		})
	}
}

func TestCaptureAgreementDoesNotCompleteWithoutFinalConfirmation(t *testing.T) {
	left, right := net.Pipe()
	defer left.Close()
	defer right.Close()
	left.SetDeadline(time.Now().Add(time.Second))
	right.SetDeadline(time.Now().Add(time.Second))
	result := make(chan error, 1)
	go func() {
		_, err := exchangeCapture(left, "worker", nil, "control")
		result <- err
	}()
	data, _ := json.Marshal(CaptureProfile{Quality: "high", FrameRate: 30})
	if err := writeEnvelope(right, 2, captureKind, data); err != nil {
		t.Fatal(err)
	}
	if _, _, err := readEnvelopeBounded(right, 2, maximumJSON); err != nil {
		t.Fatal(err)
	}
	right.Close()
	if err := <-result; err == nil {
		t.Fatal("readiness allowed without confirmation")
	}
}

func TestVirtualDisplayProfilePreservesCanonicalExistingShape(t *testing.T) {
	existing := CaptureProfile{Quality: "high", FrameRate: 30}
	encoded, err := json.Marshal(existing)
	if err != nil || string(encoded) != `{"quality":"high","frame_rate":30}` {
		t.Fatalf("default profile changed: %s %v", encoded, err)
	}
	virtual := CaptureProfile{Quality: "high", FrameRate: 30, DisplaySource: "virtual"}
	encoded, err = json.Marshal(virtual)
	if err != nil || string(encoded) != `{"quality":"high","frame_rate":30,"display_source":"virtual"}` {
		t.Fatalf("virtual profile changed: %s %v", encoded, err)
	}
	var decoded CaptureProfile
	if decodeJSON(encoded, &decoded) != nil || decoded != virtual {
		t.Fatalf("virtual profile not preserved: %+v", decoded)
	}
	for _, invalid := range []string{
		`{"quality":"high","frame_rate":30,"display_source":null}`,
		`{"quality":"high","frame_rate":30,"display_source":false}`,
		`{"quality":"high","frame_rate":30,"display_source":""}`,
		`{"quality":"high","frame_rate":30,"display_source":"existing"}`,
		`{"quality":"high","frame_rate":30,"display_source":"mirror"}`,
		`{"quality":"high","frame_rate":30,"display_source":{}}`,
		`{"quality":"high","frame_rate":30,"display_source":"virtual","width":2560}`,
	} {
		if decodeJSON([]byte(invalid), &decoded) == nil {
			t.Fatalf("invalid display source accepted: %s", invalid)
		}
	}
}

func TestCaptureAgreementRejectsDisplaySourceChange(t *testing.T) {
	for _, role := range []string{"viewer", "worker"} {
		t.Run(role, func(t *testing.T) {
			left, right := net.Pipe()
			defer left.Close()
			defer right.Close()
			left.SetDeadline(time.Now().Add(time.Second))
			right.SetDeadline(time.Now().Add(time.Second))
			original := CaptureProfile{Quality: "high", FrameRate: 30, DisplaySource: "virtual"}
			changed, _ := json.Marshal(CaptureProfile{Quality: "high", FrameRate: 30})
			result := make(chan error, 1)
			go func() {
				var requested *CaptureProfile
				if role == "viewer" {
					requested = &original
				}
				_, err := exchangeCapture(left, role, requested, "control")
				left.Close()
				result <- err
			}()
			if role == "viewer" {
				if _, _, err := readEnvelopeBounded(right, 2, maximumJSON); err != nil {
					t.Fatal(err)
				}
				if err := writeEnvelope(right, 2, captureKind, changed); err != nil {
					t.Fatal(err)
				}
			} else {
				data, _ := json.Marshal(original)
				if err := writeEnvelope(right, 2, captureKind, data); err != nil {
					t.Fatal(err)
				}
				if _, _, err := readEnvelopeBounded(right, 2, maximumJSON); err != nil {
					t.Fatal(err)
				}
				if err := writeEnvelope(right, 3, captureKind, changed); err != nil {
					t.Fatal(err)
				}
			}
			if err := <-result; err == nil {
				t.Fatal("display source changed before readiness")
			}
		})
	}
}

func TestLegacyCaptureShapeRejectsVirtualSelection(t *testing.T) {
	// Reproduce the previous strict two-field schema; this is a compatibility
	// contract fixture, not execution of an old external helper artifact.
	var legacy struct {
		Quality   string `json:"quality"`
		FrameRate int    `json:"frame_rate"`
	}
	existing, _ := json.Marshal(CaptureProfile{Quality: "high", FrameRate: 30})
	if decodeJSON(existing, &legacy) != nil {
		t.Fatal("existing-display shape no longer accepted by the old schema")
	}
	virtual, _ := json.Marshal(CaptureProfile{Quality: "high", FrameRate: 30, DisplaySource: "virtual"})
	if decodeJSON(virtual, &legacy) == nil {
		t.Fatal("old schema silently ignored virtual display selection")
	}
}
