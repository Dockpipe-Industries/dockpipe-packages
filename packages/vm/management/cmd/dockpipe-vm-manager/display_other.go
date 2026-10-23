//go:build !linux && !darwin && !windows

package main

import "fmt"

func runQemuLauncher(arguments []string) error {
	return fmt.Errorf("the packaged native QEMU display launcher is currently available on Linux only")
}

func resolveQemuExecutable(path string) (string, error) {
	return "", fmt.Errorf("native QEMU display is unsupported on this platform")
}

func runDisplaySupervisor() error {
	return fmt.Errorf("private QEMU display supervision is currently available on Linux only")
}

func (manager manager) nativeDisplayOptions(record sessionRecord) map[string]any {
	return nil
}
