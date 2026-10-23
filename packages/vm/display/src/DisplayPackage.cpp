#include <LauncherDisplayPackage.h>
#include "DisplayEndpoint.h"
#include "QemuDisplayTransport.h"
#include <QLabel>
#include <QTimer>
#include <QVBoxLayout>
#include <QGuiApplication>
#include <QElapsedTimer>

class QemuDisplaySurface final : public LauncherDisplaySurface {
public:
    QemuDisplaySurface(const QJsonObject &options, QWidget *parent)
        : LauncherDisplaySurface(parent)
    {
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        status_ = new QLabel(QStringLiteral("Connecting to local display"), this);
        layout->addWidget(status_);
        QString error;
        if (!DisplayEndpoint::load(options, endpoint_, error)) {
            status_->setText(error);
            QTimer::singleShot(0, this, [this, error] {
                if (failed)
                    failed(error);
            });
            return;
        }
        widget_ = new DisplayWidget(frames_, this);
        layout->addWidget(widget_, 1);
        transport_ = std::make_unique<QemuDisplayTransport>(frames_, endpoint_.busAddress,
            endpoint_.consoleId, endpoint_.qemuPid, endpoint_.qemuStartToken);
        widget_->sendInput = [this](DisplayInput input) {
            if (active_ && transport_)
                transport_->submit(input);
        };
        timer_ = new QTimer(this);
        connect(timer_, &QTimer::timeout, this, [this] { poll(); });
        timer_->start(100);
        connectionAge_.start();
        transport_->start();
    }

    ~QemuDisplaySurface() override
    {
        closeDisplay();
    }

    void setActive(bool active) override
    {
        if (!active && widget_)
            widget_->releaseInput();
        active_ = active && !closed_;
        if (widget_)
            widget_->setEnabled(active_);
    }

    void closeDisplay() override
    {
        if (closed_)
            return;
        setActive(false);
        closed_ = true;
        setProperty("displayReady", false);
        if (timer_)
            timer_->stop();
        if (transport_)
            transport_->stop();
    }

private:
    void poll()
    {
        if (closed_ || !transport_)
            return;
        auto error = transport_->error();
        if (error.isEmpty() && connectionAge_.elapsed() > 5000 && frames_.revisions() == 0)
            error = QStringLiteral("QEMU display frame unavailable");
        if (!error.isEmpty()) {
            status_->setText(QStringLiteral("Display disconnected: ") + error);
            closeDisplay();
            if (failed)
                failed(error);
            return;
        }
        widget_->setPointerState(transport_->pointerState());
        setProperty("displayTransportKind", transport_->deliveryKind());
        setProperty("displayFrameRevision", QVariant::fromValue(frames_.revisions()));
        setProperty("displayInputAcknowledgements", QVariant::fromValue(transport_->inputAcknowledgements()));
        const bool hasFrame = !frames_.snapshot().isNull();
        setProperty("displayReady", hasFrame && transport_->connected());
        if (hasFrame && !readyReported_) {
            readyReported_ = true;
            setProperty("displayReady", true);
            status_->setText(QStringLiteral("Display connected"));
            if (statusChanged)
                statusChanged(QStringLiteral("Local display ready"));
        }
    }

    FrameStore frames_;
    DisplayEndpoint endpoint_;
    std::unique_ptr<QemuDisplayTransport> transport_;
    DisplayWidget *widget_ = nullptr;
    QLabel *status_ = nullptr;
    QTimer *timer_ = nullptr;
    bool active_ = true;
    bool closed_ = false;
    bool readyReported_ = false;
    QElapsedTimer connectionAge_;
};

class QemuDisplayPackage final : public QObject, public LauncherDisplayPackage {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID LauncherDisplayPackage_iid FILE "display-metadata.json")
    Q_INTERFACES(LauncherDisplayPackage)
public:
    void initialize(const QString &packageRoot) override
    {
        packageRoot_ = packageRoot;
    }

    LauncherDisplaySurface *createDisplay(const QJsonObject &options, QWidget *parent) override
    {
        return new QemuDisplaySurface(options, parent);
    }
private:
    QString packageRoot_;
};
#include "DisplayPackage.moc"
