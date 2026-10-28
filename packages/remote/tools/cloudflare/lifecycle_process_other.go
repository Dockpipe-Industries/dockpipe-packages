//go:build !linux

package cloudflare

import "context"

func inspectActiveImages(ctx context.Context) ([]inspectedExecutable, []string) {
	return nil, []string{"Active process, service and container image inspection is unavailable on this platform; native macOS/Windows qualification has not been performed."}
}
