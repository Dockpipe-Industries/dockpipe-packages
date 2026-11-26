#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace dockpipe::desktop {
constexpr std::size_t ClipboardMaximumTextBytes = 65536;
constexpr std::size_t ClipboardHeaderBytes = 18;

enum class ClipboardOperation : std::uint8_t {
    Watch = 1,
    Pause = 2,
    Write = 3,
    Changed = 4,
    Acknowledged = 5,
    Notice = 6
};

struct ClipboardRecord {
    ClipboardOperation operation = ClipboardOperation::Pause;
    std::uint64_t epoch = 0;
    std::uint64_t revision = 0;
    std::string text;
};

inline bool validClipboardText(const std::string &text)
{
    if (text.size() > ClipboardMaximumTextBytes)
        return false;
    for (std::size_t position = 0; position < text.size();) {
        const auto first = static_cast<unsigned char>(text[position]);
        if (first == 0)
            return false;
        if (first < 0x80) {
            ++position;
            continue;
        }
        std::size_t length = 0;
        if (first >= 0xc2 && first <= 0xdf)
            length = 2;
        else if (first >= 0xe0 && first <= 0xef)
            length = 3;
        else if (first >= 0xf0 && first <= 0xf4)
            length = 4;
        else
            return false;
        if (length > text.size() - position)
            return false;
        const auto second = static_cast<unsigned char>(text[position + 1]);
        if ((first == 0xe0 && second < 0xa0) || (first == 0xed && second > 0x9f)
            || (first == 0xf0 && second < 0x90) || (first == 0xf4 && second > 0x8f))
            return false;
        for (std::size_t continuation = 1; continuation < length; ++continuation) {
            const auto value = static_cast<unsigned char>(text[position + continuation]);
            if (value < 0x80 || value > 0xbf)
                return false;
        }
        position += length;
    }
    return true;
}

inline bool validClipboardNotice(const std::string &text)
{
    return text == "too_large" || text == "not_text"
        || text == "unsupported_text" || text == "unavailable";
}

inline void validateClipboardRecord(const ClipboardRecord &record)
{
    if (!record.epoch || !validClipboardText(record.text))
        throw std::invalid_argument("Invalid clipboard epoch or text.");
    switch (record.operation) {
    case ClipboardOperation::Watch:
    case ClipboardOperation::Pause:
        if (record.revision || !record.text.empty())
            throw std::invalid_argument("Invalid clipboard watch command.");
        return;
    case ClipboardOperation::Write:
    case ClipboardOperation::Changed:
        if (!record.revision)
            throw std::invalid_argument("Missing clipboard revision.");
        return;
    case ClipboardOperation::Acknowledged:
        if (!record.revision || !record.text.empty())
            throw std::invalid_argument("Invalid clipboard acknowledgment.");
        return;
    case ClipboardOperation::Notice:
        if (record.revision || !validClipboardNotice(record.text))
            throw std::invalid_argument("Invalid clipboard notice.");
        return;
    }
    throw std::invalid_argument("Unknown clipboard operation.");
}

inline bool clipboardDirectionAllowed(ClipboardOperation operation, bool senderIsWorker)
{
    if (senderIsWorker)
        return operation == ClipboardOperation::Changed || operation == ClipboardOperation::Acknowledged
            || operation == ClipboardOperation::Notice;
    return operation == ClipboardOperation::Watch || operation == ClipboardOperation::Pause
        || operation == ClipboardOperation::Write;
}

inline std::vector<std::uint8_t> encodeClipboardRecord(const ClipboardRecord &record)
{
    validateClipboardRecord(record);
    std::vector<std::uint8_t> bytes;
    bytes.reserve(ClipboardHeaderBytes + record.text.size());
    bytes.push_back(1);
    bytes.push_back(static_cast<std::uint8_t>(record.operation));
    for (const auto value : {record.epoch, record.revision}) {
        for (int shift = 56; shift >= 0; shift -= 8)
            bytes.push_back(static_cast<std::uint8_t>(value >> shift));
    }
    bytes.insert(bytes.end(), record.text.begin(), record.text.end());
    return bytes;
}

inline ClipboardRecord decodeClipboardRecord(const std::uint8_t *bytes, std::size_t size)
{
    if (!bytes || size < ClipboardHeaderBytes || size > ClipboardHeaderBytes + ClipboardMaximumTextBytes
        || bytes[0] != 1)
        throw std::invalid_argument("Invalid clipboard record framing.");
    ClipboardRecord record;
    record.operation = static_cast<ClipboardOperation>(bytes[1]);
    for (std::size_t index = 2; index < 10; ++index)
        record.epoch = (record.epoch << 8) | bytes[index];
    for (std::size_t index = 10; index < ClipboardHeaderBytes; ++index)
        record.revision = (record.revision << 8) | bytes[index];
    record.text.assign(reinterpret_cast<const char *>(bytes + ClipboardHeaderBytes), size - ClipboardHeaderBytes);
    validateClipboardRecord(record);
    return record;
}
} // namespace dockpipe::desktop
