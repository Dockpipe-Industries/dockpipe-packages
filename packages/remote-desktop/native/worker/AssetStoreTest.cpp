#include "AssetStore.h"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <unistd.h>
using namespace dockpipe::desktop;
namespace {
class FakeDigest final : public AssetDigest {
public:
    void update(const std::uint8_t *bytes, std::size_t size) override
    {
        for (std::size_t index = 0; index < size; ++index)
            value = value * 131 + bytes[index];
    }
    std::string finish() override
    {
        std::ostringstream result;
        result << std::hex << std::setfill('0') << std::setw(64) << value;
        return result.str();
    }
    std::uint64_t value = 0;
};
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
    char name[] = "/tmp/dockpipe-assets-test-XXXXXX";
    const auto temporary = mkdtemp(name);
    assert(temporary);
    const auto base = std::filesystem::canonical(temporary);
    const auto digest = [] { return std::make_unique<FakeDigest>(); };
    std::string retained;
    {
        AssetStore store((base / "cache").string(), digest);
        auto snapshot = store.prepare({{"first.txt", 3, {}}, {"empty.txt", 0, {}}});
        const std::uint8_t bytes[]{'a', 'b', 'c'};
        refuses([&] { store.append(snapshot.identifier, 0, 1, bytes, 3); });
        store.append(snapshot.identifier, 0, 0, bytes, 3);
        FakeDigest hash;
        hash.update(bytes, 3);
        store.finishEntry(snapshot.identifier, 0, hash.finish());
        refuses([&] { store.append(snapshot.identifier, 0, 3, bytes, 1); });
        store.finishEntry(snapshot.identifier, 1, FakeDigest().finish());
        snapshot = store.finish(snapshot.identifier);
        assert(snapshot.paths[0].ends_with("/0/first.txt"));
        assert(store.read(snapshot, 0, 0, 65536) == std::vector<std::uint8_t>({'a', 'b', 'c'}));
        store.published(snapshot.identifier);
        retained = snapshot.paths[0];
        store.cancel(snapshot.identifier);
        store.reap(1000);
        assert(std::filesystem::exists(retained));
        refuses([&] { store.prepare({{"../escape", 0, {}}}); });
        auto badHash = store.prepare({{"bad-hash", 3, {}}});
        store.append(badHash.identifier, 0, 0, bytes, 3);
        refuses([&] { store.finishEntry(badHash.identifier, 0, std::string(64, 'f')); });
        refuses([&] { store.finishEntry(badHash.identifier, 0, hash.finish()); });
        store.cancel(badHash.identifier);
        auto unused = store.prepare({{"cancelled", 10, {}}});
        store.cancel(unused.identifier);
        assert(!std::filesystem::exists(unused.paths[0]));
    }
    {
        AssetStore reopened((base / "cache").string(), digest);
        assert(std::filesystem::exists(retained));
        reopened.reap(2000); // Restart alone cannot declare clipboard ownership lost.
        assert(std::filesystem::exists(retained));
        reopened.observeReferencedPaths({retained}, 2000); // A markerless subset recopy still owns the whole snapshot.
        reopened.reap(2500);
        assert(std::filesystem::exists(retained));
        reopened.observeReferencedPaths({}, 3000);
        reopened.reap(3059);
        assert(std::filesystem::exists(retained));
        reopened.reap(3060);
        assert(std::filesystem::exists(retained)); // Unfocused/unknown ownership cannot authorize unlink.
        reopened.observeReferencedPaths({}, 3060);
        reopened.reap(3060, true);
        assert(!std::filesystem::exists(retained));
    }
    const auto source = base / "source.txt";
    std::ofstream(source) << "abc";
    AssetSourceBatch files({source.string()});
    assert(files.entries()[0].size == 3);
    assert(files.read(0, 0, 65536).size() == 3);
    std::ofstream(source) << "changed";
    refuses([&] { files.verify(0); });
    std::filesystem::create_symlink(source, base / "link");
    refuses([&] { AssetSourceBatch rejected({(base / "link").string()}); });
    refuses([&] { AssetSourceBatch rejected({base.string()}); });
    std::filesystem::remove_all(base);
    std::cout << "PASS private staging, digest/offset rejection, empty item, persistent published ownership/grace, source mutation and no-follow\n";
}
