#pragma once
#include "AssetClipboardProtocol.h"
#include <algorithm>
#include <array>
#include <string>
#include <vector>

namespace dockpipe::desktop {
struct ClipboardPng {
    std::vector<std::uint8_t> bytes;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};
inline std::uint32_t clipboardPngWord(const std::uint8_t *bytes)
{
    return (std::uint32_t(bytes[0]) << 24) | (std::uint32_t(bytes[1]) << 16)
        | (std::uint32_t(bytes[2]) << 8) | bytes[3];
}
inline std::uint32_t clipboardPngCrc(const std::uint8_t *bytes, std::size_t size)
{
    static const auto table = [] {
        std::array<std::uint32_t, 256> values{};
        for (std::uint32_t index = 0; index < values.size(); ++index) {
            auto value = index;
            for (unsigned bit = 0; bit < 8; ++bit)
                value = (value >> 1) ^ ((value & 1) ? 0xedb88320U : 0U);
            values[index] = value;
        }
        return values;
    }();
    std::uint32_t value = 0xffffffffU;
    for (std::size_t index = 0; index < size; ++index)
        value = table[(value ^ bytes[index]) & 255] ^ (value >> 8);
    return value ^ 0xffffffffU;
}
inline ClipboardPng sanitizeClipboardPng(const std::vector<std::uint8_t> &input)
{
    const auto check = [](bool value) {
        if (!value)
            throw std::invalid_argument("Unsupported or malformed clipboard PNG.");
    };
    constexpr std::array<std::uint8_t, 8> signature{137, 80, 78, 71, 13, 10, 26, 10};
    check(input.size() >= 45 && input.size() <= AssetClipboardMaximumPngBytes
        && std::equal(signature.begin(), signature.end(), input.begin()));
    ClipboardPng result;
    result.bytes.assign(signature.begin(), signature.end());
    bool header = false;
    bool palette = false;
    bool transparency = false;
    bool pixels = false;
    bool ended = false;
    bool pixelSequenceEnded = false;
    std::uint8_t color = 0;
    std::size_t chunks = 0;
    for (std::size_t offset = 8; offset < input.size();) {
        check(++chunks <= input.size() / 12 && input.size() - offset >= 12 && !ended);
        const auto size = clipboardPngWord(input.data() + offset);
        check(size <= input.size() - offset - 12);
        const std::string type(reinterpret_cast<const char *>(input.data() + offset + 4), 4);
        check(std::all_of(type.begin(), type.end(), [](unsigned char value) {
            return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z');
        }));
        check((static_cast<unsigned char>(type[2]) & 32) == 0);
        const auto *data = input.data() + offset + 8;
        check(clipboardPngCrc(input.data() + offset + 4, static_cast<std::size_t>(size) + 4)
            == clipboardPngWord(data + size));
        bool retain = false;
        if (!header) {
            check(type == "IHDR" && size == 13);
            result.width = clipboardPngWord(data);
            result.height = clipboardPngWord(data + 4);
            const auto depth = data[8];
            color = data[9];
            check(result.width && result.height && result.width <= AssetClipboardMaximumPixels
                && result.height <= AssetClipboardMaximumPixels / result.width
                && (depth == 1 || depth == 2 || depth == 4 || depth == 8)
                && (color == 0 || color == 2 || color == 3 || color == 4 || color == 6)
                && (depth == 8 || color == 0 || color == 3)
                && data[10] == 0 && data[11] == 0 && data[12] <= 1);
            header = true;
            retain = true;
        } else if (type == "IHDR" || type == "acTL" || type == "fcTL" || type == "fdAT") {
            check(false);
        } else if (type == "PLTE") {
            check(!palette && !pixels && size > 0 && size <= 768 && size % 3 == 0 && color != 0 && color != 4);
            palette = true;
            retain = true;
        } else if (type == "tRNS") {
            bool validTransparency = false;
            if (color == 0)
                validTransparency = size == 2;
            else if (color == 2)
                validTransparency = size == 6;
            else if (color == 3)
                validTransparency = palette && size > 0 && size <= 256;
            check(!transparency && !pixels && validTransparency);
            transparency = true;
            retain = true;
        } else if (type == "IDAT") {
            check(!pixelSequenceEnded && (color != 3 || palette));
            pixels = true;
            retain = true;
        } else if (type == "IEND") {
            check(size == 0 && pixels && offset + 12 == input.size());
            ended = true;
            retain = true;
        } else {
            check((static_cast<unsigned char>(type[0]) & 32) != 0);
            if (pixels)
                pixelSequenceEnded = true;
            // Drop every ancillary metadata payload before invoking any image decoder,
            // including compressed profiles/text and unknown future extensions.
        }
        if (retain)
            result.bytes.insert(result.bytes.end(), input.begin() + offset, input.begin() + offset + size + 12);
        offset += static_cast<std::size_t>(size) + 12;
    }
    check(header && pixels && ended);
    return result;
}
}
