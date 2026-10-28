package main

import (
	"context"
	"flag"
	"fmt"
	"os"
	"os/exec"
	"os/signal"
	"path/filepath"
	"syscall"

	"github.com/Dockpipe-Industries/dockpipe-packages/packages/remote/tools/cloudflare"
)

func main() {
	if len(os.Args) > 1 && (os.Args[1] == "lifecycle-inspect" || os.Args[1] == "lifecycle-change") {
		ctx, cancel := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
		defer cancel()
		handler := cloudflare.InspectCommand
		if os.Args[1] == "lifecycle-change" {
			handler = cloudflare.ChangeCommand
		}
		if err := handler(ctx, os.Args[2:]); err != nil {
			fmt.Fprintln(os.Stderr, err)
			os.Exit(1)
		}
		return
	}

	state := flag.String("state", "", "private resolver state")
	output := flag.String("output", "", "edge contract output")
	hostname := flag.String("hostname", "", "Cloudflare-managed hostname")
	origin := flag.String("origin", "", "loopback broker origin")
	flag.Parse()
	executable := os.Getenv("DOCKPIPE_CLOUDFLARED_BIN")
	if executable == "" {
		var err error
		executable, err = exec.LookPath("cloudflared")
		if err != nil {
			fmt.Fprintln(os.Stderr, "Install the resolver's cloudflared dependency first (macOS: brew install cloudflared).")
			os.Exit(1)
		}
	}
	executable, err := filepath.Abs(executable)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	home, err := os.UserHomeDir()
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	ctx, cancel := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer cancel()
	setup := cloudflare.Setup{State: *state, Output: *output, Hostname: *hostname, Origin: *origin, Executable: executable, Home: home}
	if err := setup.Execute(ctx); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}
