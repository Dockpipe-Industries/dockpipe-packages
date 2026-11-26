#pragma once

#include "ChildProcess.h"
#include "WorkerProtocol.h"
#include "../ClipboardProtocol.h"
#include "../AssetClipboardProtocol.h"

namespace dockpipe::desktop {
struct WorkerChannelMessage {
    enum class Kind { None, Stop, Keyframe, Input, Clipboard, Asset };
    Kind kind = Kind::None;
    InputEvent input;
    ClipboardRecord clipboard;
    AssetClipboardRecord asset;
};

// The helper's verified Ready receipt must precede this established-channel
// path. Validate buffered data before interpreting exit as normal closure;
// malformed/truncated data never becomes a successful EOF. No event is posted
// after an observed helper exit, even when its last complete record was valid.
inline WorkerChannelMessage workerChannelMessage(bool backendCreated, bool childExited,
    RecordReader::Outcome outcome, const std::vector<std::uint8_t> &record, bool control, bool clipboardText = false, bool clipboardImages = false, bool clipboardFiles = false)
{
    require(backendCreated, "Encrypted channel never became ready.");
    require(outcome != RecordReader::Outcome::Invalid, "Native input transport is malformed.");
    WorkerChannelMessage result;
    if (outcome == RecordReader::Outcome::Complete) {
        require(record.size() >= 2, "Empty native input record.");
        if (record[0] == 3) {
            require(record.size() == 2 && record[1] == 1, "Invalid native media control.");
            result.kind = WorkerChannelMessage::Kind::Keyframe;
        } else if (record[0] == 4) {
            require(control && clipboardText, "Native clipboard exceeds negotiated authority.");
            result.clipboard = decodeClipboardRecord(record.data() + 1, record.size() - 1);
            require(clipboardDirectionAllowed(result.clipboard.operation, false), "Invalid viewer clipboard direction.");
            result.kind = WorkerChannelMessage::Kind::Clipboard;
        } else if (record[0] == 5) {
            require(control && (clipboardImages || clipboardFiles), "Native assets exceed negotiated authority.");
            result.asset = decodeAssetClipboardRecord(record.data() + 1, record.size() - 1);
            require(assetClipboardDirectionAllowed(result.asset.operation, false, false)
                || assetClipboardDirectionAllowed(result.asset.operation, false, true), "Invalid viewer asset direction.");
            result.kind = WorkerChannelMessage::Kind::Asset;
        } else {
            require(record[0] == 2 && control && record.size() <= 4097, "Native input exceeds view-only authority.");
            result.input = parseInput(Json::parse(record.begin() + 1, record.end()));
            result.kind = WorkerChannelMessage::Kind::Input;
        }
    }
    if (childExited || outcome == RecordReader::Outcome::Closed)
        result.kind = WorkerChannelMessage::Kind::Stop;
    return result;
}
// Leave established records in the bounded helper pipe until native capture
// has started successfully. In particular, a focused viewer may send Watch as
// soon as TLS readiness arrives, before the native start callback completes.
inline WorkerChannelMessage pollWorkerChannel(RecordReader &reader, int descriptor,
    WorkerProtocol::Phase phase, bool childExited, bool control, bool clipboardText, bool clipboardImages = false, bool clipboardFiles = false)
{
    if (phase != WorkerProtocol::Phase::Running) {
        WorkerChannelMessage result;
        if (childExited)
            result.kind = WorkerChannelMessage::Kind::Stop;
        return result;
    }
    const auto incoming = reader.poll(descriptor, AssetClipboardMaximumRecordBytes + 1);
    std::vector<std::uint8_t> record;
    if (incoming == RecordReader::Outcome::Complete)
        record = reader.take();
    return workerChannelMessage(true, childExited, incoming, record, control, clipboardText, clipboardImages, clipboardFiles);
}
} // namespace dockpipe::desktop
