#pragma once

#include "ClipboardProtocol.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <set>
#include <string>
#include <vector>

namespace dockpipe::desktop {
constexpr std::size_t AssetClipboardHeaderBytes = 36;
constexpr std::size_t AssetClipboardMaximumChunkBytes = 65536;
constexpr std::size_t AssetClipboardMaximumManifestBytes = 65536;
constexpr std::uint64_t AssetClipboardMaximumPngBytes = 67108864;
constexpr std::uint64_t AssetClipboardMaximumPixels = 16777216;
constexpr std::size_t AssetClipboardMaximumFiles = 128;
constexpr std::uint64_t AssetClipboardMaximumBatchBytes = 1073741824;
constexpr std::uint64_t AssetClipboardCacheQuotaBytes = 2147483648;
constexpr std::uint8_t AssetClipboardWindowChunks = 4;
constexpr std::size_t AssetClipboardMaximumRecordBytes = AssetClipboardHeaderBytes + 12 + AssetClipboardMaximumChunkBytes;

enum class AssetClipboardOperation : std::uint8_t {
    Arm = 1,
    Pause = 2,
    Offer = 3,
    Accept = 4,
    Chunk = 5,
    Credit = 6,
    Finish = 7,
    Committed = 8,
    Cancel = 9,
    CancelAcknowledged = 10,
    CancelAck = CancelAcknowledged,
    EpochAcknowledged = 11,
    EpochAck = EpochAcknowledged
};
enum class AssetClipboardFormat : std::uint8_t {
    Png = 1,
    Files = 2
};

struct AssetClipboardItem {
    std::uint32_t id = 0;
    std::uint64_t size = 0;
    std::string name;
};
struct AssetClipboardRecord {
    AssetClipboardOperation operation = AssetClipboardOperation::Pause;
    std::uint64_t epoch = 0;
    std::uint64_t serial = 0;
    std::array<std::uint8_t, 16> transfer{};
    AssetClipboardFormat format = AssetClipboardFormat::Files;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint64_t totalBytes = 0;
    std::vector<AssetClipboardItem> items;
    std::uint64_t grantBytes = 0;
    std::uint8_t window = 0;
    std::uint32_t item = 0;
    std::uint64_t offset = 0; // Finish: exact completed per-item size.
    std::vector<std::uint8_t> data;
    std::array<std::uint8_t, 32> sha256{};
    bool verified = false;
    std::string reason;
    AssetClipboardOperation control = AssetClipboardOperation::Arm;
};
using AssetRecord = AssetClipboardRecord;
using AssetItem = AssetClipboardItem;
using AssetOperation = AssetClipboardOperation;
using AssetFormat = AssetClipboardFormat;

inline bool validAssetClipboardBasename(const std::string &name)
{
    if (name.empty() || name.size() > 255 || !validClipboardText(name) || name == "." || name == "..")
        return false;
    for (std::size_t index = 0; index < name.size(); ++index) {
        const auto value = static_cast<unsigned char>(name[index]);
        if (value < 0x20 || value == 0x7f || value == '/' || value == '\\')
            return false;
        if (value == 0xc2 && index + 1 < name.size()) {
            const auto next = static_cast<unsigned char>(name[index + 1]);
            if (next >= 0x80 && next <= 0x9f)
                return false;
        }
    }
    return true;
}
inline bool validAssetClipboardReason(const std::string &reason)
{
    return reason == "unavailable" || reason == "quota" || reason == "source_changed"
        || reason == "hash_mismatch" || reason == "stale_epoch" || reason == "timeout"
        || reason == "unsupported" || reason == "user_cancelled" || reason == "invalid";
}
inline bool assetClipboardDirectionAllowed(AssetClipboardOperation operation, bool senderIsWorker, bool originIsWorker)
{
    switch (operation) {
    case AssetClipboardOperation::Arm:
    case AssetClipboardOperation::Pause:
        return !senderIsWorker;
    case AssetClipboardOperation::Offer:
    case AssetClipboardOperation::Chunk:
    case AssetClipboardOperation::Finish:
        return senderIsWorker == originIsWorker;
    case AssetClipboardOperation::Accept:
    case AssetClipboardOperation::Credit:
    case AssetClipboardOperation::Committed:
        return senderIsWorker != originIsWorker;
    case AssetClipboardOperation::Cancel:
    case AssetClipboardOperation::CancelAcknowledged:
        return true;
    case AssetClipboardOperation::EpochAcknowledged:
        return senderIsWorker;
    }
    return false;
}

namespace asset_clipboard_codec {
inline void check(bool condition)
{
    if (!condition)
        throw std::invalid_argument("Invalid asset clipboard record.");
}
inline void append(std::vector<std::uint8_t> &output, std::uint64_t value, unsigned bytes)
{
    for (unsigned remaining = bytes; remaining; --remaining)
        output.push_back(static_cast<std::uint8_t>(value >> ((remaining - 1) * 8)));
}
class Reader final {
public:
    Reader(const std::uint8_t *data, std::size_t size) : m_data(data), m_size(size) {}
    std::uint64_t number(unsigned bytes)
    {
        check(bytes <= m_size - m_position);
        std::uint64_t result = 0;
        for (unsigned index = 0; index < bytes; ++index)
            result = (result << 8) | m_data[m_position++];
        return result;
    }
    std::vector<std::uint8_t> bytes(std::size_t count)
    {
        check(count <= m_size - m_position);
        std::vector<std::uint8_t> result(m_data + m_position, m_data + m_position + count);
        m_position += count;
        return result;
    }
    std::size_t remaining() const { return m_size - m_position; }
private:
    const std::uint8_t *m_data;
    std::size_t m_size;
    std::size_t m_position = 0;
};
}

inline void validateAssetClipboardRecord(const AssetClipboardRecord &record)
{
    using namespace asset_clipboard_codec;
    check(record.epoch != 0);
    const bool zeroID = std::all_of(record.transfer.begin(), record.transfer.end(), [](auto value) { return value == 0; });
    if (record.operation == AssetClipboardOperation::Arm || record.operation == AssetClipboardOperation::Pause
        || record.operation == AssetClipboardOperation::EpochAcknowledged) {
        check(record.serial == 0 && zeroID);
        if (record.operation == AssetClipboardOperation::EpochAcknowledged)
            check(record.control == AssetClipboardOperation::Arm || record.control == AssetClipboardOperation::Pause);
        return;
    }
    check(record.serial != 0 && !zeroID);
    switch (record.operation) {
    case AssetClipboardOperation::Offer: {
        check(record.format == AssetClipboardFormat::Png || record.format == AssetClipboardFormat::Files);
        check(!record.items.empty() && record.items.size() <= AssetClipboardMaximumFiles);
        std::uint64_t total = 0;
        std::size_t manifestBytes = 19;
        std::set<std::string> names;
        for (std::size_t index = 0; index < record.items.size(); ++index) {
            const auto &item = record.items[index];
            check(item.id == index + 1 && validAssetClipboardBasename(item.name));
            check(item.size <= AssetClipboardMaximumBatchBytes - total);
            total += item.size;
            manifestBytes += 14 + item.name.size();
            auto folded = item.name;
            for (auto &character : folded) {
                if (character >= 'A' && character <= 'Z')
                    character = static_cast<char>(character + ('a' - 'A'));
            }
            check(names.insert(folded).second);
        }
        check(manifestBytes <= AssetClipboardMaximumManifestBytes && total == record.totalBytes);
        if (record.format == AssetClipboardFormat::Png) {
            check(record.items.size() == 1 && record.items[0].name == "clipboard.png"
                && total > 0 && total <= AssetClipboardMaximumPngBytes
                && record.width > 0 && record.height > 0
                && std::uint64_t(record.width) * record.height <= AssetClipboardMaximumPixels);
        } else {
            check(record.width == 0 && record.height == 0);
        }
        break;
    }
    case AssetClipboardOperation::Accept:
        check(record.grantBytes <= AssetClipboardMaximumBatchBytes && record.window == AssetClipboardWindowChunks);
        break;
    case AssetClipboardOperation::Chunk:
        check(record.item >= 1 && record.item <= AssetClipboardMaximumFiles && !record.data.empty()
            && record.data.size() <= AssetClipboardMaximumChunkBytes && record.offset <= AssetClipboardMaximumBatchBytes
            && record.data.size() <= AssetClipboardMaximumBatchBytes - record.offset);
        break;
    case AssetClipboardOperation::Credit:
    case AssetClipboardOperation::Finish:
        check(record.item >= 1 && record.item <= AssetClipboardMaximumFiles && record.offset <= AssetClipboardMaximumBatchBytes);
        break;
    case AssetClipboardOperation::Committed:
    case AssetClipboardOperation::CancelAcknowledged:
        break;
    case AssetClipboardOperation::Cancel:
        check(validAssetClipboardReason(record.reason));
        break;
    default:
        check(false);
    }
}

inline std::vector<std::uint8_t> encodeAssetClipboardRecord(const AssetClipboardRecord &record)
{
    validateAssetClipboardRecord(record);
    using namespace asset_clipboard_codec;
    std::vector<std::uint8_t> output;
    output.reserve(AssetClipboardHeaderBytes + 64);
    output.push_back(1);
    output.push_back(static_cast<std::uint8_t>(record.operation));
    append(output, 0, 2);
    append(output, record.epoch, 8);
    append(output, record.serial, 8);
    output.insert(output.end(), record.transfer.begin(), record.transfer.end());
    switch (record.operation) {
    case AssetClipboardOperation::EpochAcknowledged:
        append(output, static_cast<std::uint8_t>(record.control), 1);
        break;
    case AssetClipboardOperation::Offer:
        append(output, static_cast<std::uint8_t>(record.format), 1);
        append(output, record.width, 4);
        append(output, record.height, 4);
        append(output, record.items.size(), 2);
        append(output, record.totalBytes, 8);
        for (const auto &item : record.items) {
            append(output, item.id, 4);
            append(output, item.size, 8);
            append(output, item.name.size(), 2);
            output.insert(output.end(), item.name.begin(), item.name.end());
        }
        break;
    case AssetClipboardOperation::Accept:
        append(output, record.grantBytes, 8);
        append(output, record.window, 1);
        break;
    case AssetClipboardOperation::Chunk:
        append(output, record.item, 4);
        append(output, record.offset, 8);
        output.insert(output.end(), record.data.begin(), record.data.end());
        break;
    case AssetClipboardOperation::Credit:
        append(output, record.item, 4);
        append(output, record.offset, 8);
        append(output, record.verified ? 1 : 0, 1);
        break;
    case AssetClipboardOperation::Finish:
        append(output, record.item, 4);
        append(output, record.offset, 8);
        output.insert(output.end(), record.sha256.begin(), record.sha256.end());
        break;
    case AssetClipboardOperation::Cancel:
        output.insert(output.end(), record.reason.begin(), record.reason.end());
        break;
    default:
        break;
    }
    check(output.size() <= AssetClipboardMaximumRecordBytes);
    return output;
}

inline AssetClipboardRecord decodeAssetClipboardRecord(const std::uint8_t *data, std::size_t size)
{
    using namespace asset_clipboard_codec;
    check(data != nullptr && size >= AssetClipboardHeaderBytes && size <= AssetClipboardMaximumRecordBytes);
    Reader reader(data, size);
    check(reader.number(1) == 1);
    AssetClipboardRecord record;
    record.operation = static_cast<AssetClipboardOperation>(reader.number(1));
    check(reader.number(2) == 0);
    record.epoch = reader.number(8);
    record.serial = reader.number(8);
    const auto transfer = reader.bytes(record.transfer.size());
    std::copy(transfer.begin(), transfer.end(), record.transfer.begin());
    switch (record.operation) {
    case AssetClipboardOperation::EpochAcknowledged:
        record.control = static_cast<AssetClipboardOperation>(reader.number(1));
        break;
    case AssetClipboardOperation::Offer: {
        record.format = static_cast<AssetClipboardFormat>(reader.number(1));
        record.width = static_cast<std::uint32_t>(reader.number(4));
        record.height = static_cast<std::uint32_t>(reader.number(4));
        const auto count = reader.number(2);
        check(count >= 1 && count <= AssetClipboardMaximumFiles);
        record.totalBytes = reader.number(8);
        for (std::uint64_t index = 0; index < count; ++index) {
            AssetClipboardItem item;
            item.id = static_cast<std::uint32_t>(reader.number(4));
            item.size = reader.number(8);
            const auto nameBytes = reader.number(2);
            check(nameBytes >= 1 && nameBytes <= 255);
            const auto name = reader.bytes(nameBytes);
            item.name.assign(name.begin(), name.end());
            record.items.push_back(std::move(item));
        }
        break;
    }
    case AssetClipboardOperation::Accept:
        record.grantBytes = reader.number(8);
        record.window = static_cast<std::uint8_t>(reader.number(1));
        break;
    case AssetClipboardOperation::Chunk:
        record.item = static_cast<std::uint32_t>(reader.number(4));
        record.offset = reader.number(8);
        record.data = reader.bytes(reader.remaining());
        break;
    case AssetClipboardOperation::Credit: {
        record.item = static_cast<std::uint32_t>(reader.number(4));
        record.offset = reader.number(8);
        const auto verified = reader.number(1);
        check(verified <= 1);
        record.verified = verified == 1;
        break;
    }
    case AssetClipboardOperation::Finish: {
        record.item = static_cast<std::uint32_t>(reader.number(4));
        record.offset = reader.number(8);
        const auto hash = reader.bytes(record.sha256.size());
        std::copy(hash.begin(), hash.end(), record.sha256.begin());
        break;
    }
    case AssetClipboardOperation::Cancel: {
        const auto reason = reader.bytes(reader.remaining());
        record.reason.assign(reason.begin(), reason.end());
        break;
    }
    default:
        break;
    }
    check(reader.remaining() == 0);
    validateAssetClipboardRecord(record);
    return record;
}
} // namespace dockpipe::desktop
