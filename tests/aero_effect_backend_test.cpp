#include <QtTest>
#include <QDBusConnection>
#include <QFile>
#include <QSaveFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <KConfig>
#include <KConfigGroup>
#include "platform/kde/AeroEffectBackend.h"

class FakeAeroRuntime final : public AeroEffectRuntime {
public:
    bool compatible = true, loaded = true, reloadSuccess = true, holdProbe = false;
    int probes = 0, reloads = 0;
    void probe() override { ++probes; if (!holdProbe) report(); }
    void report() { emit probeFinished(compatible, loaded, compatible && loaded ? QString() : QStringLiteral("Unsupported or unloaded Aero effect")); }
    void reconfigure() override { ++reloads; emit reconfigureFinished(reloadSuccess, reloadSuccess ? QString() : QStringLiteral("Saved but reload failed")); }
};

class AeroEffectBackendTest final : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { qRegisterMetaType<AeroEffectState>(); }
    void init()
    {
        QVERIFY(temp.isValid()); file = temp.filePath("kwinrc"); QFile::remove(file);
        runtime = new FakeAeroRuntime;
        backend = new KdeAeroEffectBackend(file, runtime, supportedIdentity());
    }
    void cleanup() { delete backend; }
    void productionLegacyWriterIsRetired()
    {
        KdeAeroEffectBackend production;
        QSignalSpy finished(&production, &AeroEffectBackend::writeFinished);
        production.writeStrength(3, 8);
        QCOMPARE(finished.size(), 1);
        QVERIFY(!finished[0][0].toBool());
        QVERIFY(finished[0][1].toString().contains("Personalization"));
    }
    void nativeSchemaDefaultAndRangeArePinned()
    {
        // Installed blur.kcfg: Effect-aeroglassblur/BlurStrength Int default3.
        // Installed blur_config.ui: minimum2 maximum15. No generic blur keys.
        const auto state = read(); QCOMPARE(state.blurStrength, 3); QVERIFY(state.canApply());
        QVERIFY(!QFile::exists(file)); QCOMPARE(runtime->reloads, 0);
        QVERIFY(AeroEffectSettings::supportedStrength(2)); QVERIFY(AeroEffectSettings::supportedStrength(15));
        QVERIFY(!AeroEffectSettings::supportedStrength(1)); QVERIFY(!AeroEffectSettings::supportedStrength(16));
    }
    void onlyNestedStrengthIsWrittenAndUnrelatedEffectPreferencesSurvive()
    {
        put("[Effect-aeroglassblur]\nBlurStrength=3\nAeroIntensity=127\nEnableTransparency=false\n[Plugins]\naeroglassblurEnabled=true\nblurEnabled=false\n[Unrelated]\nValue=keep\n");
        QSignalSpy finished(backend, &AeroEffectBackend::writeFinished);
        backend->writeStrength(3, 8);
        QCOMPARE(finished.size(), 1); QVERIFY(finished[0][0].toBool()); QCOMPARE(runtime->reloads, 1);
        QCOMPARE(read().blurStrength, 8);
        KConfig config(file, KConfig::SimpleConfig);
        const KConfigGroup effect(&config, "Effect-aeroglassblur");
        QCOMPARE(effect.readEntry("AeroIntensity", 0), 127); QVERIFY(!effect.readEntry("EnableTransparency", true));
        const KConfigGroup plugins(&config, "Plugins");
        QVERIFY(plugins.readEntry("aeroglassblurEnabled", false)); QVERIFY(!plugins.readEntry("blurEnabled", true));
        QCOMPARE(KConfigGroup(&config, "Unrelated").readEntry("Value"), "keep");
    }
    void immutableKeyOrGroupRejectsWithoutReload()
    {
        put("[Effect-aeroglassblur]\nBlurStrength[$i]=3\n");
        QVERIFY(!read().writable);
        QSignalSpy finished(backend, &AeroEffectBackend::writeFinished); backend->writeStrength(3, 8);
        QVERIFY(!finished[0][0].toBool()); QCOMPARE(runtime->reloads, 0); QVERIFY(contents().contains("BlurStrength[$i]=3"));
        put("[Effect-aeroglassblur][$i]\nBlurStrength=3\n");
        backend->writeStrength(3, 8); QVERIFY(!finished[1][0].toBool()); QCOMPARE(runtime->reloads, 0);
    }
    void invalidRequestAndMalformedSavedStrengthNeverWriteOrReload()
    {
        backend->writeStrength(3, 0); backend->writeStrength(3, 16); QVERIFY(!QFile::exists(file));
        put("[Effect-aeroglassblur]\nBlurStrength=not-an-integer\n");
        QSignalSpy reads(backend, &AeroEffectBackend::readFinished); backend->readCurrent();
        QVERIFY(!reads[0][0].toBool());
        backend->writeStrength(3, 8); QCOMPARE(runtime->reloads, 0); QVERIFY(contents().contains("not-an-integer"));
    }
    void unsupportedOrUnloadedRuntimeNeverPersists()
    {
        runtime->compatible = false; QVERIFY(!read().runtimeSupported);
        backend->writeStrength(3, 8); QVERIFY(!QFile::exists(file));
        runtime->compatible = true; runtime->loaded = false;
        const auto state = read(); QVERIFY(state.runtimeSupported); QVERIFY(!state.effectLoaded); QVERIFY(!state.canApply());
        backend->writeStrength(3, 8); QVERIFY(!QFile::exists(file)); QCOMPARE(runtime->reloads, 0);
    }
    void delayedRuntimeProbeStillRechecksLatestFileBeforeWrite()
    {
        runtime->holdProbe = true; backend->writeStrength(3, 8);
        put("[Effect-aeroglassblur]\nBlurStrength=5\n");
        QSignalSpy finished(backend, &AeroEffectBackend::writeFinished); runtime->report();
        QVERIFY(!finished[0][0].toBool()); QCOMPARE(runtime->reloads, 0); QVERIFY(contents().contains("BlurStrength=5"));
    }
    void reloadFailureExposesSavedPreferenceForAuthoritativeReadback()
    {
        runtime->reloadSuccess = false;
        QSignalSpy finished(backend, &AeroEffectBackend::writeFinished); backend->writeStrength(3, 8);
        QVERIFY(!finished[0][0].toBool()); QVERIFY(finished[0][1].toString().contains("Saved"));
        QCOMPARE(read().blurStrength, 8); QCOMPARE(runtime->reloads, 1);
    }
    void pluginBuildAndMetadataMustMatchVerifiedSchema()
    {
        QString reason; auto identity = supportedIdentity(); QVERIFY(validateAeroEffectPlugin(identity, reason));
        QCOMPARE(identity.sha256, QByteArray("4ed00683b05ee02593c1b99da4f1aa864676ece792e7f56ab92cf83f1fd34d4e"));
        identity.sha256 = "different-build"; QVERIFY(!validateAeroEffectPlugin(identity, reason)); QVERIFY(!reason.isEmpty());
        identity = supportedIdentity(); identity.metadata["IID"] = "org.kde.kwin.EffectPluginFactory6.7.0";
        QVERIFY(!validateAeroEffectPlugin(identity, reason));
        identity = supportedIdentity(); identity.metadata["MetaData"] = QJsonObject{{"KPlugin", QJsonObject{{"Name", "Original Blur"}}}};
        QVERIFY(!validateAeroEffectPlugin(identity, reason));
        delete backend; runtime = new FakeAeroRuntime; backend = new KdeAeroEffectBackend(file, runtime, identity);
        const auto state = read(); QVERIFY(!state.pluginSupported); QVERIFY(!state.canApply());
        backend->writeStrength(3, 8); QVERIFY(!QFile::exists(file)); QCOMPARE(runtime->reloads, 0);
    }
    void missingPluginInspectionIsReadOnlyAndUnsupported()
    {
        QString reason;
        const QString missing = temp.filePath("missing-plugin.so");
        const auto identity = inspectAeroEffectPlugin(missing);
        QVERIFY(!validateAeroEffectPlugin(identity, reason)); QVERIFY(!reason.isEmpty()); QVERIFY(!QFile::exists(missing));
    }
    void privateKWinVersionAndWaylandConstraintAreStrict()
    {
        const QString report = "Version\n=======\nKWin version: 6.6.6\n\nOperation Mode: Wayland\n";
        QVERIFY(isSupportedAeroKWinReport(report));
        QVERIFY(!isSupportedAeroKWinReport(QString(report).replace("6.6.6", "6.7.0")));
        QVERIFY(!isSupportedAeroKWinReport(QString(report).replace("Wayland", "X11")));
        QVERIFY(!isSupportedAeroKWinReport(report + "KWin version: 6.6.6\n"));
        QVERIFY(!isSupportedAeroKWinReport(QString(65537, 'x')));
    }
    void disconnectedReadOnlyProbeCompletesExactlyOnce()
    {
        const QDBusConnection disconnected(QStringLiteral("aero-effect-disconnected-test"));
        QVERIFY(!disconnected.isConnected());
        auto *service = createAeroEffectRuntime(disconnected);
        KdeAeroEffectBackend isolated(file, service, supportedIdentity());
        QSignalSpy result(&isolated, &AeroEffectBackend::readFinished); isolated.readCurrent();
        QTRY_COMPARE(result.size(), 1); QVERIFY(result[0][0].toBool());
        const auto state = qvariant_cast<AeroEffectState>(result[0][1]);
        QVERIFY(!state.runtimeSupported); QVERIFY(!state.effectLoaded); QVERIFY(!state.canApply());
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); QCoreApplication::processEvents();
        QCOMPARE(result.size(), 1); QVERIFY(!QFile::exists(file));
    }
    void configAtomicReplacementRemainsObserved()
    {
        QSignalSpy changed(backend, &AeroEffectBackend::externalChange);
        atomicPut("[Effect-aeroglassblur]\nBlurStrength=5\n"); QTRY_VERIFY(!changed.isEmpty()); changed.clear();
        atomicPut("[Effect-aeroglassblur]\nBlurStrength=8\n"); QTRY_VERIFY(!changed.isEmpty());
        QCOMPARE(read().blurStrength, 8); QCOMPARE(runtime->reloads, 0);
    }
