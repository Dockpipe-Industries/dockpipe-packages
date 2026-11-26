package main

import (
	"crypto/sha256"
	"crypto/subtle"
	"hash"
	"sync"
	"time"
)

type assetPhase byte

const (
	assetOffered assetPhase = iota
	assetAccepted
	assetFinished
	assetComplete
)

type assetTransfer struct {
	identity      assetRecord
	phase         assetPhase
	items         []assetItem
	current       int
	sent          uint64
	credited      uint64
	boundaries    [assetWindow]uint64
	outstanding   int
	hasher        hash.Hash
	progress      time.Time
	deadline      time.Time
	retiringEpoch bool
	expired       bool
	cancelledBy   uint8
	acknowledged  uint8
	drainStarted  bool
	drainChunks   int
	drainMetadata int
}

func (transfer *assetTransfer) beginDrain() {
	if !transfer.drainStarted {
		transfer.drainStarted = true
		transfer.drainChunks = assetWindow
		transfer.drainMetadata = 8
	}
}

func (transfer *assetTransfer) advance(record assetRecord, origin bool) error {
	switch record.op {
	case assetAccept:
		if origin || transfer.phase != assetOffered || record.grant != transfer.identity.total || record.window != assetWindow {
			return errAsset
		}
		transfer.phase = assetAccepted
		transfer.hasher = sha256.New()
	case assetChunk:
		if !origin || transfer.phase != assetAccepted || transfer.current >= len(transfer.items) {
			return errAsset
		}
		item := transfer.items[transfer.current]
		if record.item != item.id || record.offset != transfer.sent || transfer.sent >= item.size ||
			uint64(len(record.data)) != min(uint64(assetMaximumChunk), item.size-transfer.sent) || transfer.outstanding >= assetWindow {
			return errAsset
		}
		transfer.sent += uint64(len(record.data))
		transfer.boundaries[transfer.outstanding] = transfer.sent
		transfer.outstanding++
		_, _ = transfer.hasher.Write(record.data)
	case assetCredit:
		if origin || transfer.current >= len(transfer.items) {
			return errAsset
		}
		item := transfer.items[transfer.current]
		if record.item != item.id {
			return errAsset
		}
		if record.verified {
			if transfer.phase != assetFinished || transfer.current == len(transfer.items)-1 || record.offset != item.size {
				return errAsset
			}
			transfer.current++
			transfer.sent = 0
			transfer.credited = 0
			transfer.outstanding = 0
			transfer.hasher = sha256.New()
			transfer.phase = assetAccepted
		} else {
			if transfer.phase != assetAccepted || record.offset <= transfer.credited {
				return errAsset
			}
			boundary := -1
			for index := 0; index < transfer.outstanding; index++ {
				if transfer.boundaries[index] == record.offset {
					boundary = index
					break
				}
			}
			if boundary < 0 {
				return errAsset
			}
			copy(transfer.boundaries[:], transfer.boundaries[boundary+1:transfer.outstanding])
			transfer.outstanding -= boundary + 1
			transfer.credited = record.offset
		}
	case assetFinish:
		if !origin || transfer.phase != assetAccepted || transfer.current >= len(transfer.items) {
			return errAsset
		}
		item := transfer.items[transfer.current]
		if record.item != item.id || record.offset != item.size || transfer.sent != item.size || transfer.credited != item.size ||
			transfer.outstanding != 0 || subtle.ConstantTimeCompare(record.digest[:], transfer.hasher.Sum(nil)) != 1 {
			return errAsset
		}
		transfer.phase = assetFinished
	case assetCommitted:
		if origin || transfer.phase != assetFinished || transfer.current != len(transfer.items)-1 {
			return errAsset
		}
		transfer.phase = assetComplete
		transfer.items = nil
		transfer.identity.items = nil
		transfer.hasher = nil
	default:
		return errAsset
	}
	return nil
}

type assetReceipt struct {
	identity  assetRecord
	remaining int
}

type assetEpochControl struct {
	epoch uint64
	op    byte
}

