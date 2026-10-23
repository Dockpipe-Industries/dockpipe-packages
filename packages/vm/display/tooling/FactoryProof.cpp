#include <LauncherDisplayPackage.h>
#include <LauncherNativeLibraryScope.h>
#include <QDebug>
#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPluginLoader>
#include <QSysInfo>
#include <QTimer>
#include <QVBoxLayout>
#include <memory>

namespace {
bool hasVisibleContent(const QImage &image)
{
    if (image.isNull() || image.width() > 8192 || image.height() > 8192)
        return false;
    const auto firstPixel = image.pixel(0, 0);
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (image.pixel(x, y) != firstPixel)
                return true;
        }
    }
    return false;
}
}

// Minimal installed-interface proof only. This does not qualify the complete host,
// its resource collector, action routing or shared lease implementation.
class FactoryProof {
public:
    FactoryProof(QApplication &application, LauncherDisplayPackage &factory,
                 QJsonObject options, QString output)
        : application_(application), factory_(factory), options_(std::move(options)), output_(std::move(output))
    {
        window_.resize(1024, 768);
        layout_ = new QVBoxLayout(&window_);
        layout_->setContentsMargins(0, 0, 0, 0);
        receipt_.insert("schema", 1);
        receipt_.insert("scope", "installed-plugin-only");
        receipt_.insert("resource_lifecycle_owned_by_harness", false);
        open();
    }

private:
    void open()
    {
        surface_ = factory_.createDisplay(options_, &window_);
        if (!surface_) {
            failed_ = true;
            finish();
            return;
        }
        surface_->failed = [this](const QString &) { failed_ = true; };
        layout_->addWidget(surface_);
        surface_->setActive(true);
        window_.show();
        QTimer::singleShot(6500, &application_, [this] { captureAndTransfer(); });
    }

    void captureAndTransfer()
    {
        QString phase;
        if (phase_ == 0)
            phase = QStringLiteral("first");
        else if (phase_ == 1)
            phase = QStringLiteral("handoff");
        else
            phase = QStringLiteral("returned");
        const bool ready = surface_->property("displayReady").toBool();
        const auto kind = surface_->property("displayTransportKind").toString();
        const bool validKind = kind == "raw-local" || kind == "shared-memory";
        receipt_.insert(phase + "_ready", ready);
        receipt_.insert(phase + "_transport", validKind ? kind : "unknown");
        const bool saved = surface_->grab().save(output_ + "/" + phase + ".png");
        bool canvasHasContent = false;
        for (auto *child : surface_->findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly)) {
            if (dynamic_cast<QLabel *>(child))
                continue;
            receipt_.insert(phase + "_canvas_width", child->width());
            receipt_.insert(phase + "_canvas_height", child->height());
            const auto canvas = child->grab().toImage();
            canvasHasContent = hasVisibleContent(canvas);
            canvas.save(output_ + "/" + phase + "-canvas.png");
        }
        receipt_.insert(phase + "_capture", saved);
        receipt_.insert(phase + "_canvas_nonuniform", canvasHasContent);
        failed_ = failed_ || !ready || !saved || !validKind || !canvasHasContent;
        surface_->setActive(false);
        QElapsedTimer timer;
        timer.start();
        surface_->closeDisplay();
        surface_->closeDisplay();
        const bool closed = !surface_->property("displayReady").toBool() && timer.elapsed() < 1500;
        receipt_.insert(phase + "_closed", closed);
        failed_ = failed_ || !closed;
        delete surface_;
        surface_ = nullptr;
        if (++phase_ < 3 && !failed_)
            open();
        else
            finish();
    }

    void finish()
    {
        receipt_.insert("package_failure", failed_);
        receipt_.insert("success", !failed_ && phase_ == 3);
        QFile file(output_ + "/host-factory-proof.json");
        const auto bytes = QJsonDocument(receipt_).toJson();
        const bool written = file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
        application_.exit(!failed_ && written && phase_ == 3 ? 0 : 1);
    }

    QApplication &application_;
    LauncherDisplayPackage &factory_;
    QJsonObject options_;
    QString output_;
    QWidget window_;
    QVBoxLayout *layout_ = nullptr;
    LauncherDisplaySurface *surface_ = nullptr;
    QJsonObject receipt_;
    int phase_ = 0;
    bool failed_ = false;
};

int main(int argc, char **argv)
{
    QApplication application(argc, argv);
    if (argc != 3)
        return 2;
    QFile input(QString::fromLocal8Bit(argv[1]));
    if (!input.open(QIODevice::ReadOnly) || input.size() <= 0 || input.size() > 32768)
        return 2;
    const auto configuration = QJsonDocument::fromJson(input.readAll()).object();
    const QFileInfo package(configuration.value("package_root").toString());
    const QFileInfo library(configuration.value("library").toString());
    const auto root = package.canonicalFilePath();
    const auto module = library.canonicalFilePath();
    if (root.isEmpty() || module.isEmpty() || !module.startsWith(root + "/"))
        return 2;
    LauncherNativeLibraryScope nativeLibrary;
    QPluginLoader loader(module);
    loader.setLoadHints(QLibrary::PreventUnloadHint);
    const auto document = loader.metaData();
    const auto metadata = document.value("MetaData").toObject();
    if (document.value("IID") != LauncherDisplayPackage_iid || metadata.value("display_abi") != 1 ||
        metadata.value("qt_version") != qVersion() || metadata.value("build_abi") != QSysInfo::buildAbi() ||
        metadata.value("compiler_id") != DISPLAY_PROOF_COMPILER_ID ||
        metadata.value("compiler_version") != DISPLAY_PROOF_COMPILER_VERSION)
        return 2;
    QString nativeError;
    if (!nativeLibrary.load(module, nativeError)) {
        qCritical().noquote() << nativeError;
        return 2;
    }
    auto *factory = qobject_cast<LauncherDisplayPackage *>(loader.instance());
    if (!factory)
        return 2;
    factory->initialize(root);
    FactoryProof proof(application, *factory, configuration.value("options").toObject(),
                       QString::fromLocal8Bit(argv[2]));
    return application.exec();
}
