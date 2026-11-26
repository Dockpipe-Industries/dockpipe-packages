package main

import (
	"bytes"
	"crypto/sha256"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"math"
	"net"
	"strings"
	"sync"
	"testing"
	"time"
)

func assetBase(operation byte) assetRecord {
	record := assetRecord{op: operation, epoch: 1, serial: 1}
	record.transfer[0] = 1
	return record
}

func assetFilesOffer(size uint64) assetRecord {
	record := assetBase(assetOffer)
	record.format = assetFiles
	record.total = size
	record.items = []assetItem{{id: 1, size: size, name: "fixture.txt"}}
	return record
}

func assetPayload(t *testing.T, record assetRecord) []byte {
	t.Helper()
	payload, err := encodeAssetRecord(record)
	if err != nil {
		t.Fatalf("encode asset fixture: %v", err)
	}
	return payload
}

func assetApply(t *testing.T, session *assetSession, record assetRecord, role string, forwarded bool) {
	t.Helper()
	actual, err := session.admit(assetPayload(t, record), role)
	if err != nil || actual != forwarded {
		t.Fatalf("asset operation %d role %s: forwarded=%v err=%v", record.op, role, actual, err)
	}
}

func assetRejected(t *testing.T, session *assetSession, record assetRecord, role string) {
	t.Helper()
	if _, err := session.admit(assetPayload(t, record), role); err == nil {
		t.Fatalf("asset operation %d role %s unexpectedly accepted", record.op, role)
	}
}

func assetReady(t *testing.T) *assetSession {
	t.Helper()
	session := newAssetSession(CaptureProfile{ClipboardFiles: true, ClipboardImages: true}, time.Now().Add(30*time.Minute))
	assetApply(t, session, assetRecord{op: assetArm, epoch: 1}, "viewer", true)
	assetApply(t, session, assetRecord{op: assetEpochAcknowledged, epoch: 1, control: assetArm}, "worker", true)
	return session
}

func assetBegin(t *testing.T, session *assetSession, offer assetRecord, role string) {
	t.Helper()
	assetApply(t, session, offer, role, true)
	accepted := assetBase(assetAccept)
	accepted.epoch, accepted.serial, accepted.transfer = offer.epoch, offer.serial, offer.transfer
	accepted.grant, accepted.window = offer.total, assetWindow
	assetApply(t, session, accepted, opposite(role), true)
}

