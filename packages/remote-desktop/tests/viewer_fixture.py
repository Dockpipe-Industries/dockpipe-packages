#!/usr/bin/python3
"""Local process fixture. No network, credentials, capture, or input injection."""
import hashlib
import json
import os
import select
import struct
import sys
import time
from pathlib import Path

root = Path(os.environ["DOCKPIPE_VIEWER_FIXTURE"])
behavior = (root / "behavior").read_text().strip()


def record(value):
    with (root / "events.jsonl").open("a") as output:
        output.write(json.dumps(value) + "\n")


def emit(value):
    print(json.dumps(value), flush=True)


def packet(descriptor, payload):
    data = struct.pack(">I", len(payload)) + payload
    while data:
        count = os.write(descriptor, data)
        data = data[count:]


if sys.argv[1] == "identity":
    emit({"fingerprint": "b" * 64})
    sys.exit(0)

if sys.argv[1] == "connect":
    configuration = json.loads(Path(sys.argv[3]).read_text())
    record({"event": "channel", "configuration": configuration})
    receipt = {"type": "ready", "reference": configuration["reference"],
               "role": "viewer", "mode": configuration["mode"], "capture": configuration["capture"]}
    if behavior == "wrong-capture-ready":
        receipt["capture"] = {"quality": "low", "frame_rate": 10}
    if behavior == "wrong-display-source-ready":
        receipt["capture"] = dict(configuration["capture"])
        receipt["capture"].pop("display_source", None)
    packet(4, json.dumps(receipt).encode())
    os.close(4)
    if behavior == "invalid-media":
        packet(1, b"\1invalid")
    if behavior in ("media", "profile-overflow", "media-gap"):
        annex_b = (root / "media.h264").read_bytes()
        width, height = (1920, 1080) if behavior == "profile-overflow" else (64, 64)
        header = struct.pack(">4sB3xIIQQ", b"DNP\1", 1, width, height, 0, 1)
        packet(1, b"\1" + header + annex_b)
    if behavior == "media-burst":
        annex_b = (root / "media.h264").read_bytes()
        burst = bytearray()
        for sequence in (1, 2):
            header = struct.pack(">4sB3xIIQQ", b"DNP\1", 1, 64, 64, (sequence - 1) * 16667, sequence)
            payload = b"\1" + header + annex_b
            burst += struct.pack(">I", len(payload)) + payload
        if len(burst) > os.fpathconf(1, "PC_PIPE_BUF") or os.write(1, burst) != len(burst):
            raise RuntimeError("Could not emit a single bounded media burst")
    buffer = b""
    gap_sent = False
    recovery_sent = False
    clipboard_replies = []
    clipboard_epoch = 0
    clipboard_revision = 0
    asset_transfers = {}
    asset_commits = []
    while True:
        if behavior == "assets-auto":
            trigger = root / "asset-commit"
            if trigger.exists():
                trigger.unlink()
                for reply in asset_commits:
                    packet(1, reply)
                    record({"event": "asset-committed"})
                asset_commits.clear()
            readable, _, _ = select.select([0], [], [], 0.02)
            if not readable:
                continue
        if behavior.startswith("clipboard"):
            for trigger in ("ack", "changed", "old", "future", "notice"):
                path = root / ("clipboard-" + trigger)
                if not path.exists():
                    continue
                path.unlink()
                if trigger == "ack":
                    for reply in clipboard_replies:
                        packet(1, reply)
                    clipboard_replies.clear()
                elif trigger == "notice":
                    packet(1, b"\4" + struct.pack(">BBQQ", 1, 6, clipboard_epoch, 0) + b"unavailable")
                else:
                    epoch = clipboard_epoch + (1 if trigger == "future" else -1 if trigger == "old" else 0)
                    if trigger == "changed":
                        clipboard_revision += 1
                    packet(1, b"\4" + struct.pack(">BBQQ", 1, 4, epoch, clipboard_revision or 1) + "remote λ".encode())
            readable, _, _ = select.select([0], [], [], 0.02)
            if not readable:
                continue
        if behavior == "media-gap":
            if not gap_sent and (root / "gap-trigger").exists():
                gap_sent = True
                header = struct.pack(">4sB3xIIQQ", b"DNP\1", 0, 64, 64, 100000, 3)
                packet(1, b"\1" + header + (root / "media.h264").read_bytes())
            readable, _, _ = select.select([0], [], [], 0.02)
            if not readable:
                continue
        chunk = os.read(0, 65536)
        if not chunk:
            break
        buffer += chunk
        while len(buffer) >= 4:
            length = struct.unpack(">I", buffer[:4])[0]
            if len(buffer) < length + 4:
                break
            payload = buffer[4:length + 4]
            buffer = buffer[length + 4:]
            value = {"event": "packet", "kind": payload[0]}
            if payload[0] == 3:
                (root / "channel-attached").touch()
            if payload[0] == 2:
                value["input"] = json.loads(payload[1:])
            if payload[0] == 5:
                version, operation, flags, epoch, serial, transfer = struct.unpack(">BBHQQ16s", payload[1:37])
                value.update(operation=operation, epoch=epoch, serial=serial, asset_bytes=len(payload) - 37)
                if operation in (1, 2):
                    asset_transfers.clear()
                    asset_commits.clear()
                    packet(1, b"\5" + struct.pack(">BBHQQ16sB", 1, 11, 0, epoch, 0, bytes(16), operation))
                elif behavior == "assets-auto":
                    identity = (epoch, serial, transfer)
                    body = payload[37:]
                    def asset_reply(reply_operation, data=b""):
                        return b"\5" + struct.pack(">BBHQQ16s", 1, reply_operation, 0, epoch, serial, transfer) + data
                    if operation == 3:
                        image_format, width, height, item_count, total = struct.unpack(">BIIHQ", body[:19])
                        position = 19
                        sizes = {}
                        for _ in range(item_count):
                            item, size, name_length = struct.unpack(">IQH", body[position:position + 14])
                            sizes[item] = size
                            position += 14 + name_length
                        if position != len(body) or sum(sizes.values()) != total:
                            raise RuntimeError("Invalid generated asset manifest")
                        asset_transfers[identity] = {"sizes": sizes, "hashes": {}, "offsets": {}, "finished": 0}
                        packet(1, asset_reply(4, struct.pack(">QB", total, 4)))
                    elif operation in (5, 7):
                        item, offset = struct.unpack(">IQ", body[:12])
                        state = asset_transfers[identity]
                        digest = state["hashes"].setdefault(item, hashlib.sha256())
                        if operation == 5:
                            if offset != state["offsets"].get(item, 0):
                                raise RuntimeError("Generated asset chunk offset changed")
                            digest.update(body[12:])
                            state["offsets"][item] = offset + len(body) - 12
                            packet(1, asset_reply(6, struct.pack(">IQB", item, state["offsets"][item], 0)))
                        else:
                            if offset != state["sizes"][item] or offset != state["offsets"].get(item, 0) or digest.digest() != body[12:]:
                                raise RuntimeError("Generated asset finish verification failed")
                            state["finished"] += 1
                            if state["finished"] < len(state["sizes"]):
                                packet(1, asset_reply(6, struct.pack(">IQB", item, offset, 1)))
                            else:
                                asset_commits.append(asset_reply(8))
                                record({"event": "asset-ready-to-commit", "item_count": len(state["sizes"]), "verified": True})
                    elif operation == 9:
                        asset_transfers.pop(identity, None)
                        asset_commits.clear()
                        packet(1, asset_reply(10))
            if payload[0] == 4:
                version, operation, epoch, revision = struct.unpack(">BBQQ", payload[1:19])
                value.update(operation=operation, epoch=epoch, revision=revision, text_bytes=len(payload) - 19)
                if operation == 1:
                    clipboard_epoch = epoch
                    clipboard_revision = 0
                if operation == 3:
                    clipboard_replies.append(b"\4" + struct.pack(">BBQQ", 1, 5, epoch, revision))
            record(value)
            if behavior == "media-gap" and gap_sent and not recovery_sent and payload[0] == 3:
                recovery_sent = True
                header = struct.pack(">4sB3xIIQQ", b"DNP\1", 1, 64, 64, 200000, 4)
                packet(1, b"\1" + header + (root / "recovered.h264").read_bytes())
    sys.exit(0)

