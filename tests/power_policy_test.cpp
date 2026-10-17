#include <QtTest>
#include "power_policy_fixture.h"
#include "settings/PowerPolicySettings.h"
#include "ui/pages/PowerOptionsPage.h"
#include <QComboBox>
#include <QPushButton>
#include <QRadioButton>

class FakePowerPolicyBackend final : public PowerPolicyBackend {
public:
    int reads = 0;
    int writes = 0;
    int cancellations = 0;
    PowerPolicySnapshot expected, requested;
    void readCurrent() override { ++reads; }
    void writePolicy(const PowerPolicySnapshot &before, const PowerPolicySnapshot &after) override
    { ++writes; expected = before; requested = after; }
    void cancel() override { ++cancellations; }
    void read(const PowerPolicySnapshot &snapshot) { emit readFinished(true, PowerPolicyFields::hasIntents(snapshot) ? savedPowerPolicyRequest(snapshot) : snapshot, {}); }
    void readError() { emit readFinished(false, {}, QStringLiteral("Read failed")); }
    void write(bool saved = true, bool acknowledged = true, bool cancelled = false)
    { emit writeFinished(saved, acknowledged, cancelled, acknowledged ? QString() : QStringLiteral("Reload failed")); }
    void changed() { emit externalChange(); }
};

class FakePagePowerProfiles final : public PowerProfileBackend {
public:
    QStringList availableProfiles(QString *) override { return {"balanced", "performance"}; }
    QString activeProfile(QString *) override { return active; }
    void setActiveProfile(const QString &id, Completion callback) override
    { requested = id; finish = std::move(callback); }
    void complete()
    {
        active = requested;
        auto callback = std::move(finish);
        callback({});
    }
    QString active = QStringLiteral("balanced"), requested;
    Completion finish;
};