func TestAssetCodecAndResourceBounds(t *testing.T) {
	arm := assetRecord{op: assetArm, epoch: 1}
	golden := "010100000000000000000001000000000000000000000000000000000000000000000000"
	if actual := hex.EncodeToString(assetPayload(t, arm)); actual != golden {
		t.Fatalf("Arm wire changed: %s", actual)
	}
	for _, name := range []string{"", ".", "..", "a/b", "a\\b", "a\x00b", "\x1f", "\x7f", "\u0085", "\xff", "\xc0\xaf", "\xed\xa0\x80", strings.Repeat("a", 256)} {
		if validAssetName(name) {
			t.Fatalf("invalid basename accepted")
		}
	}
	if !validAssetName("café-日.txt") {
		t.Fatal("valid Unicode basename rejected")
	}
	png := assetBase(assetOffer)
	png.format, png.width, png.height, png.total = assetPNG, 4096, 4096, assetMaximumPNG
	png.items = []assetItem{{id: 1, size: assetMaximumPNG, name: "clipboard.png"}}
	if len(assetPayload(t, png)) > 1024 {
		t.Fatal("PNG manifest materialized its declared image payload")
	}
	png.width++
	if _, err := encodeAssetRecord(png); err == nil {
		t.Fatal("pixel overflow accepted")
	}
	png.width, png.total, png.items[0].size = 4096, assetMaximumPNG+1, assetMaximumPNG+1
	if _, err := encodeAssetRecord(png); err == nil {
		t.Fatal("PNG encoded limit not enforced")
	}
	offer := assetFilesOffer(assetMaximumBatch)
	for index := 2; index <= assetMaximumFiles; index++ {
		name := fmt.Sprintf("%03d", index) + strings.Repeat("x", 252)
		offer.items = append(offer.items, assetItem{id: uint32(index), name: name})
	}
	payload := assetPayload(t, offer)
	if len(payload)-assetHeaderBytes > assetMaximumManifest {
		t.Fatal("maximum file manifest exceeds budget")
	}
	offer.items[1].name = strings.ToUpper(offer.items[0].name)
	if _, err := encodeAssetRecord(offer); err == nil {
		t.Fatal("ASCII basename collision accepted")
	}
	overflow := assetFilesOffer(math.MaxUint64)
	if _, err := encodeAssetRecord(overflow); err == nil {
		t.Fatal("uint64 size overflow accepted")
	}
	overflow = assetFilesOffer(assetMaximumBatch)
	overflow.items = append(overflow.items, assetItem{id: 2, size: 1, name: "second"})
	overflow.total = 0
	if _, err := encodeAssetRecord(overflow); err == nil {
		t.Fatal("aggregate overflow accepted")
	}
	chunk := assetBase(assetChunk)
	chunk.item, chunk.data = 1, make([]byte, assetMaximumChunk)
	if len(assetPayload(t, chunk)) != assetMaximumRecord {
		t.Fatal("maximum chunk framing mismatch")
	}
	chunk.data = append(chunk.data, 0)
	if _, err := encodeAssetRecord(chunk); err == nil {
		t.Fatal("oversize chunk accepted")
	}
	for _, record := range []assetRecord{arm, assetFilesOffer(3), assetBase(assetCancelAcknowledged), {op: assetEpochAcknowledged, epoch: 1, control: assetPause}} {
		encoded := assetPayload(t, record)
		for length := 0; length < len(encoded); length++ {
			if _, err := decodeAssetRecord(encoded[:length]); err == nil {
				t.Fatalf("truncated fixed/manifest record accepted at %d", length)
			}
		}
		for _, index := range []int{0, 2} {
			mutated := bytes.Clone(encoded)
			mutated[index] = 2
			if _, err := decodeAssetRecord(mutated); err == nil {
				t.Fatal("version/reserved bits accepted")
			}
		}
	}
	malformed := assetPayload(t, assetFilesOffer(1))
	binary.BigEndian.PutUint16(malformed[45:47], math.MaxUint16)
	if _, err := decodeAssetRecord(malformed); err == nil {
		t.Fatal("unbounded manifest count accepted")
	}
}

func TestAssetIndependentCaptureAndAdmission(t *testing.T) {
	for _, field := range []string{"clipboard_images", "clipboard_files"} {
		for _, invalid := range []string{"false", "null", "0", `"true"`} {
			var profile CaptureProfile
			data := `{"quality":"high","frame_rate":30,"` + field + `":` + invalid + `}`
			if json.Unmarshal([]byte(data), &profile) == nil {
				t.Fatalf("noncanonical %s accepted", field)
			}
		}
		var profile CaptureProfile
		if err := json.Unmarshal([]byte(`{"quality":"high","frame_rate":30,"`+field+`":true}`), &profile); err != nil ||
			profile.validateForMode("control") != nil || profile.validateForMode("view") == nil {
			t.Fatal("rich capability did not remain independent/control-only")
		}
	}
	legacy := CaptureProfile{Quality: "high", FrameRate: 30, ClipboardText: true}
	encoded, _ := json.Marshal(legacy)
	if string(encoded) != `{"quality":"high","frame_rate":30,"clipboard_text":true}` {
		t.Fatalf("legacy text capture bytes changed: %s", encoded)
	}
	profile := CaptureProfile{Quality: "ultra", FrameRate: 60, ClipboardImages: true, ClipboardFiles: true}
	left, right := net.Pipe()
	defer left.Close()
	defer right.Close()
	_ = left.SetDeadline(time.Now().Add(time.Second))
	_ = right.SetDeadline(time.Now().Add(time.Second))
	result := make(chan error, 1)
	go func() {
		agreed, err := exchangeCapture(right, "worker", nil, "control")
		if err == nil && agreed != profile {
			err = errAsset
		}
		result <- err
	}()
	if agreed, err := exchangeCapture(left, "viewer", &profile, "control"); err != nil || agreed != profile {
		t.Fatal("exact rich agreement failed", err)
	}
	if err := <-result; err != nil {
		t.Fatal(err)
	}
	arm := assetPayload(t, assetRecord{op: assetArm, epoch: 1})
	if admitAsset(arm, "viewer", "view", profile) == nil || admitAsset(arm, "worker", "control", profile) == nil ||
		admitAsset(arm, "viewer", "control", CaptureProfile{}) == nil {
		t.Fatal("rich authority fence missing")
	}
	file := assetPayload(t, assetFilesOffer(0))
	if admitAsset(file, "viewer", "control", CaptureProfile{ClipboardImages: true}) == nil {
		t.Fatal("files accepted by images-only grant")
	}
}

