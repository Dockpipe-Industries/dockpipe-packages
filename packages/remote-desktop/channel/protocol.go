package main

import (
	"bytes"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"regexp"
	"time"
)

const (
	maximumChunk          = 64 << 10
	maximumMedia          = 2 << 20
	maximumRecord         = maximumMedia + 1
	maximumJSON           = 16 << 10
	mediaKind        byte = 1
	inputKind        byte = 2
	mediaControlKind byte = 3
	heartbeatKind    byte = 240
	helloKind        byte = 241
	captureKind      byte = 242
	channelVersion        = "dockpipe.desktop.channel/v2"
)

type Target struct {
	Node        string `json:"node"`
	Fingerprint string `json:"fingerprint"`
	Generation  string `json:"generation"`
}

type Reference struct {
	Schema string `json:"schema"`
	ID     string `json:"id"`
	Target Target `json:"target"`
	Epoch  string `json:"epoch"`
}

var nodePattern = regexp.MustCompile(`^[A-Za-z0-9][A-Za-z0-9_-]{0,63}$`)

func digest(value string) bool {
	if len(value) != 64 {
		return false
	}
	decoded, err := hex.DecodeString(value)
	return err == nil && len(decoded) == 32 && value == fmt.Sprintf("%x", decoded)
}

func (reference Reference) validate() error {
	if reference.Schema != "dockpipe.remote.desktop/v1" || !digest(reference.ID) || !digest(reference.Epoch) ||
		!nodePattern.MatchString(reference.Target.Node) || !digest(reference.Target.Fingerprint) || !digest(reference.Target.Generation) {
		return errors.New("invalid desktop reference")
	}
	return nil
}

func writeFrame(writer io.Writer, value []byte, maximum int) error {
	if len(value) == 0 || len(value) > maximum {
		return errors.New("record length outside contract")
	}
	var header [4]byte
	binary.BigEndian.PutUint32(header[:], uint32(len(value)))
	if err := writeAll(writer, header[:]); err != nil {
		return err
	}
	return writeAll(writer, value)
}

func writeAll(writer io.Writer, value []byte) error {
	for len(value) > 0 {
		count, err := writer.Write(value)
		if count < 0 || count > len(value) {
			return errors.New("invalid writer count")
		}
		value = value[count:]
		if err != nil {
			return err
		}
		if count == 0 {
			return io.ErrNoProgress
		}
	}
	return nil
}

func readFrame(reader io.Reader, maximum int) ([]byte, error) {
	var header [4]byte
	if _, err := io.ReadFull(reader, header[:]); err != nil {
		return nil, err
	}
	size := binary.BigEndian.Uint32(header[:])
	if size == 0 || uint64(size) > uint64(maximum) {
		return nil, errors.New("record length outside contract")
	}
	value := make([]byte, int(size))
	if _, err := io.ReadFull(reader, value); err != nil {
		return nil, err
	}
	return value, nil
}

func decodeJSON(data []byte, value any) error {
	if len(data) == 0 || len(data) > maximumJSON {
		return errors.New("JSON outside contract")
	}
	decoder := json.NewDecoder(bytes.NewReader(data))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(value); err != nil {
		return errors.New("invalid channel JSON")
	}
	if err := decoder.Decode(new(any)); err != io.EOF {
		return errors.New("trailing channel JSON")
	}
	return nil
}

type Hello struct {
	Schema    string    `json:"schema"`
	Reference Reference `json:"reference"`
	Role      string    `json:"role"`
	Mode      string    `json:"mode"`
}

func (hello Hello) validate() error {
	if hello.Schema != channelVersion || hello.Reference.validate() != nil ||
		hello.Role != "viewer" && hello.Role != "worker" || hello.Mode != "view" && hello.Mode != "control" {
		return errors.New("invalid encrypted desktop binding")
	}
	return nil
}

func opposite(role string) string {
	if role == "worker" {
		return "viewer"
	}
	return "worker"
}

func admitNative(record []byte, senderRole, mode string, clipboardEnabled bool, profiles ...CaptureProfile) error {
	if len(record) < 2 || len(record) > maximumRecord {
		return errors.New("invalid native desktop record")
	}
	switch record[0] {
	case mediaKind:
		if senderRole != "worker" || len(record)-1 > maximumMedia {
			return errors.New("media direction or limit rejected")
		}
	case inputKind:
		if senderRole != "viewer" || mode != "control" || len(record)-1 > 4096 {
			return errors.New("input authority or limit rejected")
		}
	case assetKind:
		if len(profiles) != 1 {
			return errAsset
		}
		return admitAsset(record[1:], senderRole, mode, profiles[0])
	case clipboardKind:
		return admitClipboard(record[1:], senderRole, mode, clipboardEnabled)
	case mediaControlKind:
		if senderRole != "viewer" || len(record) != 2 || record[1] != 1 {
			return errors.New("media control direction or request rejected")
		}
	default:
		return errors.New("unsupported native desktop record kind")
	}
	return nil
}

// The helper's encrypted sequence is independent of application input sequence.
// TLS already authenticates order; this envelope also prevents accidental
// duplicate delivery and makes the package's own stream contract explicit.
func writeEnvelope(writer io.Writer, sequence uint64, kind byte, payload []byte) error {
	value := make([]byte, 9+len(payload))
	value[0] = kind
	binary.BigEndian.PutUint64(value[1:9], sequence)
	copy(value[9:], payload)
	return writeFrame(writer, value, maximumMedia+9)
}

func readEnvelope(reader io.Reader, expected uint64) (byte, []byte, error) {
	return readEnvelopeBounded(reader, expected, maximumMedia)
}

func readEnvelopeBounded(reader io.Reader, expected uint64, maximumPayload int) (byte, []byte, error) {
	value, err := readFrame(reader, maximumPayload+9)
	if err != nil {
		return 0, nil, err
	}
	if len(value) < 9 || expected == 0 || binary.BigEndian.Uint64(value[1:9]) != expected {
		return 0, nil, errors.New("encrypted record sequence rejected")
	}
	return value[0], value[9:], nil
}

func validLease(expiry, deadline, now time.Time) bool {
	return expiry.After(now) && !expiry.After(deadline) && !expiry.After(now.Add(10*time.Second))
}