private:
    static AeroEffectPluginIdentity supportedIdentity()
    {
        AeroEffectPluginIdentity identity; identity.sha256 = supportedAeroEffectSha256();
        identity.metadata = QJsonObject{{"IID", "org.kde.kwin.EffectPluginFactory6.6.6"},
            {"MetaData", QJsonObject{{"KPlugin", QJsonObject{{"Name", "Aero Glass Blur"}}}, {"X-KDE-ConfigModule", "kwin_aeroglassblur_config"}}}};
        return identity;
    }
    AeroEffectState read()
    {
        QSignalSpy result(backend, &AeroEffectBackend::readFinished); backend->readCurrent();
        if (result.size() != 1 || !result[0][0].toBool()) { QTest::qFail("Temporary Aero config could not be read", __FILE__, __LINE__); return {}; }
        return qvariant_cast<AeroEffectState>(result[0][1]);
    }
    void put(const QByteArray &text)
    {
        QFile output(file); QVERIFY(output.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(output.write(text), qint64(text.size())); output.close();
    }
    void atomicPut(const QByteArray &text)
    {
        QSaveFile output(file); QVERIFY(output.open(QIODevice::WriteOnly));
        QCOMPARE(output.write(text), qint64(text.size())); QVERIFY(output.commit());
    }
    QByteArray contents() { QFile input(file); if (!input.open(QIODevice::ReadOnly)) return {}; return input.readAll(); }
    QTemporaryDir temp;
    QString file;
    FakeAeroRuntime *runtime = nullptr;
    KdeAeroEffectBackend *backend = nullptr;
};
QTEST_GUILESS_MAIN(AeroEffectBackendTest)
#include "aero_effect_backend_test.moc"
