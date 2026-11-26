package main

import (
	"context"
	"io"
	"sync"
	"time"
)

// The trusted parent pipe is observed before TLS handshake starts. Its EOF
// revokes authority immediately, including while peer admission is blocked.
func watchAuthority(ctx context.Context, cancel context.CancelFunc, control io.ReadCloser, initial, deadline time.Time) *sync.WaitGroup {
	var workers sync.WaitGroup
	updates := make(chan time.Time)
	if control != nil {
		workers.Add(1)
		go func() {
			defer workers.Done()
			defer cancel()
			for {
				data, err := readFrame(control, 4096)
				if err != nil {
					return
				}
				var update struct {
					Type      string    `json:"type"`
					ExpiresAt time.Time `json:"expires_at"`
				}
				if decodeJSON(data, &update) != nil || update.Type != "lease" || !validLease(update.ExpiresAt, deadline, time.Now()) {
					return
				}
				select {
				case updates <- update.ExpiresAt:
				case <-ctx.Done():
					return
				}
			}
		}()
	}
	workers.Add(1)
	go func() {
		defer workers.Done()
		defer cancel()
		lease := initial
		timer := time.NewTimer(time.Until(lease))
		defer timer.Stop()
		for {
			select {
			case expiry := <-updates:
				now := time.Now()
				if !now.Before(lease) || !expiry.After(lease) || !validLease(expiry, deadline, now) {
					return
				}
				lease = expiry
				timer.Reset(time.Until(lease))
			case <-timer.C:
				return
			case <-ctx.Done():
				return
			}
		}
	}()
	return &workers
}
