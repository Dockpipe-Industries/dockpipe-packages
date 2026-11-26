#include "NativeClipboard.h"
#import <AppKit/AppKit.h>

namespace dockpipe::desktop {
namespace {
class NativeClipboard final : public ClipboardAdapter {
public:
    std::int64_t changeCount() override
    {
        @autoreleasepool {
            requireMainThread();
            return [NSPasteboard generalPasteboard].changeCount;
        }
    }
    ClipboardRead readText() override
    {
        @autoreleasepool {
            requireMainThread();
            NSPasteboard *pasteboard = [NSPasteboard generalPasteboard];
            if ([pasteboard canReadObjectForClasses:@[[NSURL class]]
                    options:@{NSPasteboardURLReadingFileURLsOnlyKey: @YES}]
                || [pasteboard.types containsObject:NSPasteboardTypePNG]
                || [pasteboard.types containsObject:NSPasteboardTypeTIFF])
                return {{}, "not_text"}; // Never export a richer clipboard generation as file paths or stale text.
            if (![pasteboard.types containsObject:NSPasteboardTypeString])
                return {{}, "not_text"};
            NSString *text = [pasteboard stringForType:NSPasteboardTypeString];
            if (!text)
                return {{}, "unavailable"};
            // UTF-8 needs at least one byte per UTF-16 unit for accepted text.
            if (text.length > ClipboardMaximumTextBytes)
                return {{}, "too_large"};
            NSData *bytes = [text dataUsingEncoding:NSUTF8StringEncoding allowLossyConversion:NO];
            if (!bytes)
                return {{}, "unsupported_text"};
            if (bytes.length > ClipboardMaximumTextBytes)
                return {{}, "too_large"};
            if (!bytes.length)
                return {{}, {}};
            return {std::string(static_cast<const char *>(bytes.bytes), bytes.length), {}};
        }
    }
    bool writeText(const std::string &text) override
    {
        @autoreleasepool {
            requireMainThread();
            if (!validClipboardText(text))
                return false;
            NSString *value = [[NSString alloc] initWithBytes:text.data() length:text.size() encoding:NSUTF8StringEncoding];
            if (!value)
                return false;
            NSPasteboard *pasteboard = [NSPasteboard generalPasteboard];
            [pasteboard clearContents];
            return [pasteboard setString:value forType:NSPasteboardTypeString];
        }
    }
private:
    static void requireMainThread()
    {
        if (![NSThread isMainThread])
            throw std::runtime_error("Clipboard requires the native main thread.");
    }
};
}
std::unique_ptr<ClipboardAdapter> makeNativeClipboard()
{
    return std::make_unique<NativeClipboard>();
}
} // namespace dockpipe::desktop
