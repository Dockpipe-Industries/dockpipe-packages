#include "DesktopViewerSurface.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QObject>
#include <unistd.h>

namespace dockpipe::desktop {

class DesktopDisplayPlugin final : public QObject, public LauncherDisplayPackage {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID LauncherDisplayPackage_iid FILE "display-metadata.json")
    Q_INTERFACES(LauncherDisplayPackage)

public:
    void initialize(const QString &packageRoot) override
    {
        m_helper.clear();
        const QFileInfo package(packageRoot);
        if (!package.isAbsolute() || !package.isDir() || package.isSymLink())
            return;
        const QDir root(package.canonicalFilePath());
        const QFileInfo descriptor(root.filePath("assets/display/display-factory.json"));
        const auto descriptorPath = descriptor.canonicalFilePath();
        if (!descriptor.isFile() || descriptor.isSymLink() || descriptor.size() <= 0 || descriptor.size() > 16384
            || !descriptorPath.startsWith(root.canonicalPath() + QDir::separator()))
            return;
        QFile manifest(descriptorPath);
        if (!manifest.open(QIODevice::ReadOnly) || manifest.size() != descriptor.size())
            return;
        QJsonParseError error{};
        const auto document = QJsonDocument::fromJson(manifest.readAll(), &error);
        if (error.error != QJsonParseError::NoError || !document.isObject())
            return;
        const auto factory = document.object();
        const auto channel = factory.value("channel_helper").toObject();
        const auto relative = channel.value("path").toString();
        const auto expected = channel.value("sha256").toString().toLatin1();
        if (factory.value("schema").toInteger() != 1 || relative.isEmpty()
            || QDir::isAbsolutePath(relative) || QDir::cleanPath(relative) != relative
            || relative == ".." || relative.startsWith("../") || expected.size() != 64)
            return;
        const QFileInfo helper(root.filePath(relative));
        const auto resolved = helper.canonicalFilePath();
        if (!helper.isFile() || helper.isSymLink() || !helper.isExecutable()
            || (helper.ownerId() != 0 && helper.ownerId() != ::geteuid())
            || (helper.permissions() & (QFileDevice::WriteGroup | QFileDevice::WriteOther))
            || !resolved.startsWith(root.canonicalPath() + QDir::separator())
            || helper.size() <= 0 || helper.size() > 64 * 1024 * 1024)
            return;
        QFile input(resolved);
        if (!input.open(QIODevice::ReadOnly))
            return;
        QCryptographicHash hash(QCryptographicHash::Sha256);
        qint64 total = 0;
        while (!input.atEnd()) {
            const auto chunk = input.read(65536);
            if (chunk.isEmpty())
                return;
            total += chunk.size();
            if (total > 64 * 1024 * 1024)
                return;
            hash.addData(chunk);
        }
        if (input.error() != QFileDevice::NoError || total != helper.size() || hash.result().toHex() != expected)
            return;
        m_helper = resolved;
    }

    LauncherDisplaySurface *createDisplay(const QJsonObject &options, QWidget *parent) override
    {
        if (m_helper.isEmpty())
            return nullptr;
        try {
            return new DesktopViewerSurface(options, m_helper, parent);
        } catch (const std::exception &) {
            return nullptr;
        }
    }

private:
    QString m_helper;
};

} // namespace dockpipe::desktop

#include "DesktopDisplayPlugin.moc"
