package main

import (
	"context"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"time"
)

func main() {
	if len(os.Args) == 2 && os.Args[1] == "display-supervise" {
		if err := runDisplaySupervisor(); err != nil {
			fail(err)
		}
		return
	}
	if strings.TrimSuffix(filepath.Base(os.Args[0]), ".exe") == "dockpipe-qemu-launch" {
		if err := runQemuLauncher(os.Args[1:]); err != nil {
			var exit *displayProcessExitError
			if errors.As(err, &exit) {
				os.Exit(exit.Code)
			}
			fail(err)
		}
		return
	}
	if len(os.Args) == 2 && os.Args[1] == "clipboard-exchange" {
		ctx, cancel := context.WithTimeout(context.Background(), 8*time.Second)
		defer cancel()
		runClipboardExchange(ctx)
		return
	}
	if len(os.Args) != 2 {
		fail(fmt.Errorf("expected report, snapshot, repair, open, or shutdown"))
	}
	deadline := 12 * time.Second
	if os.Args[1] == "snapshot" {
		deadline = 25 * time.Second
	}
	if os.Args[1] == "report" {
		deadline = 8 * time.Second
	}
	ctx, cancel := context.WithTimeout(context.Background(), deadline)
	defer cancel()
	manager, err := newManager(ctx)
	if err != nil {
		fail(err)
	}
	switch os.Args[1] {
	case "report":
		err = manager.report(ctx)
	case "snapshot":
		err = manager.snapshot(ctx)
	case "resolution":
		err = manager.action(ctx, "resolution", resolutionRunID())
	case "repair", "open", "shutdown", "clipboard-start", "clipboard-stop":
		err = manager.action(ctx, os.Args[1], os.Getenv("DOCKPIPE_SURFACE_RESOURCE_ID"))
	default:
		err = fmt.Errorf("unsupported VM management action")
	}
	if err != nil {
		fail(err)
	}
}

func fail(err error) {
	fmt.Fprintln(os.Stderr, "VM management:", err)
	os.Exit(1)
}
