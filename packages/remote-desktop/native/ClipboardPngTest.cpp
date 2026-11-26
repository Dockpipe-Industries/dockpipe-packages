#include "ClipboardPng.h"
#include <cassert>
#include <iostream>
using namespace dockpipe::desktop;
namespace {
void word(std::vector<std::uint8_t> &output, std::uint32_t value)
{
    for (int shift = 24; shift >= 0; shift -= 8)
        output.push_back(static_cast<std::uint8_t>(value >> shift));
}
void chunk(std::vector<std::uint8_t> &output, const std::string &type, const std::vector<std::uint8_t> &data)
{
    word(output, static_cast<std::uint32_t>(data.size()));
    const auto start = output.size();
    output.insert(output.end(), type.begin(), type.end());
    output.insert(output.end(), data.begin(), data.end());
    word(output, clipboardPngCrc(output.data() + start, type.size() + data.size()));
}
std::vector<std::uint8_t> png(unsigned depth = 8, unsigned width = 2, bool metadata = false, bool animation = false)
{
    std::vector<std::uint8_t> output{137, 80, 78, 71, 13, 10, 26, 10};
    std::vector<std::uint8_t> header;
    word(header, width);
    word(header, 2);
    header.insert(header.end(), {static_cast<std::uint8_t>(depth), 6, 0, 0, 0});
    chunk(output, "IHDR", header);
    if (metadata) {
        // Intentionally not inflated: compressed metadata must never reach the decoder.
        chunk(output, "iCCP", std::vector<std::uint8_t>(2 * 1024 * 1024, 255));
        chunk(output, "zTXt", {1, 2, 3});
        chunk(output, "iTXt", {4, 5, 6});
    }
    if (animation)
        chunk(output, "acTL", std::vector<std::uint8_t>(8));
    chunk(output, "IDAT", {1});
    chunk(output, "IEND", {});
    return output;
}
template<class Action> void refuses(Action action)
{
    bool rejected = false;
    try {
        action();
    } catch (const std::exception &) {
        rejected = true;
    }
    assert(rejected);
}
}
int main()
{
    const auto clean = png();
    assert(sanitizeClipboardPng(clean).bytes == clean);
    const auto stripped = sanitizeClipboardPng(png(8, 2, true));
    assert(stripped.bytes == clean && stripped.width == 2 && stripped.height == 2);
    refuses([&] { sanitizeClipboardPng(png(16)); });
    refuses([&] { sanitizeClipboardPng(png(8, 16777216)); });
    refuses([&] { sanitizeClipboardPng(png(8, 2, false, true)); });
    auto truncated = clean;
    truncated.pop_back();
    refuses([&] { sanitizeClipboardPng(truncated); });
    auto corrupt = clean;
    corrupt[29] ^= 1;
    refuses([&] { sanitizeClipboardPng(corrupt); });
    std::cout << "PASS PNG metadata stripped before decoding, CRC/truncation/depth/pixel/APNG refusal\n";
}