if sys.argv[1:3] != ["remote", "desktop"]:
    sys.exit(100)
request_path = Path(sys.argv[sys.argv.index("--request") + 1])
request = json.loads(request_path.read_text())
reference = {key: request[key] for key in ("schema", "target", "epoch")}
reference["id"] = "c" * 64
prepared = {"type": "prepared", "reference": reference,
            "provider": request["provider"], "mode": request["mode"], "deadline": request["deadline"]}
if behavior == "wrong-target":
    prepared = json.loads(json.dumps(prepared))
    prepared["reference"]["target"]["node"] = "other"
emit(prepared)
line = sys.stdin.readline()
if not line:
    sys.exit(0)
ack = json.loads(line)
record({"event": "ack", "ack": ack})
if ack.get("type") != "prepared-ack" or ack.get("reference") != reference:
    sys.exit(101)
view = {"reference": reference, "state": "queued", "lease_millis": 9000, "ack": 0}
emit({"type": "view", "view": view})
started = time.monotonic()
acknowledge_at = None
while True:
    if behavior == "control-loss" and (root / "channel-attached").exists() and time.monotonic() - started > 0.9:
        sys.exit(0)
    ready, _, _ = select.select([sys.stdin], [], [], 0.1)
    if not ready:
        if acknowledge_at is not None and time.monotonic() >= acknowledge_at and view["ack"] == 0:
            view["ack"] = 1
            record({"event": "binding-authorized"})
        emit({"type": "view", "view": view})
        continue
    line = sys.stdin.readline()
    if not line:
        break
    message = json.loads(line)
    record({"event": "control", "message": message})
    if message.get("type") == "signal":
        view["state"] = "opening"
        view["signals"] = [{"reference": reference, "type": "binding", "sequence": 1,
                            "fingerprint": "d" * 64}]
        emit({"type": "view", "view": view})
        view.pop("signals")
        acknowledge_at = time.monotonic() + 0.25
    if message.get("type") == "close":
        if behavior == "late-close":
            time.sleep(3)
        if behavior == "close-timeout":
            time.sleep(20)
        if behavior == "close-revoke":
            break
        view["state"] = "unknown" if behavior == "close-unknown" else "closed"
        view["lease_millis"] = 0
        if behavior == "close-foreign":
            view["reference"] = dict(reference, id="9" * 64)
        emit({"type": "view", "view": view})
        break
