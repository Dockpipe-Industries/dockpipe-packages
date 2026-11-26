package main

import (
	"bytes"
	"encoding/binary"
	"errors"
	"strings"
	"unicode"
	"unicode/utf8"
)

const (
	assetKind               byte   = 5
	assetHeaderBytes               = 36
	assetMaximumChunk              = 64 << 10
	assetMaximumManifest           = 64 << 10
	assetMaximumRecord             = assetHeaderBytes + 12 + assetMaximumChunk
	assetMaximumPNG         uint64 = 64 << 20
	assetMaximumPixels      uint64 = 1 << 24
	assetMaximumFiles              = 128
	assetMaximumBatch       uint64 = 1 << 30
	assetWindow                    = 4
	assetArm                byte   = 1
	assetPause              byte   = 2
	assetOffer              byte   = 3
	assetAccept             byte   = 4
	assetChunk              byte   = 5
	assetCredit             byte   = 6
	assetFinish             byte   = 7
	assetCommitted          byte   = 8
	assetCancel             byte   = 9
	assetCancelAcknowledged byte   = 10
	assetEpochAcknowledged  byte   = 11
	assetPNG                byte   = 1
	assetFiles              byte   = 2
)

var errAsset = errors.New("asset clipboard protocol rejected")

type assetItem struct {
	id   uint32
	size uint64
	name string
}

type assetRecord struct {
	op       byte
	epoch    uint64
	serial   uint64
	transfer [16]byte
	format   byte
	width    uint32
	height   uint32
	total    uint64
	items    []assetItem
	grant    uint64
	window   byte
	item     uint32
	offset   uint64
	data     []byte
	digest   [32]byte
	verified bool
	reason   string
	control  byte
}

func validAssetName(name string) bool {
	if len(name) == 0 || len(name) > 255 || !utf8.ValidString(name) || name == "." || name == ".." {
		return false
	}
	for _, character := range name {
		if unicode.IsControl(character) || character == '/' || character == '\\' {
			return false
		}
	}
	return true
}

func foldAssetName(name string) string {
	return strings.Map(func(character rune) rune {
		if character >= 'A' && character <= 'Z' {
			return character + ('a' - 'A')
		}
		return character
	}, name)
}

func validAssetReason(reason string) bool {
	switch reason {
	case "unavailable", "quota", "source_changed", "hash_mismatch", "stale_epoch", "timeout", "unsupported", "user_cancelled", "invalid":
		return true
	}
	return false
}

func (record assetRecord) validate() error {
	if record.epoch == 0 {
		return errAsset
	}
	zeroID := record.transfer == [16]byte{}
	if record.op == assetArm || record.op == assetPause || record.op == assetEpochAcknowledged {
		if record.serial != 0 || !zeroID {
			return errAsset
		}
		if record.op == assetEpochAcknowledged && record.control != assetArm && record.control != assetPause {
			return errAsset
		}
		return nil
	}
	if record.serial == 0 || zeroID {
		return errAsset
	}
	switch record.op {
	case assetOffer:
		if record.format != assetPNG && record.format != assetFiles || len(record.items) == 0 || len(record.items) > assetMaximumFiles {
			return errAsset
		}
		var total uint64
		manifestBytes := 19
		names := make(map[string]struct{}, len(record.items))
		for index, item := range record.items {
			if item.id != uint32(index+1) || !validAssetName(item.name) || item.size > assetMaximumBatch-total {
				return errAsset
			}
			folded := foldAssetName(item.name)
			if _, exists := names[folded]; exists {
				return errAsset
			}
			names[folded] = struct{}{}
			total += item.size
			manifestBytes += 14 + len(item.name)
		}
		if total != record.total || manifestBytes > assetMaximumManifest {
			return errAsset
		}
		if record.format == assetPNG {
			if len(record.items) != 1 || record.items[0].name != "clipboard.png" || total == 0 || total > assetMaximumPNG ||
				record.width == 0 || record.height == 0 || uint64(record.width)*uint64(record.height) > assetMaximumPixels {
				return errAsset
			}
		} else if record.width != 0 || record.height != 0 {
			return errAsset
		}
	case assetAccept:
		if record.grant > assetMaximumBatch || record.window != assetWindow {
			return errAsset
		}
	case assetChunk:
		if record.item == 0 || record.item > assetMaximumFiles || len(record.data) == 0 || len(record.data) > assetMaximumChunk ||
			record.offset > assetMaximumBatch || uint64(len(record.data)) > assetMaximumBatch-record.offset {
			return errAsset
		}
	case assetCredit, assetFinish:
		if record.item == 0 || record.item > assetMaximumFiles || record.offset > assetMaximumBatch {
			return errAsset
		}
	case assetCommitted, assetCancelAcknowledged:
	case assetCancel:
		if !validAssetReason(record.reason) {
			return errAsset
		}
	default:
		return errAsset
	}
	return nil
}

type assetReader struct {
	data     []byte
	position int
	err      error
}

func (reader *assetReader) take(size int) []byte {
	if reader.err != nil || size < 0 || size > len(reader.data)-reader.position {
		reader.err = errAsset
		return nil
	}
	value := reader.data[reader.position : reader.position+size]
	reader.position += size
	return value
}

