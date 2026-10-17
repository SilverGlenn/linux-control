#include <QtTest>
#include <QTemporaryDir>
#include <QDir>
#include <QFile>
#include <sys/stat.h>
#include "platform/kde/ScreenLockAppearance.h"
#include "settings/AeroThemeContract.h"

class ScreenLockAppearanceTest final : public QObject {
    Q_OBJECT
    static bool write(const QString &path, const QByteArray &data) {
        if (!QDir().mkpath(QFileInfo(path).absolutePath())) return false;
        QFile file(path); return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
    }
    static ScreenLockAppearance::Sources fixture(const QTemporaryDir &dir) {
        ScreenLockAppearance::Sources s; s.includeGlobalFallback = false;
        s.themeIdentities = AeroThemeContract::identities(); s.shellId = QStringLiteral("io.gitgud.wackyideas.desktop");
        s.shellDirectory = dir.filePath(QStringLiteral("shell"));
        write(s.shellDirectory + QStringLiteral("/metadata.json"), "{\"KPlugin\":{\"Id\":\"io.gitgud.wackyideas.desktop\"},\"X-Plasma-APIVersion\":\"2\"}");
        const auto auth = s.shellDirectory + QStringLiteral("/contents/lockscreen/AuthUI.qml");
        const auto lock = s.shellDirectory + QStringLiteral("/contents/lockscreen/LockScreen.qml");
        write(auth, "fixture UI: never execute"); write(lock, "fixture entry: never execute");
        s.authUiDigest = QString::fromLatin1(AeroThemeContract::inspectAssetDigest(auth));
        s.lockScreenDigest = QString::fromLatin1(AeroThemeContract::inspectAssetDigest(lock));
        s.providerPath = dir.filePath(QStringLiteral("provider.so")); write(s.providerPath, "fixture: not a shared library");
        s.providerDigest = QString::fromLatin1(AeroThemeContract::inspectAssetDigest(s.providerPath));
        s.sddmConfig = dir.filePath(QStringLiteral("sddm.conf")); write(s.sddmConfig, "[Theme]\nCurrent=sddm-theme-mod\n");
        s.dataRoots = {dir.filePath(QStringLiteral("data"))};
        write(s.dataRoots[0] + QStringLiteral("/sddm/themes/sddm-theme-mod/background"), "image evidence, not decoded");
        return s;
    }
private slots:
    void passiveReadOnlyEvidenceNeverLoadsProvider() {
        QTemporaryDir dir; QVERIFY(dir.isValid()); const auto s = fixture(dir);
        const auto before = AeroThemeContract::inspectAssetDigest(dir.path());
        const auto e = ScreenLockAppearance::inspect(s);
        QVERIFY(e.approvedIdentity); QVERIFY(e.providerVerified); QVERIFY(e.backgroundAvailable); QVERIFY(!e.editable);
        QCOMPARE(e.loginTheme, QStringLiteral("sddm-theme-mod"));
        QCOMPARE(e.backgroundDigest, AeroThemeContract::inspectAssetDigest(e.backgroundPath));
        QVERIFY(e.description.contains(QStringLiteral("not a verified image")));
        QVERIFY(e.reason.contains(QStringLiteral("KDE lock-wallpaper settings do not control")));
        QCOMPARE(AeroThemeContract::inspectAssetDigest(dir.path()), before);
    }
    void changedIdentityUiOrProviderFailsClosed() {
        QTemporaryDir dir; auto s = fixture(dir);
        s.themeIdentities[QStringLiteral("globalTheme")] = QStringLiteral("Breeze");
        auto e = ScreenLockAppearance::inspect(s); QVERIFY(!e.approvedIdentity); QVERIFY(!e.backgroundAvailable);
        s = fixture(dir); s.shellId = QStringLiteral("org.kde.plasma.desktop"); QVERIFY(!ScreenLockAppearance::inspect(s).approvedIdentity);
        s = fixture(dir); QVERIFY(write(s.providerPath, "changed"));
        e = ScreenLockAppearance::inspect(s); QVERIFY(!e.providerVerified); QVERIFY(!e.backgroundAvailable); QVERIFY(!e.editable);
        s = fixture(dir); QVERIFY(write(s.shellDirectory + QStringLiteral("/contents/lockscreen/AuthUI.qml"), "changed"));
        QVERIFY(!ScreenLockAppearance::inspect(s).approvedIdentity);
        s = fixture(dir); QVERIFY(write(s.shellDirectory + QStringLiteral("/contents/lockscreen/LockScreen.qml"), "changed"));
        QVERIFY(!ScreenLockAppearance::inspect(s).approvedIdentity);
    }
    void followsNativeLastDataDirectoryAndBackgroundPrecedence() {
        QTemporaryDir dir; auto s = fixture(dir);
        const auto other = dir.filePath(QStringLiteral("last")); s.dataRoots.append(other);
        const auto theme = other + QStringLiteral("/sddm/themes/sddm-theme-mod/");
        QVERIFY(write(theme + QStringLiteral("default-background"), "default"));
        auto e = ScreenLockAppearance::inspect(s); QVERIFY(e.backgroundAvailable);
        QCOMPARE(e.backgroundPath, theme + QStringLiteral("default-background"));
        QVERIFY(write(theme + QStringLiteral("background"), "preferred"));
        e = ScreenLockAppearance::inspect(s); QCOMPARE(e.backgroundPath, theme + QStringLiteral("background"));
        QVERIFY(QFile::remove(theme + QStringLiteral("background"))); QVERIFY(QFile::remove(theme + QStringLiteral("default-background")));
        QVERIFY(write(theme + QStringLiteral("theme.conf"), "[General]\nbackground=photo.png\n"));
        QVERIFY(write(theme + QStringLiteral("photo.png"), "photo"));
        e = ScreenLockAppearance::inspect(s); QVERIFY(e.backgroundAvailable); QCOMPARE(e.backgroundPath, theme + QStringLiteral("photo.png"));
    }
    void nonregularUiAndSymlinkTargetsAreInspectedWithoutOpeningDevices() {
        QTemporaryDir dir; auto s = fixture(dir);
        const auto auth = s.shellDirectory + QStringLiteral("/contents/lockscreen/AuthUI.qml");
        QVERIFY(QFile::remove(auth));
        const auto fifo = dir.filePath(QStringLiteral("not-a-ui.fifo"));
        QVERIFY(::mkfifo(QFile::encodeName(fifo).constData(), 0600) == 0);
        QVERIFY(QFile::link(fifo, auth));
        auto e = ScreenLockAppearance::inspect(s);
        QVERIFY(!e.approvedIdentity); QVERIFY(!e.backgroundAvailable);
        QVERIFY(QFile::remove(auth));
        const auto regular = dir.filePath(QStringLiteral("audited-ui.txt"));
        QVERIFY(write(regular, "fixture UI: never execute"));
        QVERIFY(QFile::link(regular, auth));
        e = ScreenLockAppearance::inspect(s);
        QVERIFY(e.approvedIdentity); QVERIFY(e.backgroundAvailable);
        QVERIFY(write(regular, "changed through symlink"));
        QVERIFY(!ScreenLockAppearance::inspect(s).approvedIdentity);
        s = fixture(dir);
        const auto lock = s.shellDirectory + QStringLiteral("/contents/lockscreen/LockScreen.qml");
        QVERIFY(QFile::remove(lock)); QVERIFY(::mkfifo(QFile::encodeName(lock).constData(), 0600) == 0);
        QVERIFY(!ScreenLockAppearance::inspect(s).approvedIdentity);
        QVERIFY(QFile::remove(lock));
        QVERIFY(QFile::link(dir.filePath(QStringLiteral("missing-target")), lock));
        QVERIFY(!ScreenLockAppearance::inspect(s).approvedIdentity);
    }
    void differentThemeMissingAssetAndOversizedConfigStayReadOnly() {
        QTemporaryDir dir; auto s = fixture(dir);
        QVERIFY(write(s.sddmConfig, "[Theme]\nCurrent=other-theme\n"));
        auto e = ScreenLockAppearance::inspect(s); QVERIFY(!e.backgroundAvailable); QVERIFY(!e.editable); QVERIFY(!e.issues.isEmpty());
        s = fixture(dir); QVERIFY(QFile::remove(s.dataRoots[0] + QStringLiteral("/sddm/themes/sddm-theme-mod/background")));
        e = ScreenLockAppearance::inspect(s); QVERIFY(!e.backgroundAvailable); QVERIFY(!e.editable);
        s = fixture(dir); QVERIFY(write(s.sddmConfig, QByteArray(2 * 1024 * 1024 + 1, 'x')));
        e = ScreenLockAppearance::inspect(s); QVERIFY(!e.backgroundAvailable); QVERIFY(!e.issues.isEmpty());
    }
};
QTEST_GUILESS_MAIN(ScreenLockAppearanceTest)
#include "screen_lock_appearance_test.moc"
