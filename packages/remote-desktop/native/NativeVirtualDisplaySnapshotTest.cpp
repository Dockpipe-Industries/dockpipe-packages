#include "NativeVirtualDisplaySnapshot.h"
#include <cassert>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <string>
#include <unistd.h>

using namespace dockpipe::desktop;
namespace {
struct Fixture {
    std::string root;
    std::string temporary;
    std::string directory;
    std::string file;
    Fixture()
    {
        char pattern[] = "/tmp/dockpipe-snapshot-test-XXXXXX";
        assert(mkdtemp(pattern));
        char canonical[4096];
        assert(realpath(pattern, canonical));
        root = canonical;
        temporary = root + "/host-tmp";
        directory = temporary + "/qt-private";
        file = directory + "/native-worker-module";
        assert(mkdir(temporary.c_str(), 0700) == 0);
        assert(mkdir(directory.c_str(), 0700) == 0);
        std::ofstream output(file);
        output << "module fixture";
        output.close();
        assert(chmod(file.c_str(), 0500) == 0);
    }
    ~Fixture()
    {
        unlink(file.c_str());
        unlink((directory + "/unexpected").c_str());
        unlink((directory + "/saved").c_str());
        rmdir(directory.c_str());
        rmdir(temporary.c_str());
        rmdir(root.c_str());
    }
};
bool verify(const std::string &path)
{
    std::ifstream input(path);
    std::string bytes((std::istreambuf_iterator<char>(input)), {});
    return bytes == "module fixture";
}
}
int main()
{
    {
        Fixture fixture;
        NativeVirtualDisplaySnapshot snapshot;
        assert(snapshot.admit(fixture.temporary, fixture.file, verify));
        assert(snapshot.cleanupAfterReap());
        assert(access(fixture.temporary.c_str(), F_OK) < 0);
    }
    {
        Fixture fixture;
        NativeVirtualDisplaySnapshot snapshot;
        assert(snapshot.admit(fixture.temporary, fixture.file, verify));
        assert(unlink(fixture.file.c_str()) == 0);
        assert(rmdir(fixture.directory.c_str()) == 0); // Clean host destructor.
        assert(snapshot.cleanupAfterReap());
    }
    {
        Fixture fixture;
        NativeVirtualDisplaySnapshot snapshot;
        assert(snapshot.admit(fixture.temporary, fixture.file, verify));
        const auto saved = fixture.directory + "/saved";
        assert(rename(fixture.file.c_str(), saved.c_str()) == 0);
        assert(symlink(saved.c_str(), fixture.file.c_str()) == 0);
        assert(!snapshot.cleanupAfterReap());
        assert(access(saved.c_str(), F_OK) == 0);
    }
    {
        Fixture fixture;
        NativeVirtualDisplaySnapshot snapshot;
        assert(snapshot.admit(fixture.temporary, fixture.file, verify));
        std::ofstream extra(fixture.directory + "/unexpected");
        extra << "unrelated";
        extra.close();
        assert(!snapshot.cleanupAfterReap()); // No recursive deletion.
        assert(access((fixture.directory + "/unexpected").c_str(), F_OK) == 0);
    }
    {
        Fixture fixture;
        NativeVirtualDisplaySnapshot snapshot;
        assert(!snapshot.admit(fixture.root, fixture.file, verify)); // Extra path component.
    }
    {
        Fixture fixture;
        NativeVirtualDisplaySnapshot snapshot;
        assert(!snapshot.admit(fixture.temporary, fixture.file, [](const auto &) { return false; }));
        assert(access(fixture.file.c_str(), F_OK) == 0);
    }
    {
        Fixture fixture;
        NativeVirtualDisplaySnapshot snapshot;
        assert(snapshot.admit(fixture.temporary, fixture.file, verify));
        const auto saved = fixture.directory + "/saved";
        assert(rename(fixture.file.c_str(), saved.c_str()) == 0);
        std::ofstream replacement(fixture.file);
        replacement << "module fixture";
        replacement.close();
        assert(chmod(fixture.file.c_str(), 0500) == 0);
        assert(!snapshot.cleanupAfterReap()); // Same bytes do not authorize a different inode.
        assert(access(fixture.file.c_str(), F_OK) == 0);
    }
    {
        Fixture fixture;
        assert(unlink(fixture.file.c_str()) == 0);
        assert(mkfifo(fixture.file.c_str(), 0400) == 0);
        NativeVirtualDisplaySnapshot snapshot;
        assert(!snapshot.admit(fixture.temporary, fixture.file, verify));
    }
    {
        Fixture fixture;
        assert(chmod(fixture.file.c_str(), 0600) == 0);
        NativeVirtualDisplaySnapshot snapshot;
        assert(!snapshot.admit(fixture.temporary, fixture.file, verify));
    }
}
