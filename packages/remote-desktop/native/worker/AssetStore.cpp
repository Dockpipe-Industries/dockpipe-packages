#include "AssetStore.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <dirent.h>
#include <sys/statvfs.h>
#include <fcntl.h>
#include <map>
#include <set>
#include <stdexcept>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __APPLE__
#include <sys/mount.h>
#else
#include <sys/vfs.h>
#endif

namespace dockpipe::desktop {
namespace {
using Json = nlohmann::json;
constexpr std::uint64_t Quota = 2ULL << 30;
constexpr std::uint64_t MaximumBatch = 1ULL << 30;
constexpr std::size_t MaximumSnapshots = 32;
constexpr std::size_t ChunkBytes = 65536;

void ensure(bool value, const char *reason)
{
    if (!value)
        throw std::runtime_error(reason);
}

class Descriptor final {
public:
    explicit Descriptor(int value = -1) : value(value) {}
    ~Descriptor()
    {
        if (value >= 0)
            close(value);
    }
    Descriptor(const Descriptor &) = delete;
    int value;
};

void writeAll(int descriptor, const std::uint8_t *bytes, std::size_t size)
{
    while (size) {
        const auto count = write(descriptor, bytes, size);
        if (count < 0 && errno == EINTR)
            continue;
        ensure(count > 0, "Private asset write failed.");
        bytes += count;
        size -= static_cast<std::size_t>(count);
    }
}

bool safeName(const std::string &name)
{
    return !name.empty() && name.size() <= 255 && name != "." && name != ".."
        && name.find_first_of("/\\:") == std::string::npos && name.find('\0') == std::string::npos;
}

bool identifier(const std::string &value)
{
    return value.size() == 41 && value.starts_with("snapshot-")
        && value.substr(9).find_first_not_of("0123456789abcdef") == std::string::npos;
}

std::string freshIdentifier()
{
    // O_EXCL publication below also rejects a collision. No peer controls this identifier.
    Descriptor random(open("/dev/urandom", O_RDONLY | O_CLOEXEC));
    ensure(random.value >= 0, "Private asset randomness unavailable.");
    std::array<unsigned char, 16> bytes{};
    ensure(::read(random.value, bytes.data(), bytes.size()) == static_cast<ssize_t>(bytes.size()),
        "Private asset randomness incomplete.");
    constexpr char hex[] = "0123456789abcdef";
    std::string result = "snapshot-";
    for (std::size_t index = 0; index < 32; ++index)
        result += hex[(bytes[index / 2] >> (index % 2 ? 0 : 4)) & 15];
    return result;
}

bool localSourceVolume(int descriptor)
{
    struct statfs volume{};
    if (fstatfs(descriptor, &volume) != 0)
        return false;
#ifdef __APPLE__
    return (volume.f_flags & MNT_LOCAL) != 0;
#else
    // Explicit local filesystem families. Network/FUSE/unknown providers are
    // refused rather than triggering a remote fetch on a supposedly local URL.
    constexpr std::array<unsigned long, 7> localTypes{
        0xef53UL, 0x9123683eUL, 0x58465342UL, 0x01021994UL,
        0x794c7630UL, 0x2fc12fc1UL, 0xf2f52010UL};
    return std::find(localTypes.begin(), localTypes.end(), static_cast<unsigned long>(volume.f_type)) != localTypes.end();
#endif
}

int openSource(const std::string &path)
{
    ensure(!path.empty() && path.front() == '/' && path.size() < 4096
        && path.find('\0') == std::string::npos, "Clipboard source is not an absolute local file.");
    int directory = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    ensure(directory >= 0, "Cannot inspect clipboard source root.");
    std::size_t position = 1;
    while (position < path.size()) {
        const auto separator = path.find('/', position);
        const auto component = path.substr(position, separator - position);
        if (!safeName(component)) {
            close(directory);
            throw std::runtime_error("Clipboard source contains an unsafe component.");
        }
        const bool last = separator == std::string::npos;
#ifdef __APPLE__
        struct stat metadata{};
        if (fstatat(directory, component.c_str(), &metadata, AT_SYMLINK_NOFOLLOW) != 0
            || (metadata.st_flags & SF_DATALESS) != 0) {
            close(directory);
            throw std::runtime_error("Clipboard source requires unavailable cloud materialization.");
        }
#endif
        const int next = openat(directory, component.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC
            | (last ? O_NONBLOCK : O_DIRECTORY));
        close(directory);
        ensure(next >= 0, "Clipboard source could not be opened without following links.");
        if (last) {
            if (!localSourceVolume(next)) {
                close(next);
                throw std::runtime_error("Clipboard source is not on a supported local filesystem.");
            }
            return next;
        }
        directory = next;
        position = separator + 1;
    }
    close(directory);
    throw std::runtime_error("Clipboard source is not a regular file.");
}
}

struct AssetStore::State {
    struct Record {
        AssetSnapshot snapshot;
        std::vector<std::uint64_t> written;
        std::vector<std::unique_ptr<AssetDigest>> hashes;
        std::vector<bool> verified;
        bool complete = false;
        bool failed = false;
        bool published = false;
        std::int64_t retireAt = 0;
    };
    std::string root;
    int directory = -1;
    int lock = -1;
    Digests digests;
    std::function<void(const std::vector<AssetEntry> &)> validateNames;
    std::map<std::string, Record> records;

