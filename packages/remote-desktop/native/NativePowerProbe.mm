#include "NativePowerAssertion.h"

#import <CoreFoundation/CoreFoundation.h>
#import <IOKit/pwr_mgt/IOPMLib.h>

#include <chrono>
#include <cstdio>
#include <string_view>
#include <thread>
#include <unistd.h>

using namespace dockpipe::desktop;
using namespace std::chrono_literals;

namespace {
struct Snapshot {
    bool known = false;
    unsigned system = 0;
    unsigned display = 0;
    const char *failure = "not-observed";
    std::int32_t apiStatus = 0;
    bool tablePresent = false;
};

Snapshot ownAssertions()
{
    CFDictionaryRef processes = nullptr;
    const auto status = IOPMCopyAssertionsByProcess(&processes);
    if (status != kIOReturnSuccess || !processes) {
        if (processes)
            CFRelease(processes);
        return {false, 0, 0, status == kIOReturnSuccess ? "null-success" : "snapshot-api", static_cast<std::int32_t>(status)};
    }
    if (CFGetTypeID(processes) != CFDictionaryGetTypeID() || CFDictionaryGetCount(processes) > 4096) {
        CFRelease(processes);
        return {false, 0, 0, "table-shape", static_cast<std::int32_t>(status)};
    }
    const int pid = getpid();
    CFNumberRef key = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &pid);
    if (!key) {
        CFRelease(processes);
        return {false, 0, 0, "pid-key-allocation", static_cast<std::int32_t>(status)};
    }
    const auto value = CFDictionaryGetValue(processes, key);
    CFRelease(key);
    Snapshot result{true, 0, 0, "none", static_cast<std::int32_t>(status), true};
    if (value) {
        if (CFGetTypeID(value) != CFArrayGetTypeID() || CFArrayGetCount(static_cast<CFArrayRef>(value)) > 256) {
            CFRelease(processes);
            return {false, 0, 0, "own-array-shape", static_cast<std::int32_t>(status)};
        }
        CFArrayRef assertions = static_cast<CFArrayRef>(value);
        for (CFIndex index = 0; index < CFArrayGetCount(assertions); ++index) {
            const auto item = CFArrayGetValueAtIndex(assertions, index);
            if (!item || CFGetTypeID(item) != CFDictionaryGetTypeID()) {
                result.known = false;
                result.failure = "own-entry-shape";
                break;
            }
            const auto assertion = static_cast<CFDictionaryRef>(item);
            const auto name = CFDictionaryGetValue(assertion, kIOPMAssertionNameKey);
            if (!name || CFGetTypeID(name) != CFStringGetTypeID()) {
                result.known = false;
                result.failure = "own-name-shape";
                break;
            }
            const bool system = CFEqual(name, CFSTR("Dockpipe remote desktop system awake"));
            const bool display = CFEqual(name, CFSTR("Dockpipe remote desktop display awake"));
            if (!system && !display)
                continue;
            const auto type = CFDictionaryGetValue(assertion, kIOPMAssertionTypeKey);
            const auto level = CFDictionaryGetValue(assertion, kIOPMAssertionLevelKey);
            int active = 0;
            if (!type || CFGetTypeID(type) != CFStringGetTypeID()
                || !CFEqual(type, system ? kIOPMAssertionTypePreventUserIdleSystemSleep : kIOPMAssertionTypePreventUserIdleDisplaySleep)
                || !level || CFGetTypeID(level) != CFNumberGetTypeID()
                || !CFNumberGetValue(static_cast<CFNumberRef>(level), kCFNumberIntType, &active)
                || active != kIOPMAssertionLevelOn) {
                result.known = false;
                result.failure = "owned-properties";
                break;
            }
            if (system)
                ++result.system;
            else
                ++result.display;
        }
    }
    CFRelease(processes);
    return result;
}

bool acceptedZero(const Snapshot &snapshot)
{
    // Apple's _copyAssertionsByProcess can leave the initialized output null
    // on success (empty flattened array or allocation failure). Null is never
    // accepted as positive acquisition evidence. Baseline has no owned IDs;
    // final absence additionally requires exact successful releases below.
    // https://github.com/apple-oss-distributions/IOKitUser/blob/main/pwr_mgt.subproj/IOPMAssertions.c
    const bool nullSuccess = !snapshot.tablePresent && snapshot.apiStatus == kIOReturnSuccess
        && std::string_view(snapshot.failure) == "null-success";
    return (snapshot.known || nullSuccess) && !snapshot.system && !snapshot.display;
}

