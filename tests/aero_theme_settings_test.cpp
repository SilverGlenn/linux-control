#include <QtTest>
#include <QSignalSpy>
#include <QColor>
#include "settings/AeroThemeSettings.h"
#include "settings/AeroThemeContract.h"

class SessionAeroBackend final : public AeroThemeBackend {
public:
    int reads = 0, writes = 0;
    QVariantMap expected, requested;
    void readCurrent() override { ++reads; }
    void writeSettings(const QVariantMap &old, const QVariantMap &next) override { ++writes; expected = old; requested = next; }
    static AeroThemeState available(int intensity = 60)
    {
        AeroThemeState state; state.values = AeroThemeContract::defaults();
        state.values["aeroIntensity"] = intensity;
        for (auto it = state.values.cbegin(); it != state.values.cend(); ++it) state.writableKeys.insert(it.key());
        state.assetsSupported = state.runtimeSupported = true; return state;
    }
    void report(const AeroThemeState &state = available()) { emit readFinished(true, state, {}); }
    void failRead() { emit readFinished(false, {}, "Read failed"); }
    void written(bool success = true) { emit writeFinished(success, success ? QString() : QStringLiteral("Some files saved; reload failed")); }
    void changed() { emit externalChange(); }
};
class AeroThemeSettingsTest final : public QObject {
    Q_OBJECT
private slots:
    void init() { backend = new SessionAeroBackend; session = new AeroThemeSettings(backend); QTRY_COMPARE(backend->reads, 1); backend->report(); }
    void cleanup() { delete session; }
    void defaultsAreExplicitStagingAndRequireApplyReadback()
    {
        QSignalSpy finished(session, &SettingsSession::applyFinished);
        session->stageRestoreDefaults(); QVERIFY(session->isDirty()); QCOMPARE(backend->writes, 0);
        QCOMPARE(session->stagedValues(), AeroThemeContract::defaults());
        session->apply(); QVERIFY(session->isBusy()); QCOMPARE(backend->writes, 0);
        backend->report(); QCOMPARE(backend->writes, 1); QCOMPARE(backend->expected, SessionAeroBackend::available().values);
        QCOMPARE(backend->requested, AeroThemeContract::defaults());
        session->selectValue("aeroIntensity", 80); session->revert(); QCOMPARE(session->stagedValue("aeroIntensity").toInt(), 50);
        backend->written(); QVERIFY(finished.isEmpty()); backend->report(SessionAeroBackend::available(50));
        QCOMPARE(finished.size(), 1); QVERIFY(finished[0][0].toBool()); QVERIFY(!session->isDirty());
        QVERIFY(session->resultText().contains("readback")); QVERIFY(session->resultText().contains("not verified"));
    }
    void identitiesAndUnsupportedParameterTypesCannotBeStaged()
    {
        session->selectValue("iconTheme", "breeze"); session->selectValue("arbitraryKey", "other");
        session->selectValue("aeroIntensity", "80"); session->selectValue("aeroIntensity", 218);
        session->selectValue("cursorSize", 36); session->selectValue("customColor", "red");
        QVERIFY(!session->isDirty()); QCOMPARE(backend->writes, 0);
        session->selectValue("aeroIntensity", 80); QVERIFY(session->isDirty());
        QCOMPARE(session->stagedValue("aeroIntensity").toInt(), 80);
    }
    void colorEditsKeepNativeMetadataAndShaderInAgreement()
    {
        session->selectValue("customColor", QStringLiteral("#ff54a9df"));
        QCOMPARE(session->stagedValue("accentColorGroup").toInt(), 0);
        QCOMPARE(session->stagedValue("accentColorName").toInt(), 0);
        const QColor selected(session->stagedValue("customColor").toString());
        QCOMPARE(selected.alpha(), 60);
        QCOMPARE(session->stagedValue("aeroHue").toInt(), QColor("#54a9df").hsvHue());
        session->selectValue("aeroIntensity", 80);
        QCOMPARE(QColor(session->stagedValue("customColor").toString()).alpha(), 80);
        session->selectValue("aeroHue", 120);
        QVERIFY(qAbs(QColor(session->stagedValue("customColor").toString()).hsvHue() - 120) <= 1);
        QCOMPARE(backend->writes, 0);
    }
    void customizationStagesRepairOfDetectedIdentityDrift()
    {
        auto drift = SessionAeroBackend::available(); drift.values["iconTheme"] = "breeze";
        session->refresh(); backend->report(drift); QVERIFY(!session->isDirty());
        QCOMPARE(session->appliedValue("iconTheme").toString(), "breeze");
        session->selectValue("aeroIntensity", 80);
        QCOMPARE(session->stagedValue("iconTheme").toString(), "Windows 7 Aero");
        QCOMPARE(session->appliedValue("iconTheme").toString(), "breeze"); QCOMPARE(backend->writes, 0);
        QVERIFY(session->changeSummary().join('\n').contains("Aero icons"));
    }
    void wholeBaselinePreflightDetectsUnstagedSiblingConflict()
    {
        session->selectValue("aeroIntensity", 80); session->apply();
        auto state = SessionAeroBackend::available(); state.values["cursorSize"] = 48; backend->report(state);
        QCOMPARE(backend->writes, 0); QVERIFY(session->hasConflict()); QCOMPARE(session->stagedValue("aeroIntensity").toInt(), 80);
        session->apply(); QCOMPARE(backend->writes, 0);
        session->revert(); backend->report(state); QVERIFY(!session->isDirty()); QVERIFY(!session->hasConflict());
    }
    void capabilityLossBeforeWriteIsBlocked()
    {
        session->selectValue("aeroIntensity", 80); session->apply();
        auto state = SessionAeroBackend::available(); state.writableKeys.remove("cursorSize"); state.issues << "Pointer policy lock";
        backend->report(state); QCOMPARE(backend->writes, 0); QVERIFY(session->hasConflict()); QVERIFY(session->errorText().contains("policy"));
        session->revert(); state = SessionAeroBackend::available(); state.assetsSupported = false; backend->report(state);
        session->selectValue("aeroIntensity", 80); QVERIFY(!session->isDirty()); QVERIFY(!session->canApply());
        session->stageRestoreDefaults(); QVERIFY(session->isDirty()); session->apply(); QCOMPARE(backend->writes, 0);
    }
    void failedWritePartialReadbackPreservesSelectionAndErrorUntilRevert()
    {
        QSignalSpy finished(session, &SettingsSession::applyFinished);
        session->selectValue("aeroIntensity", 80); session->selectValue("cursorSize", 48); session->apply(); backend->report(); backend->written(false);
        backend->report(SessionAeroBackend::available(80));
        QVERIFY(!finished[0][0].toBool()); QVERIFY(session->hasPartialFailure()); QVERIFY(session->hasConflict()); QVERIFY(session->isDirty());
        QCOMPARE(session->appliedValue("aeroIntensity").toInt(), 80); QCOMPARE(session->stagedValue("cursorSize").toInt(), 48);
        backend->changed(); backend->report(SessionAeroBackend::available(80));
        QVERIFY(session->errorText().contains("reload failed")); QVERIFY(session->hasPartialFailure());
        session->revert(); backend->report(SessionAeroBackend::available(80));
        QVERIFY(!session->hasConflict()); QVERIFY(!session->hasPartialFailure()); QVERIFY(!session->isDirty());
    }
    void savedReadbackWithoutRuntimeAcknowledgementStillFails()
    {
        QSignalSpy finished(session, &SettingsSession::applyFinished);
        session->selectValue("aeroIntensity", 80); session->apply(); backend->report(); backend->written(false);
        auto saved = SessionAeroBackend::available(80); saved.values = backend->requested; backend->report(saved);
        QVERIFY(!finished[0][0].toBool()); QVERIFY(!session->isDirty()); QVERIFY(session->hasConflict()); QVERIFY(session->hasPartialFailure());
    }
    void failedReadbackAndDeferredRefreshKeepUncertainty()
    {
        session->selectValue("aeroIntensity", 80); session->apply(); backend->report(); backend->changed(); backend->written(); backend->failRead();
        QVERIFY(!session->isReady()); QVERIFY(session->hasConflict()); QVERIFY(session->hasPartialFailure());
        const int count = backend->reads; QTRY_COMPARE(backend->reads, count + 1); backend->report(SessionAeroBackend::available(80));
        QVERIFY(session->isReady()); QVERIFY(session->hasConflict()); QVERIFY(!session->errorText().isEmpty());
    }
    void externalChangeWhileDirtyNeverOverwritesStaging()
    {
        session->selectValue("aeroIntensity", 80); backend->changed(); backend->report(SessionAeroBackend::available(70));
        QVERIFY(session->hasConflict()); QCOMPARE(session->stagedValue("aeroIntensity").toInt(), 80); QCOMPARE(session->appliedValue("aeroIntensity").toInt(), 70);
        session->selectValue("cursorSize", 48); QVERIFY(session->hasConflict()); session->apply(); QCOMPARE(backend->writes, 0);
    }
private:
    SessionAeroBackend *backend = nullptr;
    AeroThemeSettings *session = nullptr;
};
QTEST_MAIN(AeroThemeSettingsTest)
#include "aero_theme_settings_test.moc"
