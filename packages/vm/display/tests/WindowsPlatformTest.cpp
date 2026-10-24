#define NOMINMAX
#include <windows.h>
#include <aclapi.h>
#include "NativeDisplayPlatform.h"
#include "SharedFrameSource.h"
#include "NativeFixtureOwnership.h"
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

class WindowsPlatformTest : public QObject {
    Q_OBJECT
private slots:
    void liveCreationToken()
    {
        FILETIME creation{}, exit{}, kernel{}, user{};
        QVERIFY(GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user));
        const auto token = QString::number((quint64(creation.dwHighDateTime) << 32) | creation.dwLowDateTime);
        QVERIFY(NativeDisplayPlatform::processMatches(int(GetCurrentProcessId()), token));
        QVERIFY(!NativeDisplayPlatform::processMatches(int(GetCurrentProcessId()), "0"));
        QVERIFY(!NativeDisplayPlatform::processMatches(int(GetCurrentProcessId()), "+" + token));
    }
    void readOnlyMappingAndLifetime()
    {
        HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 4096, nullptr);
        QVERIFY(mapping != nullptr);
        auto *writer = static_cast<quint32 *>(MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, 4096));
        QVERIFY(writer != nullptr);
        writer[2] = 0x00ff0000;
        writer[3] = 0x000000ff;
        HANDLE reader = nullptr;
        QVERIFY(DuplicateHandle(GetCurrentProcess(), mapping, GetCurrentProcess(), &reader,
                                 FILE_MAP_READ | SECTION_QUERY, FALSE, 0));
        FrameStore frames;
        std::atomic<bool> stopped{false};
        SharedFrameSource source(frames, stopped);
        QVERIFY(source.replace(reinterpret_cast<qintptr>(reader), 8, 2, 1, 8, 0x20020888));
        QCOMPARE(frames.snapshot().pixelColor(0, 0), QColor(Qt::red));
        QCOMPARE(frames.snapshot().pixelColor(1, 0), QColor(Qt::blue));
        writer[3] = 0x0000ff00;
        QVERIFY(source.update(1, 0, 1, 1));
        QCOMPARE(frames.snapshot().pixelColor(1, 0), QColor(Qt::green));
        UnmapViewOfFile(writer);
        CloseHandle(mapping);
        QVERIFY(source.update(0, 0, 2, 1));
        QVERIFY(!source.update(-1, 0, 1, 1));
        DWORD flags = 0;
        QVERIFY(!GetHandleInformation(reader, &flags));
        QCOMPARE(frames.snapshot().pixelColor(1, 0), QColor(Qt::green));
    }
    void invalidMappingAndCancelledRead()
    {
        FrameStore frames;
        std::atomic<bool> stopped{true};
        SharedFrameSource source(frames, stopped);
        QVERIFY(!source.replace(0, 0, 1, 1, 4, 0x20020888));
        HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 4096, nullptr);
        QVERIFY(mapping != nullptr);
        QVERIFY(!source.replace(reinterpret_cast<qintptr>(mapping), 0, 1, 1, 4, 0x20020888));
        QVERIFY(frames.snapshot().isNull());
    }
    void privateMetadataAcl()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QVERIFY(NativeDisplayPlatform::protectDirectory(directory.path()));
        QVERIFY(!NativeFixtureOwnership::assignFreshObjectToCurrentUser(directory.path()));
        const auto path = directory.path() + "/display.json";
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("{}");
        file.close();
        QVERIFY(NativeFixtureOwnership::assignFreshObjectToCurrentUser(path));
        QByteArray bytes;
        QVERIFY(NativeDisplayPlatform::readPrivateDescriptor(directory.path(), bytes));
        QCOMPARE(bytes, QByteArray("{}"));
        char sid[SECURITY_MAX_SID_SIZE]{};
        DWORD sidSize = sizeof(sid);
        QVERIFY(CreateWellKnownSid(WinWorldSid, nullptr, sid, &sidSize));
        EXPLICIT_ACCESSW access{};
        access.grfAccessPermissions = FILE_GENERIC_READ;
        access.grfAccessMode = GRANT_ACCESS;
        access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        access.Trustee.ptstrName = reinterpret_cast<LPWSTR>(sid);
        PACL acl = nullptr;
        QCOMPARE(SetEntriesInAclW(1, &access, nullptr, &acl), DWORD(ERROR_SUCCESS));
        auto native = path.toStdWString();
        QCOMPARE(SetNamedSecurityInfoW(native.data(), SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
            nullptr, nullptr, acl, nullptr), DWORD(ERROR_SUCCESS));
        LocalFree(acl);
        QVERIFY(!NativeDisplayPlatform::readPrivateDescriptor(directory.path(), bytes));
    }
};
QTEST_MAIN(WindowsPlatformTest)
#include "WindowsPlatformTest.moc"
