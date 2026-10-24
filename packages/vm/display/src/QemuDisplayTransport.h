#pragma once
#include "DisplayWidget.h"
#include "DisplayPointerState.h"
#include <memory>

class QemuDisplayTransport {
public:
    QemuDisplayTransport(FrameStore &frames, QString busAddress, unsigned consoleId,
                         int expectedPid, QString expectedStartToken, bool allowSharedMemory = true, QString inputAddress = {});
    ~QemuDisplayTransport();
    void start();
    void stop();
    void submit(DisplayInput input);
    QString error() const;
    bool connected() const;
    bool absolutePointer() const;
    DisplayPointerState pointerState() const;
    quint64 inputAcknowledgements() const;
    quint64 sharedScanouts() const;
    quint64 sharedUpdates() const;
    quint64 rawScanouts() const;
    quint64 rawBatches() const;
    QString deliveryKind() const;
private:
    class Implementation;
    std::unique_ptr<Implementation> implementation_;
};
