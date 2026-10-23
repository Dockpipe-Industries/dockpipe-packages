package main

import (
	"encoding/json"
	"fmt"
	"os"
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
