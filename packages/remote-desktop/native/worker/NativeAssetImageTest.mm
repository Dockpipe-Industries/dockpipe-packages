#include "NativeAssetClipboard.h"
#include "../ClipboardPng.h"
#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>
#include <cassert>
#include <iostream>
using namespace dockpipe::desktop;
namespace {
std::vector<std::uint8_t> encode(CGImageRef image, CFStringRef type, unsigned count)
{
    NSMutableData *data = [NSMutableData data];
    auto destination = CGImageDestinationCreateWithData((__bridge CFMutableDataRef)data, type, count, nullptr);
    assert(destination);
    for (unsigned index = 0; index < count; ++index)
        CGImageDestinationAddImage(destination, image, nullptr);
    assert(CGImageDestinationFinalize(destination));
    CFRelease(destination);
    const auto first = static_cast<const std::uint8_t *>(data.bytes);
    return {first, first + data.length};
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
    @autoreleasepool {
        std::array<std::uint8_t, 16> pixels{255, 0, 0, 255, 0, 255, 0, 255,
            0, 0, 255, 255, 255, 255, 255, 255};
        auto color = CGColorSpaceCreateDeviceRGB();
        assert(color);
        auto bitmap = CGBitmapContextCreate(pixels.data(), 2, 2, 8, 8, color,
            static_cast<CGBitmapInfo>(kCGImageAlphaPremultipliedLast) | static_cast<CGBitmapInfo>(kCGBitmapByteOrder32Big));
        assert(bitmap);
        auto image = CGBitmapContextCreateImage(bitmap);
        assert(image);
        for (auto type : {CFSTR("public.png"), CFSTR("public.tiff")}) {
            const auto bytes = encode(image, type, 1);
            const auto normalized = normalizeNativeClipboardPng(bytes);
            const auto checked = sanitizeClipboardPng(normalized.bytes);
            assert(normalized.width == 2 && normalized.height == 2 && checked.width == 2 && checked.height == 2);
        }
        validateNativeAssetNames({{"first.txt", 0, {}}, {"second.txt", 0, {}}});
        refuses([&] { validateNativeAssetNames({{"A.txt", 0, {}}, {"a.txt", 0, {}}}); });
        refuses([&] { validateNativeAssetNames({{"\xc3\xa9.txt", 0, {}}, {"e\xcc\x81.txt", 0, {}}}); });
        const auto multi = encode(image, CFSTR("public.tiff"), 2);
        refuses([&] { normalizeNativeClipboardPng(multi); });
        auto deep = encode(image, CFSTR("public.png"), 1);
        deep[24] = 16;
        const auto crc = clipboardPngCrc(deep.data() + 12, 17);
        for (unsigned index = 0; index < 4; ++index)
            deep[29 + index] = static_cast<std::uint8_t>(crc >> (24 - index * 8));
        refuses([&] { normalizeNativeClipboardPng(deep); });
        CFRelease(image);
        CFRelease(bitmap);
        CFRelease(color);
        std::cout << "PASS in-memory ImageIO PNG/TIFF to sanitized RGBA8 PNG, multipage TIFF and deep PNG refusal; no pasteboard access\n";
    }
}
