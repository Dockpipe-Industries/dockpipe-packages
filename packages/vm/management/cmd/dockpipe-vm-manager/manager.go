package main

import (
	"context"
	"fmt"
	"os"
	"path/filepath"
	"strings"
)

const packageOwner = "vm.management.v1"

type manager struct {
	program   string
	workspace string
	state     string
}

func newManager(ctx context.Context) (manager, error) {
	var result manager
	result.program = os.Getenv("DOCKPIPE_BIN")
	result.workspace = os.Getenv("DOCKPIPE_VM_EVENT_WORKDIR")
	if result.workspace == "" {
		result.workspace = os.Getenv("DOCKPIPE_WORKDIR")
	}
	if result.workspace == "" {
		result.workspace, _ = os.Getwd()
	}
	if !filepath.IsAbs(result.program) || !filepath.IsAbs(result.workspace) {
		return result, fmt.Errorf("absolute Dockpipe executable and workspace are required")
	}
	var scopeEnvironment []string
	// Installed native tools live in assets/tooling/bin. Resolve state policy
	// from that same package closure, without consulting a source checkout.
	if executable, executableError := os.Executable(); executableError == nil {
		packageRoot := filepath.Clean(filepath.Join(filepath.Dir(executable), "..", "..", ".."))
		manifest := filepath.Join(packageRoot, "package.yml")
		if info, inspectError := os.Lstat(manifest); inspectError == nil && info.Mode().IsRegular() {
			scopeEnvironment = append(scopeEnvironment, "DOCKPIPE_PACKAGE_MANIFEST="+manifest)
		}
	}
	output, err := runCommand(ctx, result.program, []string{"scope", "--package", packageOwner, ".", "--workdir", result.workspace}, result.workspace, scopeEnvironment...)
	if err != nil {
		return result, fmt.Errorf("resolve package state: %w", err)
	}
	result.state = strings.TrimSpace(string(output))
	if !filepath.IsAbs(result.state) {
		return result, fmt.Errorf("Dockpipe returned an invalid package state scope")
	}
	info, err := os.Lstat(result.state)
	if err != nil || !info.IsDir() || info.Mode()&os.ModeSymlink != 0 {
		return result, fmt.Errorf("package state must be an ordinary directory")
	}
	return result, nil
}
