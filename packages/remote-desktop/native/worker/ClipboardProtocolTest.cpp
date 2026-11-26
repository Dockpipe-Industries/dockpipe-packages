#include "ClipboardProtocol.h"

#include <cassert>
#include <stdexcept>

using namespace dockpipe::desktop;

int main()
{
    for (const auto operation : {ClipboardOperation::Write, ClipboardOperation::Changed}) {
        ClipboardRecord record{operation, 1, 2, "\xce\xbb\xf0\x9f\x99\x82\ncommand"};
        const auto encoded = encodeClipboardRecord(record);
        assert(encoded.size() == ClipboardHeaderBytes + record.text.size());
        assert(encoded[0] == 1 && encoded[1] == static_cast<std::uint8_t>(operation));
        assert(encoded[9] == 1 && encoded[17] == 2);
        const auto decoded = decodeClipboardRecord(encoded.data(), encoded.size());
        assert(decoded.operation == operation && decoded.epoch == 1 && decoded.revision == 2);
        assert(decoded.text == record.text);
    }
    const auto rejected = [](const ClipboardRecord &record) {
        try {
            encodeClipboardRecord(record);
            return false;
        } catch (const std::invalid_argument &) {
            return true;
        }
    };
    assert(validClipboardText(std::string(ClipboardMaximumTextBytes, 'x')));
    assert(!validClipboardText(std::string(ClipboardMaximumTextBytes + 1, 'x')));
    assert(!validClipboardText(std::string("a\0b", 3)));
    assert(!validClipboardText("\xc0\xaf"));
    assert(!validClipboardText("\xed\xa0\x80"));
    assert(!validClipboardText("\xf4\x90\x80\x80"));
    assert(!validClipboardText("\xe2\x82"));
    assert(rejected({ClipboardOperation::Watch, 1, 1, {}}));
    assert(rejected({ClipboardOperation::Pause, 0, 0, {}}));
    assert(rejected({ClipboardOperation::Write, 1, 0, {}}));
    assert(rejected({ClipboardOperation::Acknowledged, 1, 1, "unexpected"}));
    assert(rejected({ClipboardOperation::Notice, 1, 0, "arbitrary-text"}));
    assert(rejected({static_cast<ClipboardOperation>(255), 1, 0, {}}));
    assert(clipboardDirectionAllowed(ClipboardOperation::Watch, false));
    assert(!clipboardDirectionAllowed(ClipboardOperation::Watch, true));
    assert(clipboardDirectionAllowed(ClipboardOperation::Changed, true));
    assert(!clipboardDirectionAllowed(ClipboardOperation::Changed, false));
    auto wire = encodeClipboardRecord({ClipboardOperation::Write, 1, 1, "text"});
    wire[0] = 2;
    bool refused = false;
    try {
        decodeClipboardRecord(wire.data(), wire.size());
    } catch (const std::invalid_argument &) {
        refused = true;
    }
    assert(refused);
}