func TestAssetWindowHashAndSequentialEmptyFiles(t *testing.T) {
	session := assetReady(t)
	offer := assetFilesOffer(5 * assetMaximumChunk)
	assetBegin(t, session, offer, "viewer")
	chunk := assetBase(assetChunk)
	chunk.item, chunk.data = 1, make([]byte, assetMaximumChunk)
	for index := 0; index < assetWindow; index++ {
		chunk.offset = uint64(index * assetMaximumChunk)
		assetApply(t, session, chunk, "viewer", true)
	}
	chunk.offset = 4 * assetMaximumChunk
	assetRejected(t, session, chunk, "viewer")
	credit := assetBase(assetCredit)
	credit.item, credit.offset = 1, 3*assetMaximumChunk+1
	assetRejected(t, session, credit, "worker")
	credit.offset--
	assetRejected(t, session, credit, "viewer")
	assetApply(t, session, credit, "worker", true)
	assetRejected(t, session, credit, "worker")
	assetApply(t, session, chunk, "viewer", true)
	finish := assetBase(assetFinish)
	finish.item, finish.offset = 1, offer.total
	hasher := sha256.New()
	for range 5 {
		_, _ = hasher.Write(chunk.data)
	}
	copy(finish.digest[:], hasher.Sum(nil))
	assetRejected(t, session, finish, "viewer") // Final bytes not credited yet.
	credit.offset = offer.total
	assetApply(t, session, credit, "worker", true)
	finish.digest[0] ^= 1
	assetRejected(t, session, finish, "viewer")
	finish.digest[0] ^= 1
	assetApply(t, session, finish, "viewer", true)
	assetRejected(t, session, assetBase(assetCommitted), "viewer")
	assetApply(t, session, assetBase(assetCommitted), "worker", true)
	assetRejected(t, session, chunk, "viewer")

	session = assetReady(t)
	offer = assetFilesOffer(0)
	offer.items = append(offer.items, assetItem{id: 2, name: "second-empty"})
	assetBegin(t, session, offer, "worker")
	finish = assetBase(assetFinish)
	finish.item, finish.digest = 1, sha256.Sum256(nil)
	assetApply(t, session, finish, "worker", true)
	assetRejected(t, session, assetBase(assetCommitted), "viewer")
	finish.item = 2
	assetRejected(t, session, finish, "worker")
	credit = assetBase(assetCredit)
	credit.item, credit.verified = 1, true
	assetApply(t, session, credit, "viewer", true)
	assetApply(t, session, finish, "worker", true)
	credit.item = 2
	assetRejected(t, session, credit, "viewer")
	assetApply(t, session, assetBase(assetCommitted), "viewer", true)
}