func (reader *assetReader) number(size int) uint64 {
	var value uint64
	for _, character := range reader.take(size) {
		value = value<<8 | uint64(character)
	}
	return value
}

func decodeAssetRecord(payload []byte) (assetRecord, error) {
	var record assetRecord
	if len(payload) < assetHeaderBytes || len(payload) > assetMaximumRecord {
		return record, errAsset
	}
	reader := assetReader{data: payload}
	if reader.number(1) != 1 {
		return record, errAsset
	}
	record.op = byte(reader.number(1))
	if reader.number(2) != 0 {
		return record, errAsset
	}
	record.epoch = reader.number(8)
	record.serial = reader.number(8)
	copy(record.transfer[:], reader.take(16))
	switch record.op {
	case assetEpochAcknowledged:
		record.control = byte(reader.number(1))
	case assetOffer:
		record.format = byte(reader.number(1))
		record.width = uint32(reader.number(4))
		record.height = uint32(reader.number(4))
		count := reader.number(2)
		if count == 0 || count > assetMaximumFiles {
			return record, errAsset
		}
		record.total = reader.number(8)
		record.items = make([]assetItem, 0, int(count))
		for range count {
			item := assetItem{id: uint32(reader.number(4)), size: reader.number(8)}
			length := reader.number(2)
			if length == 0 || length > 255 {
				return record, errAsset
			}
			item.name = string(reader.take(int(length)))
			record.items = append(record.items, item)
		}
	case assetAccept:
		record.grant = reader.number(8)
		record.window = byte(reader.number(1))
	case assetChunk:
		record.item = uint32(reader.number(4))
		record.offset = reader.number(8)
		record.data = reader.take(len(payload) - reader.position)
	case assetCredit:
		record.item = uint32(reader.number(4))
		record.offset = reader.number(8)
		verified := reader.number(1)
		if verified > 1 {
			return record, errAsset
		}
		record.verified = verified == 1
	case assetFinish:
		record.item = uint32(reader.number(4))
		record.offset = reader.number(8)
		copy(record.digest[:], reader.take(32))
	case assetCancel:
		record.reason = string(reader.take(len(payload) - reader.position))
	}
	if reader.err != nil || reader.position != len(payload) || record.validate() != nil {
		return record, errAsset
	}
	return record, nil
}

func encodeAssetRecord(record assetRecord) ([]byte, error) {
	if err := record.validate(); err != nil {
		return nil, err
	}
	output := make([]byte, 0, assetHeaderBytes+64)
	output = append(output, 1, record.op, 0, 0)
	output = binary.BigEndian.AppendUint64(output, record.epoch)
	output = binary.BigEndian.AppendUint64(output, record.serial)
	output = append(output, record.transfer[:]...)
	switch record.op {
	case assetEpochAcknowledged:
		output = append(output, record.control)
	case assetOffer:
		output = append(output, record.format)
		output = binary.BigEndian.AppendUint32(output, record.width)
		output = binary.BigEndian.AppendUint32(output, record.height)
		output = binary.BigEndian.AppendUint16(output, uint16(len(record.items)))
		output = binary.BigEndian.AppendUint64(output, record.total)
		for _, item := range record.items {
			output = binary.BigEndian.AppendUint32(output, item.id)
			output = binary.BigEndian.AppendUint64(output, item.size)
			output = binary.BigEndian.AppendUint16(output, uint16(len(item.name)))
			output = append(output, item.name...)
		}
	case assetAccept:
		output = binary.BigEndian.AppendUint64(output, record.grant)
		output = append(output, record.window)
	case assetChunk:
		output = binary.BigEndian.AppendUint32(output, record.item)
		output = binary.BigEndian.AppendUint64(output, record.offset)
		output = append(output, record.data...)
	case assetCredit:
		output = binary.BigEndian.AppendUint32(output, record.item)
		output = binary.BigEndian.AppendUint64(output, record.offset)
		if record.verified {
			output = append(output, 1)
		} else {
			output = append(output, 0)
		}
	case assetFinish:
		output = binary.BigEndian.AppendUint32(output, record.item)
		output = binary.BigEndian.AppendUint64(output, record.offset)
		output = append(output, record.digest[:]...)
	case assetCancel:
		output = append(output, record.reason...)
	}
	if len(output) > assetMaximumRecord {
		return nil, errAsset
	}
	return output, nil
}

func sameAssetIdentity(left, right assetRecord) bool {
	return left.epoch == right.epoch && left.serial == right.serial && bytes.Equal(left.transfer[:], right.transfer[:])
}

func admitAsset(payload []byte, role, mode string, profile CaptureProfile) error {
	if mode != "control" || role != "worker" && role != "viewer" || !profile.ClipboardImages && !profile.ClipboardFiles {
		return errAsset
	}
	record, err := decodeAssetRecord(payload)
	if err != nil {
		return err
	}
	if (record.op == assetArm || record.op == assetPause) && role != "viewer" ||
		record.op == assetEpochAcknowledged && role != "worker" {
		return errAsset
	}
	if record.op == assetOffer && (record.format == assetPNG && !profile.ClipboardImages || record.format == assetFiles && !profile.ClipboardFiles) {
		return errAsset
	}
	return nil
}