// One shared state guards both concurrent authenticated directions. A terminal
// receipt holds only an exact identity and eight cleanup slots, never file data.
type assetSession struct {
	mu           sync.Mutex
	profile      CaptureProfile
	epoch        uint64
	armed        bool
	desiredArmed bool
	closed       bool
	disabled     bool
	controls     []assetEpochControl
	crossedOffer [2]bool
	offerEpoch   [2]uint64
	offerSerial  [2]uint64
	transfers    [2]*assetTransfer
	retired      [2]*assetReceipt
	deadline     time.Time
	now          func() time.Time
}

func newAssetSession(profile CaptureProfile, deadline time.Time) *assetSession {
	return &assetSession{profile: profile, deadline: deadline, now: time.Now}
}

func assetRole(role string) (int, bool) {
	switch role {
	case "viewer":
		return 0, true
	case "worker":
		return 1, true
	}
	return 0, false
}

func assetIdentityOnly(record assetRecord) assetRecord {
	return assetRecord{epoch: record.epoch, serial: record.serial, transfer: record.transfer}
}

func (session *assetSession) retire(origin int) {
	transfer := session.transfers[origin]
	if transfer != nil {
		session.retired[origin] = &assetReceipt{identity: assetIdentityOnly(transfer.identity), remaining: 8}
		session.transfers[origin] = nil
	}
}

func (session *assetSession) close() {
	session.mu.Lock()
	defer session.mu.Unlock()
	session.closed = true
	session.armed = false
	session.controls = nil
	session.transfers = [2]*assetTransfer{}
	session.retired = [2]*assetReceipt{}
}

func (session *assetSession) expire(now time.Time) {
	for _, transfer := range session.transfers {
		if transfer == nil || transfer.phase == assetComplete {
			continue
		}
		if transfer.cancelledBy != 0 && !now.Before(transfer.progress.Add(10*time.Second)) {
			session.disabled = true
		}
		if !now.Before(transfer.deadline) || !now.Before(transfer.progress.Add(10*time.Second)) {
			transfer.expired = true
			transfer.beginDrain()
		}
	}
}

func (session *assetSession) epochControl(record assetRecord, role string) (bool, error) {
	if role != "viewer" {
		return false, errAsset
	}
	if record.op == assetArm {
		if record.epoch <= session.epoch || len(session.controls) != 0 || session.disabled {
			return false, errAsset
		}
		session.epoch = record.epoch
		session.desiredArmed = true
	} else {
		if record.epoch != session.epoch || !session.desiredArmed || len(session.controls) > 1 ||
			len(session.controls) == 1 && session.controls[0].op != assetArm {
			return false, errAsset
		}
		session.desiredArmed = false
	}
	if len(session.controls) == 0 {
		for origin, transfer := range session.transfers {
			session.crossedOffer[origin] = transfer != nil && transfer.phase != assetComplete
		}
	}
	session.armed = false
	session.controls = append(session.controls, assetEpochControl{epoch: record.epoch, op: record.op})
	for _, transfer := range session.transfers {
		if transfer != nil {
			transfer.retiringEpoch = true
			transfer.beginDrain()
		}
	}
	return true, nil
}

func (session *assetSession) epochAcknowledged(record assetRecord, role string) (bool, error) {
	if role != "worker" || len(session.controls) == 0 || session.controls[0].epoch != record.epoch ||
		session.controls[0].op != record.control {
		return false, errAsset
	}
	session.controls = session.controls[1:]
	if len(session.controls) == 0 {
		for origin := range session.transfers {
			session.retire(origin)
		}
		session.armed = session.desiredArmed && record.control == assetArm && !session.disabled
	}
	return true, nil
}

