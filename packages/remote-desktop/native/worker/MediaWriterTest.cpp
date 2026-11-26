#include "MediaWriter.h"
#include "TestSps.h"
#include <cassert>
#include <csignal>
#include <fcntl.h>
#include <iostream>
#include <sys/ioctl.h>
#include <unistd.h>

using namespace dockpipe::desktop;

namespace {
EncodedFrame frame(bool keyframe, unsigned width = 1920, unsigned height = 1080, unsigned level = 40)
{
    EncodedFrame value;
    value.width = width;
    value.height = height;
    value.keyframe = keyframe;
    if (keyframe)
        value.annexB = test::annexB(width, height, level);
    else
        value.annexB = {0, 0, 0, 1, 0x41, 0x88};
    return value;
}

std::vector<std::uint8_t> readRecord(RecordReader &reader, int descriptor)
{
    auto outcome = RecordReader::Outcome::Pending;
    const auto deadline = Clock::now() + std::chrono::seconds(1);
    while (outcome == RecordReader::Outcome::Pending && Clock::now() < deadline) {
        outcome = reader.poll(descriptor, FrameCodec::MaximumBytes + 1);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(outcome == RecordReader::Outcome::Complete);
    return reader.take();
}

std::size_t fillPipe(int descriptor)
{
    const std::uint8_t byte = 0xee;
    std::size_t count = 0;
    while (write(descriptor, &byte, 1) == 1)
        ++count;
    assert(count >= 4);
    return count;
}

void discard(int descriptor, std::size_t length)
{
    std::uint8_t bytes[4096];
    while (length) {
        const auto count = read(descriptor, bytes, std::min(length, sizeof(bytes)));
        assert(count > 0);
        length -= static_cast<std::size_t>(count);
    }
}
}

int main()
{
    signal(SIGPIPE, SIG_IGN);
    int descriptors[2];
    assert(pipe(descriptors) == 0);
    assert(fcntl(descriptors[0], F_SETFL, O_NONBLOCK) == 0);
    assert(fcntl(descriptors[1], F_SETFL, O_NONBLOCK) == 0);
    std::atomic_uint requests{0};
    {
        MediaWriter writer(descriptors[1], [&] { ++requests; });
        auto oversized = frame(true);
        oversized.annexB.resize(FrameCodec::MaximumBytes - FrameCodec::HeaderBytes + 1);
        writer.submit(std::move(oversized));
        assert(requests == 1 && !writer.failed());
        writer.submit(frame(true));
        RecordReader reader;
        const auto record = readRecord(reader, descriptors[0]);
        assert(record[0] == 1);
        const auto decoded = FrameCodec::decode(record.data() + 1, record.size() - 1);
        assert(decoded.sequence == 1 && decoded.keyframe);
    }
    const H264ProfileBounds ultra{3840, 2160, 52};
    {
        const auto filled = fillPipe(descriptors[1]);
        constexpr std::size_t released = 4096;
        assert(filled >= released);
        discard(descriptors[0], released);
        MediaWriter writer(descriptors[1], [&] { ++requests; }, ultra);
        auto first = frame(true, 3840, 2160, 52);
        first.annexB.resize(16384, 0x88); // Large parser-only IDR, not a decode fixture.
        writer.submit(std::move(first));
        // Observe a record header after freeing one pipe page. Its larger body
        // cannot complete until the fixture drains more bytes, so overwrite
        // recovery is tested without depending on thread scheduling sleeps.
        int available = 0;
        const auto deadline = Clock::now() + std::chrono::milliseconds(150);
        while (available <= static_cast<int>(filled - released) && Clock::now() < deadline) {
            assert(ioctl(descriptors[0], FIONREAD, &available) == 0);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        assert(available > static_cast<int>(filled - released));
        const auto before = requests.load();
        writer.submit(frame(false, 3840, 2160, 52));
        writer.submit(frame(false, 3840, 2160, 52));
        assert(requests == before + 1);
        for (unsigned index = 0; index < 100; ++index)
            writer.submit(frame(false, 3840, 2160, 52));
        writer.submit(frame(true, 3840, 2160, 52));
        discard(descriptors[0], filled - released);
        RecordReader reader;
        for (std::uint64_t sequence : {1, 2}) {
            const auto record = readRecord(reader, descriptors[0]);
            assert(record[0] == 1 && record.size() <= FrameCodec::MaximumBytes + 1);
            const auto decoded = FrameCodec::decode(record.data() + 1, record.size() - 1, ultra);
            assert(decoded.sequence == sequence && decoded.keyframe && decoded.width == 3840 && decoded.height == 2160);
        }
        assert(reader.poll(descriptors[0], FrameCodec::MaximumBytes + 1) == RecordReader::Outcome::Pending);
        assert(!writer.failed());
    }
    {
        const auto filled = fillPipe(descriptors[1]);
        discard(descriptors[0], 4096);
        MediaWriter writer(descriptors[1], [&] { ++requests; });
        auto blocking = frame(true);
        blocking.annexB.resize(16384, 0x88);
        writer.submit(std::move(blocking));
        int available = 0;
        const auto deadline = Clock::now() + std::chrono::milliseconds(150);
        while (available <= static_cast<int>(filled - 4096) && Clock::now() < deadline) {
            assert(ioctl(descriptors[0], FIONREAD, &available) == 0);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        assert(available > static_cast<int>(filled - 4096));
        writer.clipboardEpoch(1);
        for (unsigned revision = 1; revision <= 8; ++revision)
            assert(writer.submitClipboard({ClipboardOperation::Acknowledged, 1, revision, {}}));
        assert(!writer.submitClipboard({ClipboardOperation::Acknowledged, 1, 9, {}}));
        writer.clipboardEpoch(2); // Retires queued old-epoch acknowledgements.
        assert(!writer.submitClipboard({ClipboardOperation::Changed, 1, 1, "old epoch"}));
        assert(writer.submitClipboard({ClipboardOperation::Changed, 2, 1, "before Write"}));
        assert(writer.submitClipboard({ClipboardOperation::Notice, 2, 0, "not_text"}));
        writer.discardClipboardObservations(2);
        assert(writer.submitClipboard({ClipboardOperation::Acknowledged, 2, 1, {}}));
        discard(descriptors[0], filled - 4096);
        RecordReader reader;
        assert(readRecord(reader, descriptors[0])[0] == 1); // No interleaved video/clipboard bytes.
        auto record = readRecord(reader, descriptors[0]);
        assert(record[0] == 4);
        auto clipboard = decodeClipboardRecord(record.data() + 1, record.size() - 1);
        assert(clipboard.operation == ClipboardOperation::Acknowledged && clipboard.revision == 1);
        assert(reader.poll(descriptors[0], FrameCodec::MaximumBytes + 1) == RecordReader::Outcome::Pending);
        // No old Changed or Notice survived the successful-Write discard. Block a new
        // video record to exercise latest-value coalescing independently.
        const auto filledAgain = fillPipe(descriptors[1]);
        discard(descriptors[0], 4096);
        auto blockingAgain = frame(true);
        blockingAgain.annexB.resize(16384, 0x88);
        writer.submit(std::move(blockingAgain));
        available = 0;
        const auto nextDeadline = Clock::now() + std::chrono::milliseconds(150);
        while (available <= static_cast<int>(filledAgain - 4096) && Clock::now() < nextDeadline) {
            assert(ioctl(descriptors[0], FIONREAD, &available) == 0);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        assert(available > static_cast<int>(filledAgain - 4096));
        for (unsigned index = 0; index < 1000; ++index)
            assert(writer.submitClipboard({ClipboardOperation::Changed, 2, 1, "latest " + std::to_string(index)}));
        discard(descriptors[0], filledAgain - 4096);
        assert(readRecord(reader, descriptors[0])[0] == 1);
        record = readRecord(reader, descriptors[0]);
        clipboard = decodeClipboardRecord(record.data() + 1, record.size() - 1);
        assert(clipboard.operation == ClipboardOperation::Changed && clipboard.revision == 1 && clipboard.text == "latest 999");
        assert(writer.submitClipboard({ClipboardOperation::Changed, 2, 1, "next"}));
        record = readRecord(reader, descriptors[0]);
        clipboard = decodeClipboardRecord(record.data() + 1, record.size() - 1);
        assert(clipboard.revision == 2 && clipboard.text == "next");
        writer.clipboardEpoch(0);
        assert(!writer.submitClipboard({ClipboardOperation::Acknowledged, 2, 2, {}}));
        writer.stop();
        assert(!writer.submitClipboard({ClipboardOperation::Notice, 2, 0, "unavailable"}));
        assert(!writer.failed());
    }
    {
        const auto filled = fillPipe(descriptors[1]);
        discard(descriptors[0], 4096);
        MediaWriter writer(descriptors[1], [&] { ++requests; });
        auto blocking = frame(true);
        blocking.annexB.resize(16384, 0x88);
        writer.submit(std::move(blocking));
        int available = 0;
        const auto deadline = Clock::now() + std::chrono::milliseconds(150);
        while (available <= static_cast<int>(filled - 4096) && Clock::now() < deadline) {
            assert(ioctl(descriptors[0], FIONREAD, &available) == 0);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        assert(available > static_cast<int>(filled - 4096));
        writer.clipboardEpoch(1);
        writer.submit(frame(true)); // Already pending while observations refill.
        assert(writer.submitClipboard({ClipboardOperation::Notice, 1, 0, "not_text"}));
        assert(writer.submitClipboard({ClipboardOperation::Changed, 1, 1, std::string(ClipboardMaximumTextBytes, 'c')}));
        discard(descriptors[0], filled - 4096);
        RecordReader reader;
        assert(readRecord(reader, descriptors[0])[0] == 1);
        auto record = readRecord(reader, descriptors[0]);
        assert(record[0] == 4);
        auto clipboard = decodeClipboardRecord(record.data() + 1, record.size() - 1);
        assert(clipboard.operation == ClipboardOperation::Notice);
        // Refill both observation slots while there is still a pending frame.
        // The next non-ACK output must be that video, rather than more clipboard.
        assert(writer.submitClipboard({ClipboardOperation::Notice, 1, 0, "unsupported_text"}));
        assert(writer.submitClipboard({ClipboardOperation::Changed, 1, 1, std::string(ClipboardMaximumTextBytes, 'z')}));
        record = readRecord(reader, descriptors[0]);
        assert(record[0] == 1);
        assert(FrameCodec::decode(record.data() + 1, record.size() - 1).sequence == 2);
        bool latestReceived = false;
        for (unsigned records = 0; records < 4 && !latestReceived; ++records) {
            record = readRecord(reader, descriptors[0]);
            assert(record[0] == 4);
            clipboard = decodeClipboardRecord(record.data() + 1, record.size() - 1);
            latestReceived = clipboard.operation == ClipboardOperation::Changed
                && clipboard.text == std::string(ClipboardMaximumTextBytes, 'z');
        }
        assert(latestReceived && !writer.failed());
    }
    for (const bool focusBarrier : {false, true}) {
        const auto filled = fillPipe(descriptors[1]);
        discard(descriptors[0], 4096);
        MediaWriter writer(descriptors[1], [&] { ++requests; });
        auto blocking = frame(true);
        blocking.annexB.resize(16384, 0x88);
        writer.submit(std::move(blocking));
        int available = 0;
        const auto deadline = Clock::now() + std::chrono::milliseconds(150);
        while (available <= static_cast<int>(filled - 4096) && Clock::now() < deadline) {
            assert(ioctl(descriptors[0], FIONREAD, &available) == 0);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        assert(available > static_cast<int>(filled - 4096));
        AssetClipboardRecord offer;
        offer.operation = AssetClipboardOperation::Offer;
        offer.epoch = 1;
        offer.serial = 1;
        offer.transfer[0] = 1;
        offer.totalBytes = 65536;
        offer.items = {{1, 65536, "selected.bin"}};
        assert(writer.submitAsset(offer));
        auto chunk = offer;
        chunk.operation = AssetClipboardOperation::Chunk;
        chunk.item = 1;
        chunk.data.assign(65536, 'x');
        assert(writer.submitAsset(chunk));
        if (focusBarrier) {
            AssetClipboardRecord acknowledged;
            acknowledged.operation = AssetClipboardOperation::EpochAcknowledged;
            acknowledged.epoch = 2;
            acknowledged.control = AssetClipboardOperation::Arm;
            assert(writer.submitAsset(acknowledged));
            acknowledged.control = AssetClipboardOperation::Pause;
            assert(writer.submitAsset(acknowledged)); // Pause arrives before blocked Arm ACK can leave the writer.
        } else {
            auto cancel = offer;
            cancel.operation = AssetClipboardOperation::Cancel;
            cancel.reason = "user_cancelled";
            assert(writer.submitAsset(cancel));
        }
        discard(descriptors[0], filled - 4096);
        RecordReader reader;
        assert(readRecord(reader, descriptors[0])[0] == 1); // Barrier never bypasses the in-flight record.
        auto record = readRecord(reader, descriptors[0]);
        assert(record[0] == 5);
        auto asset = decodeAssetClipboardRecord(record.data() + 1, record.size() - 1);
        if (focusBarrier) {
            assert(asset.operation == AssetClipboardOperation::EpochAcknowledged && asset.epoch == 2
                && asset.control == AssetClipboardOperation::Arm);
            record = readRecord(reader, descriptors[0]);
            asset = decodeAssetClipboardRecord(record.data() + 1, record.size() - 1);
            assert(asset.operation == AssetClipboardOperation::EpochAcknowledged && asset.epoch == 2
                && asset.control == AssetClipboardOperation::Pause);
        } else {
            assert(asset.operation == AssetClipboardOperation::Offer); // Introduce tuple before Cancel.
            record = readRecord(reader, descriptors[0]);
            asset = decodeAssetClipboardRecord(record.data() + 1, record.size() - 1);
            assert(asset.operation == AssetClipboardOperation::Cancel);
        }
        writer.submit(frame(true));
        assert(readRecord(reader, descriptors[0])[0] == 1); // No old chunk can follow either boundary.
        assert(!writer.failed());
    }
    fillPipe(descriptors[1]);
    {
        MediaWriter writer(descriptors[1], [&] { ++requests; }, ultra);
        writer.submit(frame(true, 3840, 2160, 52));
        const auto deadline = Clock::now() + std::chrono::seconds(1);
        while (!writer.failed() && Clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        assert(writer.failed()); // Unrelieved partial/backpressured record fails closed.
    }
    close(descriptors[0]);
    close(descriptors[1]);
    std::cout << "PASS default envelope/2MiB cap,4K60 bounded overwrite/P-frame discard/IDR recovery, fatal sustained backpressure,serialized clipboard ACK priority/coalescing/contiguous revisions/epoch retirement/8-ACK cap,video fairness under observation refill,superseded Notice removal\n";
}