func TestAssetCancellationRacesAndBoundedTerminalReceipts(t *testing.T) {
	session := assetReady(t)
	offer := assetFilesOffer(0)
	assetApply(t, session, offer, "viewer", true)
	cancel := assetBase(assetCancel)
	cancel.reason = "user_cancelled"
	assetApply(t, session, cancel, "viewer", true)
	accepted := assetBase(assetAccept)
	accepted.window = assetWindow
	assetApply(t, session, accepted, "worker", false) // Crossed Accept is drained, not exposed.
	ack := assetBase(assetCancelAcknowledged)
	assetRejected(t, session, ack, "viewer")
	assetApply(t, session, ack, "worker", true)
	for range 8 {
		assetApply(t, session, cancel, "worker", false)
	}
	assetRejected(t, session, cancel, "worker")
	newOffer := assetFilesOffer(0)
	newOffer.serial, newOffer.transfer[0] = 2, 2
	assetApply(t, session, newOffer, "viewer", true)
	unknown := cancel
	unknown.transfer[0] = 99
	assetRejected(t, session, unknown, "worker")

	session = assetReady(t)
	offer = assetFilesOffer(5 * assetMaximumChunk)
	assetBegin(t, session, offer, "viewer")
	assetApply(t, session, cancel, "worker", true)
	chunk := assetBase(assetChunk)
	chunk.item, chunk.data = 1, make([]byte, assetMaximumChunk)
	for index := 0; index < assetWindow; index++ {
		chunk.offset = uint64(index * assetMaximumChunk)
		assetApply(t, session, chunk, "viewer", false)
	}
	chunk.offset = 4 * assetMaximumChunk
	assetRejected(t, session, chunk, "viewer")
	assetRejected(t, session, newOffer, "viewer")  // No origin reuse before terminal ACK.
	assetApply(t, session, cancel, "viewer", true) // Crossed Cancel.
	assetApply(t, session, ack, "worker", true)
	assetRejected(t, session, newOffer, "viewer") // Other request still unacknowledged.
	assetApply(t, session, ack, "viewer", true)
	assetRejected(t, session, chunk, "viewer") // No data is legal after terminal boundary.
	assetApply(t, session, newOffer, "viewer", true)
}

func TestAssetEpochSetupPauseAndCloseRaces(t *testing.T) {
	session := newAssetSession(CaptureProfile{ClipboardFiles: true}, time.Now().Add(time.Hour))
	assetApply(t, session, assetRecord{op: assetArm, epoch: 1}, "viewer", true)
	assetRejected(t, session, assetFilesOffer(0), "viewer")
	assetApply(t, session, assetRecord{op: assetPause, epoch: 1}, "viewer", true)
	assetRejected(t, session, assetRecord{op: assetArm, epoch: 2}, "viewer")
	assetApply(t, session, assetRecord{op: assetEpochAcknowledged, epoch: 1, control: assetArm}, "worker", true)
	crossed := assetFilesOffer(0)
	assetApply(t, session, crossed, "worker", false)
	second := crossed
	second.serial, second.transfer[0] = 2, 2
	assetRejected(t, session, second, "worker")
	assetApply(t, session, assetRecord{op: assetEpochAcknowledged, epoch: 1, control: assetPause}, "worker", true)
	if session.armed {
		t.Fatal("rapid Arm/Pause armed the paused session")
	}
	assetRejected(t, session, assetBase(assetCommitted), "worker")
	assetApply(t, session, assetRecord{op: assetArm, epoch: 2}, "viewer", true)
	assetApply(t, session, assetRecord{op: assetEpochAcknowledged, epoch: 2, control: assetArm}, "worker", true)
	current := assetFilesOffer(0)
	current.epoch, current.transfer[0] = 2, 3
	assetApply(t, session, current, "viewer", true)
	assetRejected(t, session, crossed, "viewer")

	closed := newAssetSession(CaptureProfile{ClipboardFiles: true}, time.Now().Add(time.Hour))
	assetApply(t, closed, assetRecord{op: assetArm, epoch: 1}, "viewer", true)
	closed.close()
	assetRejected(t, closed, assetRecord{op: assetEpochAcknowledged, epoch: 1, control: assetArm}, "worker")
}

