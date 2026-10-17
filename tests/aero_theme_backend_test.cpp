#include "platform/kde/AeroThemeBackend.h"
#include "settings/AeroThemeContract.h"
#include <KConfig>
#include <KConfigGroup>
#include <QSignalSpy>
#include <QFile>
#include <functional>
#include <QTemporaryDir>
#include <QTest>

class FakeThemeAssets final : public AeroThemeAssetProbe {
public:
    bool supported = true;
    bool validate(QStringList &issues) const override
    { issues = supported ? QStringList() : QStringList{QStringLiteral("Asset changed")}; return supported; }
};
class FakeThemeRuntime final : public AeroThemeRuntime {
public:
    bool supported = true, reloadSuccess = true, replyToReload = true;
    int probes = 0, reloads = 0;
    std::function<void()> beforeProbeFinished;
    std::function<void()> beforeReloadFinished;
    void probe() override {
        ++probes;
        if (beforeProbeFinished) beforeProbeFinished();
        emit probeFinished(supported, supported ? QString() : QStringLiteral("Runtime incompatible"));
    }
    void reload(const QSet<QString> &) override {
        ++reloads;
        if (beforeReloadFinished) beforeReloadFinished();
        if (!replyToReload) return;
        emit reloadFinished(reloadSuccess, reloadSuccess ? QString() : QStringLiteral("Saved, but reload failed"));
    }
};
static void seed(const QString &directory, const QVariantMap &values)
{
    for (const auto &field : AeroThemeContract::fields()) {
        KConfig config(directory + QLatin1Char('/') + field.file, KConfig::SimpleConfig);
        if (field.key == QLatin1String("palette")) {
            const auto palette = values.value(field.key).toMap();
            for (auto group = palette.cbegin(); group != palette.cend(); ++group) {
                KConfigGroup target(&config, group.key()); const auto entries = group.value().toMap();
                for (auto entry = entries.cbegin(); entry != entries.cend(); ++entry) target.writeEntry(entry.key(), entry.value());
            }
        } else KConfigGroup(&config, field.group).writeEntry(field.entry, values.value(field.key));
        QVERIFY(config.sync());
    }
}
static AeroThemeState read(KdeAeroThemeBackend &backend)
{
    QSignalSpy spy(&backend, &AeroThemeBackend::readFinished); backend.readCurrent();
    if (spy.isEmpty() || !spy.first().at(0).toBool()) return {};
    return qvariant_cast<AeroThemeState>(spy.first().at(1));
}
class AeroThemeBackendTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { qRegisterMetaType<AeroThemeState>(); }
    void absentSmodResourceUsesItsRealAeroDefault()
    {
        QTemporaryDir temporary;
        const auto defaults = AeroThemeContract::defaults(); seed(temporary.path(), defaults);
        {
            KConfig config(temporary.filePath(QStringLiteral("smodrc")), KConfig::SimpleConfig);
            KConfigGroup(&config, "Windeco").deleteEntry("DecorationTheme");
            QVERIFY(config.sync());
        }
        auto *runtime = new FakeThemeRuntime;
        KdeAeroThemeBackend backend(temporary.path(), runtime, std::make_unique<FakeThemeAssets>());
        const auto state = read(backend);
        QCOMPARE(state.values, defaults);
        auto requested = defaults; requested["blurStrength"] = 6;
        QSignalSpy writes(&backend, &AeroThemeBackend::writeFinished);
        backend.writeSettings(state.values, requested);
        QVERIFY(writes.last().at(0).toBool());
        QCOMPARE(read(backend).values, requested);
    }
    void restoreAndTunePreserveUnrelatedSettings()
    {
        QTemporaryDir temporary; QVERIFY(temporary.isValid());
        auto defaults = AeroThemeContract::defaults(); seed(temporary.path(), defaults);
        {
            KConfig config(temporary.filePath(QStringLiteral("kdeglobals")), KConfig::SimpleConfig);
            KConfigGroup(&config, "General").writeEntry("font", "My font,11");
            KConfigGroup(&config, "WM").writeEntry("activeFont", "My title font,11");
            KConfigGroup(&config, "Colors:Header").writeEntry("BackgroundNormal", "1,2,3");
            QVERIFY(config.sync());
            KConfig shell(temporary.filePath(QStringLiteral("plasma-org.kde.plasma.desktop-appletsrc")), KConfig::SimpleConfig);
            KConfigGroup(&shell, "Containments").writeEntry("Wallpaper", "keep-me"); QVERIFY(shell.sync());
        }
        auto *runtime = new FakeThemeRuntime;
        KdeAeroThemeBackend backend(temporary.path(), runtime, std::make_unique<FakeThemeAssets>());
        const auto baseline = read(backend); QVERIFY(baseline.canApply());
        QVERIFY(baseline.values.value(QStringLiteral("palette")).toMap().contains(QStringLiteral("Colors:Header")));
        QSignalSpy writes(&backend, &AeroThemeBackend::writeFinished);
        backend.writeSettings(baseline.values, defaults); QCOMPARE(writes.count(), 1); QVERIFY(writes.first().at(0).toBool());
        QCOMPARE(read(backend).values, defaults);
        KConfig config(temporary.filePath(QStringLiteral("kdeglobals")), KConfig::SimpleConfig);
        QCOMPARE(KConfigGroup(&config, "General").readEntry("font", QString()), QStringLiteral("My font,11"));
        QCOMPARE(KConfigGroup(&config, "WM").readEntry("activeFont", QString()), QStringLiteral("My title font,11"));
        QVERIFY(!KConfigGroup(&config, "Colors:Header").hasKey("BackgroundNormal"));
        auto tuned = defaults; tuned[QStringLiteral("blurStrength")] = 8; tuned[QStringLiteral("smodEnableShadow")] = false;
        backend.writeSettings(defaults, tuned); QVERIFY(writes.last().at(0).toBool()); QCOMPARE(read(backend).values, tuned);
        KConfig shell(temporary.filePath(QStringLiteral("plasma-org.kde.plasma.desktop-appletsrc")), KConfig::SimpleConfig);
        QCOMPARE(KConfigGroup(&shell, "Containments").readEntry("Wallpaper", QString()), QStringLiteral("keep-me"));
        QCOMPARE(runtime->reloads, 2);
    }
    void rejectUnapprovedIncompleteStaleAndLockedBeforeAnyWrite()
    {
        QTemporaryDir temporary; auto defaults = AeroThemeContract::defaults(); seed(temporary.path(), defaults);
        auto *runtime = new FakeThemeRuntime;
        KdeAeroThemeBackend backend(temporary.path(), runtime, std::make_unique<FakeThemeAssets>());
        QSignalSpy writes(&backend, &AeroThemeBackend::writeFinished);
        auto requested = defaults; requested[QStringLiteral("iconTheme")] = QStringLiteral("breeze");
        backend.writeSettings(defaults, requested); QVERIFY(!writes.last().at(0).toBool()); QCOMPARE(runtime->probes, 0);
        requested = defaults; requested.remove(QStringLiteral("palette"));
        backend.writeSettings(defaults, requested); QVERIFY(!writes.last().at(0).toBool()); QCOMPARE(runtime->probes, 0);
        requested = defaults; requested[QStringLiteral("blurStrength")] = 9;
        { KConfig config(temporary.filePath(QStringLiteral("kwinrc")), KConfig::SimpleConfig);
          KConfigGroup(&config, "Effect-aeroglassblur").writeEntry("AeroHue", 7); QVERIFY(config.sync()); }
        backend.writeSettings(defaults, requested); QVERIFY(!writes.last().at(0).toBool()); QCOMPARE(runtime->reloads, 0);
        QCOMPARE(read(backend).values.value(QStringLiteral("blurStrength")).toInt(), 3);
        seed(temporary.path(), defaults);
        QFile locked(temporary.filePath(QStringLiteral("smodrc"))); QVERIFY(locked.open(QIODevice::WriteOnly));
        locked.write("[Common][$i]\nEnableShadow=true\nOutlineCloseButton=false\n[Windeco]\nDecorationTheme=Aero\n"); locked.close();
        backend.writeSettings(defaults, requested); QVERIFY(!writes.last().at(0).toBool()); QCOMPARE(runtime->reloads, 0);
        QCOMPARE(read(backend).values.value(QStringLiteral("blurStrength")).toInt(), 3);
    }
    void revalidateAssetsAndRuntimeAndReportSavedReloadFailure()
    {
        QTemporaryDir temporary; const auto defaults = AeroThemeContract::defaults(); seed(temporary.path(), defaults);
        auto assets = std::make_unique<FakeThemeAssets>(); auto *probe = assets.get(); auto *runtime = new FakeThemeRuntime;
        KdeAeroThemeBackend backend(temporary.path(), runtime, std::move(assets)); QVERIFY(read(backend).canApply());
        auto requested = defaults; requested[QStringLiteral("blurStrength")] = 6;
        QSignalSpy writes(&backend, &AeroThemeBackend::writeFinished);
        probe->supported = false; backend.writeSettings(defaults, requested); QVERIFY(!writes.last().at(0).toBool());
        QCOMPARE(read(backend).values, defaults); QCOMPARE(runtime->reloads, 0);
        probe->supported = true; runtime->supported = false; backend.writeSettings(defaults, requested); QVERIFY(!writes.last().at(0).toBool());
        QCOMPARE(read(backend).values, defaults);
        runtime->supported = true; runtime->reloadSuccess = false; backend.writeSettings(defaults, requested);
        QVERIFY(!writes.last().at(0).toBool()); QCOMPARE(read(backend).values, requested);
        QVERIFY(writes.last().at(1).toString().contains(QStringLiteral("Saved")));
    }
    void reloadTimeoutKeepsSavedValuesAndPoisonsLateCallbacks()
    {
        QTemporaryDir temporary; const auto defaults = AeroThemeContract::defaults(); seed(temporary.path(), defaults);
        auto *runtime = new FakeThemeRuntime; runtime->replyToReload = false;
        KdeAeroThemeBackend backend(temporary.path(), runtime, std::make_unique<FakeThemeAssets>());
        auto requested = defaults; requested[QStringLiteral("blurStrength")] = 7;
        QSignalSpy writes(&backend, &AeroThemeBackend::writeFinished);
        backend.writeSettings(defaults, requested);
        QVERIFY(writes.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(writes.count(), 1, 14000);
        QVERIFY(!writes.last().at(0).toBool());
        QVERIFY(writes.last().at(1).toString().contains(QStringLiteral("timed out")));
        emit runtime->reloadFinished(true, QString());
        emit runtime->probeFinished(true, QString());
        QCOMPARE(writes.count(), 1);
        const auto saved = read(backend);
        QCOMPARE(saved.values, requested);
        QVERIFY(!saved.runtimeSupported); QVERIFY(!saved.canApply());
        QVERIFY(saved.issues.join(QLatin1Char('\n')).contains(QStringLiteral("restart")));
        const int probes = runtime->probes;
        backend.writeSettings(requested, defaults);
        QCOMPARE(writes.count(), 2); QVERIFY(!writes.last().at(0).toBool());
        QCOMPARE(runtime->probes, probes); QCOMPARE(runtime->reloads, 1);
        QCOMPARE(read(backend).values, requested);
    }
    void savedReadbackDetectsConcurrentChangesDuringReload()
    {
        QTemporaryDir temporary; const auto defaults = AeroThemeContract::defaults(); seed(temporary.path(), defaults);
        auto *runtime = new FakeThemeRuntime;
        KdeAeroThemeBackend backend(temporary.path(), runtime, std::make_unique<FakeThemeAssets>());
        runtime->beforeReloadFinished = [&] {
            KConfig config(temporary.filePath(QStringLiteral("kcminputrc")), KConfig::SimpleConfig);
            KConfigGroup(&config, "Mouse").writeEntry("cursorSize", 48); QVERIFY(config.sync());
        };
        auto requested = defaults; requested[QStringLiteral("blurStrength")] = 5;
        QSignalSpy writes(&backend, &AeroThemeBackend::writeFinished); backend.writeSettings(defaults, requested);
        QVERIFY(!writes.last().at(0).toBool());
        QCOMPARE(read(backend).values.value(QStringLiteral("blurStrength")).toInt(), 5);
        QCOMPARE(read(backend).values.value(QStringLiteral("cursorSize")).toInt(), 48);
    }
};
QTEST_GUILESS_MAIN(AeroThemeBackendTest)
#include "aero_theme_backend_test.moc"
