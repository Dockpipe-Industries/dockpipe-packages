#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dockpipe::desktop {
struct AssetEntry {
    std::string name;
    std::uint64_t size = 0;
    std::string sha256;
};

class AssetDigest {
public:
    virtual ~AssetDigest() = default;
    virtual void update(const std::uint8_t *bytes, std::size_t size) = 0;
    virtual std::string finish() = 0;
};

struct AssetSnapshot {
    std::string identifier;
    std::vector<AssetEntry> entries;
    std::vector<std::string> paths;
    std::uint64_t totalBytes = 0;
};

// Owns only generated private paths. Peer paths never enter this interface.
// Source paths come exclusively from the admitted local pasteboard adapter.
class AssetSourceBatch final {
public:
    explicit AssetSourceBatch(const std::vector<std::string> &paths);
    ~AssetSourceBatch();
    const std::vector<AssetEntry> &entries() const;
    std::vector<std::uint8_t> read(std::size_t entry, std::uint64_t offset, std::size_t maximum);
    void verify(std::size_t entry) const;
private:
    struct State;
    std::unique_ptr<State> m_state;
};

class AssetStore final {
public:
    using Digests = std::function<std::unique_ptr<AssetDigest>()>;
    AssetStore(std::string root, Digests digests,
        std::function<void(const std::vector<AssetEntry> &)> validateNames = {});
    ~AssetStore();
    AssetStore(const AssetStore &) = delete;
    AssetStore &operator=(const AssetStore &) = delete;

    AssetSnapshot prepare(const std::vector<AssetEntry> &entries);
    void append(const std::string &identifier, std::size_t entry, std::uint64_t offset,
        const std::uint8_t *bytes, std::size_t size);
    void finishEntry(const std::string &identifier, std::size_t entry, const std::string &sha256);
    AssetSnapshot finish(const std::string &identifier);
    std::vector<std::uint8_t> read(const AssetSnapshot &snapshot, std::size_t entry,
        std::uint64_t offset, std::size_t maximum);
    void published(const std::string &identifier);
    void observeOwned(const std::string &identifier, std::int64_t unixSeconds);
    void observeReferencedPaths(const std::vector<std::string> &paths, std::int64_t unixSeconds);
    void cancel(const std::string &identifier);
    void reap(std::int64_t unixSeconds, bool allowPublished = false);
    std::uint64_t reservedBytes() const;

private:
    struct State;
    std::unique_ptr<State> m_state;
};
}
