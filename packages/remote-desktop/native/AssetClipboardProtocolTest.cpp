#include "AssetClipboardProtocol.h"
#include "CaptureProfile.h"
#include <cassert>
#include <iostream>
#include <limits>

using namespace dockpipe::desktop;

namespace {
template<typename Action>
bool rejected(Action action)
{
    try {
        action();
    } catch (const std::invalid_argument &) {
        return true;
    }
    return false;
}
AssetRecord identity(AssetOperation operation)
{
    AssetRecord record;
    record.operation = operation;
    record.epoch = 1;
    record.serial = 1;
    record.transfer[0] = 1;
    return record;
}
}

int main()
{
    assert(!CaptureProfile{}.clipboardImages && !CaptureProfile{}.clipboardFiles && !CaptureProfile{}.clipboardText);
    AssetRecord arm;
    arm.operation = AssetOperation::Arm;
    arm.epoch = 1;
    const auto encodedArm = encodeAssetClipboardRecord(arm);
    assert(encodedArm.size() == 36 && encodedArm[0] == 1 && encodedArm[1] == 1 && encodedArm[11] == 1);
    for (std::size_t index = 12; index < encodedArm.size(); ++index)
        assert(encodedArm[index] == 0);
    for (const auto &name : {std::string(), std::string("."), std::string(".."), std::string("a/b"), std::string("a\\b"),
            std::string("a\0b", 3), std::string("\x1f"), std::string("\x7f"), std::string("\xc2\x85"),
            std::string("\xff"), std::string("\xc0\xaf"), std::string("\xed\xa0\x80"), std::string(256, 'a')})
        assert(!validAssetClipboardBasename(name));
    assert(validAssetClipboardBasename("café-日.txt"));
    auto offer = identity(AssetOperation::Offer);
    offer.items = {{1, AssetClipboardMaximumBatchBytes, "first"}};
    offer.totalBytes = AssetClipboardMaximumBatchBytes;
    for (std::uint32_t index = 2; index <= AssetClipboardMaximumFiles; ++index) {
        const auto prefix = std::to_string(index);
        offer.items.push_back({index, 0, prefix + std::string(255 - prefix.size(), 'x')});
    }
    const auto manifest = encodeAssetClipboardRecord(offer);
    assert(manifest.size() <= AssetClipboardHeaderBytes + AssetClipboardMaximumManifestBytes);
    assert(decodeAssetClipboardRecord(manifest.data(), manifest.size()).items.size() == 128);
    offer.items.push_back({129, 0, "overflow"});
    assert(rejected([&] { encodeAssetClipboardRecord(offer); }));
    offer.items = {{1, std::numeric_limits<std::uint64_t>::max(), "overflow"}};
    assert(rejected([&] { encodeAssetClipboardRecord(offer); }));
    offer.items = {{1, AssetClipboardMaximumBatchBytes, "first"}, {2, 1, "second"}};
    assert(rejected([&] { encodeAssetClipboardRecord(offer); }));
    offer.items = {{1, 0, "a"}, {2, 0, "A"}};
    offer.totalBytes = 0;
    assert(rejected([&] { encodeAssetClipboardRecord(offer); }));
    offer.items = {{1, AssetClipboardMaximumPngBytes, "clipboard.png"}};
    offer.totalBytes = AssetClipboardMaximumPngBytes;
    offer.format = AssetFormat::Png;
    offer.width = 4096;
    offer.height = 4096;
    const auto png = encodeAssetClipboardRecord(offer);
    assert(png.size() < 1024); // A64MiB declaration never materializes image bytes.
    ++offer.width;
    assert(rejected([&] { encodeAssetClipboardRecord(offer); }));
    offer.width = 4096;
    ++offer.totalBytes;
    ++offer.items[0].size;
    assert(rejected([&] { encodeAssetClipboardRecord(offer); }));
    auto chunk = identity(AssetOperation::Chunk);
    chunk.item = 1;
    chunk.data.resize(AssetClipboardMaximumChunkBytes);
    const auto maximumChunk = encodeAssetClipboardRecord(chunk);
    assert(maximumChunk.size() == AssetClipboardMaximumRecordBytes);
    chunk.data.push_back(0);
    assert(rejected([&] { encodeAssetClipboardRecord(chunk); }));
    auto accept = identity(AssetOperation::Accept);
    accept.window = 0;
    assert(rejected([&] { encodeAssetClipboardRecord(accept); }));
    accept.window = 4;
    auto epoch = arm;
    epoch.operation = AssetOperation::EpochAcknowledged;
    epoch.control = AssetOperation::Pause;
    assert(assetClipboardDirectionAllowed(epoch.operation, true, false));
    assert(!assetClipboardDirectionAllowed(epoch.operation, false, false));
    auto cancelled = identity(AssetOperation::CancelAcknowledged);
    for (const auto &record : {arm, epoch, accept, cancelled}) {
        const auto wire = encodeAssetClipboardRecord(record);
        const auto decoded = decodeAssetClipboardRecord(wire.data(), wire.size());
        assert(encodeAssetClipboardRecord(decoded) == wire);
        for (std::size_t size = 0; size < wire.size(); ++size)
            assert(rejected([&] { decodeAssetClipboardRecord(wire.data(), size); }));
        auto invalid = wire;
        invalid[2] = 1;
        assert(rejected([&] { decodeAssetClipboardRecord(invalid.data(), invalid.size()); }));
    }
    std::cout << "PASS richcodecUTF8/truncation/overflow/manifest128/64KiBchunks/PNGpixel+encodedbounds/cancel+epochACK/controlprofiledefaults\n";
}