func (session *assetSession) offer(record assetRecord, origin int, now time.Time) (bool, error) {
	if session.disabled || record.format == assetPNG && !session.profile.ClipboardImages ||
		record.format == assetFiles && !session.profile.ClipboardFiles || record.epoch > session.epoch {
		return false, errAsset
	}
	retiring := !session.armed || record.epoch != session.epoch
	// A worker offer can cross Arm/Pause in the reverse stream before its epoch
	// ACK. Keep exactly the one origin slot, never expose it to the OS boundary.
	if retiring && (origin != 1 || len(session.controls) == 0 || session.crossedOffer[origin] ||
		record.epoch == session.epoch && session.controls[0].op != assetPause) {
		return false, errAsset
	}
	previous := session.transfers[origin]
	if previous != nil && previous.phase != assetComplete {
		return false, errAsset
	}
	if record.epoch < session.offerEpoch[origin] ||
		record.epoch == session.offerEpoch[origin] && (session.offerSerial[origin] == ^uint64(0) || record.serial != session.offerSerial[origin]+1) ||
		record.epoch > session.offerEpoch[origin] && record.serial != 1 {
		return false, errAsset
	}
	for index := range session.transfers {
		if current := session.transfers[index]; current != nil && current.identity.transfer == record.transfer {
			return false, errAsset
		}
		if receipt := session.retired[index]; receipt != nil && receipt.identity.transfer == record.transfer {
			return false, errAsset
		}
	}
	session.retire(origin)
	session.offerEpoch[origin] = record.epoch
	session.offerSerial[origin] = record.serial
	transfer := &assetTransfer{identity: record, items: record.items, phase: assetOffered, progress: now,
		deadline: minTime(session.deadline, now.Add(10*time.Minute)), retiringEpoch: retiring}
	if retiring {
		session.crossedOffer[origin] = true
		transfer.beginDrain()
	}
	session.transfers[origin] = transfer
	return !retiring, nil
}

func (session *assetSession) cancel(record assetRecord, sender, origin int, transfer *assetTransfer, now time.Time) (bool, error) {
	if transfer.drainMetadata <= 0 {
		return false, errAsset
	}
	transfer.drainMetadata--
	if record.op == assetCancel {
		bit := uint8(1 << sender)
		if transfer.cancelledBy&bit != 0 {
			return false, nil
		}
		transfer.cancelledBy |= bit
		transfer.progress = now
		return !transfer.retiringEpoch, nil
	}
	request := uint8(1 << (1 - sender))
	if transfer.cancelledBy&request == 0 {
		return false, errAsset
	}
	if transfer.acknowledged&request != 0 {
		return false, nil
	}
	transfer.acknowledged |= request
	if transfer.acknowledged == transfer.cancelledBy {
		session.retire(origin)
	}
	return !transfer.retiringEpoch, nil
}

// false,nil drains only bounded, structurally valid records of an exact retiring
// identity. Unknown IDs and malformed packets remain protocol failures.
func (session *assetSession) admit(payload []byte, role string) (bool, error) {
	record, err := decodeAssetRecord(payload)
	if err != nil {
		return false, err
	}
	sender, validRole := assetRole(role)
	if !validRole || !session.profile.ClipboardImages && !session.profile.ClipboardFiles {
		return false, errAsset
	}
	session.mu.Lock()
	defer session.mu.Unlock()
	if session.closed || !session.now().Before(session.deadline) {
		return false, errAsset
	}
	now := session.now()
	session.expire(now)
	switch record.op {
	case assetArm, assetPause:
		return session.epochControl(record, role)
	case assetEpochAcknowledged:
		return session.epochAcknowledged(record, role)
	case assetOffer:
		return session.offer(record, sender, now)
	}
	var transfer *assetTransfer
	var origin int
	for index, candidate := range session.transfers {
		if candidate != nil && sameAssetIdentity(candidate.identity, record) {
			transfer = candidate
			origin = index
			break
		}
	}
	if transfer == nil {
		if record.op == assetCancel || record.op == assetCancelAcknowledged {
			for _, receipt := range session.retired {
				if receipt != nil && receipt.remaining > 0 && sameAssetIdentity(receipt.identity, record) {
					receipt.remaining--
					return false, nil
				}
			}
		}
		return false, errAsset
	}
	if record.op == assetCancel || record.op == assetCancelAcknowledged {
		transfer.beginDrain()
		return session.cancel(record, sender, origin, transfer, now)
	}
	draining := transfer.retiringEpoch || transfer.expired || transfer.cancelledBy != 0
	if draining {
		if transfer.cancelledBy&(1<<sender) != 0 {
			return false, errAsset // Nothing follows that sender's own terminal Cancel.
		}
		if record.op == assetChunk {
			if transfer.drainChunks <= 0 {
				return false, errAsset
			}
			transfer.drainChunks--
		} else {
			if transfer.drainMetadata <= 0 {
				return false, errAsset
			}
			transfer.drainMetadata--
		}
	}
	if err := transfer.advance(record, sender == origin); err != nil {
		return false, err
	}
	if !draining {
		transfer.progress = now
	}
	return !draining, nil
}
