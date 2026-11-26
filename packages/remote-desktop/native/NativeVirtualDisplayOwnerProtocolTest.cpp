#include "NativeVirtualDisplayOwnerProtocol.h"
#include <cassert>
#include <limits>

using namespace dockpipe::desktop;
using Json = nlohmann::json;

template <typename Function> bool rejects(Function function)
{
    try {
        function();
        return false;
    } catch (...) {
        return true;
    }
}

int main()
{
    VirtualDisplayOwnerRequest request;
    request.spec = {3840, 2160, 1234};
    request.nonce = std::string(64, 'a');
    request.parentProcess = 123;
    request.createDeadlineUnixMilliseconds = 2000000000000;
    const auto profile = virtualDisplayOwnerProfile(request);
    const auto parsed = parseVirtualDisplayOwnerRequest(parseVirtualDisplayOwnerJson(profile.dump()));
    assert(parsed.spec.width == 3840 && parsed.spec.height == 2160 && parsed.spec.serial == 1234);
    assert(parsed.nonce == request.nonce && parsed.parentProcess == 123);
    assert(parsed.createDeadlineUnixMilliseconds == request.createDeadlineUnixMilliseconds);
    for (const auto field : {"serial", "width", "height", "parent_pid"}) {
        auto invalid = profile;
        invalid["request"][field] = -1;
        assert(rejects([&] { parseVirtualDisplayOwnerRequest(invalid); }));
    }
    auto invalid = profile;
    invalid["request"]["serial"] = 0;
    assert(rejects([&] { parseVirtualDisplayOwnerRequest(invalid); }));
    invalid = profile;
    invalid["request"]["width"] = 3000;
    assert(rejects([&] { parseVirtualDisplayOwnerRequest(invalid); }));
    invalid = profile;
    invalid["request"]["nonce"] = std::string(64, 'g');
    assert(rejects([&] { parseVirtualDisplayOwnerRequest(invalid); }));
    invalid = profile;
    invalid["request"]["create_deadline_unix_ms"] = std::numeric_limits<std::uint64_t>::max();
    assert(rejects([&] { parseVirtualDisplayOwnerRequest(invalid); }));
    invalid = profile;
    invalid["program"] = "/bin/sh";
    assert(rejects([&] { parseVirtualDisplayOwnerRequest(invalid); }));
    assert(rejects([] { parseVirtualDisplayOwnerJson("{\"serial\":1,\"serial\":2}"); }));
    assert(rejects([] { parseVirtualDisplayOwnerJson(std::string(4097, ' ')); }));
    assert(rejects([] { parseVirtualDisplayOwnerJson(std::string(12, '[') + "0" + std::string(12, ']')); }));
    auto create = virtualDisplayOwnerMessage(request, "create");
    validateVirtualDisplayOwnerMessage(create, request, "create", 5);
    create["nonce"] = std::string(64, 'b');
    assert(rejects([&] { validateVirtualDisplayOwnerMessage(create, request, "create", 5); }));
}
