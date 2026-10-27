//go:build linux

package cloudflare

import (
	"context"
	"os"
	"path/filepath"
	"strconv"
	"strings"
)

func inspectActiveImages(ctx context.Context) ([]inspectedExecutable, []string) {
	return inspectLinuxProcesses(ctx, "/proc", probeExecutableVersion)
}

func inspectLinuxProcesses(ctx context.Context, root string, probe func(context.Context, string) (string, error)) ([]inspectedExecutable, []string) {
	gaps := []string{"Linux process inspection is limited to the current PID namespace and readable image handles; hidden users/namespaces and containers remain unproven."}
	directory, err := os.Open(root)
	if err != nil {
		return nil, append(gaps, "Linux process image inventory is unavailable.")
	}
	defer directory.Close()
	entries, err := directory.ReadDir(maximumProcesses + 1)
	if err != nil && len(entries) == 0 {
		return nil, append(gaps, "Linux process image inventory is unavailable.")
	}
	if len(entries) > maximumProcesses {
		entries = entries[:maximumProcesses]
		gaps = append(gaps, "Linux process inventory reached its 4096 entry limit.")
	}
	var images []inspectedExecutable
	imageChanged := false
	inspectedImages := 0
	for _, entry := range entries {
		if ctx.Err() != nil {
			break
		}
		if _, err := strconv.ParseUint(entry.Name(), 10, 32); err != nil || !entry.IsDir() {
			continue
		}
		path := filepath.Join(root, entry.Name(), "exe")
		target, err := os.Readlink(path)
		if err != nil || filepath.Base(strings.TrimSuffix(target, " (deleted)")) != "cloudflared" {
			continue
		}
		if inspectedImages == maximumActiveImages {
			gaps = append(gaps, "Active image inventory reached its 64 image limit.")
			break
		}
		inspectedImages++
		before, err := os.Stat(path)
		if err != nil {
			continue
		}
		version, _ := probe(ctx, path)
		after, err := os.Stat(path)
		if err != nil || !unchangedImage(before, after) {
			imageChanged = true
			continue
		}
		images = append(images, inspectedExecutable{path: path, info: after, version: version})
	}
	if imageChanged {
		gaps = append(gaps, "A process image changed during inspection and was not treated as active-version proof.")
	}
	return images, gaps
}