class PowerPolicyTest : public QObject {
    Q_OBJECT
private slots:
    void init()
    {
        baseline = {};
        baseline.serviceAvailable = true;
        baseline.owner = ":1.42"; baseline.capabilityRevision = "caps1";
        for (const QString &id : {QStringLiteral("AC"), QStringLiteral("Battery"), QStringLiteral("LowBattery")}) {
            PowerPolicyProfile p;
            p.id = id;
            p.displayEditable = p.sleepEditable = true;
            p.idleActions = {{0, "Do nothing"}, {1, "Sleep"}, {2, "Hibernate"}, {8, "Shut down"}};
            p.powerButtonActions = {{0, "Do nothing"}, {1, "Sleep"}, {32, "Lock"}, {64, "Turn off screen"}};
            p.lidActions = {{0, "Do nothing"}, {1, "Sleep"}, {32, "Lock"}};
            p.sleepModes = {{1, "Suspend"}, {2, "Hybrid suspend"}, {3, "Suspend then hibernate"}};
            p.dimEditable = p.brightnessEditable = p.keyboardBrightnessEditable = true;
            p.lockedDisplayEditable = p.lockBeforeDisplayOffEditable = true;
            p.powerButtonEditable = p.powerDownEditable = p.lidEditable = true;
            p.lidExternalMonitorEditable = p.sleepModeEditable = true;
            p.displaySeconds = 600;
            p.suspendAction = 1;
            p.suspendSeconds = 900;
            baseline.profiles << p;
        }
        baseline.batteryLevelsEditable = baseline.batteryCriticalActionEditable = true;
        baseline.batteryCriticalActions = baseline.profiles.first().idleActions;
        seedPowerPolicyMetadata(baseline);
        backend = new FakePowerPolicyBackend;
        session = new PowerPolicySettings(backend);
        QTRY_COMPARE(backend->reads, 1);
        backend->read(baseline);
        QVERIFY(session->isReady());
    }
    void cleanup() { delete session; }
    void stagesEachSourceWithoutWritingAndRevertReads()
    {
        session->selectDisplayTimeout("AC", 1200);
        session->selectSleepTimeout("LowBattery", -1);
        QCOMPARE(session->applied().profiles[0].displaySeconds, 600);
        QCOMPARE(session->staged().profiles[0].displaySeconds, 1200);
        QCOMPARE(session->staged().profiles[2].suspendAction, 0u);
        QCOMPARE(session->staged().profiles[1].suspendAction, 1u);
        QVERIFY(session->isDirty());
        QCOMPARE(backend->writes, 0);
        session->revert();
        QCOMPARE(backend->reads, 2);
        backend->read(baseline);
        QVERIFY(!session->isDirty());
        QCOMPARE(backend->writes, 0);
    }
    void validatesBoundsAndUnavailableControls()
    {
        for (int seconds : {0, 1, 29, 604801, -2})
            session->selectDisplayTimeout("AC", seconds);
        for (int seconds : {0, 1, 59, 604801, -2})
            session->selectSleepTimeout("AC", seconds);
        session->selectSleepTimeout("Unknown", 600);
        QVERIFY(!session->isDirty());
        baseline.profiles[1].displayEditable = false;
        seedPowerPolicyMetadata(baseline);
        backend->changed();
        backend->read(baseline);
        session->selectDisplayTimeout("Battery", 1200);
        QVERIFY(!session->isDirty());
        session->selectDisplayTimeout("AC", 30);
        QVERIFY(session->isDirty());
    }
    void successRequiresPreflightReloadAndExactReadback()
    {
        QSignalSpy finished(session, &SettingsSession::applyFinished);
        session->selectDisplayTimeout("AC", 1200);
        session->apply();
        QVERIFY(session->isBusy());
        QCOMPARE(backend->writes, 0);
        backend->read(baseline);
        QCOMPARE(backend->writes, 1);
        session->selectDisplayTimeout("Battery", 1800);
        session->revert();
        session->apply();
        QCOMPARE(backend->writes, 1);
        QCOMPARE(session->staged().profiles[1].displaySeconds, 600);
        backend->write();
        QVERIFY(session->isBusy());
        QVERIFY(finished.isEmpty());
        backend->read(backend->requested);
        QCOMPARE(finished.size(), 1);
        QVERIFY(finished.first().first().toBool());
        QVERIFY(!session->isDirty());
        QVERIFY(session->resultText().contains("acknowledged"));
    }
    void preflightDetectsDelayedExternalChange()
    {
        session->selectDisplayTimeout("AC", 1200);
        session->apply();
        baseline.profiles[1].suspendSeconds = 1800;
        backend->read(baseline);
        QCOMPARE(backend->writes, 0);
        QVERIFY(session->hasConflict());
        QCOMPARE(session->staged().profiles[0].displaySeconds, 1200);
        QVERIFY(!session->isBusy());
    }
    void dirtyExternalChangeRetainsEditsAndBlocksApply()
    {
        session->selectDisplayTimeout("AC", 1200);
        backend->changed();
        baseline.profiles[0].displaySeconds = 1800;
        backend->read(baseline);
        QVERIFY(session->hasConflict());
        session->apply();
        QCOMPARE(backend->writes, 0);
        QCOMPARE(session->staged().profiles[0].displaySeconds, 1200);
        session->revert();
        backend->read(baseline);
        QCOMPARE(session->staged().profiles[0].displaySeconds, 1800);
        QVERIFY(!session->hasConflict());
    }
    void cleanExternalChangeUpdatesSelection()
    {
        baseline.profiles[0].displaySeconds = 1800;
        backend->changed();
        backend->read(baseline);
        QCOMPARE(session->staged().profiles[0].displaySeconds, 1800);
        QVERIFY(!session->isDirty());
    }
    void failedPreflightNeverWrites()
    {
        session->selectSleepTimeout("AC", 1800);
        session->apply();
        backend->readError();
        QCOMPARE(backend->writes, 0);
        QVERIFY(!session->isReady());
        QVERIFY(session->hasConflict());
        QVERIFY(session->isDirty());
    }
    void fileSavedWithoutReloadAcknowledgementIsFailure()
    {
        QSignalSpy finished(session, &SettingsSession::applyFinished);
        session->selectDisplayTimeout("AC", 1200);
        session->apply();
        backend->read(baseline);
        backend->write(true, false);
        backend->read(backend->requested);
        QVERIFY(!finished.first().first().toBool());
        QCOMPARE(session->applied().profiles[0].displaySeconds, 1200);
        QVERIFY(session->hasConflict());
        QVERIFY(!session->errorText().isEmpty());
        backend->changed();
        backend->read(backend->requested);
        QCOMPARE(session->errorText(), QStringLiteral("Reload failed"));
        session->apply();
        QCOMPARE(backend->writes, 1);
    }
    void mismatchedReadbackDoesNotClaimSuccess()
    {
        QSignalSpy finished(session, &SettingsSession::applyFinished);
        session->selectDisplayTimeout("AC", 1200);
        session->apply();
        backend->read(baseline);
        backend->write();
        backend->read(baseline);
        QVERIFY(!finished.first().first().toBool());
        QVERIFY(session->isDirty());
        QVERIFY(session->hasConflict());
    }
    void cancelledReloadIsDistinctFromFailureAndReadsBack()
    {
        QSignalSpy finished(session, &SettingsSession::applyFinished);
        session->selectDisplayTimeout("AC", -1);
        session->apply();
        backend->read(baseline);
        session->cancel();
        QCOMPARE(backend->cancellations, 1);
        backend->write(true, false, true);
        QVERIFY(finished.isEmpty());
        backend->read(backend->requested);
        QVERIFY(session->wasCancelled());
        QVERIFY(session->errorText().contains("cancelled"));
        QVERIFY(!finished.first().first().toBool());
    }
    void failedReadbackRequiresRevert()
    {
        session->selectSleepTimeout("Battery", 1800);
        session->apply();
        backend->read(baseline);
        backend->write();
        backend->readError();
        QVERIFY(!session->isReady());
        QVERIFY(session->hasConflict());
        session->apply();
        QCOMPARE(backend->writes, 1);
        session->revert();
        backend->read(backend->requested);
        QVERIFY(session->isReady());
        QVERIFY(!session->hasConflict());
    }
    void watchDuringApplyRefreshesAfterReadback()
    {
        session->selectDisplayTimeout("AC", 1200);
        session->apply();
        backend->read(baseline);
        backend->changed();
        backend->write();
        backend->read(backend->requested);
        QTRY_COMPARE(backend->reads, 4);
        baseline.profiles[0].displaySeconds = 1800;
        backend->read(baseline);
        QCOMPARE(session->staged().profiles[0].displaySeconds, 1800);
        QVERIFY(!session->isDirty());
    }
    void extendedPolicyIsOneExplicitVerifiedTransaction()
    {
        session->selectIdleAction("AC", 2);
        session->selectIdleTimeout("AC", 1800);
        session->selectDimTimeout("Battery", 10);
        session->selectBrightness("Battery", true, 45);
        session->selectKeyboardBrightness("Battery", true, 20);
        session->selectLockedDisplayTimeout("AC", -2);
        session->selectLockBeforeDisplayOff("AC", true);
        session->selectPowerButtonAction("AC", 32);
        session->selectPowerDownAction("AC", 64);
        session->selectLidAction("Battery", 32);
        session->selectLidExternalMonitorInhibition("Battery", true);
        session->selectSleepMode("Battery", 3);
        session->selectBatteryLevels(20, 10);
        session->selectBatteryCriticalAction(2);
        QCOMPARE(backend->writes, 0);
        QVERIFY(session->isDirty());
        const auto requested = session->staged();
        QCOMPARE(requested.profiles[0].suspendAction, 2u);
        QCOMPARE(requested.profiles[0].suspendSeconds, 1800);
        QCOMPARE(requested.profiles[1].brightnessPercent, 45);
        QCOMPARE(requested.profiles[1].keyboardBrightnessPercent, 20);
        QCOMPARE(requested.profiles[1].sleepMode, 3u);
        QCOMPARE(requested.batteryCriticalAction, 2u);
        QSignalSpy finished(session, &SettingsSession::applyFinished);
        session->apply(); backend->read(baseline);
        QCOMPARE(backend->writes, 1);
        QVERIFY(backend->expected == baseline);
        QVERIFY(backend->requested == requested);
        backend->write(); QVERIFY(finished.isEmpty());
        backend->read(requested);
        QCOMPARE(finished.size(), 1); QVERIFY(finished[0][0].toBool());
        QVERIFY(!session->isDirty()); QVERIFY(!session->hasConflict());
    }
    void unsupportedRawActionsAndDisabledFieldsStayUnchanged()
    {
        auto current = baseline;
        auto &p = current.profiles[0];
        p.suspendAction = 128; p.powerButtonAction = 999; p.lidAction = 128;
        p.dimEditable = p.brightnessEditable = p.lidEditable = false;
        seedPowerPolicyMetadata(current);
        backend->changed(); backend->read(current);
        session->selectIdleTimeout("AC", 1800);
        QCOMPARE(session->staged().profiles[0].suspendAction, 128u);
        session->selectPowerButtonAction("AC", 999);
        session->selectLidAction("AC", 1);
        session->selectDimTimeout("AC", 20);
        session->selectBrightness("AC", true, 20);
        QCOMPARE(session->staged().profiles[0].powerButtonAction, 999u);
        QCOMPARE(session->staged().profiles[0].lidAction, 128u);
        QCOMPARE(session->staged().profiles[0].dimSeconds, current.profiles[0].dimSeconds);
        QCOMPARE(session->staged().profiles[0].brightnessPercent, current.profiles[0].brightnessPercent);
        session->selectIdleAction("AC", 2);
        QCOMPARE(session->staged().profiles[0].suspendAction, 2u);
        QCOMPARE(session->staged().profiles[0].suspendSeconds, 1800);
        QCOMPARE(backend->writes, 0);
    }
    void extendedBoundsAndBatteryOrderingAreAtomic()
    {
        for (int value : {-3, -1, 1, 9, 604801}) session->selectLockedDisplayTimeout("AC", value);
        for (int value : {-2, 0, 9, 604801}) session->selectDimTimeout("AC", value);
        session->selectBrightness("AC", true, 101);
        session->selectKeyboardBrightness("AC", true, -1);
        session->selectSleepMode("AC", 4);
        session->selectIdleAction("AC", 999);
        session->selectBatteryLevels(101, 5);
        session->selectBatteryLevels(10, 11);
        session->selectBatteryLevels(10, -1);
        session->selectBatteryCriticalAction(999);
        QVERIFY(!session->isDirty());
        QVERIFY(session->staged() == baseline);
        session->selectBatteryLevels(0, 0);
        QCOMPARE(session->staged().batteryLowLevel, 0);
        QCOMPARE(session->staged().batteryCriticalLevel, 0);
        session->selectLockedDisplayTimeout("AC", 0);
        QCOMPARE(session->staged().profiles[0].lockedDisplaySeconds, 0);
        QCOMPARE(backend->writes, 0);
    }
    void ownerOrCapabilityChangesPreventFalseSuccess_data()
    {
        QTest::addColumn<bool>("owner"); QTest::addColumn<bool>("duringReadback");
        QTest::newRow("owner preflight") << true << false;
        QTest::newRow("capabilities preflight") << false << false;
        QTest::newRow("owner readback") << true << true;
        QTest::newRow("capabilities readback") << false << true;
    }
    void ownerOrCapabilityChangesPreventFalseSuccess()
    {
        QFETCH(bool, owner); QFETCH(bool, duringReadback);
        session->selectPowerButtonAction("AC", 32);
        session->apply();
        auto changed = duringReadback ? session->staged() : baseline;
        if (owner) changed.owner = ":1.43";
        else changed.capabilityRevision = "caps2";
        QSignalSpy finished(session, &SettingsSession::applyFinished);
        if (duringReadback) { backend->read(baseline); backend->write(); }
        backend->read(changed);
        QCOMPARE(backend->writes, duringReadback ? 1 : 0);
        QCOMPARE(finished.size(), 1); QVERIFY(!finished[0][0].toBool());
        QVERIFY(session->hasConflict()); QVERIFY(session->isDirty());
        QCOMPARE(session->staged().profiles[0].powerButtonAction, 32u);
    }
    void unacknowledgedMatchingReadbackStaysPendingUntilExplicitRevert()
    {
        session->selectLidAction("Battery", 32);
        session->apply(); backend->read(baseline);
        const auto requested = backend->requested;
        backend->write(true, false); backend->read(requested);
        QVERIFY(session->isDirty()); QVERIFY(session->hasConflict());
        backend->changed(); backend->read(requested);
        QVERIFY(session->isDirty()); QVERIFY(session->hasConflict());
        session->revert(); backend->read(requested);
        QVERIFY(!session->isDirty()); QVERIFY(!session->hasConflict());
    }
    void failedRevertDoesNotLosePendingPolicy()
    {
        session->selectPowerButtonAction("AC", 32);
        session->revert();
        QVERIFY(session->isBusy()); QVERIFY(session->isDirty());
        backend->readError();
        QVERIFY(!session->isReady()); QVERIFY(session->isDirty()); QVERIFY(session->hasConflict());
        QCOMPARE(session->staged().profiles[0].powerButtonAction, 32u);
        session->revert(); backend->read(baseline);
        QVERIFY(session->isReady()); QVERIFY(!session->isDirty()); QVERIFY(!session->hasConflict());
    }
    void nativeControlsStageAndShareSessionAcrossPageLifetimes()
    {
        auto profilesBackend = std::make_unique<FakePagePowerProfiles>();
        PowerProfileService profiles(std::move(profilesBackend));
        auto page = std::make_unique<PowerOptionsPage>(nullptr, session, &profiles, nullptr);
        QCOMPARE(page->settingsSession(), session);
        auto *display = page->findChild<QComboBox *>("powerDisplayTimeout_AC");
        auto *sleep = page->findChild<QComboBox *>("powerSleepTimeout_LowBattery");
        auto *apply = page->findChild<QPushButton *>("powerPolicyApply");
        auto *revert = page->findChild<QPushButton *>("powerPolicyRevert");
        QVERIFY(display && sleep && apply && revert);
        QVERIFY(display->isEnabled());
        QVERIFY(!apply->isEnabled());
        const int index = display->findData(1200);
        display->setCurrentIndex(index);
        QVERIFY(QMetaObject::invokeMethod(display, "activated", Q_ARG(int, index)));
        QCOMPARE(backend->writes, 0);
        QCOMPARE(session->applied().profiles[0].displaySeconds, 600);
        QVERIFY(apply->isEnabled());
        page.reset();
        QVERIFY(session->isDirty());
        page = std::make_unique<PowerOptionsPage>(nullptr, session, &profiles, nullptr);
        display = page->findChild<QComboBox *>("powerDisplayTimeout_AC");
        QCOMPARE(display->currentData().toInt(), 1200);
        page->findChild<QPushButton *>("powerPolicyApply")->click();
        QVERIFY(session->isBusy());
        QVERIFY(!display->isEnabled());
        backend->read(baseline);
        QCOMPARE(backend->writes, 1);
        page.reset(); // Completion is owned by the application session.
        backend->write();
        backend->read(backend->requested);
        QVERIFY(!session->isBusy());
        QVERIFY(!session->isDirty());
        page = std::make_unique<PowerOptionsPage>(nullptr, session, &profiles, nullptr);
        QCOMPARE(page->findChild<QComboBox *>("powerDisplayTimeout_AC")->currentData().toInt(), 1200);
        QVERIFY(!page->findChild<QPushButton *>("powerPolicyApply")->isEnabled());
    }
    void immediatePerformanceProfileDoesNotApplyStagedPolicy()
    {
        auto profilesBackend = std::make_unique<FakePagePowerProfiles>();
        auto *ppd = profilesBackend.get();
        PowerProfileService profiles(std::move(profilesBackend));
        PowerOptionsPage page(nullptr, session, &profiles, nullptr);
        session->selectDisplayTimeout("AC", 1200);
        QRadioButton *performance = nullptr;
        for (auto *radio : page.findChildren<QRadioButton *>())
            if (radio->property("profileId").toString() == QLatin1String("performance"))
                performance = radio;
        QVERIFY(performance);
        performance->click();
        QCOMPARE(ppd->requested, QStringLiteral("performance"));
        QVERIFY(profiles.isBusy());
        QVERIFY(session->isDirty());
        QCOMPARE(backend->writes, 0);
        ppd->complete();
        QCOMPARE(profiles.activeProfile(), QStringLiteral("performance"));
        QVERIFY(session->isDirty());
        QCOMPARE(backend->writes, 0);
    }
    void nativeUnavailableControlsCannotStageValues()
    {
        baseline.profiles[1].displayEditable = baseline.profiles[1].sleepEditable = false;
        baseline.profiles[1].displayReason = baseline.profiles[1].sleepReason = QStringLiteral("No system battery is present.");
        seedPowerPolicyMetadata(baseline);
        backend->changed();
        backend->read(baseline);
        auto profilesBackend = std::make_unique<FakePagePowerProfiles>();
        PowerProfileService profiles(std::move(profilesBackend));
        PowerOptionsPage page(nullptr, session, &profiles, nullptr);
        QVERIFY(!page.findChild<QComboBox *>("powerDisplayTimeout_Battery")->isEnabled());
        QVERIFY(!page.findChild<QComboBox *>("powerSleepTimeout_Battery")->isEnabled());
        QVERIFY(page.findChild<QComboBox *>("powerDisplayTimeout_Battery")->toolTip().contains("battery"));
        QVERIFY(page.findChild<QComboBox *>("powerDisplayTimeout_AC")->isEnabled());
    }
    void unknownFallbackCanBeExplicitlySavedWithoutChangingItsGuessedNumber()
    {
        const auto key=PowerPolicyKey::DisplaySeconds;
        auto &state=baseline.profiles[0].keys[key]; state.valueKnown=false;
        state.localPresent=state.effectivePresent=false; state.defaultKnown=false;
        state.defaultReason=QStringLiteral("Unknown native default");
        baseline.profiles[0].displayEditable=false; // locked companion must not block this key
        session->refresh(); backend->read(baseline);
        session->selectValue("AC",key,600);
        QVERIFY(session->isDirty());
        QCOMPARE(session->staged().profiles[0].displaySeconds,600);
        QCOMPARE(session->staged().profiles[0].intents.value(key),PowerPolicyIntent::Set);
        QVERIFY(session->applySupportText().isEmpty());
        session->apply(); backend->read(baseline); QCOMPARE(backend->writes,1);
        backend->write(); backend->read(backend->requested);
        QVERIFY(!session->isDirty()); QVERIFY(session->applied().profiles[0].keys.value(key).valueKnown);
        QVERIFY(session->applied().profiles[0].keys.value(key).localPresent);
        QVERIFY(!session->applied().profiles[0].keys.value(key).defaultKnown);
    }
    void nativeDefaultsAreScopedReviewedAndDoNotApplyAutomatically()
    {
        auto &state=baseline.profiles[0].keys[PowerPolicyKey::DisplaySeconds];
        state.defaultValue=300; state.resettable=true; state.resetReason.clear();
        session->refresh(); backend->read(baseline);
        session->selectIdleTimeout("Battery",1800);
        const auto review=session->reviewDefaults(PowerPolicySettings::DefaultsScope::SelectedProfile,"AC");
        QCOMPARE(review.changes.size(),1); QVERIFY(review.changes[0].contains("300 seconds"));
        session->stageDefaults(PowerPolicySettings::DefaultsScope::SelectedProfile,"AC",review.revision);
        QCOMPARE(session->staged().profiles[0].displaySeconds,300);
        QCOMPARE(session->staged().profiles[0].intents.value(PowerPolicyKey::DisplaySeconds),PowerPolicyIntent::RestoreDefault);
        QCOMPARE(session->staged().profiles[1].suspendSeconds,1800);
        QVERIFY(session->isDirty()); QCOMPARE(backend->writes,0);
        QVERIFY(session->applySupportText().isEmpty());
    }
    void changedDraftInvalidatesDefaultsReview()
    {
        auto &state=baseline.profiles[0].keys[PowerPolicyKey::DisplaySeconds];
        state.defaultValue=300; state.resettable=true;
        session->refresh(); backend->read(baseline);
        const auto review=session->reviewDefaults(PowerPolicySettings::DefaultsScope::SelectedProfile,"AC");
        session->selectDisplayTimeout("AC",1200);
        session->stageDefaults(PowerPolicySettings::DefaultsScope::SelectedProfile,"AC",review.revision);
        QCOMPARE(session->staged().profiles[0].displaySeconds,1200);
        QVERIFY(session->errorText().contains("Review defaults again")); QCOMPARE(backend->writes,0);
    }
    void defaultsCanCancelUnsavedSameFallbackOverrideWithoutAWrite()
    {
        auto &state=baseline.profiles[0].keys[PowerPolicyKey::DisplaySeconds];
        state.localPresent=state.effectivePresent=false; state.valueKnown=false;
        state.resetReason=QStringLiteral("No saved override; effective default unverified.");
        session->refresh(); backend->read(baseline);
        session->selectValue("AC",PowerPolicyKey::DisplaySeconds,600); QVERIFY(session->isDirty());
        session->restoreKeyDefault("AC",PowerPolicyKey::DisplaySeconds);
        QVERIFY(!session->isDirty()); QCOMPARE(backend->writes,0);
        QVERIFY(!session->staged().profiles[0].keys.value(PowerPolicyKey::DisplaySeconds).valueKnown);
    }
    void restoredDefaultNeedNotBeAnExplicitActionChoiceOrVerifiedDaemonFallback()
    {
        const auto key=PowerPolicyKey::PowerButtonAction;
        baseline.profiles[0].powerButtonAction=8;
        auto &state=baseline.profiles[0].keys[key]; state.defaultValue=128u; state.resettable=true;
        session->refresh(); backend->read(baseline);
        session->selectValue("AC",key,128u); QVERIFY(!session->isDirty());
        session->restoreKeyDefault("AC",key); QVERIFY(session->isDirty());
        QCOMPARE(session->staged().profiles[0].powerButtonAction,128u);
        session->apply(); backend->read(baseline); QCOMPARE(backend->writes,1);
        auto actual=savedPowerPolicyRequest(backend->requested);
        actual.profiles[0].powerButtonAction=16; actual.profiles[0].keys[key].valueKnown=false;
        backend->write(); backend->read(actual);
        QVERIFY(!session->isDirty()); QCOMPARE(session->applied().profiles[0].powerButtonAction,16u);
        QVERIFY(!session->applied().profiles[0].keys.value(key).valueKnown);
    }
    void batteryOrderingRequiresExplicitUnknownCompanionAndCanBeRepaired()
    {
        baseline.keys[PowerPolicyKey::BatteryCriticalLevel].valueKnown=false;
        session->refresh(); backend->read(baseline);
        session->selectGlobalValue(PowerPolicyKey::BatteryLowLevel,20);
        QVERIFY(session->isDirty()); QVERIFY(session->applySupportText().contains("explicit low and critical"));
        session->apply(); QCOMPARE(backend->writes,0); QVERIFY(!session->isBusy());
        session->selectGlobalValue(PowerPolicyKey::BatteryCriticalLevel,25);
        QVERIFY(session->applySupportText().contains("must not exceed"));
        session->selectGlobalValue(PowerPolicyKey::BatteryCriticalLevel,10);
        QVERIFY(session->applySupportText().isEmpty());
        QCOMPARE(session->staged().intents.size(),2);
    }
    void customTimeoutsAcceptTheNativeOneWeekMaximum()
    {
        QCOMPARE(PowerPolicySettings::MaximumTimeoutSeconds,604800);
        for(auto key:{PowerPolicyKey::DisplaySeconds,PowerPolicyKey::IdleSeconds,PowerPolicyKey::DimSeconds,PowerPolicyKey::LockedDisplaySeconds})
            session->selectValue("AC",key,604800);
        QVERIFY(session->isDirty()); QCOMPARE(session->staged().profiles[0].intents.size(),4);
        QVERIFY(session->applySupportText().isEmpty());
        for(auto key:{PowerPolicyKey::DisplaySeconds,PowerPolicyKey::IdleSeconds,PowerPolicyKey::DimSeconds,PowerPolicyKey::LockedDisplaySeconds}) {
            session->selectValue("AC",key,604801); QCOMPARE(PowerPolicyFields::value(session->staged().profiles[0],key).toInt(),604800);
        }
        session->apply(); backend->read(baseline); QCOMPARE(backend->writes,1);
        backend->write(); backend->read(backend->requested); QVERIFY(!session->isDirty());
    }
    void unsupportedSavedBatteryCompanionMustBeRepairedOnlyForRelatedEdits()
    {
        baseline.batteryLowLevel=120;
        session->refresh(); backend->read(baseline);
        session->selectPowerButtonAction("AC",32u); QVERIFY(session->applySupportText().isEmpty());
        session->selectGlobalValue(PowerPolicyKey::BatteryCriticalLevel,10);
        QVERIFY(session->applySupportText().contains("repair the saved companion"));
        session->selectGlobalValue(PowerPolicyKey::BatteryLowLevel,90);
        QVERIFY(session->applySupportText().isEmpty());
        session->revert(); backend->read(baseline);
        baseline.batteryLowLevel=10; baseline.batteryCriticalLevel=-5;
        session->refresh(); backend->read(baseline);
        session->selectIdleAction("AC",2u); QVERIFY(session->applySupportText().isEmpty());
        session->selectGlobalValue(PowerPolicyKey::BatteryLowLevel,20);
        QVERIFY(session->applySupportText().contains("repair the saved companion"));
        session->selectGlobalValue(PowerPolicyKey::BatteryCriticalLevel,5);
        QVERIFY(session->applySupportText().isEmpty());
    }
    void requestsRejectUnownedEditsForgedMetadataAndForeignReadback()
    {
        auto request=baseline; QString error;
        request.profiles[0].displaySeconds=1200;
        QVERIFY(!PowerPolicyFields::requestValid(baseline,request,error));
        QVERIFY(PowerPolicyFields::stage(request.profiles[0],baseline.profiles[0],PowerPolicyKey::DisplaySeconds,1200,error));
        QVERIFY(PowerPolicyFields::requestValid(baseline,request,error));
        auto forged=request; forged.profiles[0].keys[PowerPolicyKey::DisplaySeconds].immutable=true;
        QVERIFY(!PowerPolicyFields::requestValid(baseline,forged,error));
        auto actual=savedPowerPolicyRequest(request);
        QVERIFY(PowerPolicyFields::matchesRequest(baseline,request,actual,error));
        actual.profiles[1].keys[PowerPolicyKey::DimSeconds].revision="foreign numeric-equivalent change";
        QVERIFY(!PowerPolicyFields::matchesRequest(baseline,request,actual,error));
    }
private:
    PowerPolicySnapshot baseline;
    FakePowerPolicyBackend *backend = nullptr;
    PowerPolicySettings *session = nullptr;
};
QTEST_MAIN(PowerPolicyTest)
#include "power_policy_test.moc"
