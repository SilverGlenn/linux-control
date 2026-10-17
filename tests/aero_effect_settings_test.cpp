#include <QtTest>
#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QSignalSpy>
#include "settings/AeroEffectSettings.h"
#include "ui/pages/AeroEffectsPage.h"

class FakeAeroBackend final : public AeroEffectBackend {
public:
    int reads = 0, writes = 0, expected = -1, requested = -1;
    void readCurrent() override { ++reads; }
    void writeStrength(int baseline, int strength) override { ++writes; expected = baseline; requested = strength; }
    static AeroEffectState available(int value = 3)
    {
        AeroEffectState state; state.blurStrength = value;
        state.writable = state.pluginSupported = state.runtimeSupported = state.effectLoaded = true;
        return state;
    }
    void report(const AeroEffectState &state = available()) { emit readFinished(true, state, {}); }
    void failedRead() { emit readFinished(false, {}, "Read failed"); }
    void written(bool success = true) { emit writeFinished(success, success ? QString() : QStringLiteral("Saved, but reload failed")); }
    void changed() { emit externalChange(); }
};

class AeroEffectSettingsTest final : public QObject {
    Q_OBJECT
private slots:
    void init()
    {
        backend = new FakeAeroBackend; session = new AeroEffectSettings(backend);
        QTRY_COMPARE(backend->reads, 1); backend->report();
    }
    void cleanup() { delete session; }
    void nativePageStagesWithoutWritesAndSurvivesRecreation()
    {
        auto *page = new AeroEffectsPage(nullptr, session, nullptr);
        auto *combo = page->findChild<QComboBox *>("aeroBlurStrength");
        QVERIFY(combo); QVERIFY(!combo->accessibleName().isEmpty());
        QCOMPARE(combo->count(), 14); QCOMPARE(combo->itemData(0).toInt(), 2); QCOMPARE(combo->itemData(13).toInt(), 15);
        combo->setCurrentIndex(combo->findData(8));
        QCOMPARE(session->stagedStrength(), 8); QVERIFY(session->isDirty()); QCOMPARE(backend->writes, 0);
        delete page;
        AeroEffectsPage replacement(nullptr, session, nullptr);
        QCOMPARE(replacement.settingsSession(), static_cast<SettingsSession *>(session));
        QCOMPARE(replacement.findChild<QComboBox *>("aeroBlurStrength")->currentData().toInt(), 8);
        QVERIFY(replacement.findChild<QPushButton *>("applyAeroEffect")->isEnabled());
        QVERIFY(replacement.findChild<QLabel *>("aeroEffectStatus")->text().contains("loaded"));
    }
    void applyRequiresPreflightReloadReplyAndSavedReadback()
    {
        QSignalSpy finished(session, &SettingsSession::applyFinished);
        session->selectStrength(8); session->apply();
        QVERIFY(session->isBusy()); QCOMPARE(backend->writes, 0);
        backend->report(); QCOMPARE(backend->writes, 1); QCOMPARE(backend->expected, 3); QCOMPARE(backend->requested, 8);
        session->selectStrength(10); session->revert(); QCOMPARE(session->stagedStrength(), 8);
        backend->written(); QVERIFY(finished.isEmpty());
        backend->report(FakeAeroBackend::available(8));
        QCOMPARE(finished.size(), 1); QVERIFY(finished[0][0].toBool());
        QVERIFY(!session->isDirty()); QVERIFY(!session->hasConflict());
        QVERIFY(session->resultText().contains("Saved")); QVERIFY(session->resultText().contains("acknowledged"));
        QVERIFY(session->resultText().contains("does not expose"));
    }
    void preflightConflictPreservesSelectionAndBlocksWrite()
    {
        session->selectStrength(8); session->apply(); backend->report(FakeAeroBackend::available(5));
        QCOMPARE(backend->writes, 0); QVERIFY(session->hasConflict());
        QCOMPARE(session->appliedStrength(), 5); QCOMPARE(session->stagedStrength(), 8);
        session->apply(); QCOMPARE(backend->writes, 0);
        session->revert(); backend->report(FakeAeroBackend::available(5));
        QVERIFY(!session->hasConflict()); QVERIFY(!session->isDirty()); QCOMPARE(session->stagedStrength(), 5);
    }
    void unsupportedOrUnloadedEffectCannotBeEditedOrApplied()
    {
        auto state = FakeAeroBackend::available(); state.pluginSupported = false;
        state.unavailableReason = "Unsupported Aero plugin build";
        session->refresh(); backend->report(state);
        AeroEffectsPage page(nullptr, session, nullptr);
        QVERIFY(!page.findChild<QComboBox *>("aeroBlurStrength")->isEnabled());
        QVERIFY(page.findChild<QLabel *>("aeroEffectMessage")->text().contains("Unsupported"));
        session->selectStrength(8); QVERIFY(!session->isDirty());
        session->apply(); QCOMPARE(backend->writes, 0);
        state = FakeAeroBackend::available(); state.effectLoaded = false;
        state.unavailableReason = "Aero Glass Blur is not loaded";
        session->revert(); backend->report(state);
        QVERIFY(!session->canApply()); QVERIFY(!page.findChild<QComboBox *>("aeroBlurStrength")->isEnabled());
    }
    void policyOrRuntimeLossDuringPreflightAborts()
    {
        session->selectStrength(8); session->apply();
        auto state = FakeAeroBackend::available(); state.writable = false; state.unavailableReason = "Policy lock";
        backend->report(state);
        QCOMPARE(backend->writes, 0); QVERIFY(session->hasConflict()); QVERIFY(session->errorText().contains("Policy"));
        session->revert(); backend->report(); session->selectStrength(8); session->apply();
        state = FakeAeroBackend::available(); state.runtimeSupported = false; state.unavailableReason = "KWin version changed";
        backend->report(state); QCOMPARE(backend->writes, 0); QVERIFY(session->hasConflict());
    }
    void rangeValidationAndUnknownSavedValueNeverCoerce()
    {
        session->selectStrength(1); session->selectStrength(16); QVERIFY(!session->isDirty());
        session->refresh(); backend->report(FakeAeroBackend::available(18));
        AeroEffectsPage page(nullptr, session, nullptr);
        QCOMPARE(page.findChild<QComboBox *>("aeroBlurStrength")->currentData().toInt(), 18);
        QCOMPARE(session->stagedStrength(), 18); QVERIFY(!session->isDirty()); QCOMPARE(backend->writes, 0);
        session->selectStrength(3); QVERIFY(session->isDirty()); QCOMPARE(backend->writes, 0);
    }
    void failedReloadWithSavedReadbackRemainsExplicitUntilRevert()
    {
        QSignalSpy finished(session, &SettingsSession::applyFinished);
        session->selectStrength(8); session->apply(); backend->report(); backend->written(false);
        backend->report(FakeAeroBackend::available(8));
        QVERIFY(!finished[0][0].toBool()); QVERIFY(!session->isDirty()); QVERIFY(session->hasConflict());
        QVERIFY(session->errorText().contains("reload failed"));
        backend->changed(); backend->report(FakeAeroBackend::available(8));
        QVERIFY(session->hasConflict()); QVERIFY(!session->errorText().isEmpty());
        session->revert(); backend->report(FakeAeroBackend::available(8));
        QVERIFY(!session->hasConflict()); QVERIFY(session->errorText().isEmpty());
    }
    void changedSavedValueOrLostLoadedEffectCannotPassReadback()
    {
        QSignalSpy finished(session, &SettingsSession::applyFinished);
        session->selectStrength(8); session->apply(); backend->report(); backend->written(); backend->report();
        QVERIFY(!finished[0][0].toBool()); QVERIFY(session->isDirty()); QVERIFY(session->hasConflict());
        session->revert(); backend->report(); session->selectStrength(8); session->apply(); backend->report(); backend->written();
        auto state = FakeAeroBackend::available(8); state.effectLoaded = false; backend->report(state);
        QVERIFY(!finished[1][0].toBool()); QVERIFY(session->hasConflict()); QVERIFY(!session->isDirty());
    }
    void failedReadbackAndDeferredRefreshPreserveError()
    {
        session->selectStrength(8); session->apply(); backend->report(); backend->changed(); backend->written(); backend->failedRead();
        QVERIFY(!session->isReady()); QVERIFY(session->hasConflict());
        const int reads = backend->reads; QTRY_COMPARE(backend->reads, reads + 1);
        backend->report(FakeAeroBackend::available(8));
        QVERIFY(session->isReady()); QVERIFY(session->hasConflict()); QVERIFY(!session->errorText().isEmpty());
    }
private:
    FakeAeroBackend *backend = nullptr;
    AeroEffectSettings *session = nullptr;
};
QTEST_MAIN(AeroEffectSettingsTest)
#include "aero_effect_settings_test.moc"
