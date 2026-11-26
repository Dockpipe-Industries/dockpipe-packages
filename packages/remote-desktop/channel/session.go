package main

import (
	"context"
	"crypto/tls"
	"encoding/json"
	"errors"
	"io"
	"net"
	"sync"
	"time"
)

type LocalPipes struct {
	Input   io.ReadCloser
	Output  io.WriteCloser
	Control io.ReadCloser
}

func (pipes LocalPipes) close() {
	pipes.Input.Close()
	pipes.Output.Close()
	if pipes.Control != nil {
		pipes.Control.Close()
	}
}

type Session struct {
	Capture  *CaptureProfile
	Hello    Hello
	TLS      *tls.Config
	Deadline time.Time
	Lease    time.Time
	Ready    func(CaptureProfile) error
}

func exchangeHello(connection *tls.Conn, local Hello) error {
	data, err := json.Marshal(local)
	if err != nil || local.validate() != nil {
		return errors.New("invalid local desktop binding")
	}
	receive := func() error {
		kind, payload, err := readEnvelopeBounded(connection, 1, maximumJSON)
		if err != nil || kind != helloKind || len(payload) > maximumJSON {
			return errors.New("missing encrypted desktop binding")
		}
		var peer Hello
		if decodeJSON(payload, &peer) != nil || peer.validate() != nil || peer.Reference != local.Reference || peer.Role != opposite(local.Role) || peer.Mode != local.Mode {
			return errors.New("encrypted desktop binding mismatch")
		}
		return nil
	}
	send := func() error { return writeEnvelope(connection, 1, helloKind, data) }
	if local.Role == "viewer" {
		if err := send(); err != nil {
			return err
		}
		return receive()
	}
	if err := receive(); err != nil {
		return err
	}
	return send()
}

func (session Session) Run(parent context.Context, raw net.Conn, pipes LocalPipes) error {
	now := time.Now()
	captureValid := session.Hello.Role == "viewer" && session.Capture != nil && session.Capture.validateForMode(session.Hello.Mode) == nil || session.Hello.Role == "worker" && session.Capture == nil
	if !captureValid || session.Hello.validate() != nil || session.TLS == nil || !validLease(session.Lease, session.Deadline, now) || session.Deadline.After(now.Add(time.Hour)) {
		raw.Close()
		pipes.close()
		return errors.New("invalid channel authority")
	}
	ctx, cancel := context.WithDeadline(parent, session.Deadline)
	defer cancel()
	defer raw.Close()
	defer pipes.close()
	var connection *tls.Conn
	if session.Hello.Role == "worker" {
		connection = tls.Server(raw, session.TLS)
	} else {
		connection = tls.Client(raw, session.TLS)
	}
	// Closing the transport is the cancellation primitive even for a blocked
	// handshake or local pipe consumer. No protocol or plaintext retry exists.
	finished := make(chan struct{})
	defer close(finished)
	go func() {
		select {
		case <-ctx.Done():
			raw.Close()
			pipes.close()
		case <-finished:
		}
	}()
	authority := watchAuthority(ctx, cancel, pipes.Control, session.Lease, session.Deadline)
	defer func() {
		cancel()
		raw.Close()
		pipes.close()
		authority.Wait()
	}()
	initialDeadline := minTime(session.Lease, now.Add(8*time.Second))
	if err := raw.SetDeadline(initialDeadline); err != nil {
		return errors.New("cannot bound channel handshake")
	}
	if err := connection.HandshakeContext(ctx); err != nil {
		return errors.New("pinned channel TLS handshake failed")
	}
	if err := exchangeHello(connection, session.Hello); err != nil {
		return err
	}
	profile, err := exchangeCapture(connection, session.Hello.Role, session.Capture, session.Hello.Mode)
	if err != nil {
		return err
	}
	if session.Ready != nil {
		if err := session.Ready(profile); err != nil {
			return errors.New("native channel readiness delivery failed")
		}
	}
	if err := raw.SetDeadline(time.Time{}); err != nil {
		return errors.New("cannot set channel transport policy")
	}
	results := make(chan error, 3)
	var workers sync.WaitGroup
	start := func(operation func() error) {
		workers.Add(1)
		go func() {
			defer workers.Done()
			results <- operation()
		}()
	}
	assets := newAssetSession(profile, session.Deadline)
	defer assets.close()
	records := make(chan []byte)
	start(func() error {
		for {
			record, err := readFrame(pipes.Input, maximumRecord)
			if err != nil {
				return err
			}
			if err := admitNative(record, session.Hello.Role, session.Hello.Mode, profile.ClipboardText, profile); err != nil {
				return err
			}
			if record[0] == assetKind {
				forward, err := assets.admit(record[1:], session.Hello.Role)
				if err != nil {
					return err
				}
				if !forward {
					continue
				}
			}
			select {
			case records <- record:
			case <-ctx.Done():
				return ctx.Err()
			}
		}
	})
	start(func() error {
		heartbeat := time.NewTicker(5 * time.Second)
		defer heartbeat.Stop()
		sequence := uint64(4)
		for {
			var kind byte
			var payload []byte
			select {
			case record := <-records:
				kind, payload = record[0], record[1:]
			case <-heartbeat.C:
				kind = heartbeatKind
			case <-ctx.Done():
				return ctx.Err()
			}
			if sequence == 0 {
				return errors.New("desktop sequence exhausted")
			}
			if err := connection.SetWriteDeadline(minTime(time.Now().Add(5*time.Second), session.Deadline)); err != nil {
				return err
			}
			if err := writeEnvelope(connection, sequence, kind, payload); err != nil {
				return err
			}
			sequence++
		}
	})
	start(func() error {
		for sequence := uint64(4); sequence != 0; sequence++ {
			if err := connection.SetReadDeadline(minTime(time.Now().Add(15*time.Second), session.Deadline)); err != nil {
				return err
			}
			kind, payload, err := readEnvelope(connection, sequence)
			if err != nil {
				return err
			}
			if kind == heartbeatKind {
				if len(payload) != 0 {
					return errors.New("invalid encrypted heartbeat")
				}
				continue
			}
			record := append([]byte{kind}, payload...)
			if err := admitNative(record, opposite(session.Hello.Role), session.Hello.Mode, profile.ClipboardText, profile); err != nil {
				return err
			}
			if record[0] == assetKind {
				forward, err := assets.admit(record[1:], opposite(session.Hello.Role))
				if err != nil {
					return err
				}
				if !forward {
					continue
				}
			}
			if err := writeFrame(pipes.Output, record, maximumRecord); err != nil {
				return err
			}
		}
		return errors.New("desktop sequence exhausted")
	})
	var outcome error
	select {
	case outcome = <-results:
	case <-ctx.Done():
		outcome = ctx.Err()
	}
	cancel()
	raw.Close()
	pipes.close()
	workers.Wait()
	return outcome
}

func minTime(left, right time.Time) time.Time {
	if left.Before(right) {
		return left
	}
	return right
}