    ~State()
    {
        if (lock >= 0)
            close(lock);
        if (directory >= 0)
            close(directory);
    }

    void validateDirectory()
    {
        DIR *stream = fdopendir(dup(directory));
        ensure(stream != nullptr, "Private asset directory cannot be inspected.");
        std::size_t count = 0;
        bool valid = true;
        errno = 0;
        while (const auto *entry = readdir(stream)) {
            const std::string name = entry->d_name;
            if (++count > MaximumSnapshots + 5 || (name != "." && name != ".."
                && name != "owner.lock" && name != "journal.json" && !records.contains(name))) {
                valid = false;
                break;
            }
        }
        const int error = errno;
        closedir(stream);
        ensure(valid && error == 0, "Unaccounted or pending private asset storage requires inspection.");
    }

    void persist()
    {
        Json values = Json::array();
        for (const auto &[key, record] : records) {
            Json entries = Json::array();
            for (const auto &entry : record.snapshot.entries)
                entries.push_back({{"name", entry.name}, {"size", entry.size}, {"sha256", entry.sha256}});
            values.push_back({{"id", key}, {"entries", entries}, {"complete", record.complete},
                {"published", record.published}, {"retire_at", record.retireAt}});
        }
        const auto bytes = Json{{"schema", "dockpipe.desktop.asset-store/v1"}, {"snapshots", values}}.dump();
        ensure(bytes.size() <= (2U << 20), "Private asset journal exceeds its bound.");
        // A leftover temporary journal is uncertainty, not permission to overwrite it.
        Descriptor file(openat(directory, "journal.next", O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
        ensure(file.value >= 0, "Private asset journal publication is already pending.");
        writeAll(file.value, reinterpret_cast<const std::uint8_t *>(bytes.data()), bytes.size());
        ensure(fsync(file.value) == 0 && renameat(directory, "journal.next", directory, "journal.json") == 0
            && fsync(directory) == 0, "Private asset journal could not be committed.");
    }

    int openEntry(const Record &record, std::size_t entry, int flags)
    {
        ensure(entry < record.snapshot.entries.size(), "Private asset entry is out of range.");
        Descriptor folder(openat(directory, record.snapshot.identifier.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        ensure(folder.value >= 0, "Private asset snapshot disappeared.");
        const auto subdirectory = std::to_string(entry);
        Descriptor itemDirectory(openat(folder.value, subdirectory.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        ensure(itemDirectory.value >= 0, "Private asset item directory changed.");
        const auto &name = record.snapshot.entries[entry].name;
        const int file = openat(itemDirectory.value, name.c_str(), flags | O_NOFOLLOW | O_CLOEXEC, 0600);
        ensure(file >= 0, "Private asset entry could not be opened.");
        struct stat status{};
        if (fstat(file, &status) != 0 || !S_ISREG(status.st_mode) || status.st_uid != getuid()
            || (status.st_mode & 0777) != 0600 || status.st_nlink != 1) {
            close(file);
            throw std::runtime_error("Private asset entry ownership changed.");
        }
        return file;
    }
};

AssetStore::AssetStore(std::string root, Digests digests,
    std::function<void(const std::vector<AssetEntry> &)> validateNames) : m_state(std::make_unique<State>())
{
    auto &state = *m_state;
    state.root = std::move(root);
    state.digests = std::move(digests);
    state.validateNames = std::move(validateNames);
    ensure(state.digests && !state.root.empty() && state.root.front() == '/', "Private asset storage is unavailable.");
    ensure(mkdir(state.root.c_str(), 0700) == 0 || errno == EEXIST, "Private asset storage creation failed.");
    state.directory = open(state.root.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    struct stat status{};
    ensure(state.directory >= 0 && fstat(state.directory, &status) == 0
        && status.st_uid == getuid() && (status.st_mode & 0777) == 0700, "Private asset storage ownership changed.");
    state.lock = openat(state.directory, "owner.lock", O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    ensure(state.lock >= 0 && fstat(state.lock, &status) == 0 && S_ISREG(status.st_mode)
        && status.st_uid == getuid() && (status.st_mode & 0777) == 0600 && status.st_nlink == 1
        && flock(state.lock, LOCK_EX | LOCK_NB) == 0, "Another clipboard owns private asset storage.");
    Descriptor journal(openat(state.directory, "journal.json", O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
    if (journal.value < 0) {
        ensure(errno == ENOENT, "Private asset journal is unavailable.");
        state.validateDirectory();
        return;
    }
    ensure(fstat(journal.value, &status) == 0 && S_ISREG(status.st_mode) && status.st_uid == getuid()
        && (status.st_mode & 0777) == 0600 && status.st_size >= 0 && status.st_size <= (2 << 20),
        "Private asset journal ownership or size changed.");
    std::string bytes(static_cast<std::size_t>(status.st_size), '\0');
    ensure(pread(journal.value, bytes.data(), bytes.size(), 0) == static_cast<ssize_t>(bytes.size()),
        "Private asset journal is incomplete.");
    const auto value = Json::parse(bytes);
    ensure(value.size() == 2 && value.at("schema") == "dockpipe.desktop.asset-store/v1"
        && value.at("snapshots").is_array() && value.at("snapshots").size() <= MaximumSnapshots,
        "Private asset journal schema changed.");
    for (const auto &saved : value.at("snapshots")) {
        State::Record record;
        record.snapshot.identifier = saved.at("id").get<std::string>();
        ensure(saved.size() == 5 && identifier(record.snapshot.identifier)
            && saved.at("entries").is_array() && saved.at("entries").size() <= 128, "Private asset journal entry changed.");
        for (const auto &entry : saved.at("entries")) {
            AssetEntry item{entry.at("name"), entry.at("size"), entry.at("sha256")};
            ensure(entry.size() == 3 && safeName(item.name) && item.size <= MaximumBatch
                && record.snapshot.totalBytes <= MaximumBatch - item.size, "Private asset journal size changed.");
            record.snapshot.totalBytes += item.size;
            record.snapshot.entries.push_back(item);
            record.snapshot.paths.push_back(state.root + "/" + record.snapshot.identifier + "/" + std::to_string(record.snapshot.paths.size()) + "/" + item.name);
        }
        record.complete = saved.at("complete");
        record.published = saved.at("published");
        ensure(!record.published || record.complete, "Private asset journal claims an incomplete publication.");
        record.failed = !record.complete; // Interrupted payloads cannot be resumed or upgraded.
        record.verified.assign(record.snapshot.entries.size(), record.complete);
        // A prior current clipboard snapshot stays reserved until an admitted observation.
        // Interrupted incoming/unpublished work may be reaped on the next active epoch.
        record.retireAt = record.published ? 0 : 1;
        ensure(state.records.emplace(record.snapshot.identifier, std::move(record)).second,
            "Private asset journal contains duplicate snapshots.");
    }
    state.validateDirectory();
    ensure(reservedBytes() <= Quota, "Private asset storage quota is exceeded.");
}

AssetStore::~AssetStore() = default;

std::uint64_t AssetStore::reservedBytes() const
{
    std::uint64_t total = 0;
    for (const auto &[key, record] : m_state->records)
        total += record.snapshot.totalBytes + record.snapshot.entries.size() * 8192 + 4096;
    return total;
}

AssetSnapshot AssetStore::prepare(const std::vector<AssetEntry> &entries)
{
    auto &state = *m_state;
    ensure(!entries.empty() && entries.size() <= 128 && state.records.size() < MaximumSnapshots,
        "Private asset snapshot count is full.");
    if (state.validateNames)
        state.validateNames(entries);
    State::Record record;
    record.snapshot.identifier = freshIdentifier();
    for (const auto &entry : entries) {
        ensure(safeName(entry.name) && entry.size <= MaximumBatch
            && record.snapshot.totalBytes <= MaximumBatch - entry.size, "Private asset manifest exceeds its bound.");
        record.snapshot.totalBytes += entry.size;
        record.snapshot.entries.push_back(entry);
        record.snapshot.paths.push_back(state.root + "/" + record.snapshot.identifier + "/" + std::to_string(record.snapshot.paths.size()) + "/" + entry.name);
        record.written.push_back(0);
        record.hashes.push_back(state.digests());
        record.verified.push_back(false);
        ensure(bool(record.hashes.back()), "Private asset digest is unavailable.");
    }
    const auto charge = record.snapshot.totalBytes + record.snapshot.entries.size() * 8192 + 4096;
    struct statvfs capacity{};
    ensure(charge <= Quota && reservedBytes() <= Quota - charge, "Private asset storage quota is full.");
    ensure(fstatvfs(state.directory, &capacity) == 0
        && capacity.f_frsize > 0 && capacity.f_bavail >= (charge + capacity.f_frsize - 1) / capacity.f_frsize,
        "Private asset storage has insufficient free space.");
    const auto snapshot = record.snapshot;
    ensure(state.records.emplace(snapshot.identifier, std::move(record)).second, "Private asset snapshot collided.");
    state.persist(); // Reserve quota durably BEFORE creating any payload files.
    ensure(mkdirat(state.directory, snapshot.identifier.c_str(), 0700) == 0, "Private asset snapshot creation failed.");
    Descriptor folder(openat(state.directory, snapshot.identifier.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    ensure(folder.value >= 0, "Private asset snapshot disappeared.");
    for (std::size_t index = 0; index < entries.size(); ++index) {
        ensure(mkdirat(folder.value, std::to_string(index).c_str(), 0700) == 0,
            "Private asset item directory creation failed.");
        Descriptor file(state.openEntry(state.records.at(snapshot.identifier), index, O_WRONLY | O_CREAT | O_EXCL));
    }
    return snapshot;
}

void AssetStore::append(const std::string &identifier, std::size_t entry, std::uint64_t offset,
    const std::uint8_t *bytes, std::size_t size)
{
    auto &record = m_state->records.at(identifier);
    ensure(!record.complete && !record.failed && entry < record.written.size() && !record.verified[entry] && size > 0 && size <= ChunkBytes
        && offset == record.written[entry] && offset <= record.snapshot.entries[entry].size
        && size <= record.snapshot.entries[entry].size - offset, "Private asset chunk changed its sequence or size.");
    Descriptor file(m_state->openEntry(record, entry, O_WRONLY));
    ensure(lseek(file.value, static_cast<off_t>(offset), SEEK_SET) == static_cast<off_t>(offset), "Private asset chunk seek failed.");
    writeAll(file.value, bytes, size);
    record.hashes[entry]->update(bytes, size);
    record.written[entry] += size;
}

void AssetStore::finishEntry(const std::string &identifier, std::size_t entry, const std::string &sha256)
{
    auto &record = m_state->records.at(identifier);
    ensure(!record.complete && !record.failed && entry < record.verified.size() && !record.verified[entry]
        && record.written[entry] == record.snapshot.entries[entry].size, "Private asset payload is incomplete or already verified.");
    const auto actual = record.hashes[entry]->finish();
    auto &expected = record.snapshot.entries[entry].sha256;
    if ((!sha256.empty() && actual != sha256) || (!expected.empty() && actual != expected)) {
        record.failed = true;
        throw std::runtime_error("Private asset digest differs from its authenticated proof.");
    }
    expected = actual;
    Descriptor file(m_state->openEntry(record, entry, O_RDWR));
    ensure(fsync(file.value) == 0, "Private asset payload could not be committed.");
    Descriptor folder(openat(m_state->directory, identifier.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    ensure(folder.value >= 0, "Private asset snapshot disappeared before commit.");
    Descriptor item(openat(folder.value, std::to_string(entry).c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    ensure(item.value >= 0 && fsync(item.value) == 0, "Private asset item directory could not be committed.");
    record.hashes[entry].reset();
    record.verified[entry] = true;
}

AssetSnapshot AssetStore::finish(const std::string &identifier)
{
    auto &record = m_state->records.at(identifier);
    ensure(!record.complete && !record.failed, "Private asset finish was replayed or previously failed.");
    for (std::size_t index = 0; index < record.snapshot.entries.size(); ++index) {
        if (!record.verified[index])
            finishEntry(identifier, index, record.snapshot.entries[index].sha256);
    }
    Descriptor folder(openat(m_state->directory, identifier.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    ensure(folder.value >= 0 && fsync(folder.value) == 0, "Private asset batch directory could not be committed.");
    record.complete = true;
    m_state->persist();
    return record.snapshot;
}

std::vector<std::uint8_t> AssetStore::read(const AssetSnapshot &snapshot, std::size_t entry,
    std::uint64_t offset, std::size_t maximum)
{
    const auto &record = m_state->records.at(snapshot.identifier);
    ensure(record.complete && entry < record.snapshot.entries.size() && maximum <= ChunkBytes
        && offset <= record.snapshot.entries[entry].size, "Private asset read exceeds its bound.");
    const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(maximum, record.snapshot.entries[entry].size - offset));
    std::vector<std::uint8_t> result(count);
    Descriptor file(m_state->openEntry(record, entry, O_RDONLY));
    ensure(pread(file.value, result.data(), count, static_cast<off_t>(offset)) == static_cast<ssize_t>(count),
        "Private asset snapshot changed while streaming.");
    return result;
}

void AssetStore::published(const std::string &identifier)
{
    auto &record = m_state->records.at(identifier);
    ensure(record.complete, "Incomplete assets cannot own the clipboard.");
    if (record.published)
        return;
    record.published = true;
    record.retireAt = 0;
    m_state->persist();
}

void AssetStore::observeOwned(const std::string &identifier, std::int64_t now)
{
    bool changed = false;
    for (auto &[key, record] : m_state->records) {
        if (record.published && key != identifier && !record.retireAt) {
            record.retireAt = now + 60;
            changed = true;
        }
        if (key == identifier && record.retireAt) {
            record.retireAt = 0;
            changed = true;
        }
    }
    if (changed)
        m_state->persist();
}

void AssetStore::observeReferencedPaths(const std::vector<std::string> &paths, std::int64_t now)
{
    ensure(paths.size() <= 128, "Clipboard ownership observation exceeds its bound.");
    const std::set<std::string> referenced(paths.begin(), paths.end());
    bool changed = false;
    for (auto &[key, record] : m_state->records) {
        if (!record.published)
            continue;
        const bool current = std::any_of(record.snapshot.paths.begin(), record.snapshot.paths.end(),
            [&](const std::string &path) { return referenced.contains(path); });
        if (current && record.retireAt) {
            record.retireAt = 0;
            changed = true;
        } else if (!current && !record.retireAt) {
            record.retireAt = now + 60;
            changed = true;
        }
    }
    if (changed)
        m_state->persist();
}

void AssetStore::cancel(const std::string &identifier)
{
    auto found = m_state->records.find(identifier);
    if (found == m_state->records.end() || found->second.published)
        return;
    found->second.retireAt = 1;
    reap(std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
}

void AssetStore::reap(std::int64_t now, bool allowPublished)
{
    auto &state = *m_state;
    bool changed = false;
    for (auto iterator = state.records.begin(); iterator != state.records.end();) {
        const auto &record = iterator->second;
        if (!record.retireAt || record.retireAt > now || (record.published && !allowPublished)) {
            ++iterator;
            continue;
        }
        Descriptor folder(openat(state.directory, iterator->first.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (folder.value >= 0) {
            for (std::size_t index = 0; index < record.snapshot.entries.size(); ++index) {
                const auto component = std::to_string(index);
                Descriptor item(openat(folder.value, component.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
                if (item.value >= 0) {
                    ensure(unlinkat(item.value, record.snapshot.entries[index].name.c_str(), 0) == 0 || errno == ENOENT,
                        "Retired private asset could not be removed.");
                    ensure(unlinkat(folder.value, component.c_str(), AT_REMOVEDIR) == 0,
                        "Retired private asset item directory is not empty.");
                } else {
                    ensure(errno == ENOENT, "Retired private asset item directory changed.");
                }
            }
            ensure(unlinkat(state.directory, iterator->first.c_str(), AT_REMOVEDIR) == 0,
                "Retired private asset directory is not empty.");
        } else {
            ensure(errno == ENOENT, "Retired private asset directory changed.");
        }
        iterator = state.records.erase(iterator);
        changed = true;
    }
    if (changed)
        state.persist();
}
}

namespace dockpipe::desktop {
namespace {
bool sameFile(const struct stat &first, const struct stat &last)
{
    if (!S_ISREG(last.st_mode) || first.st_dev != last.st_dev || first.st_ino != last.st_ino
        || first.st_size != last.st_size || first.st_mtime != last.st_mtime || first.st_ctime != last.st_ctime)
        return false;
#ifdef __APPLE__
    return first.st_mtimespec.tv_nsec == last.st_mtimespec.tv_nsec && first.st_ctimespec.tv_nsec == last.st_ctimespec.tv_nsec;
#else
    return first.st_mtim.tv_nsec == last.st_mtim.tv_nsec && first.st_ctim.tv_nsec == last.st_ctim.tv_nsec;
#endif
}
}
struct AssetSourceBatch::State {
    std::vector<std::string> paths;
    std::vector<AssetEntry> entries;
    std::vector<struct stat> identities;
    std::unique_ptr<Descriptor> current;
    std::size_t currentIndex = 0;
};

AssetSourceBatch::AssetSourceBatch(const std::vector<std::string> &paths) : m_state(std::make_unique<State>())
{
    ensure(!paths.empty() && paths.size() <= 128, "Clipboard selection exceeds its file count.");
    std::uint64_t total = 0;
    for (const auto &path : paths) {
        Descriptor file(openSource(path)); // Metadata only: no source bytes before peer Accept.
        struct stat status{};
        ensure(fstat(file.value, &status) == 0 && S_ISREG(status.st_mode) && status.st_size >= 0
            && static_cast<std::uint64_t>(status.st_size) <= MaximumBatch
            && total <= MaximumBatch - static_cast<std::uint64_t>(status.st_size), "Clipboard selection exceeds its byte bound.");
        const auto name = path.substr(path.find_last_of('/') + 1);
        ensure(safeName(name), "Clipboard selection has an unsupported basename.");
        total += static_cast<std::uint64_t>(status.st_size);
        m_state->paths.push_back(path);
        m_state->entries.push_back({name, static_cast<std::uint64_t>(status.st_size), {}});
        m_state->identities.push_back(status);
    }
}
AssetSourceBatch::~AssetSourceBatch() = default;
const std::vector<AssetEntry> &AssetSourceBatch::entries() const { return m_state->entries; }

std::vector<std::uint8_t> AssetSourceBatch::read(std::size_t entry, std::uint64_t offset, std::size_t maximum)
{
    auto &state = *m_state;
    ensure(entry < state.entries.size() && maximum > 0 && maximum <= ChunkBytes
        && offset <= state.entries[entry].size, "Clipboard source read exceeds its bound.");
    if (!state.current || state.currentIndex != entry) {
        state.current = std::make_unique<Descriptor>(openSource(state.paths[entry]));
        state.currentIndex = entry;
    }
    verify(entry);
    const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(maximum, state.entries[entry].size - offset));
    std::vector<std::uint8_t> bytes(count);
    ensure(pread(state.current->value, bytes.data(), count, static_cast<off_t>(offset)) == static_cast<ssize_t>(count),
        "Clipboard source became unavailable while streaming.");
    verify(entry);
    return bytes;
}

void AssetSourceBatch::verify(std::size_t entry) const
{
    auto &state = *m_state;
    ensure(entry < state.entries.size(), "Clipboard source entry changed.");
    Descriptor reopened(openSource(state.paths[entry]));
    struct stat status{};
    ensure(fstat(reopened.value, &status) == 0 && sameFile(state.identities[entry], status),
        "Clipboard source identity changed.");
    if (state.current && state.currentIndex == entry)
        ensure(fstat(state.current->value, &status) == 0 && sameFile(state.identities[entry], status),
            "Clipboard source open descriptor changed.");
}
}
