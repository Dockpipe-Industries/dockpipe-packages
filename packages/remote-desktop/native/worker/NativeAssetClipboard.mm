#include "NativeAssetClipboard.h"
#include "../ClipboardPng.h"
#import <AppKit/AppKit.h>
#import <CommonCrypto/CommonDigest.h>
#import <ImageIO/ImageIO.h>
#import <Security/SecRandom.h>
#include <limits>

namespace dockpipe::desktop {
namespace {
void checkAsset(bool value, const char *reason)
{
    if (!value)
        throw std::runtime_error(reason);
}
void mainThread()
{
    checkAsset([NSThread isMainThread], "Native clipboard assets require the main thread.");
}
class NativeDigest final : public AssetDigest {
public:
    NativeDigest() { CC_SHA256_Init(&m_context); }
    void update(const std::uint8_t *bytes, std::size_t size) override
    {
        checkAsset(!m_finished && size <= 65536, "Native clipboard digest update exceeds its bound.");
        CC_SHA256_Update(&m_context, bytes, static_cast<CC_LONG>(size));
    }
    std::string finish() override
    {
        checkAsset(!m_finished, "Native clipboard digest finish was replayed.");
        m_finished = true;
        unsigned char bytes[CC_SHA256_DIGEST_LENGTH];
        CC_SHA256_Final(bytes, &m_context);
        constexpr char digits[] = "0123456789abcdef";
        std::string value;
        for (const auto byte : bytes) {
            value += digits[byte >> 4];
            value += digits[byte & 15];
        }
        return value;
    }
private:
    CC_SHA256_CTX m_context{};
    bool m_finished = false;
};
}
void validateNativeAssetNames(const std::vector<AssetEntry> &entries)
{
    @autoreleasepool {
        NSMutableSet<NSString *> *names = [NSMutableSet set];
        for (const auto &entry : entries) {
            NSString *name = [[NSString alloc] initWithBytes:entry.name.data() length:entry.name.size() encoding:NSUTF8StringEncoding];
            checkAsset(name != nil, "Native clipboard filename is not UTF-8.");
            NSString *key = [[name precomposedStringWithCanonicalMapping]
                stringByFoldingWithOptions:NSCaseInsensitiveSearch | NSWidthInsensitiveSearch
                locale:[NSLocale localeWithLocaleIdentifier:@"en_US_POSIX"]];
            checkAsset(![names containsObject:key], "Native clipboard basenames collide after Unicode normalization.");
            [names addObject:key];
        }
    }
}
std::unique_ptr<AssetDigest> makeNativeAssetDigest() { return std::make_unique<NativeDigest>(); }
std::array<std::uint8_t, 16> nativeAssetTransferIdentifier()
{
    std::array<std::uint8_t, 16> value{};
    checkAsset(SecRandomCopyBytes(kSecRandomDefault, value.size(), value.data()) == errSecSuccess,
        "Native clipboard transfer identity is unavailable.");
    return value;
}

PreparedClipboardPng normalizeNativeClipboardPng(const std::vector<std::uint8_t> &bytes)
{
    @autoreleasepool {
        checkAsset(!bytes.empty() && bytes.size() <= AssetClipboardMaximumPngBytes,
            "Native clipboard image exceeds its encoded bound.");
        const std::vector<std::uint8_t> signature{137, 80, 78, 71, 13, 10, 26, 10};
        std::vector<std::uint8_t> sanitized;
        if (bytes.size() >= signature.size() && std::equal(signature.begin(), signature.end(), bytes.begin()))
            sanitized = sanitizeClipboardPng(bytes).bytes;
        const auto &decodeBytes = sanitized.empty() ? bytes : sanitized;
        NSData *input = [NSData dataWithBytesNoCopy:const_cast<std::uint8_t *>(decodeBytes.data()) length:decodeBytes.size() freeWhenDone:NO];
        const auto options = @{(__bridge NSString *)kCGImageSourceShouldCache: @NO};
        CGImageSourceRef source = CGImageSourceCreateWithData((__bridge CFDataRef)input, (__bridge CFDictionaryRef)options);
        checkAsset(source != nullptr, "Native clipboard image is unavailable.");
        auto releaseSource = std::shared_ptr<const void>(source, [](const void *value) { CFRelease(value); });
        const auto type = CGImageSourceGetType(source);
        checkAsset(type && (CFEqual(type, CFSTR("public.png")) || CFEqual(type, CFSTR("public.tiff")))
            && CGImageSourceGetCount(source) == 1, "Native clipboard image format is unsupported.");
        CFDictionaryRef properties = CGImageSourceCopyPropertiesAtIndex(source, 0, nullptr);
        checkAsset(properties != nullptr, "Native clipboard image dimensions are unavailable.");
        auto releaseProperties = std::shared_ptr<const void>(properties, [](const void *value) { CFRelease(value); });
        const auto widthNumber = static_cast<CFNumberRef>(CFDictionaryGetValue(properties, kCGImagePropertyPixelWidth));
        const auto heightNumber = static_cast<CFNumberRef>(CFDictionaryGetValue(properties, kCGImagePropertyPixelHeight));
        const auto depthNumber = static_cast<CFNumberRef>(CFDictionaryGetValue(properties, kCGImagePropertyDepth));
        std::int64_t depth = 0;
        checkAsset(depthNumber && CFGetTypeID(depthNumber) == CFNumberGetTypeID()
            && CFNumberGetValue(depthNumber, kCFNumberSInt64Type, &depth) && depth > 0 && depth <= 8,
            "Native clipboard image exceeds its eight-bit component bound.");
        std::int64_t width = 0;
        std::int64_t height = 0;
        checkAsset(widthNumber && heightNumber && CFGetTypeID(widthNumber) == CFNumberGetTypeID()
            && CFGetTypeID(heightNumber) == CFNumberGetTypeID()
            && CFNumberGetValue(widthNumber, kCFNumberSInt64Type, &width)
            && CFNumberGetValue(heightNumber, kCFNumberSInt64Type, &height)
            && width > 0 && height > 0 && width <= AssetClipboardMaximumPixels
            && height <= AssetClipboardMaximumPixels / width, "Native clipboard image exceeds its pixel bound.");
        CGImageRef decoded = CGImageSourceCreateImageAtIndex(source, 0, (__bridge CFDictionaryRef)options);
        checkAsset(decoded != nullptr, "Native clipboard image could not be decoded.");
        auto releaseImage = std::shared_ptr<const void>(decoded, [](const void *value) { CFRelease(value); });
        checkAsset(CGImageGetBitsPerComponent(decoded) <= 8
            && CGImageGetWidth(decoded) == static_cast<std::size_t>(width)
            && CGImageGetHeight(decoded) == static_cast<std::size_t>(height), "Decoded clipboard dimensions changed.");
        CGColorSpaceRef color = CGColorSpaceCreateDeviceRGB();
        checkAsset(color != nullptr, "Native clipboard image color space is unavailable.");
        auto releaseColor = std::shared_ptr<const void>(color, [](const void *value) { CFRelease(value); });
        CGContextRef bitmap = CGBitmapContextCreate(nullptr, static_cast<std::size_t>(width),
            static_cast<std::size_t>(height), 8, static_cast<std::size_t>(width) * 4, color,
            static_cast<CGBitmapInfo>(kCGImageAlphaPremultipliedLast) | static_cast<CGBitmapInfo>(kCGBitmapByteOrder32Big));
        checkAsset(bitmap != nullptr, "Native clipboard image allocation failed.");
        auto releaseBitmap = std::shared_ptr<const void>(bitmap, [](const void *value) { CFRelease(value); });
        CGContextDrawImage(bitmap, CGRectMake(0, 0, width, height), decoded);
        CGImageRef normalized = CGBitmapContextCreateImage(bitmap);
        checkAsset(normalized != nullptr, "Native clipboard image normalization failed.");
        auto releaseNormalized = std::shared_ptr<const void>(normalized, [](const void *value) { CFRelease(value); });
        NSMutableData *encoded = [NSMutableData data];
        CGImageDestinationRef destination = CGImageDestinationCreateWithData((__bridge CFMutableDataRef)encoded,
            CFSTR("public.png"), 1, nullptr);
        checkAsset(destination != nullptr, "Native clipboard PNG encoder is unavailable.");
        auto releaseDestination = std::shared_ptr<const void>(destination, [](const void *value) { CFRelease(value); });
        CGImageDestinationAddImage(destination, normalized, nullptr); // No input metadata is propagated.
        checkAsset(CGImageDestinationFinalize(destination) && encoded.length > 0
            && encoded.length <= AssetClipboardMaximumPngBytes, "Native clipboard PNG exceeds its encoded bound.");
        PreparedClipboardPng result;
        const auto first = static_cast<const std::uint8_t *>(encoded.bytes);
        result.bytes.assign(first, first + encoded.length);
        result.bytes = sanitizeClipboardPng(result.bytes).bytes;
        result.width = static_cast<std::uint32_t>(width);
        result.height = static_cast<std::uint32_t>(height);
        return result;
    }
}

std::int64_t nativeAssetClipboardGeneration()
{
    @autoreleasepool {
        mainThread();
        return [NSPasteboard generalPasteboard].changeCount;
    }
}
LocalAssetSelection readNativeAssetClipboard(bool images, bool files)
{
    @autoreleasepool {
        mainThread();
        NSPasteboard *pasteboard = [NSPasteboard generalPasteboard];
        LocalAssetSelection result;
        result.generation = pasteboard.changeCount;
        const auto options = @{NSPasteboardURLReadingFileURLsOnlyKey: @YES};
        if ([pasteboard canReadObjectForClasses:@[[NSURL class]] options:options]) {
            if (!files) {
                result.ownershipKnown = false; // Retain possible own URLs without inspecting or exporting disabled files.
                return result;
            }
            NSArray<NSURL *> *urls = [pasteboard readObjectsForClasses:@[[NSURL class]] options:options];
            checkAsset(urls.count > 0 && urls.count <= 128, "Native clipboard file selection exceeds its bound.");
            for (NSURL *url in urls) {
                checkAsset(url.isFileURL && (url.host.length == 0
                    || [url.host.lowercaseString isEqualToString:@"localhost"]),
                    "Native clipboard file URL is not local.");
                NSError *error = nil;
                const auto keys = @[NSURLIsRegularFileKey, NSURLIsSymbolicLinkKey, NSURLIsAliasFileKey,
                    NSURLIsPackageKey, NSURLVolumeIsLocalKey, NSURLIsUbiquitousItemKey,
                    NSURLUbiquitousItemDownloadingStatusKey];
                NSDictionary *values = [url resourceValuesForKeys:keys error:&error];
                bool known = values && !error;
                for (NSString *key in @[NSURLIsRegularFileKey, NSURLIsSymbolicLinkKey,
                        NSURLIsAliasFileKey, NSURLIsPackageKey, NSURLVolumeIsLocalKey])
                    known = known && [values[key] isKindOfClass:[NSNumber class]];
                checkAsset(url.isFileURL && known && [values[NSURLIsRegularFileKey] boolValue]
                    && ![values[NSURLIsSymbolicLinkKey] boolValue] && ![values[NSURLIsAliasFileKey] boolValue]
                    && ![values[NSURLIsPackageKey] boolValue] && [values[NSURLVolumeIsLocalKey] boolValue],
                    "Native clipboard selection is not a regular local file.");
                if ([values[NSURLIsUbiquitousItemKey] boolValue]) {
                    NSString *status = values[NSURLUbiquitousItemDownloadingStatusKey];
                    checkAsset([status isEqualToString:NSURLUbiquitousItemDownloadingStatusCurrent]
                        || [status isEqualToString:NSURLUbiquitousItemDownloadingStatusDownloaded],
                        "Native clipboard cloud file is not downloaded.");
                }
                const char *path = url.path.UTF8String;
                checkAsset(path != nullptr, "Native clipboard filename is not UTF-8.");
                std::string selected(path);
                // macOS's documented /tmp alias is normalized only after the selected URL itself
                // was rejected if it was a symlink. Arbitrary path-component links remain refused.
                if (selected.starts_with("/tmp/"))
                    selected = "/private" + selected;
                result.paths.push_back(std::move(selected));
            }
            std::vector<AssetEntry> names;
            for (const auto &path : result.paths)
                names.push_back({path.substr(path.find_last_of('/') + 1), 0, {}});
            validateNativeAssetNames(names);
            return result;
        }
        if (!images)
            return result;
        NSPasteboardType type = nil;
        if ([pasteboard.types containsObject:NSPasteboardTypePNG])
            type = NSPasteboardTypePNG;
        else if ([pasteboard.types containsObject:NSPasteboardTypeTIFF])
            type = NSPasteboardTypeTIFF;
        if (!type)
            return result;
        NSData *data = [pasteboard dataForType:type];
        checkAsset(data && data.length > 0 && data.length <= AssetClipboardMaximumPngBytes,
            "Native clipboard image exceeds its encoded input bound.");
        result.format = AssetClipboardFormat::Png;
        const auto first = static_cast<const std::uint8_t *>(data.bytes);
        result.image.assign(first, first + data.length);
        return result;
    }
}

bool publishNativeAssetClipboard(const AssetClipboardRecord &offer, const AssetSnapshot &snapshot,
    const std::vector<std::uint8_t> &verifiedPng, std::int64_t expectedGeneration,
    const std::function<bool()> &admitted)
{
    @autoreleasepool {
        mainThread();
        NSPasteboard *pasteboard = [NSPasteboard generalPasteboard];
        if (!admitted() || pasteboard.changeCount != expectedGeneration)
            return false;
        if (offer.format == AssetClipboardFormat::Files) {
            NSMutableArray<NSURL *> *urls = [NSMutableArray array];
            for (const auto &path : snapshot.paths) {
                NSString *name = [[NSString alloc] initWithBytes:path.data() length:path.size() encoding:NSUTF8StringEncoding];
                if (!name)
                    return false;
                [urls addObject:[NSURL fileURLWithPath:name]];
            }
            if (!admitted() || pasteboard.changeCount != expectedGeneration)
                return false;
            [pasteboard clearContents];
            if (![pasteboard writeObjects:urls] || !admitted())
                return false;
            const auto count = pasteboard.changeCount;
            NSArray<NSURL *> *observed = [pasteboard readObjectsForClasses:@[[NSURL class]]
                options:@{NSPasteboardURLReadingFileURLsOnlyKey: @YES}];
            return [observed isEqualToArray:urls] && pasteboard.changeCount == count && admitted();
        }
        if (verifiedPng.empty() || verifiedPng.size() > AssetClipboardMaximumPngBytes)
            return false;
        NSData *data = [NSData dataWithBytes:verifiedPng.data() length:verifiedPng.size()];
        if (!admitted() || pasteboard.changeCount != expectedGeneration)
            return false;
        [pasteboard clearContents];
        if (![pasteboard setData:data forType:NSPasteboardTypePNG] || !admitted())
            return false;
        const auto count = pasteboard.changeCount;
        NSData *observed = [pasteboard dataForType:NSPasteboardTypePNG];
        return observed && observed.length <= AssetClipboardMaximumPngBytes
            && [observed isEqualToData:data] && pasteboard.changeCount == count && admitted();
    }
}
}
