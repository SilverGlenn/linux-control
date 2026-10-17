#include <QtTest>
#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLabel>
#include <QKeySequence>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QSignalSpy>
#include <QStandardItemModel>
#include <QTimer>
#include <limits>
#include <cmath>
#include "screen_lock_fixture.h"
#include "ui/pages/ScreenLockPage.h"

namespace {
constexpr int Custom=-100;
ScreenLockAppearance::Evidence appearance()
{ ScreenLockAppearance::Evidence e; e.approvedIdentity=e.providerVerified=e.backgroundAvailable=true; e.description=QStringLiteral("Fake approved Aero evidence"); e.backgroundPath=QStringLiteral("/fake/shared/background.png"); e.reason=QStringLiteral("Read-only fake evidence; no privileged atomic writer."); return e; }
void activate(QComboBox *combo,int value)
{ QVERIFY(combo); const int index=combo->findData(value); QVERIFY(index>=0); combo->setCurrentIndex(index); QVERIFY(QMetaObject::invokeMethod(combo,"activated",Q_ARG(int,index))); if (value!=Custom) QCoreApplication::processEvents(); }
bool enabledChoice(QComboBox *combo,int value)
{ auto *model=qobject_cast<QStandardItemModel *>(combo->model()); const int index=combo->findData(value); return model && index>=0 && model->item(index)->isEnabled(); }
void accept(QDialog *dialog)
{ QVERIFY(dialog); dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click(); }
}
class PageScreenLockBackend final : public ScreenLockBackend {
public:
    ScreenLockSnapshot current=screenLockFixture(),expected,requested; int reads=0,writes=0; quint64 readToken=0,writeToken=0; bool hold=false;
    void readCurrent(quint64 token) override { ++reads; readToken=token; if (!hold) emit readFinished(token,true,current,{}); }
    void writeChanges(quint64 token,const ScreenLockSnapshot &before,const ScreenLockSnapshot &after) override { ++writes; writeToken=token; expected=before; requested=after; }
    void native(ScreenLockKey key,const QVariant &value) { auto &f=current.fields[key]; f.value=value; f.known=true; f.defaultNoOp=f.value==f.defaultValue; f.resettable=f.writable && !f.defaultNoOp; }
    void changed() { emit externalChange(); }
    void finishRead() { emit readFinished(readToken,true,current,{}); }
    void failRead() { emit readFinished(readToken,false,{},QStringLiteral("Fake read failed")); }
    void complete(bool acknowledged=true) { current=savedScreenLockFixture(requested); emit writeFinished(writeToken,true,acknowledged,{}); }
};
class ScreenLockPageTest final : public QObject {
    Q_OBJECT
private slots:
    void init() { backend=new PageScreenLockBackend; session=new ScreenLockSettings(backend); QTRY_VERIFY(session->isReady()); }
    void cleanup() { delete session; }
    void injectedViewIsPassiveAccessibleAndAppearanceIsReadOnly() {
        ScreenLockPage page(nullptr,session,appearance(),nullptr); QCOMPARE(page.objectName(),QStringLiteral("win7Page")); QCOMPARE(page.settingsSession(),static_cast<SettingsSession *>(session));
        for (auto name:{"screenLockTimeout","screenLockGrace"}) { auto *control=page.findChild<QWidget *>(name); QVERIFY(control); QVERIFY(!control->accessibleName().isEmpty()); bool buddy=false; for (auto *label:page.findChildren<QLabel *>()) buddy|=label->buddy()==control; QVERIFY(buddy); }
        for (auto name:{"screenLockSaved","screenLockChanges","screenLockEffect","screenLockAppearance","screenLockMessage"}) QCOMPARE(page.findChild<QLabel *>(name)->textFormat(),Qt::PlainText);
        QVERIFY(page.findChild<QLabel *>("screenLockAppearance")->text().contains("/fake/shared/background.png")); QVERIFY(page.findChild<QLabel *>("screenLockAppearance")->text().contains("reviewed privileged transaction"));
        QSignalSpy appearanceRoute(&page,&ScreenLockPage::appearanceRequested); page.findChild<QPushButton *>("screenLockEditAppearance")->click(); QCOMPARE(appearanceRoute.count(),1);
        QVERIFY(!session->isDirty()); QCOMPARE(backend->writes,0); QVERIFY(!page.findChild<QPushButton *>("screenLockApply")->isEnabled()); QCOMPARE(page.findChildren<QCheckBox *>().size(),1); QVERIFY(!ScreenLockPage::sidebarLinks().isEmpty());
    }
    void technicalDetailsStartCollapsedAndKeyboardToggleNeverHidesWarnings() {
        ScreenLockPage page(nullptr,session,appearance(),nullptr); page.show(); page.activateWindow(); QTest::qWait(1);
        auto *details=page.findChild<QWidget *>("screenLockDetails"); auto *toggle=page.findChild<QPushButton *>("screenLockDetailsToggle"); auto *saved=page.findChild<QLabel *>("screenLockSaved");
        QVERIFY(details->isHidden()); QVERIFY(!saved->isVisible()); QVERIFY(page.findChild<QLabel *>("screenLockAppearanceSummary")->isVisible()); QVERIFY(page.findChild<QLabel *>("screenLockEffect")->isHidden()); QVERIFY(page.findChild<QLabel *>("screenLockChanges")->isHidden()); QCOMPARE(toggle->shortcut(),QKeySequence(QStringLiteral("Alt+D")));
        QTest::keyClick(&page,Qt::Key_D,Qt::AltModifier); QTRY_VERIFY(!details->isHidden()); QVERIFY(saved->isVisible()); QVERIFY(page.findChild<QLabel *>("screenLockConfiguration")->text().contains("Physical locking and authentication are not tested"));
        toggle->setFocus(); QTest::keyClick(toggle,Qt::Key_Space); QVERIFY(details->isHidden());
        backend->native(ScreenLockKey::RequirePassword,false); backend->native(ScreenLockKey::LockGrace,-1); backend->native(ScreenLockKey::Lock,false); backend->native(ScreenLockKey::Timeout,3.25); session->refresh();
        QVERIFY(details->isHidden()); QVERIFY(page.findChild<QLabel *>("screenLockGraceDescription")->isVisible()); QVERIFY(page.findChild<QLabel *>("screenLockGraceDescription")->text().contains("other screen-lock entry points")); QVERIFY(page.findChild<QLabel *>("screenLockGraceDescription")->text().contains("indefinite")); QVERIFY(page.findChild<QLabel *>("screenLockTimeoutDescription")->isVisible()); QVERIFY(page.findChild<QLabel *>("screenLockEffect")->isVisible());
        backend->current.fields[ScreenLockKey::Lock].known=false; session->refresh(); QVERIFY(page.findChild<QLabel *>("screenLockEffect")->isVisible()); QVERIFY(page.findChild<QLabel *>("screenLockMessage")->isVisible()); QVERIFY(!saved->isVisible());
        toggle->click(); ScreenLockPage reopened(nullptr,session,appearance(),nullptr); QVERIFY(!reopened.findChild<QWidget *>("screenLockDetails")->isHidden()); QCOMPARE(backend->writes,0);
    }
    void neverDisplayPreservesDisabledRawTimerUntilExplicitChoice() {
        backend->native(ScreenLockKey::Autolock,false); backend->native(ScreenLockKey::Timeout,12.5); session->refresh(); ScreenLockPage page(nullptr,session,appearance(),nullptr);
        auto *timeout=page.findChild<QComboBox *>("screenLockTimeout"); QCOMPARE(timeout->currentData().toInt(),0); QVERIFY(page.findChild<QLabel *>("screenLockTimeoutDescription")->text().contains("12.5 minutes")); QVERIFY(!page.findChild<QComboBox *>("screenLockGrace")->isEnabled()); QVERIFY(!session->isDirty());
        activate(timeout,0); QCOMPARE(session->staged().fields[ScreenLockKey::Timeout].value.toDouble(),0.0); QVERIFY(!session->staged().fields[ScreenLockKey::Autolock].value.toBool()); QVERIFY(!session->staged().intents.contains(ScreenLockKey::Lock)); QCOMPARE(backend->writes,0);
    }
    void positiveTimeoutAndPasswordIntervalStageTheirCompanionsAtomically() {
        backend->native(ScreenLockKey::Autolock,false); backend->native(ScreenLockKey::RequirePassword,false); session->refresh(); ScreenLockPage page(nullptr,session,appearance(),nullptr);
        activate(page.findChild<QComboBox *>("screenLockTimeout"),10); QCOMPARE(session->staged().fields[ScreenLockKey::Timeout].value.metaType().id(),int(QMetaType::Double)); QCOMPARE(session->staged().fields[ScreenLockKey::Timeout].value.toDouble(),10.0); QVERIFY(session->staged().fields[ScreenLockKey::Autolock].value.toBool());
        activate(page.findChild<QComboBox *>("screenLockGrace"),300); QCOMPARE(session->staged().fields[ScreenLockKey::LockGrace].value.toInt(),300); QVERIFY(session->staged().fields[ScreenLockKey::RequirePassword].value.toBool()); QCOMPARE(session->staged().intents.size(),4); QCOMPARE(backend->writes,0);
    }
    void protectedMatchingCompanionDoesNotBlockItsWritableTimer() {
        backend->current.fields[ScreenLockKey::Autolock].immutable=true; backend->current.fields[ScreenLockKey::Autolock].writable=false; session->refresh(); ScreenLockPage page(nullptr,session,appearance(),nullptr); auto *timeout=page.findChild<QComboBox *>("screenLockTimeout");
        QVERIFY(enabledChoice(timeout,10)); QVERIFY(!enabledChoice(timeout,0)); activate(timeout,10); QCOMPARE(session->staged().fields[ScreenLockKey::Timeout].value.toDouble(),10.0); QVERIFY(!session->staged().intents.contains(ScreenLockKey::Autolock));
        session->revert(); backend->native(ScreenLockKey::Autolock,false); session->refresh(); QVERIFY(!enabledChoice(timeout,10)); activate(timeout,10); QVERIFY(!session->isDirty()); QCOMPARE(backend->writes,0);
    }
    void noPasswordOnlyStagesTheRequirementAndDisclosesOtherLocks() {
        backend->native(ScreenLockKey::LockGrace,91); session->refresh(); ScreenLockPage page(nullptr,session,appearance(),nullptr); activate(page.findChild<QComboBox *>("screenLockGrace"),-1);
        QVERIFY(!session->staged().fields[ScreenLockKey::RequirePassword].value.toBool()); QCOMPARE(session->staged().fields[ScreenLockKey::LockGrace].value.toInt(),91); QCOMPARE(session->staged().intents.size(),1); QVERIFY(page.findChild<QLabel *>("screenLockGraceDescription")->text().contains("other screen-lock entry points")); QCOMPARE(backend->writes,0);
    }
    void rawFractionalTimeoutAndNegativeGraceRemainTruthfulAndUnchanged() {
        backend->native(ScreenLockKey::Timeout,3.25); backend->native(ScreenLockKey::LockGrace,-1); backend->native(ScreenLockKey::Lock,false); session->refresh(); ScreenLockPage page(nullptr,session,appearance(),nullptr); page.show();
        auto *timeout=page.findChild<QComboBox *>("screenLockTimeout"),*grace=page.findChild<QComboBox *>("screenLockGrace"); QVERIFY(timeout->currentText().contains("3.25")); QVERIFY(grace->currentText().contains("-1 seconds")); QVERIFY(grace->currentText()!=QStringLiteral("No password required"));
        QVERIFY(page.findChild<QLabel *>("screenLockGraceDescription")->text().contains("indefinite")); QVERIFY(page.findChild<QLabel *>("screenLockEffect")->text().contains("without a protected lock")); timeout->setFocus(); grace->setFocus(); QCOMPARE(session->staged().fields[ScreenLockKey::Timeout].value.toDouble(),3.25); QCOMPARE(session->staged().fields[ScreenLockKey::LockGrace].value.toInt(),-1); QVERIFY(!session->isDirty()); QCOMPARE(backend->writes,0);
    }
    void nonfiniteDormantTimeoutIsNotPresentedAsAPositiveTimer() {
        backend->native(ScreenLockKey::Autolock,false); backend->native(ScreenLockKey::Timeout,std::numeric_limits<double>::quiet_NaN()); session->refresh(); ScreenLockPage page(nullptr,session,appearance(),nullptr);
        QCOMPARE(page.findChild<QComboBox *>("screenLockTimeout")->currentData().toInt(),0); QVERIFY(page.findChild<QLabel *>("screenLockTimeoutDescription")->text().contains("outside the whole-minute")); QVERIFY(std::isnan(session->staged().fields[ScreenLockKey::Timeout].value.toDouble())); QVERIFY(!session->isDirty()); QCOMPARE(backend->writes,0);
    }
    void unknownResumeAndLegacyPolicyAreNotInvented() {
        backend->current.fields[ScreenLockKey::LockOnResume].known=false; backend->current.fields[ScreenLockKey::Lock].known=false; session->refresh(); ScreenLockPage page(nullptr,session,appearance(),nullptr);
        auto *resume=page.findChild<QCheckBox *>("screenLockResume"); QCOMPARE(resume->checkState(),Qt::PartiallyChecked); QVERIFY(page.findChild<QLabel *>("screenLockEffect")->text().contains("unavailable")); resume->click(); QVERIFY(session->staged().fields[ScreenLockKey::LockOnResume].known); QVERIFY(!session->staged().intents.contains(ScreenLockKey::Lock)); QCOMPARE(backend->writes,0);
    }
    void shortcutButtonOnlyEmitsNavigationIntent() {
        ScreenLockPage page(nullptr,session,appearance(),nullptr); QSignalSpy navigation(&page,&ScreenLockPage::editLockShortcutRequested); page.findChild<QPushButton *>("screenLockEditShortcut")->click(); QCOMPARE(navigation.size(),1); QVERIFY(!session->isDirty()); QCOMPARE(backend->writes,0);
    }
    void customTimeoutCancelPreservesFractionAndAcceptStagesWholeMaximum() {
        backend->native(ScreenLockKey::Timeout,2.125); session->refresh(); ScreenLockPage page(nullptr,session,appearance(),nullptr); auto *combo=page.findChild<QComboBox *>("screenLockTimeout");
        bool canceled=false; activate(combo,Custom); QTimer::singleShot(0,&page,[&page,&canceled]{ auto *dialog=page.findChild<QDialog *>("screenLockCustomDialog"); QVERIFY(dialog); QVERIFY(dialog->findChild<QLabel *>("screenLockCustomCurrent")->text().contains("2.125 minutes")); canceled=true; dialog->reject(); }); QTRY_VERIFY(canceled); QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
        QVERIFY(!session->isDirty()); QCOMPARE(session->staged().fields[ScreenLockKey::Timeout].value.toDouble(),2.125);
        activate(combo,Custom); QTimer::singleShot(0,&page,[&page]{ auto *dialog=page.findChild<QDialog *>("screenLockCustomDialog"); QVERIFY(dialog); dialog->findChild<QLineEdit *>("screenLockCustomValue")->setText(QStringLiteral("9999")); accept(dialog); }); QTRY_COMPARE(session->staged().fields[ScreenLockKey::Timeout].value.toDouble(),9999.0); QCOMPARE(backend->writes,0);
    }
    void customGraceSupportsMinutesAndStrictInvalidTextDoesNotStage() {
        ScreenLockPage page(nullptr,session,appearance(),nullptr); activate(page.findChild<QComboBox *>("screenLockGrace"),Custom);
        QTimer::singleShot(0,&page,[&page]{ auto *dialog=page.findChild<QDialog *>("screenLockCustomDialog"); QVERIFY(dialog); auto *line=dialog->findChild<QLineEdit *>("screenLockCustomValue"); line->setText(QStringLiteral("-")); accept(dialog); QVERIFY(dialog->isVisible()); QVERIFY(dialog->findChild<QLabel *>("screenLockCustomError")->text().contains("No preference")); line->setText(QStringLiteral("9999")); auto *unit=dialog->findChild<QComboBox *>("screenLockCustomUnit"); unit->setCurrentIndex(unit->findData(60)); accept(dialog); });
        QTRY_COMPARE(session->staged().fields[ScreenLockKey::LockGrace].value.toInt(),599940); QCOMPARE(backend->writes,0);
    }
    void customDialogPreservesLocaleTextCursorAndSelectionAcrossRefresh() {
        ScreenLockPage page(nullptr,session,appearance(),nullptr); activate(page.findChild<QComboBox *>("screenLockTimeout"),Custom);
        QTimer::singleShot(0,&page,[&page,this]{ auto *dialog=page.findChild<QDialog *>("screenLockCustomDialog"); QVERIFY(dialog); auto *line=dialog->findChild<QLineEdit *>("screenLockCustomValue"); line->setLocale(QLocale(QLocale::German,QLocale::Germany)); line->setText(QStringLiteral("1.234")); line->setSelection(4,-2); const int cursor=line->cursorPosition(),start=line->selectionStart(); const QString selected=line->selectedText(); backend->hold=true; session->refresh(); QCOMPARE(line->text(),QStringLiteral("1.234")); QCOMPARE(line->cursorPosition(),cursor); backend->finishRead(); QCOMPARE(line->selectionStart(),start); QCOMPARE(line->selectedText(),selected); accept(dialog); });
        QTRY_COMPARE(session->staged().fields[ScreenLockKey::Timeout].value.toDouble(),1234.0); QCOMPARE(backend->writes,0);
    }
    void staleCustomIntervalCannotCrossProviderOrDraftChanges() {
        ScreenLockPage page(nullptr,session,appearance(),nullptr); activate(page.findChild<QComboBox *>("screenLockTimeout"),Custom);
        QTimer::singleShot(0,&page,[&page,this]{ auto *dialog=page.findChild<QDialog *>("screenLockCustomDialog"); QVERIFY(dialog); dialog->findChild<QLineEdit *>("screenLockCustomValue")->setText(QStringLiteral("22")); backend->current.runtime.epoch++; backend->changed(); accept(dialog); });
        QTRY_VERIFY(page.findChild<QLabel *>("screenLockMessage")->text().contains("22 minutes")); QVERIFY(!session->isDirty()); QCOMPARE(backend->writes,0);
    }
    void queuedDialogAndOpenDialogSurviveViewDestructionSafely() {
        auto *page=new ScreenLockPage(nullptr,session,appearance(),nullptr); activate(page->findChild<QComboBox *>("screenLockTimeout"),Custom); delete page; QCoreApplication::processEvents(); QVERIFY(!session->isDirty());
        page=new ScreenLockPage(nullptr,session,appearance(),nullptr); QPointer<ScreenLockPage> guard(page); activate(page->findChild<QComboBox *>("screenLockTimeout"),Custom); QTimer::singleShot(0,[page]{ delete page; }); QTRY_VERIFY(!guard); QVERIFY(!session->isDirty()); QCOMPARE(backend->writes,0);
    }
    void presetStagingCanDestroyTheViewDuringStateChanged() {
        auto *page=new ScreenLockPage(nullptr,session,appearance(),nullptr); QPointer<ScreenLockPage> guard(page);
        auto connection=connect(session,&SettingsSession::stateChanged,session,[page]{ delete page; },Qt::SingleShotConnection);
        activate(page->findChild<QComboBox *>("screenLockTimeout"),15); QVERIFY(!guard); QCOMPARE(session->staged().fields[ScreenLockKey::Timeout].value.toDouble(),15.0); disconnect(connection);
        session->revert(); page=new ScreenLockPage(nullptr,session,appearance(),nullptr); guard=page;
        connection=connect(session,&SettingsSession::stateChanged,session,[page]{ delete page; },Qt::SingleShotConnection);
        activate(page->findChild<QComboBox *>("screenLockGrace"),300); QVERIFY(!guard); QCOMPARE(session->staged().fields[ScreenLockKey::LockGrace].value.toInt(),300); disconnect(connection); QCOMPARE(backend->writes,0);
    }
    void queuedPresetRejectsAnInterveningSavedOrDraftChange() {
        ScreenLockPage page(nullptr,session,appearance(),nullptr); auto *combo=page.findChild<QComboBox *>("screenLockTimeout"); const int index=combo->findData(15); combo->setCurrentIndex(index); QVERIFY(QMetaObject::invokeMethod(combo,"activated",Q_ARG(int,index))); QVERIFY(!session->isDirty());
        backend->native(ScreenLockKey::Timeout,2.0); backend->changed(); QCoreApplication::processEvents(); QCOMPARE(session->staged().fields[ScreenLockKey::Timeout].value.toDouble(),2.0); QVERIFY(!session->isDirty()); QVERIFY(page.findChild<QLabel *>("screenLockMessage")->text().contains("was not staged"));
        combo->setCurrentIndex(index); QVERIFY(QMetaObject::invokeMethod(combo,"activated",Q_ARG(int,index))); session->selectGrace(300); QCoreApplication::processEvents(); QCOMPARE(session->staged().fields[ScreenLockKey::Timeout].value.toDouble(),2.0); QCOMPARE(session->staged().fields[ScreenLockKey::LockGrace].value.toInt(),300); QCOMPARE(backend->writes,0);
    }
    void customStagingCanDestroyTheViewDuringStateChanged() {
        auto *page=new ScreenLockPage(nullptr,session,appearance(),nullptr); QPointer<ScreenLockPage> guard(page);
        connect(session,&SettingsSession::stateChanged,session,[page]{ delete page; },Qt::SingleShotConnection);
        activate(page->findChild<QComboBox *>("screenLockTimeout"),Custom);
        QTimer::singleShot(0,[page]{ auto *dialog=page->findChild<QDialog *>("screenLockCustomDialog"); QVERIFY(dialog); dialog->findChild<QLineEdit *>("screenLockCustomValue")->setText(QStringLiteral("22")); accept(dialog); });
        QTRY_VERIFY(!guard); QCOMPARE(session->staged().fields[ScreenLockKey::Timeout].value.toDouble(),22.0); QCOMPARE(backend->writes,0);
    }
    void defaultStagingCanDestroyTheViewDuringStateChanged() {
        backend->native(ScreenLockKey::Timeout,15.0); session->refresh(); auto *page=new ScreenLockPage(nullptr,session,appearance(),nullptr); QPointer<ScreenLockPage> guard(page);
        connect(session,&SettingsSession::stateChanged,session,[page]{ delete page; },Qt::SingleShotConnection);
        QTimer::singleShot(0,[page]{ auto *dialog=page->findChild<QDialog *>("screenLockDefaultsDialog"); QVERIFY(dialog); accept(dialog); }); page->findChild<QPushButton *>("screenLockDefaults")->click();
        QVERIFY(!guard); QCOMPARE(session->staged().fields[ScreenLockKey::Timeout].value.toDouble(),session->applied().fields[ScreenLockKey::Timeout].defaultValue.toDouble()); QCOMPARE(backend->writes,0);
    }
    void defaultsAreReviewedCanceledAndOnlyStageOwnedPreferences() {
        backend->native(ScreenLockKey::Timeout,15.0); backend->native(ScreenLockKey::LockGrace,60); session->refresh(); ScreenLockPage page(nullptr,session,appearance(),nullptr); const auto lock=session->staged().fields[ScreenLockKey::Lock],theme=session->staged().fields[ScreenLockKey::Theme];
        QTimer::singleShot(0,&page,[&page]{ auto *dialog=page.findChild<QDialog *>("screenLockDefaultsDialog"); QVERIFY(dialog); dialog->reject(); }); page.findChild<QPushButton *>("screenLockDefaults")->click(); QVERIFY(!session->isDirty());
        QTimer::singleShot(0,&page,[&page]{ auto *dialog=page.findChild<QDialog *>("screenLockDefaultsDialog"); QVERIFY(dialog); accept(dialog); }); page.findChild<QPushButton *>("screenLockDefaults")->click(); QVERIFY(session->isDirty()); QVERIFY(session->staged().fields[ScreenLockKey::Lock]==lock); QVERIFY(session->staged().fields[ScreenLockKey::Theme]==theme); for (auto key:session->staged().intents.keys()) QVERIFY(ScreenLockTypes::owned(key)); QCOMPARE(backend->writes,0);
    }
    void defaultsReviewIdentifiesGlobalAndInheritedWriteDestinations() {
        backend->native(ScreenLockKey::Timeout,15.0); auto &timer=backend->current.fields[ScreenLockKey::Timeout]; timer.localPresent=false; timer.globalPresent=true;
        backend->native(ScreenLockKey::LockGrace,60); auto &grace=backend->current.fields[ScreenLockKey::LockGrace]; grace.inheritedPresent=true; session->refresh(); ScreenLockPage page(nullptr,session,appearance(),nullptr);
        QTimer::singleShot(0,&page,[&page]{ auto *dialog=page.findChild<QDialog *>("screenLockDefaultsDialog"); QVERIFY(dialog); QString text; for (auto *label:dialog->findChildren<QLabel *>()) text+=label->text(); QVERIFY(text.contains("global-only entry in user kdeglobals")); QVERIFY(text.contains("typed local value in user kscreenlockerrc")); QVERIFY(text.contains("Saved source:")); dialog->reject(); }); page.findChild<QPushButton *>("screenLockDefaults")->click(); QVERIFY(!session->isDirty()); QCOMPARE(backend->writes,0);
        QVERIFY(page.findChild<QLabel *>("screenLockAppearance")->text().contains("evidence read when this page opened"));
    }
    void defaultsAreDisabledAfterFailureAndConflictAndRejectAStaleReview() {
        backend->native(ScreenLockKey::Timeout,15.0); session->refresh(); ScreenLockPage page(nullptr,session,appearance(),nullptr); auto *defaults=page.findChild<QPushButton *>("screenLockDefaults"); QVERIFY(defaults->isEnabled());
        backend->hold=true; session->refresh(); backend->failRead(); QVERIFY(!defaults->isEnabled()); for (auto *action:page.findChildren<QAction *>()) if (action->objectName()==QStringLiteral("screenLockFieldDefault")) QVERIFY(!action->isEnabled());
        backend->hold=false; session->revert(); QTimer::singleShot(0,&page,[&page,this]{ auto *dialog=page.findChild<QDialog *>("screenLockDefaultsDialog"); QVERIFY(dialog); session->selectTimeout(30); accept(dialog); }); defaults->click(); QCOMPARE(session->staged().fields[ScreenLockKey::Timeout].value.toDouble(),30.0); QVERIFY(page.findChild<QLabel *>("screenLockMessage")->text().contains("stale"));
        backend->native(ScreenLockKey::LockGrace,60); backend->changed(); QVERIFY(session->hasConflict()); QVERIFY(!defaults->isEnabled()); QCOMPARE(backend->writes,0);
    }
    void draftsPersistAcrossViewsAndApplyIsExplicit() {
        { ScreenLockPage page(nullptr,session,appearance(),nullptr); activate(page.findChild<QComboBox *>("screenLockTimeout"),15); } ScreenLockPage reopened(nullptr,session,appearance(),nullptr); QCOMPARE(reopened.findChild<QComboBox *>("screenLockTimeout")->currentData().toInt(),15); QCOMPARE(backend->writes,0);
        reopened.findChild<QPushButton *>("screenLockApply")->click(); QCOMPARE(backend->writes,1); backend->complete(); QVERIFY(!session->isDirty()); QVERIFY(!reopened.findChild<QLabel *>("screenLockMessage")->text().isEmpty()); QCOMPARE(backend->writes,1);
    }
private:
    PageScreenLockBackend *backend=nullptr; ScreenLockSettings *session=nullptr;
};
QTEST_MAIN(ScreenLockPageTest)
#include "screen_lock_page_test.moc"
