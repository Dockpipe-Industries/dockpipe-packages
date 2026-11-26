#include "WorkerHostTemplate.h"
#include "../NativeVirtualDisplayOwner.h"
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>

using namespace dockpipe::desktop;

namespace {
bool rejects(const std::string &path)
{
    try {
        loadVirtualDisplayOwnerContext({path, std::string(64, 'a')}, "/not/reached/cli",
            "/not/reached/state", "/not/reached/module");
        return false;
    } catch (...) {
        return true;
    }
}
}

int main()
{
    char directory[] = "/private/tmp/dockpipe-host-template-XXXXXX";
    assert(mkdtemp(directory));
    const std::filesystem::path root(directory);
    const auto fifo = root / "template-fifo";
    assert(mkfifo(fifo.c_str(), 0600) == 0);
    const auto started = std::chrono::steady_clock::now();
    assert(rejects(fifo.string()));
    assert(std::chrono::steady_clock::now() - started < std::chrono::milliseconds(500));
    const auto ordinary = root / "invalid.json";
    std::ofstream(ordinary) << "{}";
    assert(chmod(ordinary.c_str(), 0600) == 0);
    assert(rejects(ordinary.string()));
    const auto alias = root / "alias.json";
    assert(symlink(ordinary.c_str(), alias.c_str()) == 0);
    assert(rejects(alias.string()));
    std::filesystem::remove_all(root);
    std::cout << "PASS pinned template FIFO/invalid bytes/symlink rejection; no native display or permission API\n";
}
