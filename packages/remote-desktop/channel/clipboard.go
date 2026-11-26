package main

import (
	"bytes"
	"encoding/binary"
	"errors"
	"unicode/utf8"
)

const (
	clipboardKind         byte = 4
	clipboardHeaderBytes       = 18
	maximumClipboardText       = 64 << 10
	clipboardWatch        byte = 1
	clipboardPause        byte = 2
	clipboardWrite        byte = 3
	clipboardChanged      byte = 4
	clipboardAcknowledged byte = 5
	clipboardNotice       byte = 6
)

func admitClipboard(payload []byte, senderRole, mode string, enabled bool) error {
	if !enabled || mode != "control" || len(payload) < clipboardHeaderBytes ||
		len(payload) > clipboardHeaderBytes+maximumClipboardText || payload[0] != 1 {
		return errors.New("clipboard authority or framing rejected")
	}
	epoch := binary.BigEndian.Uint64(payload[2:10])
	revision := binary.BigEndian.Uint64(payload[10:18])
	text := payload[clipboardHeaderBytes:]
	if epoch == 0 || !utf8.Valid(text) || bytes.IndexByte(text, 0) >= 0 {
		return errors.New("clipboard epoch or text rejected")
	}
	switch payload[1] {
	case clipboardWatch, clipboardPause:
		if senderRole != "viewer" || revision != 0 || len(text) != 0 {
			return errors.New("clipboard watch direction or structure rejected")
		}
	case clipboardWrite:
		if senderRole != "viewer" || revision == 0 {
			return errors.New("clipboard write direction or revision rejected")
		}
	case clipboardChanged:
		if senderRole != "worker" || revision == 0 {
			return errors.New("clipboard change direction or revision rejected")
		}
	case clipboardAcknowledged:
		if senderRole != "worker" || revision == 0 || len(text) != 0 {
			return errors.New("clipboard acknowledgment direction or structure rejected")
		}
	case clipboardNotice:
		reason := string(text)
		if senderRole != "worker" || revision != 0 ||
			reason != "too_large" && reason != "not_text" && reason != "unsupported_text" && reason != "unavailable" {
			return errors.New("clipboard notice direction or reason rejected")
		}
	default:
		return errors.New("unsupported clipboard operation")
	}
	return nil
}
