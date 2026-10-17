#include <QtTest>
#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPointer>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QTimeZone>
#include <limits>
#include "settings/NightLightSettings.h"
#include "ui/pages/NightLightPage.h"

namespace {
NightLightSnapshot fixture()
{
    NightLightSnapshot snapshot; snapshot.revision="fake-config-1";
    for (auto key:NightLightTypes::keys()) {
        NightLightField field; field.value=field.defaultValue=NightLightTypes::builtinDefault(key);
        field.known=field.defaultKnown=field.writable=field.effectivePresent=field.localPresent=field.defaultNoOp=true;
        field.revision=QByteArray::number(int(key))+"-saved"; field.inheritedRevision="no-inherited"; field.globalRevision="no-global";
        snapshot.fields.insert(key,field);
    }
    auto &active=snapshot.fields[NightLightKey::Active]; active.value=true; active.defaultNoOp=false; active.resettable=true;
    auto &runtime=snapshot.runtime; runtime.owner=QStringLiteral(":fake-night-light"); runtime.epoch=1; runtime.version=QStringLiteral("fake-supported");
    runtime.serviceAvailable=runtime.schemaSupported=runtime.known=runtime.available=runtime.previewSupported=true;
    runtime.enabled=runtime.running=true; runtime.daylight=false; runtime.mode=NightLightTypes::DarkLightMode;
    runtime.currentTemperature=runtime.targetTemperature=4500;
    runtime.previousTransitionDateTime=1700000000; runtime.previousTransitionDuration=1500;
    runtime.scheduledTransitionDateTime=1700003600; runtime.scheduledTransitionDuration=120000;
    return snapshot;
}
void activate(QComboBox *combo,int value)
{
    QVERIFY(combo); const int index=combo->findData(value); QVERIFY(index>=0); combo->setCurrentIndex(index);
    QVERIFY(QMetaObject::invokeMethod(combo,"activated",Q_ARG(int,index)));
}
void edit(QSpinBox *spin,int value)
{ QVERIFY(spin); spin->setValue(value); QVERIFY(QMetaObject::invokeMethod(spin,"editingFinished")); }
void typeTemperature(QSpinBox *spin,const QString &text)
{
    auto *line=spin->findChild<QLineEdit *>(); QVERIFY(line); line->setText(text); line->setCursorPosition(text.size());
    QVERIFY(QMetaObject::invokeMethod(line,"textEdited",Q_ARG(QString,text)));
}

}
class PageNightLightBackend final : public NightLightBackend {
public:
    NightLightSnapshot current=fixture(),expected,requested;
    int reads=0,writes=0,previews=0,stops=0,temperature=0,saves=0;
    quint64 writeToken=0,previewToken=0,readToken=0,previewEpoch=0,stopEpoch=0;
    QString previewOwner,stopOwner;
    bool holdRead=false,holdPreview=false,failNextStop=false;
    void readCurrent(quint64 token) override { ++reads; readToken=token; if (!holdRead) emit readFinished(token,true,current,{}); }
    void writeChanges(quint64 token,const NightLightSnapshot &before,const NightLightSnapshot &after) override
    { ++writes; writeToken=token; expected=before; requested=after; }
    void preview(quint64 token,int value,const QString &owner,quint64 epoch) override
    { previewEpoch=epoch; ++previews; previewToken=token; temperature=value; previewOwner=owner; if (!holdPreview) emit previewFinished(token,true,false,{}); }
    void stopPreview(quint64 token,const QString &owner,quint64 epoch) override
    {
        stopEpoch=epoch; ++stops; stopOwner=owner;
        if (failNextStop) { failNextStop=false; emit previewFinished(token,false,false,QStringLiteral("Stop request failed; preview may remain active.")); }
        else emit previewFinished(token,true,true,{});
    }
    void finishPreview() { emit previewFinished(previewToken,true,false,{}); }
    void finishRead() { emit readFinished(readToken,true,current,{}); }
    void changed() { emit externalChange(); }
    void complete(bool notified=true,bool runtimeMatches=true)
    {
        current=requested; current.intents.clear(); current.revision="fake-config-"+QByteArray::number(++saves+1);
        for (auto i=requested.intents.cbegin();i!=requested.intents.cend();++i) {
            auto &field=current.fields[i.key()]; const auto &before=expected.fields[i.key()];
            field.effectivePresent=field.localPresent=i.value()==NightLightIntent::Set || before.inheritedPresent;
            if (!field.localPresent) field.globalPresent=false;
            field.known=true; field.defaultNoOp=field.value==field.defaultValue;
            field.resettable=field.writable && !field.immutable && !field.defaultNoOp;
            field.revision=QByteArray::number(int(i.key()))+"-readback-"+QByteArray::number(saves);
        }
        current.runtime.enabled=current.fields[NightLightKey::Active].value.toBool(); current.runtime.running=current.runtime.enabled && !current.runtime.inhibited;
        current.runtime.mode=uint(current.fields[NightLightKey::Mode].value.toInt());
        const auto target=current.runtime.mode!=uint(NightLightTypes::ConstantMode) && current.runtime.daylight ? NightLightKey::DayTemperature : NightLightKey::NightTemperature;
        current.runtime.targetTemperature=uint(qBound(NightLightTypes::MinimumTemperature,current.fields[target].value.toInt(),NightLightTypes::MaximumTemperature));
        current.runtime.currentTemperature=current.runtime.targetTemperature;
        if (!runtimeMatches) current.runtime.targetTemperature=1000;
        emit writeFinished(writeToken,true,notified,notified?QString():QStringLiteral("Saved, but native notification failed."));
    }
};
class NightLightPageTest final : public QObject {
    Q_OBJECT
private slots:
    void init() { backend=new PageNightLightBackend; session=new NightLightSettings(backend); QTRY_VERIFY(session->isReady()); }
    void cleanup() { delete session; }
    void injectedPageRendersWithoutWritesOrPreviewsAndHasKeyboardBuddies() {
        NightLightPage page(nullptr,session,nullptr); QCOMPARE(page.objectName(),QStringLiteral("win7Page")); QCOMPARE(page.settingsSession(),static_cast<SettingsSession *>(session));
        for (const auto &name:{"nightLightMode","nightLightDayTemperature","nightLightNightTemperature"}) {
            auto *control=page.findChild<QWidget *>(name); QVERIFY(control); QVERIFY(!control->accessibleName().isEmpty());
            bool buddy=false; for (auto *label:page.findChildren<QLabel *>()) buddy|=label->buddy()==control; QVERIFY(buddy);
        }
        for (const auto &name:{"nightLightSaved","nightLightChanges","nightLightRuntime","nightLightPreviousTransition","nightLightScheduledTransition","nightLightPreviewStatus","nightLightDefaultsReview","nightLightMessage"})
            QCOMPARE(page.findChild<QLabel *>(name)->textFormat(),Qt::PlainText);
        QCOMPARE(page.findChild<QSpinBox *>("nightLightDayTemperature")->minimum(),1000); QCOMPARE(page.findChild<QSpinBox *>("nightLightDayTemperature")->maximum(),6500);
        QVERIFY(!session->isDirty()); QCOMPARE(backend->writes,0); QCOMPARE(backend->previews,0); QCOMPARE(backend->stops,0);
    }
    void temperaturesStageIndependentlyAndSliderAndSpinStaySynchronized() {
        NightLightPage page(nullptr,session,nullptr); edit(page.findChild<QSpinBox *>("nightLightDayTemperature"),5501);
        QCOMPARE(session->staged().fields[NightLightKey::DayTemperature].value.toInt(),5501); QCOMPARE(session->staged().intents.size(),1);
        QCOMPARE(page.findChild<QSlider *>("nightLightDayTemperatureSlider")->value(),5501);
        auto *slider=page.findChild<QSlider *>("nightLightNightTemperatureSlider"); slider->setValue(1000);
        QCOMPARE(page.findChild<QSpinBox *>("nightLightNightTemperature")->value(),1000); QCOMPARE(session->staged().fields[NightLightKey::NightTemperature].value.toInt(),1000);
        edit(page.findChild<QSpinBox *>("nightLightNightTemperature"),6500); QCOMPARE(session->staged().fields[NightLightKey::NightTemperature].value.toInt(),6500);
        page.show(); slider->setFocus(); QTest::keyClick(slider,Qt::Key_Left); QCOMPARE(session->staged().fields[NightLightKey::NightTemperature].value.toInt(),6400);
        QCOMPARE(session->staged().fields[NightLightKey::Active].value.toBool(),true); QCOMPARE(session->staged().fields[NightLightKey::Mode].value.toInt(),NightLightTypes::DarkLightMode);
        QCOMPARE(backend->writes,0); QCOMPARE(backend->previews,0);
    }
    void completeTypingStagesImmediatelyAndSurvivesRuntimeRefreshAndReturn() {
        NightLightPage page(nullptr,session,nullptr); page.show(); auto *spin=page.findChild<QSpinBox *>("nightLightNightTemperature"); spin->setFocus();
        typeTemperature(spin,QStringLiteral("3400 K")); QVERIFY(session->isDirty()); QCOMPARE(session->staged().fields[NightLightKey::NightTemperature].value.toInt(),3400);
        backend->holdRead=true; backend->current.runtime.inhibited=true; backend->changed(); QVERIFY(session->isRefreshing()); QVERIFY(spin->isEnabled()); QCOMPARE(spin->text(),QStringLiteral("3400 K"));
        typeTemperature(spin,QStringLiteral("3500 K")); QCOMPARE(session->staged().fields[NightLightKey::NightTemperature].value.toInt(),3400);
        QTest::keyClick(spin,Qt::Key_Return); QCOMPARE(spin->text(),QStringLiteral("3500 K")); backend->finishRead();
        QVERIFY(!session->isBusy()); QCOMPARE(session->staged().fields[NightLightKey::NightTemperature].value.toInt(),3500); QCOMPARE(spin->value(),3500);
        QVERIFY(page.findChild<QLabel *>("nightLightRuntime")->text().contains("inhibited")); QCOMPARE(backend->writes,0); QCOMPARE(backend->previews,0);
    }
    void rawRepairIntermediateTypingSurvivesRefreshBeforeCompletion() {
        auto &raw=backend->current.fields[NightLightKey::NightTemperature]; raw.value=20000; raw.defaultNoOp=true; raw.resettable=false; session->refresh();
        NightLightPage page(nullptr,session,nullptr); page.show(); auto *spin=page.findChild<QSpinBox *>("nightLightNightTemperature"); spin->setFocus();
        typeTemperature(spin,QStringLiteral("35 K")); QVERIFY(!session->isDirty()); backend->holdRead=true; backend->changed();
        QCOMPARE(spin->text(),QStringLiteral("35 K")); QVERIFY(spin->isEnabled()); typeTemperature(spin,QStringLiteral("3500 K")); QVERIFY(!session->isDirty());
        backend->finishRead(); QVERIFY(session->isDirty()); QCOMPARE(session->staged().fields[NightLightKey::NightTemperature].value.toInt(),3500);
        QTest::keyClick(spin,Qt::Key_Return); QCOMPARE(spin->value(),3500); QCOMPARE(backend->writes,0);
    }
    void spinStepAfterTypedInputStagesItsNewValueInsteadOfRestoringTheTypedValue() {
        NightLightPage page(nullptr,session,nullptr); page.show(); auto *spin=page.findChild<QSpinBox *>("nightLightNightTemperature"); spin->setFocus();
        typeTemperature(spin,QStringLiteral("5000 K")); QCOMPARE(session->staged().fields[NightLightKey::NightTemperature].value.toInt(),5000);
        QTest::keyClick(spin,Qt::Key_Up); QCOMPARE(session->staged().fields[NightLightKey::NightTemperature].value.toInt(),5001);
        QTest::keyClick(spin,Qt::Key_Return); QCOMPARE(spin->value(),5001); QCOMPARE(session->staged().fields[NightLightKey::NightTemperature].value.toInt(),5001); QCOMPARE(backend->writes,0);
    }
    void localizedTemperatureInputUsesTheEditorLocale() {
        NightLightPage page(nullptr,session,nullptr); auto *spin=page.findChild<QSpinBox *>("nightLightNightTemperature"); spin->setLocale(QLocale(QLocale::German,QLocale::Germany));
        typeTemperature(spin,QStringLiteral("3.500 K")); QCOMPARE(session->staged().fields[NightLightKey::NightTemperature].value.toInt(),3500);
        QMetaObject::invokeMethod(spin,"editingFinished"); QCOMPARE(spin->value(),3500); QVERIFY(session->isDirty()); QCOMPARE(backend->writes,0);
    }
    void invalidCommitShowsTheActualDraftAndNeverStagesTheInvalidText() {
        NightLightPage page(nullptr,session,nullptr); page.show(); auto *spin=page.findChild<QSpinBox *>("nightLightNightTemperature"); spin->setFocus();
        typeTemperature(spin,QStringLiteral("35 K")); QVERIFY(!session->isDirty()); QVERIFY(page.findChild<QLabel *>("nightLightNightTemperatureDescription")->text().contains("not staged"));
        QTest::keyClick(spin,Qt::Key_Return); QCOMPARE(spin->value(),4500); QCOMPARE(session->staged().fields[NightLightKey::NightTemperature].value.toInt(),4500);
        QVERIFY(!session->isDirty()); auto message=page.findChild<QLabel *>("nightLightMessage")->text(); QVERIFY(message.contains("35 K")); QVERIFY(message.contains("was not staged")); QCOMPARE(backend->writes,0);
    }
    void nontrackingSliderPositionSurvivesRuntimeRefreshAndReleaseWhileBusy() {
        NightLightPage page(nullptr,session,nullptr); auto *slider=page.findChild<QSlider *>("nightLightNightTemperatureSlider"); slider->setSliderDown(true); slider->setSliderPosition(3200);
        QCOMPARE(slider->sliderPosition(),3200); QVERIFY(!session->isDirty()); backend->holdRead=true; backend->changed();
        QCOMPARE(slider->sliderPosition(),3200); QVERIFY(slider->isSliderDown()); QVERIFY(slider->isEnabled()); slider->setSliderDown(false);
        QVERIFY(!session->isDirty()); backend->finishRead(); QCOMPARE(session->staged().fields[NightLightKey::NightTemperature].value.toInt(),3200); QCOMPARE(slider->value(),3200); QCOMPARE(backend->writes,0);
    }
    void deferredInputCannotCrossProviderOrModeChanges() {
        NightLightPage page(nullptr,session,nullptr); auto *spin=page.findChild<QSpinBox *>("nightLightDayTemperature"); typeTemperature(spin,QStringLiteral("55 K"));
        backend->holdRead=true; backend->changed(); typeTemperature(spin,QStringLiteral("5500 K")); backend->current.runtime.epoch++; backend->finishRead();
        QCOMPARE(session->staged().fields[NightLightKey::DayTemperature].value.toInt(),6500); QVERIFY(!session->isDirty());
        QVERIFY(page.findChild<QLabel *>("nightLightMessage")->text().contains("5500 K")); QVERIFY(page.findChild<QLabel *>("nightLightMessage")->text().contains("was not staged"));
        backend->holdRead=false; session->revert(); auto *slider=page.findChild<QSlider *>("nightLightDayTemperatureSlider"); slider->setSliderDown(true); slider->setSliderPosition(5400);
        activate(page.findChild<QComboBox *>("nightLightMode"),NightLightTypes::ConstantMode); QCOMPARE(session->staged().fields[NightLightKey::DayTemperature].value.toInt(),6500);
        QVERIFY(!slider->isSliderDown()); QVERIFY(page.findChild<QLabel *>("nightLightMessage")->text().contains("5400")); QCOMPARE(backend->writes,0);
    }
    void maskedGlobalOverrideIsNotLabelledAsTheEffectiveValue() {
        auto &field=backend->current.fields[NightLightKey::NightTemperature]; field.localPresent=false; field.globalPresent=true; field.effectivePresent=false; session->refresh();
        NightLightPage page(nullptr,session,nullptr); QVERIFY(page.findChild<QLabel *>("nightLightSaved")->text().contains("global override masked; native fallback")); QCOMPARE(backend->writes,0);
    }
    void modeAndEnableGatesPreserveBothTemperatureDrafts() {
        NightLightPage page(nullptr,session,nullptr); edit(page.findChild<QSpinBox *>("nightLightDayTemperature"),5400);
        activate(page.findChild<QComboBox *>("nightLightMode"),NightLightTypes::ConstantMode);
        QVERIFY(!page.findChild<QSpinBox *>("nightLightDayTemperature")->isEnabled()); QVERIFY(!page.findChild<QPushButton *>("nightLightDayTemperaturePreview")->isEnabled());
        QVERIFY(page.findChild<QSpinBox *>("nightLightNightTemperature")->isEnabled()); QCOMPARE(session->staged().fields[NightLightKey::DayTemperature].value.toInt(),5400);
        auto *enabled=page.findChild<QCheckBox *>("nightLightEnabled"); enabled->setChecked(false);
        QVERIFY(!page.findChild<QSpinBox *>("nightLightNightTemperature")->isEnabled()); QCOMPARE(session->staged().fields[NightLightKey::NightTemperature].value.toInt(),4500);
        enabled->setChecked(true); activate(page.findChild<QComboBox *>("nightLightMode"),NightLightTypes::DarkLightMode);
        QVERIFY(page.findChild<QSpinBox *>("nightLightDayTemperature")->isEnabled()); QCOMPARE(page.findChild<QSpinBox *>("nightLightDayTemperature")->value(),5400);
        QCOMPARE(backend->writes,0); QCOMPARE(backend->previews,0);
    }
    void pageRecreationKeepsTheSessionDraft() {
        auto *page=new NightLightPage(nullptr,session,nullptr); edit(page->findChild<QSpinBox *>("nightLightNightTemperature"),3700); delete page;
        page=new NightLightPage(nullptr,session,nullptr); QCOMPARE(page->findChild<QSpinBox *>("nightLightNightTemperature")->value(),3700);
        QVERIFY(page->findChild<QPushButton *>("nightLightApply")->isEnabled()); QVERIFY(page->findChild<QLabel *>("nightLightChanges")->text().contains("3700")); delete page;
        QVERIFY(session->isDirty()); QCOMPARE(backend->writes,0); QCOMPARE(backend->previews,0);
    }
    void renderingAndFocusDoNotMaterializeAnAbsentDefault() {
        auto &field=backend->current.fields[NightLightKey::NightTemperature]; field.effectivePresent=field.localPresent=false; field.defaultNoOp=true; session->refresh();
        NightLightPage page(nullptr,session,nullptr); auto *spin=page.findChild<QSpinBox *>("nightLightNightTemperature");
        QVERIFY(QMetaObject::invokeMethod(spin,"editingFinished")); QVERIFY(!session->isDirty());
        page.show(); spin->setFocus(); page.findChild<QComboBox *>("nightLightMode")->setFocus(); QCoreApplication::processEvents();
        QVERIFY(!session->isDirty()); QCOMPARE(backend->previews,0); QCOMPARE(backend->writes,0);
    }
    void unsupportedModeAndRawTemperaturesStayVisibleUntilExplicitReplacement() {
        auto &mode=backend->current.fields[NightLightKey::Mode]; mode.value=17; mode.defaultNoOp=false; mode.resettable=true;
        auto &day=backend->current.fields[NightLightKey::DayTemperature]; day.value=20000; day.defaultNoOp=true; day.resettable=false; session->refresh();
        NightLightPage page(nullptr,session,nullptr); auto *combo=page.findChild<QComboBox *>("nightLightMode"); QCOMPARE(combo->currentData().toInt(),17); QVERIFY(combo->currentText().contains("17"));
        auto *spin=page.findChild<QSpinBox *>("nightLightDayTemperature"); QVERIFY(!spin->isEnabled()); QVERIFY(page.findChild<QLabel *>("nightLightSaved")->text().contains("20000"));
        QVERIFY(page.findChild<QLabel *>("nightLightDayTemperatureDescription")->text().contains("20000"));
        QVERIFY2(spin->cleanText().isEmpty(),qPrintable(QStringLiteral("Before focus/commit: text=[%1], cleanText=[%2], internal value=%3").arg(spin->text(),spin->cleanText()).arg(spin->value())));
        QMetaObject::invokeMethod(spin,"editingFinished");
        QVERIFY2(spin->cleanText().isEmpty(),qPrintable(QStringLiteral("After editingFinished: text=[%1], cleanText=[%2], internal value=%3").arg(spin->text(),spin->cleanText()).arg(spin->value())));
        QVERIFY(!session->isDirty()); activate(combo,NightLightTypes::DarkLightMode); QVERIFY(spin->isEnabled());
        QVERIFY2(spin->cleanText().isEmpty(),qPrintable(QStringLiteral("After mode change: text=[%1], cleanText=[%2], internal value=%3").arg(spin->text(),spin->cleanText()).arg(spin->value())));
        QVERIFY(!spin->findChild<QAction *>("nightLightFieldDefault")->isEnabled()); edit(spin,6400);
        QCOMPARE(session->staged().fields[NightLightKey::DayTemperature].value.toInt(),6400); QCOMPARE(session->applied().fields[NightLightKey::DayTemperature].value.toInt(),20000);
        QCOMPARE(backend->writes,0); QCOMPARE(backend->previews,0);
    }
    void unknownEnableUsesIndeterminateStateAndPolicyReasonsArePlainText() {
        auto &active=backend->current.fields[NightLightKey::Active]; active.known=false;
        auto &night=backend->current.fields[NightLightKey::NightTemperature]; night.immutable=true; night.writable=false; night.reason=QStringLiteral("Policy <b>locked</b>."); session->refresh();
        NightLightPage page(nullptr,session,nullptr); auto *enabled=page.findChild<QCheckBox *>("nightLightEnabled"); QCOMPARE(enabled->checkState(),Qt::PartiallyChecked);
        QVERIFY(!page.findChild<QSpinBox *>("nightLightNightTemperature")->isEnabled()); enabled->setCheckState(Qt::Checked);
        QVERIFY(session->staged().fields[NightLightKey::Active].known); QVERIFY(!page.findChild<QSpinBox *>("nightLightNightTemperature")->isEnabled());
        QVERIFY(page.findChild<QSpinBox *>("nightLightNightTemperature")->toolTip().contains("&lt;b&gt;")); QCOMPARE(backend->writes,0);
    }
    void previewsAreExplicitTemporaryAndStopWhenHidden() {
        NightLightPage page(nullptr,session,nullptr); page.show(); QCoreApplication::processEvents();
        page.findChild<QPushButton *>("nightLightDayTemperaturePreview")->click(); QCOMPARE(backend->previews,1); QCOMPARE(backend->temperature,6500); QCOMPARE(backend->previewOwner,backend->current.runtime.owner); QCOMPARE(backend->previewEpoch,backend->current.runtime.epoch);
        QVERIFY(session->previewActive()); QVERIFY(!session->isDirty()); QCOMPARE(backend->writes,0); QVERIFY(page.findChild<QPushButton *>("nightLightStopPreview")->isEnabled());
        page.findChild<QPushButton *>("nightLightStopPreview")->click(); QCOMPARE(backend->stops,1); QVERIFY(!session->previewActive());
        page.findChild<QPushButton *>("nightLightNightTemperaturePreview")->click(); QCOMPARE(backend->previews,2); page.hide(); QCOMPARE(backend->stops,2); QVERIFY(!session->previewActive());
        QCOMPARE(backend->stopOwner,backend->current.runtime.owner); QCOMPARE(backend->stopEpoch,backend->current.runtime.epoch); QCOMPARE(backend->writes,0);
    }
    void explicitStopCanRetryAfterTheNativeStopRequestFails() {
        NightLightPage page(nullptr,session,nullptr); page.findChild<QPushButton *>("nightLightNightTemperaturePreview")->click();
        QVERIFY(session->previewActive()); backend->failNextStop=true; auto *stop=page.findChild<QPushButton *>("nightLightStopPreview"); stop->click();
        QCOMPARE(backend->stops,1); QVERIFY(session->previewActive()); QVERIFY(stop->isEnabled());
        QVERIFY(page.findChild<QLabel *>("nightLightPreviewStatus")->text().contains("Stop request failed"));
        stop->click(); QCOMPARE(backend->stops,2); QVERIFY(!session->previewActive()); QVERIFY(!stop->isEnabled());
        QCOMPARE(backend->stopOwner,backend->current.runtime.owner); QCOMPARE(backend->stopEpoch,backend->current.runtime.epoch); QCOMPARE(backend->writes,0);
    }
    void pendingPreviewIsReleasedAfterItsViewIsDestroyed() {
        backend->holdPreview=true; auto *page=new NightLightPage(nullptr,session,nullptr); page->findChild<QPushButton *>("nightLightNightTemperaturePreview")->click();
        QCOMPARE(backend->previews,1); QVERIFY(session->previewActive()); delete page; QCOMPARE(backend->stops,0);
        backend->finishPreview(); QCOMPARE(backend->stops,1); QVERIFY(!session->previewActive()); QCOMPARE(backend->writes,0);
    }
    void editingDuringPreviewNeverRepeatsPreviewAndApplyWaitsForStop() {
        NightLightPage page(nullptr,session,nullptr); page.findChild<QPushButton *>("nightLightNightTemperaturePreview")->click();
        edit(page.findChild<QSpinBox *>("nightLightNightTemperature"),3500); QCOMPARE(backend->previews,1); QVERIFY(!page.findChild<QPushButton *>("nightLightApply")->isEnabled());
        page.findChild<QPushButton *>("nightLightStopPreview")->click(); QVERIFY(page.findChild<QPushButton *>("nightLightApply")->isEnabled()); QCOMPARE(backend->writes,0);
    }
    void applyJobSurvivesViewDestructionAndOnlyReportsObservableVerification() {
        auto *page=new NightLightPage(nullptr,session,nullptr); edit(page->findChild<QSpinBox *>("nightLightNightTemperature"),3500);
        page->findChild<QPushButton *>("nightLightApply")->click(); QCOMPARE(backend->writes,1); QVERIFY(session->isBusy());
        QVERIFY(!page->findChild<QSpinBox *>("nightLightNightTemperature")->isEnabled()); delete page; backend->complete();
        QVERIFY(!session->isBusy()); QVERIFY(!session->isDirty()); QVERIFY(!session->hasConflict());
        NightLightPage replacement(nullptr,session,nullptr); QCOMPARE(replacement.findChild<QSpinBox *>("nightLightNightTemperature")->value(),3500);
        const auto message=replacement.findChild<QLabel *>("nightLightMessage")->text(); QVERIFY(message.contains("verified")); QVERIFY(message.contains("does not expose")); QVERIFY(message.contains("acknowledgement")); QCOMPARE(backend->previews,0);
    }
    void failedObservableReadbackKeepsDraftAndErrorUntilRevert() {
        NightLightPage page(nullptr,session,nullptr); edit(page.findChild<QSpinBox *>("nightLightNightTemperature"),3500); page.findChild<QPushButton *>("nightLightApply")->click(); backend->complete(true,false);
        QVERIFY(session->hasConflict()); QVERIFY(session->isDirty()); QVERIFY(!page.findChild<QPushButton *>("nightLightApply")->isEnabled());
        QVERIFY(page.findChild<QLabel *>("nightLightMessage")->text().contains("does not match")); QCOMPARE(session->staged().fields[NightLightKey::NightTemperature].value.toInt(),3500);
        page.findChild<QPushButton *>("nightLightRevert")->click(); QVERIFY(!session->hasConflict()); QVERIFY(!session->isDirty()); QCOMPARE(backend->writes,1);
    }
    void externalSavedChangeFreezesTheDraftUntilRevert() {
        NightLightPage page(nullptr,session,nullptr); edit(page.findChild<QSpinBox *>("nightLightDayTemperature"),5500);
        backend->current.fields[NightLightKey::NightTemperature].value=3000; backend->current.fields[NightLightKey::NightTemperature].defaultNoOp=false; backend->current.fields[NightLightKey::NightTemperature].resettable=true; backend->current.fields[NightLightKey::NightTemperature].revision="external"; backend->current.revision="external-config"; backend->changed();
        QVERIFY(session->hasConflict()); QCOMPARE(session->staged().fields[NightLightKey::DayTemperature].value.toInt(),5500);
        QVERIFY(!page.findChild<QSpinBox *>("nightLightDayTemperature")->isEnabled()); page.findChild<QPushButton *>("nightLightRevert")->click();
        QVERIFY(!session->hasConflict()); QVERIFY(!session->isDirty()); QCOMPARE(page.findChild<QSpinBox *>("nightLightNightTemperature")->value(),3000); QCOMPARE(backend->writes,0);
    }
    void runtimeTimelineUsesEpochSecondsAndMillisecondDurations() {
        NightLightPage page(nullptr,session,nullptr); const auto zone=QTimeZone::systemTimeZone(); const auto previous=QDateTime::fromSecsSinceEpoch(1700000000,zone);
        const auto text=page.findChild<QLabel *>("nightLightPreviousTransition")->text(); QVERIFY(text.contains(previous.toString(Qt::ISODateWithMs))); QVERIFY(text.contains(previous.addMSecs(1500).toString(Qt::ISODateWithMs))); QVERIFY(text.contains("1500 ms"));
        const auto scheduled=QDateTime::fromSecsSinceEpoch(1700003600,zone); const auto next=page.findChild<QLabel *>("nightLightScheduledTransition")->text();
        QVERIFY(next.contains(scheduled.addMSecs(120000).toString(Qt::ISODateWithMs)));
        backend->current.runtime.previousTransitionDateTime=std::numeric_limits<qulonglong>::max(); backend->current.runtime.scheduledTransitionDateTime=0;
        backend->current.runtime.inhibited=true; backend->changed(); QVERIFY(!session->hasConflict());
        QVERIFY(page.findChild<QLabel *>("nightLightPreviousTransition")->text().contains("Unavailable")); QVERIFY(page.findChild<QLabel *>("nightLightScheduledTransition")->text().contains("No transition"));
        QVERIFY(page.findChild<QLabel *>("nightLightRuntime")->text().contains("inhibited")); QCOMPARE(backend->previews,0); QCOMPARE(backend->writes,0);
    }
    void defaultsStageOnlyKnownWritableChangesAndPreserveNativeNoOpRawValues() {
        auto &day=backend->current.fields[NightLightKey::DayTemperature]; day.value=20000; day.defaultNoOp=true; day.resettable=false;
        auto &night=backend->current.fields[NightLightKey::NightTemperature]; night.value=3000; night.defaultNoOp=false; night.resettable=false; night.immutable=true; night.writable=false; night.resetReason=QStringLiteral("Locked night temperature."); session->refresh();
        NightLightPage page(nullptr,session,nullptr); QVERIFY(page.findChild<QPushButton *>("nightLightRestoreDefaults")->isEnabled()); page.findChild<QPushButton *>("nightLightRestoreDefaults")->click();
        QCOMPARE(session->staged().fields[NightLightKey::Active].value.toBool(),false); QCOMPARE(session->staged().intents.value(NightLightKey::Active),NightLightIntent::RestoreDefault);
        QCOMPARE(session->staged().fields[NightLightKey::DayTemperature].value.toInt(),20000); QCOMPARE(session->staged().fields[NightLightKey::NightTemperature].value.toInt(),3000);
        QCOMPARE(session->staged().intents.size(),1); QVERIFY(page.findChild<QLabel *>("nightLightDefaultsReview")->text().contains("Locked night temperature")); QCOMPARE(backend->writes,0); QCOMPARE(backend->previews,0);
    }
    void destroyedSessionDisablesViewWithoutStartingAnyProvider() {
        auto *page=new NightLightPage(nullptr,session,nullptr); delete session; session=nullptr; QVERIFY(!page->isEnabled());
        QVERIFY(page->findChild<QLabel *>("nightLightMessage")->text().contains("no longer available")); delete page;
    }
private:
    PageNightLightBackend *backend=nullptr;
    NightLightSettings *session=nullptr;
};
QTEST_MAIN(NightLightPageTest)
#include "night_light_page_test.moc"
