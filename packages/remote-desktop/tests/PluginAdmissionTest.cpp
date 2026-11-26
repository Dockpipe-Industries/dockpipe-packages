#include <LauncherDisplayPackage.h>

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QPluginLoader>
#include <QProcess>
#include <QTemporaryDir>
#include <QSysInfo>
#include <QtTest>
#include <memory>
#include <sys/stat.h>
#include <unistd.h>

namespace {
QJsonObject options(const QString &state)
{
    return {{"schema", "dockpipe.desktop.native/v1"}, {"cli", QCoreApplication::applicationFilePath()},
        {"state_directory", state}, {"mode", "view"},
        {"catalog", QJsonObject{{"schema", "dockpipe.remote.desktop/v1"}, {"view", true}, {"input", false},
            {"epoch", QString(64, 'a')},
            {"target", QJsonObject{{"node", "fixture"}, {"fingerprint", QString(64, 'b')},
                                   {"generation", QString(64, 'c')}}},
            {"provider", QJsonObject{{"id", "dockpipe.desktop.native"}, {"version", "0.1.0"},
                                     {"artifact", QString(64, 'd')}}}}}};
}

struct PackageFixture {
    QTemporaryDir directory;
    QString root;
    QString assets;
    QString helper;
    QString descriptor;
    QJsonObject manifest;

    bool prepare()
    {
        root = directory.filePath("package");
        assets = root + "/assets";
        helper = assets + "/tooling/bin/helper";
        descriptor = assets + "/display/display-factory.json";
        if (!directory.isValid() || !QDir().mkpath(assets + "/tooling/bin") || !QDir().mkpath(assets + "/display"))
            return false;
        QFile binary(helper);
        const QByteArray bytes("#!/bin/sh\nexit 99\n");
        if (!binary.open(QIODevice::WriteOnly) || binary.write(bytes) != bytes.size()
            || !binary.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner))
            return false;
        binary.close();
        manifest = {{"schema", 1}, {"channel_helper", QJsonObject{{"path", "assets/tooling/bin/helper"},
            {"sha256", QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())}}}};
        return writeManifest();
    }

    bool writeManifest()
    {
        QFile output(descriptor);
        const auto bytes = QJsonDocument(manifest).toJson(QJsonDocument::Compact);
        return output.open(QIODevice::WriteOnly | QIODevice::Truncate) && output.write(bytes) == bytes.size();
    }
};
}

class PluginAdmissionTest final : public QObject {
    Q_OBJECT
private:
    QPluginLoader m_loader{QString(PLUGIN_PATH)};
    LauncherDisplayPackage *m_plugin = nullptr;

    bool admitted(PackageFixture &fixture, const QString &packageRoot = {})
    {
        m_plugin->initialize(packageRoot.isEmpty() ? fixture.root : packageRoot);
        std::unique_ptr<LauncherDisplaySurface> surface(m_plugin->createDisplay(options(fixture.root), nullptr));
        if (!surface)
            return false;
        for (auto *process : surface->findChildren<QProcess *>()) {
            if (process->state() != QProcess::NotRunning)
                return false;
        }
        surface->closeDisplay();
        return true;
    }

private slots:
    void initTestCase()
    {
        m_loader.setLoadHints(QLibrary::ResolveAllSymbolsHint);
        QCOMPARE(m_loader.metaData().value("IID").toString(), QString(LauncherDisplayPackage_iid));
        const auto contract = m_loader.metaData().value("MetaData").toObject();
        QCOMPARE(contract.value("display_abi").toInt(), 1);
        QCOMPARE(contract.value("qt_version").toString(), QString(QT_VERSION_STR));
        QCOMPARE(contract.value("build_abi").toString(), QSysInfo::buildAbi());
        QCOMPARE(contract.value("compiler_id").toString(), QString(DISPLAY_COMPILER_ID));
        QCOMPARE(contract.value("compiler_version").toString(), QString(DISPLAY_COMPILER_VERSION));
        auto *instance = m_loader.instance();
        QVERIFY2(instance, qPrintable(m_loader.errorString()));
        m_plugin = qobject_cast<LauncherDisplayPackage *>(instance);
        QVERIFY(m_plugin);
    }