void report(const char *phase, const Snapshot &before, const Snapshot &during, const Snapshot &after,
    bool attempted, bool observed, bool released, bool passed,
    const PowerAssertionResult &acquiredResult = {}, const PowerAssertionResult &releasedResult = {})
{
    const bool absent = released && acceptedZero(after);
    std::printf("{\"schema\":\"dockpipe.desktop.power-probe/v1\",\"pid\":%d,\"phase\":\"%s\","
        "\"assertions_attempted\":%s,\"both_active_verified\":%s,\"release_verified\":%s,"
        "\"owned_assertions_absent\":%s,\"passed\":%s,"
        "\"initial_snapshot\":{\"known\":%s,\"table_present\":%s,\"system\":%u,\"display\":%u,\"failure\":\"%s\",\"api_status\":%d},"
        "\"active_snapshot\":{\"known\":%s,\"table_present\":%s,\"system\":%u,\"display\":%u,\"failure\":\"%s\",\"api_status\":%d},"
        "\"final_snapshot\":{\"known\":%s,\"table_present\":%s,\"system\":%u,\"display\":%u,\"failure\":\"%s\",\"api_status\":%d},"
        "\"acquired\":{\"success\":%s,\"active\":%s,\"cleanup_verified\":%s,\"failure\":%u},"
        "\"released\":{\"success\":%s,\"cleanup_verified\":%s,\"failure\":%u}}\n",
        getpid(), phase, attempted ? "true" : "false", observed ? "true" : "false", released ? "true" : "false",
        absent ? "true" : "false", passed ? "true" : "false", before.known ? "true" : "false",
        before.tablePresent ? "true" : "false", before.system, before.display, before.failure, before.apiStatus,
        during.known ? "true" : "false", during.tablePresent ? "true" : "false",
        during.system, during.display, during.failure, during.apiStatus, after.known ? "true" : "false",
        after.tablePresent ? "true" : "false", after.system, after.display, after.failure, after.apiStatus,
        acquiredResult.success ? "true" : "false", acquiredResult.active ? "true" : "false",
        acquiredResult.cleanupVerified ? "true" : "false", static_cast<unsigned>(acquiredResult.failure),
        releasedResult.success ? "true" : "false", releasedResult.cleanupVerified ? "true" : "false",
        static_cast<unsigned>(releasedResult.failure));
    std::fflush(stdout);
}
}

int main(int argc, char **argv)
{
    // Building this executable is effect-free. Actual assertions are opt-in and
    // never registered as automatic CTests or invoked by the source producer.
    if (argc != 2 || std::string_view(argv[1]) != "--native-assertions") {
        fputs("Explicit --native-assertions authorization is required.\n", stderr);
        report("arguments", {}, {}, {}, false, false, false, false);
        return 2;
    }
    const auto before = ownAssertions();
    if (!acceptedZero(before)) {
        report(before.known ? "preexisting-owned-name" : "initial-snapshot", before, {}, {}, false, false, false, false);
        return 1;
    }
    NativePowerAssertion assertion;
    const auto acquired = assertion.setActive(true);
    const auto during = ownAssertions();
    const bool observed = acquired.success && acquired.active && during.known && during.tablePresent
        && during.system == 1 && during.display == 1;
    if (observed)
        std::this_thread::sleep_for(2s);
    const auto released = assertion.release();
    Snapshot after;
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    do {
        after = ownAssertions();
        if (acceptedZero(after))
            break;
        std::this_thread::sleep_for(20ms);
    } while (std::chrono::steady_clock::now() < deadline);
    const bool absent = released.success && released.cleanupVerified && acceptedZero(after);
    const bool passed = observed && released.success && released.cleanupVerified && absent;
    report(passed ? "complete" : "verification", before, during, after, true, observed,
        released.success && released.cleanupVerified, passed, acquired, released);
    return passed ? 0 : 1;
}