func TestAssetCancellationTimeoutCannotRenewRichAuthority(t *testing.T) {
	session := assetReady(t)
	now := time.Now()
	session.now = func() time.Time { return now }
	assetBegin(t, session, assetFilesOffer(0), "viewer")
	cancel := assetBase(assetCancel)
	cancel.reason = "timeout"
	assetApply(t, session, cancel, "viewer", true)
	now = now.Add(11 * time.Second)
	assetApply(t, session, assetBase(assetCancelAcknowledged), "worker", true)
	assetRejected(t, session, assetRecord{op: assetArm, epoch: 2}, "viewer")
	if !session.disabled {
		t.Fatal("timed-out cancellation silently renewed rich sharing")
	}
	// Rich state never gates the existing independent text/input admission.
	if err := admitNative([]byte{mediaControlKind, 1}, "viewer", "view", false); err != nil {
		t.Fatal("rich timeout changed legacy admission", err)
	}
}

func TestAssetConcurrentBidirectionalState(t *testing.T) {
	session := assetReady(t)
	var workers sync.WaitGroup
	for origin, role := range []string{"viewer", "worker"} {
		workers.Add(1)
		go func(origin int, role string) {
			defer workers.Done()
			for serial := uint64(1); serial <= 100; serial++ {
				offer := assetFilesOffer(0)
				offer.serial = serial
				offer.transfer[0] = byte(origin + 1)
				binary.BigEndian.PutUint64(offer.transfer[8:], serial)
				assetBegin(t, session, offer, role)
				finish := offer
				finish.op, finish.item, finish.digest = assetFinish, 1, sha256.Sum256(nil)
				assetApply(t, session, finish, role, true)
				committed := offer
				committed.op = assetCommitted
				assetApply(t, session, committed, opposite(role), true)
			}
		}(origin, role)
	}
	workers.Wait()
	session.close()
}

func FuzzAssetCodec(f *testing.F) {
	for _, record := range []assetRecord{{op: assetArm, epoch: 1}, assetFilesOffer(0), assetBase(assetCancelAcknowledged)} {
		payload, _ := encodeAssetRecord(record)
		f.Add(payload)
	}
	f.Fuzz(func(t *testing.T, payload []byte) {
		record, err := decodeAssetRecord(payload)
		if err != nil {
			return
		}
		encoded, err := encodeAssetRecord(record)
		if err != nil || !bytes.Equal(encoded, payload) {
			t.Fatal("accepted noncanonical asset record")
		}
	})
}

func TestAssetCrossOriginIdentityAndSerialReplay(t *testing.T) {
	session := assetReady(t)
	viewerOffer := assetFilesOffer(0)
	assetApply(t, session, viewerOffer, "viewer", true)
	assetRejected(t, session, viewerOffer, "worker") // Cross-origin ID collision.
	workerOffer := assetFilesOffer(0)
	workerOffer.transfer[0] = 2
	assetApply(t, session, workerOffer, "worker", true)
	assetRejected(t, session, viewerOffer, "viewer") // Replayed active offer.
	cancel := assetBase(assetCancel)
	cancel.reason = "source_changed"
	assetApply(t, session, cancel, "viewer", true)
	assetApply(t, session, assetBase(assetCancelAcknowledged), "worker", true)
	fresh := assetFilesOffer(0)
	fresh.transfer[0], fresh.serial = 3, 3
	assetRejected(t, session, fresh, "viewer") // Serial2 cannot be skipped.
	fresh.serial = 2
	assetApply(t, session, fresh, "viewer", true)
	unknown := assetBase(assetCancel)
	unknown.reason, unknown.transfer[0] = "user_cancelled", 99
	assetRejected(t, session, unknown, "worker")
	unknown = fresh
	unknown.op, unknown.reason, unknown.epoch = assetCancel, "user_cancelled", 2
	assetRejected(t, session, unknown, "worker") // No future epoch authority.
}
