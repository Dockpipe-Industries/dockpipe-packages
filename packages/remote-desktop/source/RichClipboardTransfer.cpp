#include "RichClipboardTransfer.h"
#include "ClipboardSelection.h"
#include <QScopeGuard>
#include "ProcessChannel.h"
#include "../native/AssetClipboardTransfer.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QFileInfo>
#include <QUrl>
#include <algorithm>
#include <QSet>
#include <QHash>
#include <QPixmap>
#include <optional>
#include <QLibrary>
#include <QPointer>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>
#include <QUuid>
#include <condition_variable>
#include <deque>
#include <dlfcn.h>
#include <mutex>
#include <cerrno>
#include <sys/stat.h>
#include <unistd.h>

namespace dockpipe::desktop {
namespace {
class QtAssetDigest final : public AssetDigest {
public:
    void update(const std::uint8_t *data, std::size_t size) override
    {
        m_hash.addData(QByteArrayView(reinterpret_cast<const char *>(data), qsizetype(size)));
    }
    std::string finish() override { return m_hash.result().toHex().toStdString(); }
private:
    QCryptographicHash m_hash{QCryptographicHash::Sha256};
};

AssetStore::Digests digests()
{
    return [] { return std::make_unique<QtAssetDigest>(); };
}

// One process cache and one executor: quotas and ownership are never per widget.
// The module remains mapped while this app-owned service retains published URLs.
class ClipboardCache final : public QObject {
public:
    ClipboardCache() : QObject(nullptr)
    {
        Dl_info module{};
        if (dladdr(reinterpret_cast<void *>(&digests), &module) && module.dli_fname
            && QFileInfo(QString::fromLocal8Bit(module.dli_fname)).canonicalFilePath()
                != QFileInfo(QCoreApplication::applicationFilePath()).canonicalFilePath()) {
            QLibrary library(QString::fromLocal8Bit(module.dli_fname));
            library.setLoadHints(QLibrary::PreventUnloadHint);
            if (!library.load())
                throw std::runtime_error("Clipboard module retention unavailable.");
        }
        m_lane.moveToThread(&m_thread);
        m_thread.start();
        connect(QGuiApplication::clipboard(), &QClipboard::changed, this, [this](QClipboard::Mode mode) {
            if (mode == QClipboard::Clipboard)
                ++m_observationGeneration;
        });
        auto *timer = new QTimer(this);
        timer->setInterval(10000);
        connect(timer, &QTimer::timeout, this, [this] {
            if (!m_maintenanceRequired || m_maintenancePending.exchange(true))
                return;
            bool admitted = false;
            bool filesAdmitted = false;
            for (const auto &observer : m_observers) {
                if (observer.proof()) {
                    admitted = true;
                    filesAdmitted |= observer.files;
                }
            }
            const auto *mime = admitted ? QGuiApplication::clipboard()->mimeData() : nullptr;
            const bool fileSelection = mime && clipboardSelectionKind(*mime) == ClipboardSelectionKind::Files;
            if (!mime || (fileSelection && !filesAdmitted)) {
                m_maintenancePending = false;
                return;
            }
            std::vector<std::string> paths;
            if (fileSelection) {
                try {
                    for (const auto &path : clipboardLocalFiles(*mime))
                        paths.push_back(path.toStdString());
                } catch (const std::exception &) {
                    // Unknown URL ownership is retained, never treated as empty.
                    m_maintenancePending = false;
                    return;
                }
            }
            const auto generation = m_observationGeneration.load();
            post([this, paths, generation] {
                try {
                    if (m_store && generation == m_observationGeneration) {
                        m_store->observeReferencedPaths(paths, QDateTime::currentSecsSinceEpoch());
                        if (generation == m_observationGeneration)
                            m_store->reap(QDateTime::currentSecsSinceEpoch(), true);
                        if (!m_store->reservedBytes())
                            m_maintenanceRequired = false;
                    }
                } catch (const std::exception &) {
                    // Keep quota/ownership receipts on uncertain private-cache cleanup.
                }
                m_maintenancePending = false;
            });
        });
        timer->start();
    }
    // Process-owned intentionally: an OS file read can outlive a closed surface.
    // The mapped module and executor remain valid until process termination.
    void post(std::function<void()> job)
    {
        QMetaObject::invokeMethod(&m_lane, std::move(job), Qt::QueuedConnection);
    }
    void registerObserver(const void *identifier, std::function<bool()> proof, bool files)
    {
        m_observers.insert(identifier, {std::move(proof), files});
        invalidateObservation();
    }
    void removeObserver(const void *identifier)
    {
        m_observers.remove(identifier);
        invalidateObservation();
    }
    void invalidateObservation() { ++m_observationGeneration; }
    void trackPublication() { m_maintenanceRequired = true; }
    AssetStore &store()
    {
        if (!m_store) {
            const auto root = QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
                + "/desktop-clipboard-assets";
            if (!QDir().mkpath(QFileInfo(root).absolutePath()))
                throw std::runtime_error("Clipboard cache unavailable.");
            const auto path = QFile::encodeName(root);
            if (::mkdir(path.constData(), 0700) != 0 && errno != EEXIST)
                throw std::runtime_error("Clipboard cache unavailable.");
            struct stat metadata{};
            if (::lstat(path.constData(), &metadata) != 0 || !S_ISDIR(metadata.st_mode)
                || metadata.st_uid != ::geteuid() || (metadata.st_mode & 0777) != 0700)
                throw std::runtime_error("Clipboard cache must be an owned private directory.");
            m_store = std::make_unique<AssetStore>(root.toStdString(), digests(), [](const std::vector<AssetEntry> &entries) {
                QSet<QString> names;
                for (const auto &entry : entries) {
                    const auto name = QString::fromUtf8(entry.name.data(), qsizetype(entry.name.size()))
                        .normalized(QString::NormalizationForm_C).toCaseFolded();
                    if (names.contains(name))
                        throw std::invalid_argument("Duplicate normalized clipboard basename.");
                    names.insert(name);
                }
            });
            m_maintenanceRequired = m_store->reservedBytes() != 0;
        }
        return *m_store;
    }
private:
    std::atomic_bool m_maintenancePending{false};
    std::atomic_bool m_maintenanceRequired{false};
    struct Observer {
        std::function<bool()> proof;
        bool files = false;
    };
    QHash<const void *, Observer> m_observers;
    std::atomic<quint64> m_observationGeneration{0};
    QThread m_thread;
    QObject m_lane;
    std::unique_ptr<AssetStore> m_store;
};

ClipboardCache &cache()
{
    static QPointer<ClipboardCache> instance;
    if (!instance)
        instance = new ClipboardCache;
    return *instance;
}

QByteArray encode(const AssetRecord &record)
{
    const auto bytes = encodeAssetClipboardRecord(record);
    return QByteArray(reinterpret_cast<const char *>(bytes.data()), qsizetype(bytes.size()));
}

AssetRecord decode(const QByteArray &bytes)
{
    return decodeAssetClipboardRecord(reinterpret_cast<const std::uint8_t *>(bytes.constData()), bytes.size());
}
}

struct RichClipboardTransfer::State {
    ClipboardPngReuse pngReuse; // Accessed only on the serial asset I/O lane.
    ProcessChannel *channel = nullptr;
    ClipboardCache *cache = nullptr;
    QPointer<RichClipboardTransfer> owner;
    std::function<bool()> eligible;
    std::unique_ptr<AssetClipboardTransfer> engine;
    std::atomic_bool admitted{false};
    std::atomic_bool stopping{false};
    std::atomic_bool disabled{false};
    std::atomic_bool ioStopped{false};
    std::atomic<qint64> generation{0};
    std::atomic<unsigned> jobs{0};
    std::atomic<unsigned> emittedRecords{0};
    std::mutex mutex;
    std::condition_variable drained;
    std::deque<QByteArray> outgoing;
    quint64 epoch = 0;
    qint64 offeredGeneration = -1;
    qint64 offeredRecordGeneration = -1;
    qint64 readyGeneration = -1;
    qint64 outgoingReadyGeneration = -1;
    bool images = false;
    bool files = false;
    bool pollPending = false;
    bool dropIntent = false;
    QByteArray ownMarker;
    qint64 ownGeneration = -1;
    quint64 ownEpoch = 0;
    std::optional<AssetRecord> offered;
    std::optional<AssetRecord> incoming;
    int sendProgressPercent = -1;
    int receiveProgressPercent = -1;
    std::mutex selectionMutex;
    std::function<void()> selectionJob;
    bool selectionScheduled = false;

