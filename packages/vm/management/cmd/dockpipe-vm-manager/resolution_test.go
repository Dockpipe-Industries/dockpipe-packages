package main

import (
	"strings"
	"testing"
)

func TestResolutionRequest(t *testing.T) {
	script, err := resolutionRequest("1920", "1080")
	if err != nil || !strings.Contains(script, `"operation":"display-resolution"`) || !strings.Contains(script, `"width":1920`) || !strings.Contains(script, "NamedPipeClientStream") {
		t.Fatalf("missing interactive request: %v", err)
	}
	if strings.Contains(script, `"operation":"show"`) {
		t.Fatal("show operation retained")
	}
	for _, values := range [][2]string{{"0", "1080"}, {"8193", "1080"}, {"1920", "479"}, {"1920", "8193"}, {"1920;exit", "1080"}, {" 1920", "1080"}, {"01920", "1080"}, {"1920", "1080.5"}, {"", ""}} {
		if _, err := resolutionRequest(values[0], values[1]); err == nil {
			t.Fatalf("accepted invalid geometry %q", values)
		}
	}
}
func TestResolutionRunID(t *testing.T) {
	t.Setenv("DOCKPIPE_SURFACE_RESOURCE_ID", "surface-run")
	t.Setenv("DOCKPIPE_VM_TARGET_RUN_ID", "")
	if resolutionRunID() != "surface-run" {
		t.Fatal("surface binding lost")
	}
	t.Setenv("DOCKPIPE_VM_TARGET_RUN_ID", "cli-run")
	if resolutionRunID() != "cli-run" {
		t.Fatal("CLI target ignored")
	}
}