    void validContainedHelperCreatesOnlyAnInactiveSurface()
    {
        PackageFixture fixture;
        QVERIFY(fixture.prepare());
        QVERIFY(admitted(fixture));
    }

    void assetsDirectoryCannotSubstituteForTheCatalogPackageRoot()
    {
        PackageFixture fixture;
        QVERIFY(fixture.prepare());
        QVERIFY(!admitted(fixture, fixture.assets));
        QVERIFY(admitted(fixture, fixture.root));
    }

    void helperHashChangeInvalidatesThePreviousInitialization()
    {
        PackageFixture fixture;
        QVERIFY(fixture.prepare());
        QVERIFY(admitted(fixture));
        QFile helper(fixture.helper);
        QVERIFY(helper.open(QIODevice::Append));
        helper.write("changed");
        helper.close();
        QVERIFY(!admitted(fixture));
    }

    void helperPathsMustBeRelativeNormalizedAndContained_data()
    {
        QTest::addColumn<QString>("path");
        QTest::newRow("absolute") << QString("/bin/true");
        QTest::newRow("escape") << QString("../outside");
        QTest::newRow("unnormalized") << QString("assets/tooling/../tooling/bin/helper");
    }

    void helperPathsMustBeRelativeNormalizedAndContained()
    {
        QFETCH(QString, path);
        PackageFixture fixture;
        QVERIFY(fixture.prepare());
        auto channel = fixture.manifest.value("channel_helper").toObject();
        channel.insert("path", path);
        fixture.manifest.insert("channel_helper", channel);
        QVERIFY(fixture.writeManifest());
        QVERIFY(!admitted(fixture));
    }

    void helperSymlinkAndUnsafeWritePermissionsAreRejected()
    {
        PackageFixture fixture;
        QVERIFY(fixture.prepare());
        const QString original = fixture.helper + ".original";
        QVERIFY(QFile::rename(fixture.helper, original));
        QVERIFY(::symlink(QFile::encodeName(original).constData(), QFile::encodeName(fixture.helper).constData()) == 0);
        QVERIFY(!admitted(fixture));
        QVERIFY(QFile::remove(fixture.helper));
        QVERIFY(QFile::rename(original, fixture.helper));
        QVERIFY(QFile::setPermissions(fixture.helper, QFileDevice::ReadOwner | QFileDevice::WriteOwner
            | QFileDevice::ExeOwner | QFileDevice::WriteOther));
        QVERIFY(!admitted(fixture));
    }

    void descriptorSymlinkAndFIFOAreRejectedWithoutOpeningThem()
    {
        PackageFixture fixture;
        QVERIFY(fixture.prepare());
        const QString outside = fixture.directory.filePath("outside.json");
        QVERIFY(QFile::rename(fixture.descriptor, outside));
        QVERIFY(::symlink(QFile::encodeName(outside).constData(), QFile::encodeName(fixture.descriptor).constData()) == 0);
        QVERIFY(!admitted(fixture));
        QVERIFY(QFile::remove(fixture.descriptor));
        QVERIFY(::mkfifo(QFile::encodeName(fixture.descriptor).constData(), 0600) == 0);
        QVERIFY(!admitted(fixture));
    }

    void oversizedDescriptorAndRelativePackageRootsAreRejected()
    {
        PackageFixture fixture;
        QVERIFY(fixture.prepare());
        const auto relative = QDir::current().relativeFilePath(fixture.root);
        QVERIFY(!QDir::isAbsolutePath(relative));
        QVERIFY(!admitted(fixture, relative));
        QFile descriptor(fixture.descriptor);
        QVERIFY(descriptor.open(QIODevice::WriteOnly | QIODevice::Truncate));
        descriptor.write(QByteArray(16385, 'x'));
        descriptor.close();
        QVERIFY(!admitted(fixture));
    }
};

QTEST_MAIN(PluginAdmissionTest)
#include "PluginAdmissionTest.moc"
