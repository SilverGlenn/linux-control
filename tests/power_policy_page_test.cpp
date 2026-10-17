#include <QtTest>
#include <QCheckBox>
#include <QAction>
#include <QDialog>
#include <QPointer>
#include <functional>
#include <QTimer>
#include <QTreeWidget>
#include <QComboBox>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include "settings/PowerPolicySettings.h"
#include "settings/PowerProfileService.h"
#include "ui/pages/PowerOptionsPage.h"
#include "power_policy_fixture.h"

namespace {
PowerPolicySnapshot fixture()
{
    PowerPolicySnapshot snapshot; snapshot.serviceAvailable = true; snapshot.owner = QStringLiteral(":fake-power-page");
    snapshot.capabilityRevision = QStringLiteral("fake-capabilities-v1"); snapshot.batteryLevelsEditable = snapshot.batteryCriticalActionEditable = true;
    snapshot.batteryCriticalActions = {{0, QStringLiteral("Do nothing")}, {2, QStringLiteral("Hibernate")}, {8, QStringLiteral("Shut down")}};
    for (const auto &id : {QStringLiteral("AC"), QStringLiteral("Battery"), QStringLiteral("LowBattery")}) {
        PowerPolicyProfile profile; profile.id = id; profile.displayEditable = profile.sleepEditable = true;
        profile.suspendAction = 1; profile.suspendSeconds = 900;
        profile.idleActions = {{0, QStringLiteral("Do nothing")}, {1, QStringLiteral("Sleep")}, {2, QStringLiteral("Hibernate")}, {8, QStringLiteral("Shut down")}};
        profile.powerButtonActions = profile.idleActions; profile.lidActions = profile.idleActions;
        profile.sleepModes = {{1, QStringLiteral("Standby")}, {2, QStringLiteral("Hybrid sleep")}};
        profile.dimEditable = profile.brightnessEditable = profile.keyboardBrightnessEditable = true;
        profile.lockedDisplayEditable = profile.lockBeforeDisplayOffEditable = true;
        profile.powerButtonEditable = profile.powerDownEditable = profile.lidEditable = true;
        profile.lidExternalMonitorEditable = profile.sleepModeEditable = true;
        snapshot.profiles.append(profile);
    }
    seedPowerPolicyMetadata(snapshot); return snapshot;
}
void activate(QComboBox *combo, const QVariant &value)
{
    QVERIFY(combo); const int index = combo->findData(value); QVERIFY(index >= 0); combo->setCurrentIndex(index);
    QVERIFY(QMetaObject::invokeMethod(combo, "activated", Q_ARG(int, index)));
}
void finish(QSpinBox *spin, int value)
{ QVERIFY(spin); spin->setValue(value); QVERIFY(QMetaObject::invokeMethod(spin, "editingFinished")); }
void reviewCustomTimeout(PowerOptionsPage *page, QComboBox *combo, const std::function<void(QDialog *)> &review)
{
    const int index=combo->findText(QStringLiteral("Custom seconds…")); QVERIFY(index>=0); combo->setCurrentIndex(index);
    QVERIFY(QMetaObject::invokeMethod(combo,"activated",Q_ARG(int,index)));
    bool visited=false;
    QTimer::singleShot(0,page,[&] { auto *dialog=page->findChild<QDialog *>("powerCustomTimeoutDialog"); visited=true; QVERIFY(dialog); review(dialog); });
    QCoreApplication::processEvents(); QVERIFY(visited);
}

const PowerPolicyProfile &profile(const PowerPolicySettings &session, const QString &id)
{
    for (const auto &candidate : session.staged().profiles) if (candidate.id == id) return candidate;
    Q_UNREACHABLE();
}
}
class PagePolicyBackend final : public PowerPolicyBackend {
public:
    PowerPolicySnapshot current = fixture(), expected, requested;
    int reads = 0, writes = 0;
    bool holdRead = false;
    void readCurrent() override { ++reads; if (!holdRead) emit readFinished(true, current, {}); }
    void writePolicy(const PowerPolicySnapshot &before, const PowerPolicySnapshot &after) override { ++writes; expected = before; requested = after; }
    void cancel() override {}
    void complete() { current = savedPowerPolicyRequest(requested); emit writeFinished(true, true, false, {}); }
    void changed() { emit externalChange(); }
};
class PageProfilesBackend final : public PowerProfileBackend {
public:
    int writes = 0;
    QStringList availableProfiles(QString *) override { return {QStringLiteral("balanced")}; }
    QString activeProfile(QString *) override { return QStringLiteral("balanced"); }
    void setActiveProfile(const QString &, Completion completion) override { ++writes; completion({}); }
};
class PowerPolicyPageTest final : public QObject {
    Q_OBJECT
private slots:
    void init() {
        backend = new PagePolicyBackend; session = new PowerPolicySettings(backend); QTRY_VERIFY(session->isReady());
        auto profilesBackend = std::make_unique<PageProfilesBackend>(); ppd = profilesBackend.get(); profiles = new PowerProfileService(std::move(profilesBackend));
    }
    void cleanup() { delete session; delete profiles; }
    void allAdvancedControlsStageWithoutApplyingOrInvokingActions() {
        PowerOptionsPage page(nullptr, session, profiles, nullptr); QCOMPARE(page.objectName(), QStringLiteral("win7Page"));
        page.findChild<QPushButton *>("powerAdvancedToggle")->click(); QVERIFY(!page.findChild<QGroupBox *>("powerAdvancedSettings")->isHidden());
        activate(page.findChild<QComboBox *>("powerIdleAction"), 2u); activate(page.findChild<QComboBox *>("powerSleepTimeout_AC"), 1200);
        activate(page.findChild<QComboBox *>("powerDimTimeout"), 120); page.findChild<QCheckBox *>("powerDimEnabled")->setChecked(true); page.findChild<QCheckBox *>("powerBrightnessEnabled")->setChecked(true);
        finish(page.findChild<QSpinBox *>("powerBrightnessPercent"), 45); page.findChild<QCheckBox *>("powerKeyboardBrightnessEnabled")->setChecked(true);
        finish(page.findChild<QSpinBox *>("powerKeyboardBrightnessPercent"), 30); activate(page.findChild<QComboBox *>("powerLockedDisplayTimeout"), 0);
        page.findChild<QCheckBox *>("powerLockBeforeDisplayOff")->setChecked(true); activate(page.findChild<QComboBox *>("powerButtonAction"), 1u);
        activate(page.findChild<QComboBox *>("powerDownAction"), 8u); activate(page.findChild<QComboBox *>("powerLidAction"), 2u);
        page.findChild<QCheckBox *>("powerLidExternalMonitor")->setChecked(true); activate(page.findChild<QComboBox *>("powerSleepMode"), 2u);
        finish(page.findChild<QSpinBox *>("powerBatteryLowLevel"), 20); finish(page.findChild<QSpinBox *>("powerBatteryCriticalLevel"), 10);
        activate(page.findChild<QComboBox *>("powerBatteryCriticalAction"), 2u);
        const auto &selected = profile(*session, QStringLiteral("AC")); QCOMPARE(selected.suspendAction, 2u); QCOMPARE(selected.suspendSeconds, 1200);
        QCOMPARE(selected.dimSeconds, 120); QVERIFY(selected.dimEnabled); QVERIFY(selected.brightnessEnabled); QCOMPARE(selected.brightnessPercent, 45);
        QVERIFY(selected.keyboardBrightnessEnabled); QCOMPARE(selected.keyboardBrightnessPercent, 30); QCOMPARE(selected.lockedDisplaySeconds, 0);
        QVERIFY(selected.lockBeforeDisplayOff); QCOMPARE(selected.powerButtonAction, 1u); QCOMPARE(selected.powerDownAction, 8u); QCOMPARE(selected.lidAction, 2u);
        QVERIFY(selected.inhibitLidActionWhenExternalMonitorPresent); QCOMPARE(selected.sleepMode, 2u);
        QCOMPARE(session->staged().batteryLowLevel, 20); QCOMPARE(session->staged().batteryCriticalLevel, 10); QCOMPARE(session->staged().batteryCriticalAction, 2u);
        QVERIFY(session->isDirty()); QVERIFY(page.findChild<QPushButton *>("powerPolicyApply")->isEnabled()); QCOMPARE(backend->writes, 0); QCOMPARE(ppd->writes, 0);
    }
    void idleTimeoutRetainsHibernateAndNeverStagesAnImplicitSleep() {
        backend->current.profiles[0].suspendAction = 2; seedPowerPolicyMetadata(backend->current); session->refresh();
        PowerOptionsPage page(nullptr, session, profiles, nullptr); activate(page.findChild<QComboBox *>("powerSleepTimeout_AC"), 1800);
        QCOMPARE(profile(*session, QStringLiteral("AC")).suspendAction, 2u); QCOMPARE(profile(*session, QStringLiteral("AC")).suspendSeconds, 1800);
        activate(page.findChild<QComboBox *>("powerSleepTimeout_AC"), -1); QCOMPARE(profile(*session, QStringLiteral("AC")).suspendAction, 0u);
        QCOMPARE(profile(*session, QStringLiteral("AC")).suspendSeconds, 1800); QVERIFY(page.findChild<QComboBox *>("powerSleepTimeout_AC")->isEnabled());
        activate(page.findChild<QComboBox *>("powerIdleAction"), 8u); QCOMPARE(profile(*session, QStringLiteral("AC")).suspendAction, 8u);
        QCOMPARE(page.findChild<QComboBox *>("powerSleepTimeout_AC")->currentData().toInt(), 1800); QCOMPARE(backend->writes, 0);
    }
    void sourceChangesAndPageRecreationRetainTheirDraftAndViewState() {
        auto *page = new PowerOptionsPage(nullptr, session, profiles, nullptr); page->findChild<QPushButton *>("powerAdvancedToggle")->click();
        auto *source = page->findChild<QComboBox *>("powerPolicySource"); source->setCurrentIndex(source->findData(QStringLiteral("Battery")));
        activate(page->findChild<QComboBox *>("powerLidAction"), 2u); delete page;
        page = new PowerOptionsPage(nullptr, session, profiles, nullptr); QCOMPARE(page->findChild<QComboBox *>("powerPolicySource")->currentData().toString(), QStringLiteral("Battery"));
        QVERIFY(!page->findChild<QGroupBox *>("powerAdvancedSettings")->isHidden()); QCOMPARE(page->findChild<QComboBox *>("powerLidAction")->currentData().toUInt(), 2u);
        QCOMPARE(profile(*session, QStringLiteral("AC")).lidAction, 0u); QCOMPARE(profile(*session, QStringLiteral("Battery")).lidAction, 2u);
        delete page; QVERIFY(session->isDirty()); QCOMPARE(backend->writes, 0);
    }
    void unsupportedRawActionsRemainVisibleAndUnavailableFieldsCannotStage() {
        auto &stored = backend->current.profiles[0]; stored.suspendAction = 128; stored.sleepEditable = false; stored.sleepReason = QStringLiteral("Unrecognized native idle action.");
        stored.lidAction = 64; stored.lidEditable = false; stored.lidReason = QStringLiteral("No lid switch is available.");
        stored.brightnessPercent = 37; stored.brightnessEditable = false; stored.brightnessReason = QStringLiteral("Brightness control is not advertised."); seedPowerPolicyMetadata(backend->current); session->refresh();
        PowerOptionsPage page(nullptr, session, profiles, nullptr);
        auto *idle = page.findChild<QComboBox *>("powerIdleAction"); QCOMPARE(idle->currentData().toUInt(), 128u); QVERIFY(idle->currentText().contains("128")); QVERIFY(!idle->isEnabled());
        auto *lid = page.findChild<QComboBox *>("powerLidAction"); QCOMPARE(lid->currentData().toUInt(), 64u); QVERIFY(!lid->isEnabled());
        QCOMPARE(page.findChild<QSpinBox *>("powerBrightnessPercent")->value(), 37); QVERIFY(!page.findChild<QCheckBox *>("powerBrightnessEnabled")->isEnabled());
        QVERIFY(page.findChild<QLabel *>("powerAdvancedReason")->text().contains("No lid switch")); QVERIFY(!session->isDirty()); QCOMPARE(backend->writes, 0);
    }
    void renderingAndSourceSelectionNeverStageControlDefaults() {
        backend->current.profiles[0].brightnessEnabled = true; backend->current.profiles[0].brightnessPercent = 37;
        backend->current.profiles[1].keyboardBrightnessEnabled = true; backend->current.profiles[1].keyboardBrightnessPercent = 22; seedPowerPolicyMetadata(backend->current); session->refresh();
        PowerOptionsPage page(nullptr, session, profiles, nullptr); QVERIFY(!session->isDirty());
        auto *source = page.findChild<QComboBox *>("powerPolicySource"); source->setCurrentIndex(source->findData(QStringLiteral("Battery"))); QVERIFY(!session->isDirty());
        QCOMPARE(page.findChild<QSpinBox *>("powerKeyboardBrightnessPercent")->value(), 22); source->setCurrentIndex(source->findData(QStringLiteral("AC")));
        QCOMPARE(page.findChild<QSpinBox *>("powerBrightnessPercent")->value(), 37); QVERIFY(!session->isDirty()); QCOMPARE(backend->writes, 0);
    }
    void rawTimersStayDistinctFromNeverAndLockedNegativeRetainsItsMeaning() {
        auto &stored = backend->current.profiles[0]; stored.displayEnabled = true; stored.displaySeconds = -1;
        stored.suspendAction = 2; stored.suspendSeconds = 0; stored.lockedDisplaySeconds = -7; seedPowerPolicyMetadata(backend->current); session->refresh();
        PowerOptionsPage page(nullptr, session, profiles, nullptr);
        auto *display = page.findChild<QComboBox *>("powerDisplayTimeout_AC"); auto *idle = page.findChild<QComboBox *>("powerSleepTimeout_AC");
        QCOMPARE(display->currentData().toInt(), -1); QVERIFY(display->currentText().contains("current")); QVERIFY(!display->currentText().contains("Never"));
        QCOMPARE(idle->currentData().toInt(), 0); QVERIFY(idle->currentText().contains("current")); QVERIFY(!idle->currentText().contains("Never"));
        auto *locked = page.findChild<QComboBox *>("powerLockedDisplayTimeout"); QCOMPARE(locked->currentData().toInt(), -7); QVERIFY(locked->currentText().contains("Same as unlocked"));
        QVERIFY(!session->isDirty()); QCOMPARE(backend->writes, 0);
        const auto sidebar = PowerOptionsPage::sidebarLinks(); QCOMPARE(sidebar.at(1).target.page, PageId::PowerOptions);
    }
    void policyJobSurvivesItsViewAndDisablesEveryAdvancedMutation() {
        auto *page = new PowerOptionsPage(nullptr, session, profiles, nullptr); activate(page->findChild<QComboBox *>("powerLidAction"), 2u);
        page->findChild<QPushButton *>("powerPolicyApply")->click(); QCOMPARE(backend->writes, 1); QVERIFY(session->isBusy());
        QVERIFY(!page->findChild<QComboBox *>("powerLidAction")->isEnabled()); QVERIFY(!page->findChild<QSpinBox *>("powerBatteryLowLevel")->isEnabled()); delete page;
        backend->complete(); QVERIFY(!session->isBusy()); QVERIFY(!session->isDirty()); page = new PowerOptionsPage(nullptr, session, profiles, nullptr);
        QCOMPARE(page->findChild<QComboBox *>("powerLidAction")->currentData().toUInt(), 2u); QVERIFY(!page->findChild<QPushButton *>("powerPolicyApply")->isEnabled()); delete page;
        QCOMPARE(ppd->writes, 0);
    }
    void conflictsRetainAllAdvancedDraftsUntilExplicitRevert() {
        PowerOptionsPage page(nullptr, session, profiles, nullptr); activate(page.findChild<QComboBox *>("powerDownAction"), 8u);
        backend->current.profiles[1].displaySeconds = 1200; backend->changed(); QVERIFY(session->hasConflict());
        QCOMPARE(profile(*session, QStringLiteral("AC")).powerDownAction, 8u); QVERIFY(!page.findChild<QComboBox *>("powerDownAction")->isEnabled());
        page.findChild<QPushButton *>("powerPolicyRevert")->click(); QVERIFY(!session->hasConflict()); QVERIFY(!session->isDirty());
        QCOMPARE(page.findChild<QComboBox *>("powerDownAction")->currentData().toUInt(), 0u); QCOMPARE(backend->writes, 0);
    }
    void perKeyMetadataAllowsExplicitReplacementWithoutChangingCompanions() {
        auto &p = backend->current.profiles[0]; p.sleepEditable = p.brightnessEditable = false;
        p.suspendAction = 128; auto &action = p.keys[PowerPolicyKey::IdleAction]; action.writable = true; action.valueKnown = false; action.defaultKnown = false;
        auto &brightness = p.keys[PowerPolicyKey::BrightnessPercent]; brightness.writable = true; brightness.valueKnown = false;
        p.keys[PowerPolicyKey::BrightnessEnabled].writable = false; session->refresh();
        PowerOptionsPage page(nullptr,session,profiles,nullptr); auto *idle = page.findChild<QComboBox *>("powerIdleAction");
        QVERIFY(idle->isEnabled()); QVERIFY(idle->currentText().contains("unavailable"));
        auto *percent = page.findChild<QSpinBox *>("powerBrightnessPercent"); QVERIFY(percent->isEnabled()); QCOMPARE(percent->text(),QStringLiteral("Unknown"));
        QVERIFY(!page.findChild<QCheckBox *>("powerBrightnessEnabled")->isEnabled());
        activate(idle,2u); finish(percent,42);
        QCOMPARE(profile(*session,"AC").suspendAction,2u); QCOMPARE(profile(*session,"AC").brightnessPercent,42);
        QVERIFY(!profile(*session,"AC").brightnessEnabled); QCOMPARE(profile(*session,"AC").intents.size(),2);
        QVERIFY(!profile(*session,"AC").intents.contains(PowerPolicyKey::IdleSeconds));
        QVERIFY(!profile(*session,"AC").intents.contains(PowerPolicyKey::BrightnessEnabled)); QCOMPARE(backend->writes,0);
    }
    void unknownCheckboxAndMissingMetadataStayTruthfulWithoutMaterializingFallbacks() {
        auto &p = backend->current.profiles[0]; p.keys[PowerPolicyKey::DimEnabled].valueKnown = false;
        p.keys.remove(PowerPolicyKey::LidAction); session->refresh();
        PowerOptionsPage page(nullptr,session,profiles,nullptr);
        auto *dim = page.findChild<QCheckBox *>("powerDimEnabled"); QCOMPARE(dim->checkState(),Qt::PartiallyChecked);
        QVERIFY(!page.findChild<QComboBox *>("powerLidAction")->isEnabled());
        auto *spin = page.findChild<QSpinBox *>("powerBrightnessPercent"); QMetaObject::invokeMethod(spin,"editingFinished");
        QVERIFY(!session->isDirty()); dim->setCheckState(Qt::Checked); QVERIFY(profile(*session,"AC").dimEnabled);
        QVERIFY(profile(*session,"AC").intents.contains(PowerPolicyKey::DimEnabled)); QCOMPARE(backend->writes,0);
    }
    void explicitFallbackEqualValueCreatesOnlyItsRequestedOverride() {
        auto &state = backend->current.profiles[0].keys[PowerPolicyKey::BrightnessPercent];
        state.localPresent = state.effectivePresent = false; state.valueKnown = state.defaultKnown = true; session->refresh();
        PowerOptionsPage page(nullptr,session,profiles,nullptr); auto *spin = page.findChild<QSpinBox *>("powerBrightnessPercent");
        QMetaObject::invokeMethod(spin,"editingFinished"); QVERIFY(!session->isDirty());
        auto *explicitValue = spin->findChild<QAction *>("policyExplicitValue"); QVERIFY(explicitValue); explicitValue->trigger();
        QVERIFY(session->isDirty()); QCOMPARE(profile(*session,"AC").brightnessPercent,100);
        QCOMPARE(profile(*session,"AC").intents.value(PowerPolicyKey::BrightnessPercent),PowerPolicyIntent::Set);
        QCOMPARE(profile(*session,"AC").intents.size(),1); QCOMPARE(backend->writes,0);
    }
    void savedOverrideWithUnknownEditorDefaultCannotOfferRestoreAfterExplicitSet() {
        auto &p=backend->current.profiles[0];
        for (auto key:{PowerPolicyKey::IdleSeconds,PowerPolicyKey::BrightnessPercent}) {
            auto &state=p.keys[key]; state.defaultKnown=false; state.resettable=false; state.defaultReason=state.resetReason=QStringLiteral("Native editor default unavailable.");
        }
        auto &global=backend->current.keys[PowerPolicyKey::BatteryCriticalAction]; global.defaultKnown=false; global.resettable=false;
        session->refresh(); PowerOptionsPage page(nullptr,session,profiles,nullptr);
        session->selectValue("AC",PowerPolicyKey::IdleSeconds,347); session->selectValue("AC",PowerPolicyKey::BrightnessPercent,42);
        session->selectGlobalValue(PowerPolicyKey::BatteryCriticalAction,2u);
        auto *timeout=page.findChild<QComboBox *>("powerSleepTimeout_AC"); QVERIFY(timeout->currentText().contains("staged custom")); QVERIFY(!timeout->currentText().contains("current"));
        for (auto *combo:{timeout,page.findChild<QComboBox *>("powerBatteryCriticalAction")}) {
            const int reset=combo->findText(QStringLiteral("Use system default")); QVERIFY(reset>=0);
            QVERIFY(!(combo->model()->flags(combo->model()->index(reset,0))&Qt::ItemIsEnabled));
            QVERIFY(!combo->findChild<QAction *>("policyRestoreDefault")->isEnabled());
            QMetaObject::invokeMethod(combo,"activated",Q_ARG(int,reset)); QVERIFY(combo->currentText()!=QStringLiteral("Use system default"));
        }
        QVERIFY(!page.findChild<QSpinBox *>("powerBrightnessPercent")->findChild<QAction *>("policyRestoreDefault")->isEnabled());
        reviewCustomTimeout(&page,timeout,[&](QDialog *dialog) {
            QCOMPARE(dialog->findChild<QComboBox *>("powerCustomTimeoutIntent")->findText(QStringLiteral("Use system default")),-1);
            dialog->reject(); });
        QCOMPARE(profile(*session,"AC").suspendSeconds,347); QCOMPARE(profile(*session,"AC").brightnessPercent,42);
        QCOMPARE(session->staged().batteryCriticalAction,2u); QVERIFY(session->errorText().isEmpty()); QCOMPARE(backend->writes,0);
    }
    void absentUnknownBaselineCanCancelItsExplicitSetThroughDefaultControls() {
        auto &state=backend->current.profiles[0].keys[PowerPolicyKey::IdleSeconds];
        state.localPresent=state.effectivePresent=state.valueKnown=state.defaultKnown=state.resettable=false;
        session->refresh(); PowerOptionsPage page(nullptr,session,profiles,nullptr); auto *timeout=page.findChild<QComboBox *>("powerSleepTimeout_AC");
        session->selectValue("AC",PowerPolicyKey::IdleSeconds,347);
        const int reset=timeout->findText(QStringLiteral("Use system default")); QVERIFY(reset>=0);
        QVERIFY(timeout->model()->flags(timeout->model()->index(reset,0))&Qt::ItemIsEnabled);
        QVERIFY(timeout->findChild<QAction *>("policyRestoreDefault")->isEnabled());
        reviewCustomTimeout(&page,timeout,[&](QDialog *dialog) {
            auto *intent=dialog->findChild<QComboBox *>("powerCustomTimeoutIntent"); const int restore=intent->findText(QStringLiteral("Use system default")); QVERIFY(restore>=0);
            intent->setCurrentIndex(restore); dialog->findChild<QPushButton *>("powerCustomTimeoutStage")->click(); });
        QVERIFY(!session->isDirty()); QVERIFY(profile(*session,"AC").intents.isEmpty()); QVERIFY(timeout->currentText().contains("unavailable"));
        session->selectValue("AC",PowerPolicyKey::IdleSeconds,348); timeout->findChild<QAction *>("policyRestoreDefault")->trigger(); QVERIFY(!session->isDirty());
        session->selectValue("AC",PowerPolicyKey::IdleSeconds,349); const int again=timeout->findText(QStringLiteral("Use system default")); QMetaObject::invokeMethod(timeout,"activated",Q_ARG(int,again));
        QVERIFY(!session->isDirty()); QVERIFY(session->errorText().isEmpty()); QCOMPARE(backend->writes,0);
    }
    void unsupportedNativeEditorDefaultIsLabelledAsStagedDefault() {
        auto &state=backend->current.profiles[0].keys[PowerPolicyKey::IdleAction]; state.defaultValue=128u; state.resettable=true; state.resetReason.clear();
        session->refresh(); PowerOptionsPage page(nullptr,session,profiles,nullptr); session->restoreKeyDefault("AC",PowerPolicyKey::IdleAction);
        auto *action=page.findChild<QComboBox *>("powerIdleAction"); QCOMPARE(action->currentData().toUInt(),128u);
        QVERIFY(action->currentText().contains("Native editor default")); QVERIFY(action->currentText().contains("staged")); QVERIFY(!action->currentText().contains("current"));
        QCOMPARE(profile(*session,"AC").intents.value(PowerPolicyKey::IdleAction),PowerPolicyIntent::RestoreDefault); QCOMPARE(backend->writes,0);
    }
    void rejectedRawPercentageEditsResyncAndCannotBePinnedAsSupportedValues() {
        auto &p=backend->current.profiles[0]; p.brightnessPercent=150;
        p.keys[PowerPolicyKey::BrightnessPercent].resettable=true;
        backend->current.batteryLowLevel=140; backend->current.keys[PowerPolicyKey::BatteryLowLevel].resettable=true;
        session->refresh(); PowerOptionsPage page(nullptr,session,profiles,nullptr);
        auto *percent=page.findChild<QSpinBox *>("powerBrightnessPercent");
        auto *low=page.findChild<QSpinBox *>("powerBatteryLowLevel");
        QCOMPARE(percent->value(),150); QCOMPARE(low->value(),140);
        QVERIFY(!percent->findChild<QAction *>("policyExplicitValue")->isEnabled());
        QVERIFY(!low->findChild<QAction *>("policyExplicitValue")->isEnabled());
        finish(percent,149); QCOMPARE(percent->value(),150); QVERIFY(!session->isDirty());
        finish(low,139); QCOMPARE(low->value(),140); QVERIFY(!session->isDirty());
        QVERIFY(page.findChild<QLabel *>("powerPolicyStatus")->text().contains("supported range"));
        finish(percent,45); finish(low,20); QVERIFY(session->isDirty());
        QCOMPARE(profile(*session,"AC").brightnessPercent,45); QCOMPARE(session->staged().batteryLowLevel,20);
        QVERIFY(percent->findChild<QAction *>("policyExplicitValue")->isEnabled());
        QVERIFY(session->applySupportText().isEmpty()); QCOMPARE(backend->writes,0);
    }
    void customSecondsCancelThenStageRetainsHibernateAndEnableState() {
        backend->current.profiles[0].suspendAction = 2; seedPowerPolicyMetadata(backend->current); session->refresh();
        PowerOptionsPage page(nullptr,session,profiles,nullptr); auto *idle = page.findChild<QComboBox *>("powerSleepTimeout_AC");
        reviewCustomTimeout(&page,idle,[&](QDialog *dialog) {
            auto *seconds=dialog->findChild<QSpinBox *>("powerCustomTimeoutSeconds"); QCOMPARE(seconds->minimum(),60); QCOMPARE(seconds->maximum(),PowerPolicySettings::MaximumTimeoutSeconds); seconds->setValue(347);
            dialog->findChild<QPushButton *>("powerCustomTimeoutCancel")->click(); }); QVERIFY(!session->isDirty());
        reviewCustomTimeout(&page,idle,[&](QDialog *dialog) {
            dialog->findChild<QSpinBox *>("powerCustomTimeoutSeconds")->setValue(347); dialog->findChild<QPushButton *>("powerCustomTimeoutStage")->click(); });
        QCOMPARE(profile(*session,"AC").suspendSeconds,347); QCOMPARE(profile(*session,"AC").suspendAction,2u);
        QCOMPARE(profile(*session,"AC").intents.size(),1); QVERIFY(profile(*session,"AC").intents.contains(PowerPolicyKey::IdleSeconds)); QCOMPARE(backend->writes,0);
        reviewCustomTimeout(&page,idle,[&](QDialog *dialog) {
            dialog->findChild<QSpinBox *>("powerCustomTimeoutSeconds")->setValue(PowerPolicySettings::MaximumTimeoutSeconds);
            dialog->findChild<QPushButton *>("powerCustomTimeoutStage")->click(); });
        QCOMPARE(profile(*session,"AC").suspendSeconds,PowerPolicySettings::MaximumTimeoutSeconds); QCOMPARE(profile(*session,"AC").suspendAction,2u); QCOMPARE(backend->writes,0);
    }
    void customSentinelAndDefaultHaveDifferentNativeIntents() {
        auto &p = backend->current.profiles[0]; p.lockedDisplaySeconds = 90; seedPowerPolicyMetadata(backend->current);
        auto &state = p.keys[PowerPolicyKey::LockedDisplaySeconds]; state.defaultValue = 60; state.resettable = true; state.resetReason.clear(); session->refresh();
        PowerOptionsPage page(nullptr,session,profiles,nullptr); auto *locked = page.findChild<QComboBox *>("powerLockedDisplayTimeout");
        reviewCustomTimeout(&page,locked,[&](QDialog *dialog) {
            auto *intent=dialog->findChild<QComboBox *>("powerCustomTimeoutIntent"); intent->setCurrentIndex(intent->findData(100));
            QVERIFY(!dialog->findChild<QSpinBox *>("powerCustomTimeoutSeconds")->isEnabled()); dialog->findChild<QPushButton *>("powerCustomTimeoutStage")->click(); });
        QCOMPARE(profile(*session,"AC").lockedDisplaySeconds,-2); QCOMPARE(profile(*session,"AC").intents.value(PowerPolicyKey::LockedDisplaySeconds),PowerPolicyIntent::Set);
        const int index=locked->findText(QStringLiteral("Use system default")); QMetaObject::invokeMethod(locked,"activated",Q_ARG(int,index));
        QCOMPARE(profile(*session,"AC").lockedDisplaySeconds,60); QCOMPARE(profile(*session,"AC").intents.value(PowerPolicyKey::LockedDisplaySeconds),PowerPolicyIntent::RestoreDefault); QCOMPARE(backend->writes,0);
    }
    void customTimeoutDialogClosesSafelyWhenItsPageIsDestroyed() {
        QPointer<PowerOptionsPage> page=new PowerOptionsPage(nullptr,session,profiles,nullptr); QPointer<QDialog> dialog;
        auto *combo=page->findChild<QComboBox *>("powerSleepTimeout_AC");
        reviewCustomTimeout(page,combo,[&](QDialog *opened) { dialog=opened; delete page.data(); });
        QVERIFY(page.isNull()); QVERIFY(dialog.isNull()); QVERIFY(!session->isDirty()); QCOMPARE(backend->writes,0); QCOMPARE(ppd->writes,0);
    }
    void defaultsReviewCancelsAndScopesResetWithoutApplying() {
        for (auto &p : backend->current.profiles) { auto &state=p.keys[PowerPolicyKey::DisplaySeconds]; state.defaultValue=300; state.resettable=true; state.resetReason.clear(); }
        auto &global=backend->current.keys[PowerPolicyKey::BatteryLowLevel]; global.defaultValue=20; global.resettable=true; global.resetReason.clear(); session->refresh();
        PowerOptionsPage page(nullptr,session,profiles,nullptr);
        QTimer::singleShot(0,&page,[&] { auto *dialog=page.findChild<QDialog *>("powerDefaultsDialog"); QVERIFY(dialog); QVERIFY(dialog->findChild<QPushButton *>("powerDefaultsStage")->isEnabled()); dialog->reject(); });
        page.findChild<QPushButton *>("powerPolicyDefaults")->click(); QVERIFY(!session->isDirty());
        QTimer::singleShot(0,&page,[&] { auto *dialog=page.findChild<QDialog *>("powerDefaultsDialog"); QVERIFY(dialog);
            auto *scope=dialog->findChild<QComboBox *>("powerDefaultsScope"); scope->setCurrentIndex(scope->findData(int(PowerPolicySettings::DefaultsScope::Battery)));
            auto *rows=dialog->findChild<QTreeWidget *>("powerDefaultsReview"); QVERIFY(rows->topLevelItemCount()>0); dialog->findChild<QPushButton *>("powerDefaultsStage")->click(); });
        page.findChild<QPushButton *>("powerPolicyDefaults")->click(); QCOMPARE(session->staged().batteryLowLevel,20);
        QCOMPARE(session->staged().intents.value(PowerPolicyKey::BatteryLowLevel),PowerPolicyIntent::RestoreDefault);
        for (const auto &p:session->staged().profiles) QVERIFY(p.intents.isEmpty());
        QTimer::singleShot(0,&page,[&] { auto *dialog=page.findChild<QDialog *>("powerDefaultsDialog"); QVERIFY(dialog);
            auto *source=dialog->findChild<QComboBox *>("powerDefaultsSource"); source->setCurrentIndex(source->findData(QStringLiteral("Battery"))); dialog->findChild<QPushButton *>("powerDefaultsStage")->click(); });
        page.findChild<QPushButton *>("powerPolicyDefaults")->click(); QCOMPARE(profile(*session,"Battery").displaySeconds,300);
        QVERIFY(profile(*session,"AC").intents.isEmpty()); QVERIFY(profile(*session,"LowBattery").intents.isEmpty()); QCOMPARE(backend->writes,0);
    }
    void changedDraftInvalidatesDefaultsReviewUntilExplicitUpdate() {
        auto &state=backend->current.profiles[0].keys[PowerPolicyKey::DisplaySeconds]; state.defaultValue=300; state.resettable=true; session->refresh();
        PowerOptionsPage page(nullptr,session,profiles,nullptr);
        QTimer::singleShot(0,&page,[&] { auto *dialog=page.findChild<QDialog *>("powerDefaultsDialog"); QVERIFY(dialog);
            auto *stage=dialog->findChild<QPushButton *>("powerDefaultsStage"); QVERIFY(stage->isEnabled()); session->selectValue("AC",PowerPolicyKey::IdleSeconds,1200);
            QVERIFY(!stage->isEnabled()); QVERIFY(dialog->findChild<QLabel *>("powerDefaultsStatus")->text().contains("Update review"));
            QCOMPARE(profile(*session,"AC").displaySeconds,600); dialog->findChild<QPushButton *>("powerDefaultsUpdateReview")->click(); QVERIFY(stage->isEnabled()); stage->click(); });
        page.findChild<QPushButton *>("powerPolicyDefaults")->click(); QCOMPARE(profile(*session,"AC").displaySeconds,300);
        QCOMPARE(profile(*session,"AC").suspendSeconds,900); QVERIFY(!profile(*session,"AC").intents.contains(PowerPolicyKey::IdleSeconds)); QCOMPARE(backend->writes,0);
    }
    void keyboardAccessAndStatusesUseHumanLabelsAndPlainText() {
        PowerOptionsPage page(nullptr, session, profiles, nullptr);
        for (const auto &name : {"powerPolicySource", "powerIdleAction", "powerDimTimeout", "powerLockedDisplayTimeout", "powerButtonAction", "powerDownAction", "powerLidAction", "powerSleepMode", "powerBatteryLowLevel", "powerBatteryCriticalLevel", "powerBatteryCriticalAction"}) {
            auto *control = page.findChild<QWidget *>(name); QVERIFY(control); QVERIFY(!control->accessibleName().isEmpty());
            bool buddy = false; for (auto *label : page.findChildren<QLabel *>()) buddy = buddy || label->buddy() == control; QVERIFY(buddy);
        }
        QVERIFY(page.findChild<QComboBox *>("powerDownAction")->accessibleName().contains("Long power-button"));
        QCOMPARE(page.findChild<QLabel *>("powerPolicyStatus")->textFormat(), Qt::PlainText); QCOMPARE(page.findChild<QLabel *>("powerAdvancedReason")->textFormat(), Qt::PlainText);
        QCOMPARE(backend->writes, 0); QCOMPARE(ppd->writes, 0);
    }
private:
    PagePolicyBackend *backend = nullptr;
    PowerPolicySettings *session = nullptr;
    PageProfilesBackend *ppd = nullptr;
    PowerProfileService *profiles = nullptr;
};
QTEST_MAIN(PowerPolicyPageTest)
#include "power_policy_page_test.moc"
