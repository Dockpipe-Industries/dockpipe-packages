package main

import (
	"fmt"
	"net/url"
	"os"
	"path/filepath"
	"strings"
)

const displayInputID = "dockpipe-display-input-qmp"

func displayInputPath(address string) (string, error) {
	if !strings.HasPrefix(address, "unix:path=") || strings.ContainsAny(address, ",;\x00\r\n") {
		return "", fmt.Errorf("invalid private display input address")
	}
	path, err := url.PathUnescape(strings.TrimPrefix(address, "unix:path="))
	if err != nil || !filepath.IsAbs(path) || filepath.Clean(path) != path || filepath.Base(path) != "bus" ||
		address != privateDisplayBusAddress(path) {
		return "", fmt.Errorf("invalid private display input path")
	}
	return filepath.Join(filepath.Dir(path), "input"), nil
}

// The native display owns a separate monitor. Never reuse or remove the
// caller's management -qmp channel. The reserved manifest channel is relocated
// to this resource's short, admitted private socket directory.
func nativeAbsoluteInputArguments(arguments []string, address string) ([]string, error) {
	path, err := displayInputPath(address)
	if err != nil || len([]byte(path)) > 100 || strings.ContainsAny(path, ",\r\n") ||
		!privateOwnedPath(filepath.Dir(path), true, 0700) {
		return nil, fmt.Errorf("private display input directory unavailable")
	}
	if _, err := os.Lstat(path); !os.IsNotExist(err) {
		return nil, fmt.Errorf("private display input endpoint already exists")
	}
	result := make([]string, 0, len(arguments)+4)
	channels, monitors := 0, 0
	for index := 0; index < len(arguments); index++ {
		argument := arguments[index]
		if (argument == "-chardev" || argument == "-mon") && index+1 < len(arguments) &&
			strings.Contains(arguments[index+1], displayInputID) {
			value := arguments[index+1]
			if argument == "-chardev" {
				prefix := "socket,id=" + displayInputID + ",path="
				suffix := ",server=on,wait=off"
				if !strings.HasPrefix(value, prefix) || !strings.HasSuffix(value, suffix) {
					return nil, fmt.Errorf("reserved display input channel conflicts")
				}
				previous := strings.TrimSuffix(strings.TrimPrefix(value, prefix), suffix)
				if !filepath.IsAbs(previous) || strings.ContainsAny(previous, ",\r\n\x00") {
					return nil, fmt.Errorf("reserved display input channel conflicts")
				}
				channels++
			} else {
				if value != "chardev="+displayInputID+",mode=control" {
					return nil, fmt.Errorf("reserved display input monitor conflicts")
				}
				monitors++
			}
			index++
			continue
		}
		result = append(result, argument)
	}
	if channels != monitors || channels > 1 {
		return nil, fmt.Errorf("reserved display input monitor is ambiguous")
	}
	return append(result, "-chardev", "socket,id="+displayInputID+",path="+path+",server=on,wait=off",
		"-mon", "chardev="+displayInputID+",mode=control"), nil
}

func validDisplayInputDescriptor(descriptor displayDescriptor) bool {
	if descriptor.Schema == 1 {
		return descriptor.InputAddress == ""
	}
	if descriptor.Schema != 2 || descriptor.InputAddress == "" {
		return false
	}
	path, err := displayInputPath(descriptor.BusAddress)
	return err == nil && descriptor.InputAddress == privateDisplayBusAddress(path)
}
