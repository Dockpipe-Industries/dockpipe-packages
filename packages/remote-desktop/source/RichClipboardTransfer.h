#pragma once

#include <QObject>
#include <QByteArray>
#include <QStringList>
#include <QImage>
#include <functional>
#include <memory>
#include <atomic>
#include <optional>

class QMimeData;
namespace dockpipe::desktop {
class ProcessChannel;

// Qt publication and focus boundary around the shared serial transfer engine.
class RichClipboardTransfer final : public QObject {
    Q_OBJECT
public:
    RichClipboardTransfer(ProcessChannel &channel, bool images, bool files,
                          std::function<bool()> eligible, QObject *parent);
    ~RichClipboardTransfer() override;
    void refresh();
    void pause();
    void receive(const QByteArray &bytes);
    void changed();
    bool paste(); // false consumes this paste until validated outgoing completion.
    void drop(const QStringList &paths);
    void superseded(bool ownershipKnown = true);
    bool hasDropIntent() const;
    void stop();
    bool stopped() const;
    bool wait();
    bool ownsClipboard() const;
    qint64 selectionGeneration() const;
    quint64 focusEpoch() const;
    bool isAdmitted() const;
    bool outgoingReady() const;

signals:
    void status(const QString &text);
    void published();
    void incomingOffer();
    void outgoingCommitted(qint64 generation, quint64 epoch);
    void invalidated();
    void progress(int percent);

private:
    friend class RichClipboardTransferTest;
    void scheduleIo(std::function<void()> job);
    unsigned pendingIoJobs() const;
    QByteArray queuedRecord() const;
    void invalidate();
    struct State;
    std::shared_ptr<State> m_state;
    void select(const QMimeData &mime, bool ownershipKnown = true);
    void pump();
    void enqueue(const QByteArray &bytes, std::optional<qint64> generation = std::nullopt);
    void publish(const QByteArray &offer, const QStringList &paths,
                 const QImage &image, const QByteArray &png, const QByteArray &marker,
                 qint64 generation, const std::shared_ptr<std::atomic_bool> &ticket);
};
} // namespace dockpipe::desktop
