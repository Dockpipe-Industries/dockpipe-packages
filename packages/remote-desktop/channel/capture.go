package main

import (
	"encoding/json"
	"errors"
	"net"
)

// CaptureProfile is immutable for one authenticated channel. Native presets
// derive dimensions and bitrate; the peer never supplies arbitrary maxima.
type CaptureProfile struct {
	Quality         string `json:"quality"`
	FrameRate       int    `json:"frame_rate"`
	DisplaySource   string `json:"display_source,omitempty"`
	ClipboardText   bool   `json:"clipboard_text,omitempty"`
	ClipboardImages bool   `json:"clipboard_images,omitempty"`
	ClipboardFiles  bool   `json:"clipboard_files,omitempty"`
}

// Omission is the canonical existing-display profile. A present source must
// explicitly select virtual; null/empty/unknown values cannot silently default.
func (profile *CaptureProfile) UnmarshalJSON(data []byte) error {
	var wire struct {
		Quality         string          `json:"quality"`
		FrameRate       int             `json:"frame_rate"`
		DisplaySource   json.RawMessage `json:"display_source"`
		ClipboardText   json.RawMessage `json:"clipboard_text"`
		ClipboardImages json.RawMessage `json:"clipboard_images"`
		ClipboardFiles  json.RawMessage `json:"clipboard_files"`
	}
	if err := decodeJSON(data, &wire); err != nil {
		return err
	}
	source := ""
	if wire.DisplaySource != nil {
		if json.Unmarshal(wire.DisplaySource, &source) != nil || source != "virtual" {
			return errors.New("unsupported capture display source")
		}
	}
	clipboard := false
	if wire.ClipboardText != nil {
		if json.Unmarshal(wire.ClipboardText, &clipboard) != nil || !clipboard {
			return errors.New("unsupported clipboard capability")
		}
	}
	images, files := false, false
	for _, optional := range []struct {
		raw   json.RawMessage
		value *bool
	}{{wire.ClipboardImages, &images}, {wire.ClipboardFiles, &files}} {
		if optional.raw != nil && (json.Unmarshal(optional.raw, optional.value) != nil || !*optional.value) {
			return errors.New("unsupported asset clipboard capability")
		}
	}
	candidate := CaptureProfile{ClipboardImages: images, ClipboardFiles: files, Quality: wire.Quality, FrameRate: wire.FrameRate, DisplaySource: source, ClipboardText: clipboard}
	if err := candidate.validate(); err != nil {
		return err
	}
	*profile = candidate
	return nil
}

func (profile CaptureProfile) validate() error {
	if profile.DisplaySource != "" && profile.DisplaySource != "virtual" {
		return errors.New("unsupported capture display source")
	}
	if profile.Quality != "low" && profile.Quality != "balanced" && profile.Quality != "high" && profile.Quality != "ultra" {
		return errors.New("unsupported capture quality")
	}
	if profile.FrameRate != 10 && profile.FrameRate != 15 && profile.FrameRate != 24 && profile.FrameRate != 30 && profile.FrameRate != 60 {
		return errors.New("unsupported capture frame rate")
	}
	return nil
}

// Called only after pinned TLS and exact-reference HELLO. Both directions use
// sequence2 for request/echo and sequence3 for confirm/final acknowledgment.
// No native readiness or media/input pipe processing precedes this agreement.
func (profile CaptureProfile) validateForMode(mode string) error {
	if err := profile.validate(); err != nil {
		return err
	}
	if mode != "view" && mode != "control" || (profile.ClipboardText || profile.ClipboardImages || profile.ClipboardFiles) && mode != "control" {
		return errors.New("clipboard requires control authority")
	}
	return nil
}

func exchangeCapture(connection net.Conn, role string, requested *CaptureProfile, mode string) (CaptureProfile, error) {
	receive := func(sequence uint64) (CaptureProfile, error) {
		kind, payload, err := readEnvelopeBounded(connection, sequence, maximumJSON)
		var profile CaptureProfile
		if err != nil || kind != captureKind || decodeJSON(payload, &profile) != nil || profile.validateForMode(mode) != nil {
			return profile, errors.New("invalid encrypted capture agreement")
		}
		return profile, nil
	}
	send := func(sequence uint64, profile CaptureProfile) error {
		payload, err := json.Marshal(profile)
		if err != nil {
			return err
		}
		return writeEnvelope(connection, sequence, captureKind, payload)
	}
	if role == "viewer" {
		if requested == nil || requested.validateForMode(mode) != nil {
			return CaptureProfile{}, errors.New("capture request required")
		}
		if err := send(2, *requested); err != nil {
			return CaptureProfile{}, err
		}
		for sequence := uint64(2); sequence <= 3; sequence++ {
			agreed, err := receive(sequence)
			if err != nil || agreed != *requested {
				return CaptureProfile{}, errors.New("capture agreement changed the request")
			}
			if sequence == 2 {
				if err := send(3, agreed); err != nil {
					return CaptureProfile{}, err
				}
			}
		}
		return *requested, nil
	}
	if role != "worker" || requested != nil {
		return CaptureProfile{}, errors.New("invalid capture agreement role")
	}
	profile, err := receive(2)
	if err != nil {
		return CaptureProfile{}, err
	}
	if err := send(2, profile); err != nil {
		return CaptureProfile{}, err
	}
	confirmed, err := receive(3)
	if err != nil || confirmed != profile {
		return CaptureProfile{}, errors.New("capture agreement confirmation changed")
	}
	if err := send(3, profile); err != nil {
		return CaptureProfile{}, err
	}
	return profile, nil
}
