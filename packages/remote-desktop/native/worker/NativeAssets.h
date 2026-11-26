#pragma once
#include "../AssetClipboardTransfer.h"
#include <memory>

namespace dockpipe::desktop {
class NativeAssets final {
public:
    NativeAssets(std::string privateRoot, bool images, bool files,
        std::function<void(AssetClipboardRecord)> emit, std::function<bool()> admitted,
        std::function<void()> clipboardPublished);
    ~NativeAssets();
    void receive(AssetClipboardRecord record);
    void poll();
    void externalClipboardWrite();
    // False means an I/O job still owns module code/state. The host must retain
    // the module and use its existing unknown-cleanup/provider-exit boundary.
    bool stop();
private:
    struct State;
    std::shared_ptr<State> m_state;
    std::function<bool()> m_admitted;
    std::function<void()> m_clipboardPublished;
    bool m_images;
    bool m_files;
    std::int64_t m_baseline = -1;
    std::uint64_t m_lastEpoch = 0;
    bool m_stopUnknown = false;
    bool m_ownershipKnown = false;
    std::vector<std::string> m_referencedPaths;
    std::chrono::steady_clock::time_point m_nextMaintenance{};
    std::chrono::steady_clock::time_point m_nextPoll{};
};
}
