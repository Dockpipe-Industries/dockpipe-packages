#include "NativeVirtualMode.h"

#include <cassert>
#include <limits>

using namespace dockpipe::desktop;

int main()
{
    for (const auto requested : {NativeVirtualDisplaySpec{1280, 720}, {1600, 900}, {1920, 1080}, {3840, 2160}}) {
        const NativeVirtualMode exact{requested.width, requested.height, requested.width, requested.height, 60};
        assert(exactVirtualMode(exact, requested));
        auto invalid = exact;
        invalid.width /= 2;
        assert(!exactVirtualMode(invalid, requested)); // No HiDPI/logical-size substitution.
        invalid = exact;
        invalid.height /= 2;
        assert(!exactVirtualMode(invalid, requested));
        invalid = exact;
        --invalid.pixelWidth;
        assert(!exactVirtualMode(invalid, requested));
        invalid = exact;
        --invalid.pixelHeight;
        assert(!exactVirtualMode(invalid, requested));
        for (const double refresh : {0.0, 30.0, 59.0, 61.0, -60.0,
                 std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
            invalid = exact;
            invalid.refreshRate = refresh;
            assert(!exactVirtualMode(invalid, requested));
        }
    }
    assert(!boundedVirtualModeCount(0));
    assert(boundedVirtualModeCount(1));
    assert(boundedVirtualModeCount(64));
    assert(!boundedVirtualModeCount(65));
    assert(!boundedVirtualModeCount(std::numeric_limits<std::size_t>::max()));
}