    void selectLatest(const std::shared_ptr<State> &self, std::function<void()> job)
    {
        {
            std::lock_guard lock(selectionMutex);
            selectionJob = std::move(job);
            if (selectionScheduled)
                return;
            selectionScheduled = true;
        }
        if (!post(self, [self] {
            std::function<void()> latest;
            {
                std::lock_guard lock(self->selectionMutex);
                latest = std::move(self->selectionJob);
                self->selectionScheduled = false;
            }
            if (latest)
                latest();
        })) {
            std::lock_guard lock(selectionMutex);
            selectionJob = {};
            selectionScheduled = false;
        }
    }

    bool post(const std::shared_ptr<State> &self, std::function<void()> job, bool final = false)
    {
        if (!final && stopping)
            return false;
        if (!final && jobs >= 32) {
            admitted = false;
            disabled = true;
            QMetaObject::invokeMethod(qApp, [owner = self->owner] {
                if (owner) {
                    owner->invalidate();
                    emit owner->status(tr("Clipboard queue exceeded its limit. Reconnect to share again."));
                }
            });
            return false;
        }
        ++jobs;
        cache->post([self, job = std::move(job), final] {
            try {
                if (self->disabled && !final) {
                    if (self->engine)
                        self->engine->stop();
                } else {
                    if (self->engine && !self->admitted && !self->stopping)
                        self->engine->pause();
                    job();
                    if (self->engine && !self->admitted && !self->stopping)
                        self->engine->pause();
                }
            } catch (const std::exception &) {
                self->admitted = false;
                self->disabled = true;
                try {
                    if (self->engine)
                        self->engine->stop();
                } catch (const std::exception &) {
                    // Retain the engine/cache receipts; close cannot claim a clean drain.
                    self->ioStopped = false;
                }
                QMetaObject::invokeMethod(qApp, [owner = self->owner] {
                    if (owner) {
                        owner->invalidate();
                        emit owner->status(tr("Clipboard transfer stopped. Reconnect to share again."));
                    }
                });
            }
            {
                std::lock_guard lock(self->mutex);
                --self->jobs;
            }
            self->drained.notify_all();
        });
        return true;
    }
};

RichClipboardTransfer::RichClipboardTransfer(ProcessChannel &channel, bool images, bool files,
    std::function<bool()> eligible, QObject *parent)
    : QObject(parent), m_state(std::make_shared<State>())
{
    const auto state = m_state;
    state->channel = &channel;
    state->cache = &cache();
    state->owner = this;
    state->eligible = std::move(eligible);
    state->images = images;
    state->files = files;
    const std::weak_ptr<State> weak = state;
    state->cache->registerObserver(state.get(), [weak] {
        const auto current = weak.lock();
        return current && current->admitted && !current->stopping && !current->disabled && current->eligible();
    }, files);
    state->post(state, [state] {
        const QPointer<RichClipboardTransfer> owner = state->owner;
        state->engine = std::make_unique<AssetClipboardTransfer>(state->cache->store(), digests(),
            [owner, state](AssetRecord record) {
                if (state->emittedRecords.fetch_add(1) >= 24) {
                    --state->emittedRecords;
                    throw std::runtime_error("Clipboard writer queue exceeded its limit.");
                }
                const auto bytes = encode(record);
                const auto generation = state->engine->outgoingGeneration(record);
                QMetaObject::invokeMethod(qApp, [owner, state, bytes, generation] {
                    --state->emittedRecords;
                    if (owner)
                        owner->enqueue(bytes, generation);
                });
            },
            [owner, state](const AssetRecord &offer, const AssetSnapshot &snapshot, qint64 generation,
                    std::shared_ptr<std::atomic_bool> ticket) {
                state->cache->trackPublication();
                QStringList paths;
                for (const auto &path : snapshot.paths)
                    paths.append(QString::fromStdString(path));
                QImage image;
                QByteArray png;
                if (offer.format == AssetFormat::Png) {
                    QFile file(paths.value(0));
                    if (!file.open(QIODevice::ReadOnly) || file.size() > qint64(AssetClipboardMaximumPngBytes))
                        throw std::runtime_error("Clipboard image unavailable.");
                    QMimeData encoded;
                    encoded.setData("image/png", file.readAll());
                    const auto prepared = prepareClipboardImage(encoded);
                    image = prepared.image();
                    png = prepared.png();
                    if (image.width() != int(offer.width) || image.height() != int(offer.height))
                        throw std::invalid_argument("Clipboard PNG dimensions changed.");
                }
                const auto record = encode(offer);
                const auto marker = QByteArray::fromStdString(snapshot.identifier);
                QMetaObject::invokeMethod(qApp, [owner, record, paths, image, png, marker, generation, ticket] {
                    if (owner)
                        owner->publish(record, paths, image, png, marker, generation, ticket);
                });
            }, [state] { return state->admitted.load() && !state->stopping.load(); },
            [state] { return state->generation.load(); }, [] {
                std::array<std::uint8_t, 16> result{};
                const auto bytes = QUuid::createUuid().toRfc4122();
                std::copy(bytes.begin(), bytes.end(), result.begin());
                return result;
            }, [state](const std::vector<std::uint8_t> &input) {
                const QByteArray bytes(reinterpret_cast<const char *>(input.data()), qsizetype(input.size()));
                const auto reused = state->pngReuse.consume(bytes);
                const auto size = reused ? *reused : decodeClipboardPng(bytes).size();
                return PreparedClipboardPng{input, std::uint32_t(size.width()), std::uint32_t(size.height())};
            }, state->images, state->files, AssetClipboardEndpointRole::Viewer);
    });
    connect(&channel, &ProcessChannel::writable, this, &RichClipboardTransfer::pump);
    auto *timer = new QTimer(this);
    timer->setInterval(100);
    connect(timer, &QTimer::timeout, this, [this, state] {
        refresh();
        pump();
        if (state->pollPending || state->stopping)
            return;
        state->pollPending = true;
        state->post(state, [state] {
            if (state->engine)
                state->engine->poll(std::chrono::steady_clock::now());
            QMetaObject::invokeMethod(qApp, [owner = state->owner] {
                if (owner)
                    owner->m_state->pollPending = false;
            });
        });
    });
    timer->start();
}

RichClipboardTransfer::~RichClipboardTransfer()
{
    stop();
    wait();
    m_state->owner.clear();
}

void RichClipboardTransfer::refresh()
{
    const auto state = m_state;
    const bool eligible = !state->stopping && !state->disabled && state->eligible();
    if (!eligible) {
        pause();
        return;
    }
    if (state->admitted.exchange(true))
        return;
    state->cache->invalidateObservation();
    const auto epoch = ++state->epoch;
    state->post(state, [state, epoch] { state->engine->arm(epoch); });
}

void RichClipboardTransfer::pause()
{
    const auto state = m_state;
    if (!state->admitted.exchange(false))
        return;
    invalidate();
    state->cache->invalidateObservation();
    ++state->generation;
    state->offeredGeneration = -1;
    {
        std::lock_guard lock(state->selectionMutex);
        state->selectionJob = {};
    }
    std::erase_if(state->outgoing, [](const QByteArray &bytes) {
        const auto operation = decode(bytes).operation;
        return operation != AssetOperation::Arm && operation != AssetOperation::Pause;
    });
    state->readyGeneration = -1;
    state->offered.reset();
    state->ownEpoch = 0;
    state->dropIntent = false;
    state->post(state, [state] { state->engine->pause(); });
}

void RichClipboardTransfer::receive(const QByteArray &bytes)
{
    AssetRecord record;
    try {
        record = decode(bytes);
    } catch (const std::exception &) {
        invalidate();
        throw;
    }
    const auto state = m_state;
    if (record.operation == AssetOperation::Offer) {
        invalidate();
        state->readyGeneration = -1;
        state->dropIntent = false;
        emit incomingOffer();
        state->incoming = record;
        state->receiveProgressPercent = -1;
        emit progress(-1);
        emit status(tr("Receiving clipboard…"));
        QSet<QString> names;
        for (const auto &item : record.items) {
            const auto name = QString::fromUtf8(item.name.data(), qsizetype(item.name.size()))
                .normalized(QString::NormalizationForm_C).toCaseFolded();
            if (names.contains(name)) {
                invalidate();
                throw std::invalid_argument("Duplicate normalized clipboard basename.");
            }
            names.insert(name);
        }
    }
    state->post(state, [state, record] {
        const auto generation = state->engine->outgoingGeneration(record);
        state->engine->receive(record, std::chrono::steady_clock::now());
        if (record.operation == AssetOperation::Cancel) {
            QMetaObject::invokeMethod(qApp, [owner = state->owner, generation, record] {
                if (!owner)
                    return;
                const auto current = owner->m_state;
                const auto matches = [&record](const std::optional<AssetRecord> &offer) {
                    return offer && offer->epoch == record.epoch && offer->serial == record.serial
                        && offer->transfer == record.transfer;
                };
                if (record.epoch != current->epoch)
                    return;
                if (generation && *generation == current->generation
                    && current->offeredRecordGeneration == *generation && matches(current->offered)) {
                    owner->invalidate();
                    current->readyGeneration = -1;
                    emit owner->status(tr("Clipboard transfer cancelled. Paste was not sent."));
                } else if (matches(current->incoming)) {
                    if (!current->offered || current->offeredRecordGeneration != current->generation)
                        emit owner->progress(-2);
                    emit owner->status(tr("Incoming clipboard transfer cancelled."));
                }
            });
            return;
        }
        if (generation && (record.operation == AssetOperation::Committed || record.operation == AssetOperation::Credit)) {
            QMetaObject::invokeMethod(qApp, [owner = state->owner, generation, record] {
                if (!owner)
                    return;
                const auto current = owner->m_state;
                if (current->generation != *generation || current->epoch != record.epoch
                    || !owner->isAdmitted() || !current->offered
                    || current->offeredRecordGeneration != *generation
                    || current->offered->transfer != record.transfer || current->offered->serial != record.serial)
                    return;
                if (record.operation == AssetOperation::Committed) {
                    current->readyGeneration = *generation;
                    current->outgoingReadyGeneration = *generation;
                    emit owner->progress(-2);
                    emit owner->status(tr("Clipboard ready."));
                    emit owner->outgoingCommitted(*generation, record.epoch);
                    return;
                }
                quint64 completed = record.offset;
                for (const auto &item : current->offered->items) {
                    if (item.id < record.item)
                        completed += item.size;
                }
                const auto total = current->offered->totalBytes;
                const int percent = total ? int(std::min<quint64>(100, completed * 100 / total)) : 100;
                if (percent != current->sendProgressPercent) {
                    current->sendProgressPercent = percent;
                    emit owner->progress(percent);
                    emit owner->status(percent == 100 ? tr("Verifying remote clipboard…")
                        : tr("Sending clipboard: %1%").arg(percent));
                }
            });
        }
    });
}

void RichClipboardTransfer::superseded(bool ownershipKnown)
{
    invalidate();
    ++m_state->generation;
    m_state->readyGeneration = -1;
    m_state->offeredGeneration = -1;
    m_state->offered.reset();
    m_state->offeredRecordGeneration = -1;
    m_state->ownMarker.clear();
    m_state->dropIntent = false;
    const auto state = m_state;
    LocalAssetSelection selection;
    selection.generation = state->generation;
    selection.ownershipKnown = ownershipKnown;
    if (state->admitted)
        state->selectLatest(state, [state, selection] { state->engine->localChanged(selection, std::chrono::steady_clock::now()); });
}

void RichClipboardTransfer::select(const QMimeData &mime, bool ownershipKnown)
{
    const auto state = m_state;
    if (!state->admitted || !state->eligible())
        return;
    LocalAssetSelection selection;
    selection.generation = state->generation;
    selection.ownershipKnown = ownershipKnown;
    const auto kind = clipboardSelectionKind(mime);
    if (kind == ClipboardSelectionKind::Files && state->files) {
        selection.format = AssetFormat::Files;
        for (const auto &path : clipboardLocalFiles(mime))
            selection.paths.push_back(path.toStdString());
    } else if (kind == ClipboardSelectionKind::Png && state->images) {
        QByteArray bytes;
        QImage image;
        if (mime.hasFormat("image/png")) {
            bytes = mime.data("image/png");
            if (bytes.size() > qsizetype(AssetClipboardMaximumPngBytes))
                throw std::invalid_argument("Clipboard PNG exceeds its limit.");
        } else {
            const auto value = mime.imageData();
            if (value.canConvert<QImage>())
                image = qvariant_cast<QImage>(value);
            else if (value.canConvert<QPixmap>())
                image = qvariant_cast<QPixmap>(value).toImage();
            if (image.isNull() || quint64(image.width()) * image.height() > AssetClipboardMaximumPixels)
                throw std::invalid_argument("Clipboard image exceeds its limit.");
        }
        state->offeredGeneration = selection.generation;
        state->readyGeneration = -1;
        emit progress(-1);
        emit status(tr("Preparing clipboard image…"));
        state->selectLatest(state, [state, generation = selection.generation, bytes, image] {
            if (state->generation != generation || !state->admitted || state->stopping)
                return;
            QMimeData snapshot;
            if (!bytes.isEmpty())
                snapshot.setData("image/png", bytes);
            else
                snapshot.setImageData(image);
            const auto prepared = prepareClipboardImage(snapshot);
            state->pngReuse.remember(prepared);
            const auto resetReuse = qScopeGuard([state] { state->pngReuse.consume({}); });
            const auto &normalized = prepared.png();
            LocalAssetSelection png;
            png.format = AssetFormat::Png;
            png.generation = generation;
            png.image.assign(normalized.begin(), normalized.end());
            state->engine->localChanged(std::move(png), std::chrono::steady_clock::now());
        });
        return;
    } else {
        superseded(kind != ClipboardSelectionKind::Files && ownershipKnown);
        emit status(tr("This clipboard format is not enabled for sharing."));
        return;
    }
    QSet<QString> names;
    for (const auto &path : selection.paths) {
        const auto name = QFileInfo(QString::fromStdString(path)).fileName()
            .normalized(QString::NormalizationForm_C).toCaseFolded();
        if (names.contains(name))
            throw std::invalid_argument("Duplicate normalized clipboard basename.");
        names.insert(name);
    }
    state->offeredGeneration = selection.generation;
    state->readyGeneration = -1;
    emit progress(-1);
    emit status(tr("Preparing clipboard files…"));
    state->selectLatest(state, [state, selection = std::move(selection)]() mutable {
        if (state->generation == selection.generation && state->admitted && !state->stopping)
            state->engine->localChanged(std::move(selection), std::chrono::steady_clock::now());
    });
}

void RichClipboardTransfer::changed()
{
    if (ownsClipboard())
        return;
    superseded(false);
    if (const auto *mime = QGuiApplication::clipboard()->mimeData()) {
        try {
            select(*mime);
        } catch (const std::exception &) {
            invalidate();
            emit status(tr("Clipboard content is unsupported or exceeds the transfer limits."));
        }
    }
}

bool RichClipboardTransfer::paste()
{
    refresh();
    if (!m_state->admitted)
        return false;
    if (m_state->readyGeneration == m_state->generation)
        return true;
    if (m_state->offeredGeneration != m_state->generation) {
        if (const auto *mime = QGuiApplication::clipboard()->mimeData()) {
            try {
                select(*mime);
            } catch (const std::exception &) {
                invalidate();
                emit status(tr("Clipboard content is unsupported or exceeds the transfer limits."));
            }
        }
    }
    return false;
}

void RichClipboardTransfer::drop(const QStringList &paths)
{
    refresh();
    if (!m_state->admitted)
        return;
    superseded(false);
    m_state->dropIntent = true;
    QMimeData mime;
    QList<QUrl> urls;
    for (const auto &path : paths)
        urls.append(QUrl::fromLocalFile(path));
    mime.setUrls(urls);
    select(mime, false);
}

qint64 RichClipboardTransfer::selectionGeneration() const { return m_state->generation; }
quint64 RichClipboardTransfer::focusEpoch() const { return m_state->epoch; }
bool RichClipboardTransfer::isAdmitted() const
{
    return m_state->admitted && !m_state->stopping && !m_state->disabled && m_state->eligible();
}
bool RichClipboardTransfer::outgoingReady() const
{
    return isAdmitted() && m_state->outgoingReadyGeneration == m_state->generation;
}
void RichClipboardTransfer::invalidate()
{
    m_state->outgoingReadyGeneration = -1;
    emit invalidated();
    emit progress(-2);
}
QByteArray RichClipboardTransfer::queuedRecord() const
{
    for (const auto &bytes : m_state->outgoing) {
        if (decode(bytes).operation == AssetOperation::Offer)
            return bytes;
    }
    return {};
}

bool RichClipboardTransfer::hasDropIntent() const { return m_state->dropIntent; }

bool RichClipboardTransfer::ownsClipboard() const
{
    const auto *mime = QGuiApplication::clipboard()->mimeData();
    return mime && !m_state->ownMarker.isEmpty()
        && mime->data(ClipboardOwnershipMime) == m_state->ownMarker
        && m_state->ownGeneration == m_state->generation && m_state->ownEpoch == m_state->epoch;
}

void RichClipboardTransfer::publish(const QByteArray &offer, const QStringList &paths,
    const QImage &image, const QByteArray &png, const QByteArray &marker, qint64 generation,
    const std::shared_ptr<std::atomic_bool> &ticket)
{
    const auto state = m_state;
    const auto record = decode(offer);
    bool success = false;
    try {
        if (ticket->load() && state->admitted && !state->stopping && state->eligible()
            && generation == state->generation && record.epoch == state->epoch) {
            std::unique_ptr<QMimeData> mime;
            if (image.isNull()) {
                mime = clipboardFileCopy(paths, marker);
            } else {
                mime = std::make_unique<QMimeData>();
                mime->setImageData(image);
                mime->setData("image/png", png);
                mime->setData(ClipboardOwnershipMime, marker);
            }
            // GUI thread: no event dispatch occurs between the final fence and write.
            if (ticket->load() && state->admitted && generation == state->generation) {
                state->ownMarker = marker;
                state->ownGeneration = generation;
                state->ownEpoch = state->epoch;
                QGuiApplication::clipboard()->setMimeData(mime.release());
                const auto *actual = QGuiApplication::clipboard()->mimeData();
                success = QGuiApplication::clipboard()->ownsClipboard() && actual
                    && actual->data(ClipboardOwnershipMime) == marker
                    && (image.isNull() ? actual->hasUrls() : actual->hasImage());
                if (!success) {
                    state->ownMarker.clear();
                    invalidate();
                    emit status(tr("Clipboard publication failed. Paste was not sent."));
                }
                if (success) {
                    state->readyGeneration = generation;
                    state->offeredGeneration = generation;
                    emit published();
                    emit progress(-2);
                    emit status(tr("Remote clipboard received. Ready to paste locally."));
                }
            }
        }
    } catch (const std::exception &) {
        success = false;
        state->ownMarker.clear();
        invalidate();
        emit status(tr("Clipboard publication failed. Paste was not sent."));
    }
    state->post(state, [state, record, success] { state->engine->publicationCompleted(record, success); });
}

void RichClipboardTransfer::enqueue(const QByteArray &bytes, std::optional<qint64> generation)
{
    const auto record = decode(bytes);
    if (m_state->stopping)
        return;
    if (!m_state->admitted && record.operation != AssetOperation::Arm && record.operation != AssetOperation::Pause)
        return;
    auto &queue = m_state->outgoing;
    // Preserve the wire FIFO even when this GUI callback belongs to an old
    // selection. Only the I/O-captured generation may establish UI ownership.
    const bool currentGeneration = generation && *generation == m_state->generation
        && record.epoch == m_state->epoch && isAdmitted();
    if (record.operation == AssetOperation::Offer && currentGeneration) {
        m_state->offered = record;
        m_state->offeredRecordGeneration = *generation;
        m_state->sendProgressPercent = -1;
        emit progress(0);
        emit status(tr("Sending clipboard…"));
    }
    if (record.operation == AssetOperation::Finish && currentGeneration) {
        emit progress(-1);
        emit status(tr("Verifying remote clipboard…"));
    }
    if (record.operation == AssetOperation::Committed) {
        emit progress(-2);
        emit status(tr("Remote clipboard received. Ready to paste locally."));
    }
    if (record.operation == AssetOperation::Cancel) {
        const auto matches = [&record](const std::optional<AssetRecord> &offer) {
            return offer && offer->epoch == record.epoch && offer->serial == record.serial
                && offer->transfer == record.transfer;
        };
        if (record.epoch == m_state->epoch && matches(m_state->offered)
            && m_state->offeredRecordGeneration == m_state->generation) {
            invalidate();
            m_state->readyGeneration = -1;
            emit status(tr("Clipboard transfer cancelled. Paste was not sent."));
        } else if (record.epoch == m_state->epoch && matches(m_state->incoming)) {
            if (!m_state->offered || m_state->offeredRecordGeneration != m_state->generation)
                emit progress(-2);
            emit status(tr("Incoming clipboard transfer cancelled."));
        }
    }
    if (record.operation == AssetOperation::Credit && m_state->incoming
        && m_state->incoming->transfer == record.transfer && m_state->incoming->totalBytes) {
        quint64 completed = record.offset;
        for (const auto &item : m_state->incoming->items) {
            if (item.id < record.item)
                completed += item.size;
        }
        const int percent = int(std::min<quint64>(100, completed * 100 / m_state->incoming->totalBytes));
        if (percent != m_state->receiveProgressPercent) {
            m_state->receiveProgressPercent = percent;
            emit progress(percent);
            emit status(percent == 100 ? tr("Finalizing received clipboard…")
                : tr("Receiving clipboard: %1%").arg(percent));
        }
    }
    if (record.operation == AssetOperation::Arm || record.operation == AssetOperation::Pause) {
        std::erase_if(queue, [](const QByteArray &pending) {
            const auto value = decode(pending);
            return value.operation != AssetOperation::Arm && value.operation != AssetOperation::Pause;
        });
    }
    if (record.operation == AssetOperation::Cancel || record.operation == AssetOperation::CancelAcknowledged) {
        std::erase_if(queue, [&record](const QByteArray &pending) {
            const auto value = decode(pending);
            return value.epoch == record.epoch && value.serial == record.serial && value.transfer == record.transfer
                && (value.operation == AssetOperation::Chunk || value.operation == AssetOperation::Credit
                    || value.operation == AssetOperation::Finish);
        });
    }
    if (queue.size() >= 24) {
        stop();
        emit status(tr("Clipboard writer exceeded its limit. Reconnect to share again."));
        return;
    }
    queue.push_back(bytes);
    pump();
}

void RichClipboardTransfer::pump()
{
    auto &queue = m_state->outgoing;
    while (!queue.empty() && m_state->channel->send(5, queue.front()))
        queue.pop_front();
}

void RichClipboardTransfer::stop()
{
    const auto state = m_state;
    if (state->stopping.exchange(true))
        return;
    invalidate();
    state->admitted = false;
    state->cache->removeObserver(state.get());
    state->outgoing.clear();
    {
        std::lock_guard lock(state->selectionMutex);
        state->selectionJob = {};
    }
    ++state->generation;
    state->post(state, [state] {
        if (state->engine)
            state->engine->stop();
        state->engine.reset();
        state->ioStopped = true;
    }, true);
}

void RichClipboardTransfer::scheduleIo(std::function<void()> job)
{
    m_state->post(m_state, std::move(job), true);
}

unsigned RichClipboardTransfer::pendingIoJobs() const { return m_state->jobs; }

bool RichClipboardTransfer::stopped() const { return m_state->stopping && m_state->ioStopped && !m_state->jobs; }
bool RichClipboardTransfer::wait()
{
    std::unique_lock lock(m_state->mutex);
    const bool drained = m_state->drained.wait_for(lock, std::chrono::seconds(3), [this] { return m_state->jobs == 0; });
    return drained && (!m_state->stopping || m_state->ioStopped);
}

} // namespace dockpipe::desktop
