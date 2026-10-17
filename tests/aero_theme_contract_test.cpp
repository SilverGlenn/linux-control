#include "settings/AeroThemeContract.h"
#include <QTest>
#include <QTemporaryDir>
#include <QFile>
#include <QDir>
#include <filesystem>

class AeroThemeContractTest : public QObject {
    Q_OBJECT
private slots:
    void onlyCompleteApprovedAero()
    {
        QString reason;
        auto values = AeroThemeContract::defaults();
        QVERIFY(AeroThemeContract::validateRequested(values, reason));
        const auto identities = AeroThemeContract::identities();
        for (auto it = identities.cbegin(); it != identities.cend(); ++it) {
            auto changed = values; changed[it.key()] = QStringLiteral("Breeze");
            QVERIFY(!AeroThemeContract::validateRequested(changed, reason));
        }
        values.remove(QStringLiteral("kvantumTheme"));
        QVERIFY(!AeroThemeContract::validateRequested(values, reason));
        values = AeroThemeContract::defaults(); values[QStringLiteral("wallpaper")] = QStringLiteral("reset");
        QVERIFY(!AeroThemeContract::validateRequested(values, reason));
    }
    void digestPinsBytesTreeNamesAndLiteralSymlinks()
    {
        QTemporaryDir temporary; QVERIFY(temporary.isValid());
        QVERIFY(QDir().mkpath(temporary.filePath(QStringLiteral("tree/sub"))));
        const auto root = temporary.filePath(QStringLiteral("tree"));
        const auto file = temporary.filePath(QStringLiteral("tree/sub/a.txt"));
        { QFile output(file); QVERIFY(output.open(QIODevice::WriteOnly)); QCOMPARE(output.write("abc"), 3); }
        std::error_code error;
        std::filesystem::create_symlink("sub/a.txt", (root + QStringLiteral("/alias")).toStdString(), error);
        QVERIFY(!error);
        const auto original = QByteArrayLiteral("c7986b05cc386e6d1d06db4b125a915c4b6b3425e6ed98ce89d1104520c4fc53");
        QCOMPARE(AeroThemeContract::inspectAssetDigest(root), original);
        QCOMPARE(AeroThemeContract::inspectAssetDigest(file), QByteArrayLiteral("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
        { QFile output(file); QVERIFY(output.open(QIODevice::WriteOnly)); QCOMPARE(output.write("abd"), 3); }
        QVERIFY(AeroThemeContract::inspectAssetDigest(root) != original);
        { QFile output(file); QVERIFY(output.open(QIODevice::WriteOnly)); QCOMPARE(output.write("abc"), 3); }
        QCOMPARE(AeroThemeContract::inspectAssetDigest(root), original);
        QVERIFY(QFile::rename(file, temporary.filePath(QStringLiteral("tree/sub/b.txt"))));
        QVERIFY(AeroThemeContract::inspectAssetDigest(root) != original);
        QVERIFY(AeroThemeContract::inspectAssetDigest(QStringLiteral("relative/path")).isEmpty());
        QVERIFY(AeroThemeContract::inspectAssetDigest(temporary.filePath(QStringLiteral("missing"))).isEmpty());
    }
    void firstResolvedAssetAndEveryPluginAlternativeMustBeApproved()
    {
        QTemporaryDir temporary; QVERIFY(temporary.isValid());
        const auto user = temporary.filePath(QStringLiteral("user"));
        const auto system = temporary.filePath(QStringLiteral("system"));
        QVERIFY(QDir().mkpath(user + QStringLiteral("/styles")));
        QVERIFY(QDir().mkpath(system + QStringLiteral("/styles")));
        const auto approved = system + QStringLiteral("/styles/libkvantum.so");
        { QFile output(approved); QVERIFY(output.open(QIODevice::WriteOnly)); output.write("approved"); }
        QVERIFY(AeroThemeContract::isApprovedAssetResolution({user, system}, QStringLiteral("styles/libkvantum.so"), approved));
        { QFile output(user + QStringLiteral("/styles/libkvantum.so")); QVERIFY(output.open(QIODevice::WriteOnly)); output.write("different"); }
        QVERIFY(!AeroThemeContract::isApprovedAssetResolution({user, system}, QStringLiteral("styles/libkvantum.so"), approved));
        QVERIFY(AeroThemeContract::isApprovedAssetResolution({system, user}, QStringLiteral("styles/libkvantum.so"), approved));
        QVERIFY(!AeroThemeContract::isApprovedAssetResolution({system, user}, QStringLiteral("styles/libkvantum.so"), approved, true));
        QVERIFY(!AeroThemeContract::isApprovedAssetResolution({system}, QStringLiteral("../system/styles/libkvantum.so"), approved));
        QVERIFY(!AeroThemeContract::isApprovedAssetResolution({user}, QStringLiteral("missing"), approved));
    }
    void nativeParameterBoundsAndTypes()
    {
        QString reason;
        const auto baseline = AeroThemeContract::defaults();
        for (const auto &field : AeroThemeContract::fields()) {
            if (field.defaultValue.metaType().id() != QMetaType::Int) continue;
            auto values = baseline; values[field.key] = field.minimum - 1;
            QVERIFY(!AeroThemeContract::validateRequested(values, reason));
            values[field.key] = field.maximum + 1;
            QVERIFY(!AeroThemeContract::validateRequested(values, reason));
            values[field.key] = QString::number(field.defaultValue.toInt());
            QVERIFY(!AeroThemeContract::validateRequested(values, reason));
        }
        auto values = baseline; values[QStringLiteral("cursorSize")] = 40;
        QVERIFY(!AeroThemeContract::validateRequested(values, reason));
        values = baseline; values[QStringLiteral("enableTransparency")] = 1;
        QVERIFY(!AeroThemeContract::validateRequested(values, reason));
        values = baseline; values[QStringLiteral("customColor")] = QStringLiteral("#ff102030");
        QVERIFY(AeroThemeContract::validateRequested(values, reason));
        values[QStringLiteral("customColor")] = QStringLiteral("red");
        QVERIFY(!AeroThemeContract::validateRequested(values, reason));
    }
};
QTEST_GUILESS_MAIN(AeroThemeContractTest)
#include "aero_theme_contract_test.moc"
