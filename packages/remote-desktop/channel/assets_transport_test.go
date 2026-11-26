package main

import (
	"bytes"
	"context"
	"io"
	"net"
	"sync"
	"sync/atomic"
	"testing"
	"time"
)

type assetBlockedOutput struct {
	net.Conn
	enabled      atomic.Bool
	entered      chan struct{}
	released     chan struct{}
	enteredOnce  sync.Once
	releasedOnce sync.Once
}

func (output *assetBlockedOutput) Write(data []byte) (int, error) {
	if output.enabled.Load() {
		output.enteredOnce.Do(func() {
			close(output.entered)
			<-output.released
		})
	}
	return output.Conn.Write(data)
}

func (output *assetBlockedOutput) release() {
	output.releasedOnce.Do(func() { close(output.released) })
}

func (output *assetBlockedOutput) Close() error {
	output.release()
	return output.Conn.Close()
}

func TestAssetActualEncryptedBlockedWriterTerminalBoundaries(t *testing.T) {
	for _, epochBoundary := range []bool{false, true} {
		name := "cancel-ack"
		if epochBoundary {
			name = "pause-epoch-ack"
		}
		t.Run(name, func(t *testing.T) {
			workerIdentity, workerPin, _ := identityFixture(t)
			viewerIdentity, viewerPin, _ := identityFixture(t)
			rawWorker, rawViewer := net.Pipe()
			observed := &observedConnection{Conn: rawWorker}
			nativeWorker, workerPeer := net.Pipe()
			nativeViewer, viewerPeer := net.Pipe()
			workerControl, workerLease := io.Pipe()
			viewerControl, viewerLease := io.Pipe()
			blocked := &assetBlockedOutput{Conn: nativeViewer, entered: make(chan struct{}), released: make(chan struct{})}
			now := time.Now()
			profile := CaptureProfile{Quality: "high", FrameRate: 30, ClipboardFiles: true, ClipboardImages: true}
			worker := Session{Hello: Hello{channelVersion, referenceFixture(), "worker", "control"},
				TLS: configFixture(t, workerIdentity, viewerPin), Deadline: now.Add(time.Minute), Lease: now.Add(9 * time.Second)}
			viewer := Session{Capture: &profile, Hello: Hello{channelVersion, referenceFixture(), "viewer", "control"},
				TLS: configFixture(t, viewerIdentity, workerPin), Deadline: now.Add(time.Minute), Lease: now.Add(9 * time.Second)}
			ctx, cancel := context.WithCancel(context.Background())
			workerResult, viewerResult := make(chan error, 1), make(chan error, 1)
			go func() {
				workerResult <- worker.Run(ctx, &chunkConnection{Conn: observed}, LocalPipes{nativeWorker, nativeWorker, workerControl})
			}()
			go func() {
				viewerResult <- viewer.Run(ctx, &chunkConnection{Conn: rawViewer}, LocalPipes{nativeViewer, blocked, viewerControl})
			}()
			t.Cleanup(func() {
				cancel()
				blocked.release()
				workerPeer.Close()
				viewerPeer.Close()
				workerLease.Close()
				viewerLease.Close()
				_ = waitResult(t, workerResult)
				_ = waitResult(t, viewerResult)
			})
			_ = workerPeer.SetDeadline(time.Now().Add(5 * time.Second))
			_ = viewerPeer.SetDeadline(time.Now().Add(5 * time.Second))
			send := func(peer net.Conn, record assetRecord) {
				t.Helper()
				if err := writeFrame(peer, append([]byte{assetKind}, assetPayload(t, record)...), maximumRecord); err != nil {
					t.Fatal("native asset fixture write failed", err)
				}
			}
			receive := func(peer net.Conn, expected assetRecord) {
				t.Helper()
				actual, err := readFrame(peer, maximumRecord)
				wanted := append([]byte{assetKind}, assetPayload(t, expected)...)
				if err != nil || !bytes.Equal(actual, wanted) {
					t.Fatalf("terminal ordering mismatch operation%d: %v", expected.op, err)
				}
			}
			arm := assetRecord{op: assetArm, epoch: 1}
			send(viewerPeer, arm)
			receive(workerPeer, arm)
			ackEpoch := assetRecord{op: assetEpochAcknowledged, epoch: 1, control: assetArm}
			send(workerPeer, ackEpoch)
			receive(viewerPeer, ackEpoch)
			offer := assetFilesOffer(2 * assetMaximumChunk)
			send(workerPeer, offer)
			receive(viewerPeer, offer)
			accepted := assetBase(assetAccept)
			accepted.grant, accepted.window = offer.total, assetWindow
			send(viewerPeer, accepted)
			receive(workerPeer, accepted)

			blocked.enabled.Store(true)
			chunk := assetBase(assetChunk)
			chunk.item, chunk.data = 1, bytes.Repeat([]byte{0xa7}, assetMaximumChunk)
			send(workerPeer, chunk)
			select {
			case <-blocked.entered:
			case <-time.After(time.Second):
				t.Fatal("native receiver did not reach the deterministic blocked write")
			}
			second := chunk
			second.offset = assetMaximumChunk
			send(workerPeer, second) // Already admitted while first write is blocked.
			terminal := assetBase(assetCancel)
			terminal.reason = "user_cancelled"
			ack := assetBase(assetCancelAcknowledged)
			if epochBoundary {
				terminal = assetRecord{op: assetPause, epoch: 1}
				ack = assetRecord{op: assetEpochAcknowledged, epoch: 1, control: assetPause}
			}
			send(viewerPeer, terminal)
			receive(workerPeer, terminal) // Opposite-direction control remains responsive.
			send(workerPeer, ack)         // FIFO behind the already admitted second chunk.
			blocked.release()
			receive(viewerPeer, chunk) // Already-writing record, still before terminal ACK.
			receive(viewerPeer, ack)   // Second old chunk drained; never forwarded to native.
			fresh := assetFilesOffer(0)
			fresh.transfer[0] = 2
			if epochBoundary {
				arm.epoch, ackEpoch.epoch = 2, 2
				send(viewerPeer, arm)
				receive(workerPeer, arm)
				send(workerPeer, ackEpoch)
				receive(viewerPeer, ackEpoch)
				fresh.epoch = 2
			} else {
				fresh.serial = 2
			}
			send(workerPeer, fresh)
			receive(viewerPeer, fresh)
			if observed.contains(bytes.Repeat([]byte{0xa7}, 64)) || observed.contains([]byte("fixture.txt")) {
				t.Fatal("opaque transport exposed rich clipboard contents")
			}
		})
	}
}
