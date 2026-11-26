#include "WorkerHostTemplate.h"
#include "../NativeVirtualDisplayOwner.h"
#import <Foundation/Foundation.h>
#import <Security/Security.h>
#import <CommonCrypto/CommonDigest.h>
#include <mach-o/dyld.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <iomanip>
#include <sstream>

namespace dockpipe::desktop {
namespace {
std::string canonicalHostPath(const std::string &path)
{
    char resolved[4096];
    require(templateAbsolutePath(Json(path)) && realpath(path.c_str(), resolved) && path == resolved,
        "Virtual display host path is not canonical.");
    return path;
}

std::string boundedOwnedBytes(const std::string &path, std::size_t maximum, bool privateFile, bool snapshotFile = false)
{
    canonicalHostPath(path);
    const int descriptor = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    require(descriptor >= 0, "Cannot open a pinned virtual display host input.");
    struct stat before{};
    const bool metadataKnown = fstat(descriptor, &before) == 0;
    const bool ownerPermitted = before.st_uid == getuid() || (!privateFile && !snapshotFile && before.st_uid == 0);
    bool modePermitted = (before.st_mode & 0022) == 0;
    if (privateFile)
        modePermitted = (before.st_mode & 0777) == 0600;
    else if (snapshotFile)
        modePermitted = (before.st_mode & 0222) == 0 && (before.st_mode & 0400) != 0;
    if (!metadataKnown || !S_ISREG(before.st_mode) || !ownerPermitted || !modePermitted
        || before.st_size <= 0 || std::uint64_t(before.st_size) > maximum) {
        close(descriptor);
        throw std::runtime_error("Unsafe pinned virtual display host input.");
    }
    std::string bytes(static_cast<std::size_t>(before.st_size), '\0');
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto count = read(descriptor, bytes.data() + offset, bytes.size() - offset);
        if (count <= 0) {
            close(descriptor);
            throw std::runtime_error("Cannot read a pinned virtual display host input.");
        }
        offset += static_cast<std::size_t>(count);
    }
    struct stat after{};
    const bool unchanged = fstat(descriptor, &after) == 0 && after.st_dev == before.st_dev
        && after.st_ino == before.st_ino && after.st_size == before.st_size
        && after.st_mtimespec.tv_sec == before.st_mtimespec.tv_sec
        && after.st_mtimespec.tv_nsec == before.st_mtimespec.tv_nsec;
    close(descriptor);
    require(unchanged, "Pinned virtual display host input changed while reading.");
    return bytes;
}

std::string hashBytes(const std::string &bytes)
{
    unsigned char result[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(bytes.data(), static_cast<CC_LONG>(bytes.size()), result);
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (const auto byte : result)
        output << std::setw(2) << unsigned(byte);
    return output.str();
}

std::string imageDigest(const std::string &path)
{
    return hashBytes(boundedOwnedBytes(path, 128 * 1024 * 1024, false));
}

std::string actualHostExecutable()
{
    char path[4096];
    std::uint32_t size = sizeof(path);
    require(_NSGetExecutablePath(path, &size) == 0, "Native Root host path exceeds its bound.");
    char resolved[4096];
    require(realpath(path, resolved), "Cannot identify the actual native Root host.");
    return resolved;
}

std::string actualProductionTeam(const std::string &path)
{
    NSURL *url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
    SecStaticCodeRef code = nullptr;
    require(SecStaticCodeCreateWithPath((__bridge CFURLRef)url, kSecCSDefaultFlags, &code) == errSecSuccess,
        "Cannot identify the native Root host signature.");
    CFDictionaryRef information = nullptr;
    const bool valid = SecStaticCodeCheckValidity(code, kSecCSStrictValidate | kSecCSCheckAllArchitectures, nullptr) == errSecSuccess
        && SecCodeCopySigningInformation(code, kSecCSSigningInformation, &information) == errSecSuccess;
    CFRelease(code);
    if (!valid || !information) {
        if (information)
            CFRelease(information);
        throw std::runtime_error("The actual native Root host signature is invalid.");
    }
    NSDictionary *values = CFBridgingRelease(information);
    NSNumber *flags = values[(__bridge NSString *)kSecCodeInfoFlags];
    require([flags isKindOfClass:[NSNumber class]], "Native Root host signature flags are unavailable.");
    NSDictionary *entitlements = values[(__bridge NSString *)kSecCodeInfoEntitlementsDict];
    require(!entitlements || ([entitlements isKindOfClass:[NSDictionary class]]
        && ![entitlements[@"com.apple.security.cs.disable-library-validation"] isEqual:@YES]),
        "Native Root host library validation must remain enabled.");
    if ((flags.unsignedIntValue & kSecCodeSignatureAdhoc) != 0)
        return {};
    NSString *team = values[(__bridge NSString *)kSecCodeInfoTeamIdentifier];
    require((flags.unsignedIntValue & kSecCodeSignatureRuntime) != 0 && [team isKindOfClass:[NSString class]]
        && team.length == 10, "Native Root host lacks its admitted production team/runtime.");
    return team.UTF8String;
}
}

VirtualDisplayOwnerContext loadVirtualDisplayOwnerContext(const VirtualDisplayHostTemplateReference &reference,
    const std::string &cli, const std::string &state, const std::string &providerImage)
{
    const auto bytes = boundedOwnedBytes(reference.path, 32768, true);
    require(hashBytes(bytes) == reference.sha256, "Pinned virtual display host template changed.");
    const auto descriptor = parseWorkerHostTemplate(bytes);
    const auto hostExecutable = actualHostExecutable();
    WorkerHostIdentity identity{imageDigest(providerImage), canonicalHostPath(cli), imageDigest(cli),
        canonicalHostPath(state), hostExecutable, imageDigest(hostExecutable), actualProductionTeam(hostExecutable)};
    validateWorkerHostTemplate(descriptor, identity);
    require(imageDigest(descriptor["module"]["path"].get<std::string>()) == identity.moduleSha256,
        "Original virtual display module differs from the actual loaded image.");
    struct stat directory{};
    require(lstat(state.c_str(), &directory) == 0 && S_ISDIR(directory.st_mode)
        && directory.st_uid == getuid() && (directory.st_mode & 0777) == 0700,
        "Virtual display template state directory is not private.");
    VirtualDisplayOwnerContext result;
    result.hostExecutable = hostExecutable;
    result.verifySnapshot = [expected = identity.moduleSha256](const std::string &path) {
        try {
            return hashBytes(boundedOwnedBytes(path, 128 * 1024 * 1024, false, true)) == expected;
        } catch (...) {
            return false;
        }
    };
    result.materializeDescriptor = [descriptor](const std::string &privateDirectory, const Json &profile) {
        canonicalHostPath(privateDirectory);
        struct stat metadata{};
        require(lstat(privateDirectory.c_str(), &metadata) == 0 && S_ISDIR(metadata.st_mode)
            && metadata.st_uid == getuid() && (metadata.st_mode & 0777) == 0700,
            "Virtual display child directory is not private.");
        const auto path = privateDirectory + "/child-host.json";
        const auto content = cloneVirtualDisplayHostDescriptor(descriptor, profile).dump();
        require(content.size() <= 32768, "Virtual display child descriptor exceeds its bound.");
        const int file = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        require(file >= 0, "Cannot create an exclusive virtual display child descriptor.");
        std::size_t offset = 0;
        while (offset < content.size()) {
            const auto count = write(file, content.data() + offset, content.size() - offset);
            if (count <= 0) {
                close(file);
                throw std::runtime_error("Cannot write virtual display child descriptor.");
            }
            offset += static_cast<std::size_t>(count);
        }
        const bool saved = fsync(file) == 0;
        close(file);
        require(saved, "Cannot persist virtual display child descriptor.");
        return path;
    };
    return result;
}
} // namespace dockpipe::desktop
