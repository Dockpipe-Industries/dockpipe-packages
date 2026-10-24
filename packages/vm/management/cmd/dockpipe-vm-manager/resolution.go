package main

import (
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"strconv"
	"strings"
)

func resolutionRequest(width, height string) (string, error) {
	dimensions := make([]int, 2)
	for index, value := range []string{width, height} {
		parsed, err := strconv.Atoi(value)
		minimum := 640
		if index == 1 {
			minimum = 480
		}
		if err != nil || strconv.Itoa(parsed) != value || parsed < minimum || parsed > 8192 {
			return "", fmt.Errorf("resolution must use integer dimensions between 640x480 and 8192x8192")
		}
		dimensions[index] = parsed
	}
	payload, err := json.Marshal(map[string]any{
		"schema": 1, "operation": "display-resolution",
		"width": dimensions[0], "height": dimensions[1],
	})
	if err != nil {
		return "", err
	}
	// Only validated integers and fixed keys enter the single-quoted request.
	return strings.Replace(showGuestTools, `{"schema":1,"operation":"show"}`, string(payload), 1), nil
}

func resolutionRunID() string {
	if value := os.Getenv("DOCKPIPE_VM_TARGET_RUN_ID"); value != "" {
		return value
	}
	return os.Getenv("DOCKPIPE_SURFACE_RESOURCE_ID")
}

// These limits match the owning software renderer's FrameStore contract.
// They apply only to verified package-native sessions, not ordinary QEMU windows.
const nativeResolutionMaximumDimension = 4096
const nativeResolutionMaximumFrameBytes = 32 * 1024 * 1024

func (manager manager) resolutionRequest(record sessionRecord, width, height string) (string, error) {
	request, err := resolutionRequest(width, height)
	if err != nil {
		return "", err
	}

	directory := filepath.Join(manager.state, "display-"+record.RunID)
	_, err = os.Lstat(directory)
	if os.IsNotExist(err) {
		return request, nil
	}
	if err != nil || manager.nativeDisplayOptions(record) == nil {
		return "", fmt.Errorf("native display identity is not verified; resolution change refused")
	}

	// The generic request parser has already admitted canonical bounded integers.
	pixelsWide, _ := strconv.Atoi(width)
	pixelsHigh, _ := strconv.Atoi(height)
	if !nativeResolutionWithinLimits(pixelsWide, pixelsHigh) {
		return "", fmt.Errorf("resolution exceeds the active native display limit: 4096 pixels per axis and 32 MiB per 32-bit frame")
	}
	return request, nil
}

func nativeResolutionWithinLimits(width, height int) bool {
	if width <= 0 || height <= 0 || width > nativeResolutionMaximumDimension || height > nativeResolutionMaximumDimension {
		return false
	}
	return uint64(width)*uint64(height)*4 <= nativeResolutionMaximumFrameBytes
}
