//go:build !linux

package cloudflare

import (
	"errors"
	"os"
)

func validateBinaryAccess(info os.FileInfo) error {
	return errors.New("standalone replacement is not qualified on this platform")
}

func openBinaryFile(path string) (*os.File, error) {
	return nil, errors.New("standalone replacement is not qualified on this platform")
}
