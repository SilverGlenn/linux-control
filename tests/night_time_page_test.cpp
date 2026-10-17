#include <QtTest>
#include <QAction>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleValidator>
#include <QLabel>
#include <QGroupBox>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QTimeEdit>
#include <QTimer>
#include "night_time_fixture.h"
#include "platform/location/NightTimeLocation.h"
#include "settings/NightTimeSettings.h"
#include "ui/pages/NightTimePage.h"
#include "ui/pages/NightTimeMapWidget.h"
#include "ui/pages/NightTimePreviewWidget.h"

namespace {
void type(QLineEdit *line,const QString &text)
{ QVERIFY(line); line->setText(text); line->setCursorPosition(text.size()); QVERIFY(QMetaObject::invokeMethod(line,"textEdited",Q_ARG(QString,text))); }
void finish(QLineEdit *line) { QVERIFY(QMetaObject::invokeMethod(line,"editingFinished")); }
void activate(QComboBox *combo,int value)
{ const int index=combo->findData(value); QVERIFY(index>=0); combo->setCurrentIndex(index); QVERIFY(QMetaObject::invokeMethod(combo,"activated",Q_ARG(int,index))); }
QLineEdit *editor(QWidget &page,const char *name)
{ auto *control=page.findChild<QWidget *>(name); return qobject_cast<QLineEdit *>(control)?qobject_cast<QLineEdit *>(control):control->findChild<QLineEdit *>(); }
}
class PageNightTimeBackend final : public NightTimeBackend {
public:
    NightTimeSnapshot current=nightTimeFixture(),expected,requested;
    int reads=0,writes=0; bool holdRead=false; quint64 readToken=0,writeToken=0;
    void readCurrent(quint64 token) override { ++reads; readToken=token; if (!holdRead) emit readFinished(token,true,current,{}); }
    void writeChanges(quint64 token,const NightTimeSnapshot &before,const NightTimeSnapshot &after) override { ++writes; writeToken=token; expected=before; requested=after; }
    void native(NightTimeKey key,const QVariant &value) { auto &f=current.fields[key]; f.value=value; f.known=true; f.defaultNoOp=value==f.defaultValue; f.resettable=!f.defaultNoOp; current.daemonFields[key]=f; }
    void changed() { emit externalChange(); }
    void finishRead() { emit readFinished(readToken,true,current,{}); }
    void failRead() { emit readFinished(readToken,false,{},QStringLiteral("Fake read failure")); }
    void complete(bool notified=true) { current=savedNightTimeFixture(requested); emit writeFinished(writeToken,true,notified,{}); }
};
class PageLocationSource final : public NightTimeLocationSource {
public:
    PageLocationSource(int *requests,int *stops,QObject *parent):NightTimeLocationSource(parent),requests(requests),stops(stops){}
    int *requests,*stops;
    void request(int timeout) override { ++*requests; QCOMPARE(timeout,30000); }
    void stop() override { ++*stops; }
    void fix(double latitude=51.5,double longitude=-0.125) { emit position({latitude,longitude,QDateTime::currentDateTimeUtc(),12.5}); }
};
class NightTimePageTest final : public QObject {
    Q_OBJECT
private slots:
    void init() {
        backend=new PageNightTimeBackend; session=new NightTimeSettings(backend); QTRY_VERIFY(session->isReady());
        constructions=requests=stops=0; location=new NightTimeLocation([this](QObject *parent){ ++constructions; source=new PageLocationSource(&requests,&stops,parent); return source.data(); });
    }
    void cleanup() { delete location; delete session; }
    void injectedConstructionIsPassiveAndAccessible() {
        NightTimePage page(nullptr,session,location,nullptr); page.show();
        QCOMPARE(page.objectName(),QStringLiteral("win7Page")); QCOMPARE(page.settingsSession(),static_cast<SettingsSession *>(session));
        for (auto name:{"nightTimeSource","nightTimeLatitude","nightTimeLongitude","nightTimeSunrise","nightTimeSunset","nightTimeDuration"}) {
            auto *control=page.findChild<QWidget *>(name); QVERIFY(control); QVERIFY(!control->accessibleName().isEmpty()); bool buddy=false; for (auto *label:page.findChildren<QLabel *>()) buddy|=label->buddy()==control; QVERIFY(buddy);
        }
        for (auto name:{"nightTimeForecastDetails","nightTimePrivacy","nightTimeLocationStatus","nightTimeInputs","nightTimeChanges","nightTimeMessage"}) QCOMPARE(page.findChild<QLabel *>(name)->textFormat(),Qt::PlainText);
        auto *duration=page.findChild<QSpinBox *>("nightTimeDuration"); QCOMPARE(duration->minimum(),1); QCOMPARE(duration->maximum(),600); QCOMPARE(duration->value(),30);
        QVERIFY(page.findChild<QLabel *>("nightTimeMapAttribution")->text().contains("CC BY-SA 3.0"));
        QCOMPARE(constructions,0); QCOMPARE(requests,0); QCOMPARE(backend->writes,0); QVERIFY(!session->isDirty()); QVERIFY(!page.findChild<QPushButton *>("nightTimeApply")->isEnabled());
        QVERIFY(!NightTimePage::sidebarLinks().isEmpty()); QVERIFY(!NightTimePage::sidebarSeeAlso().isEmpty());
    }
    void sourceShowsOnlyItsEditorsWhileDaemonPrivacyRemainsVisible() {
        backend->native(NightTimeKey::Source,1); backend->current.daemonFields[NightTimeKey::Source].value=0; session->refresh(); NightTimePage page(nullptr,session,location,nullptr); page.show();
        auto *locationGroup=page.findChild<QGroupBox *>("nightTimeLocationGroup"),*timesGroup=page.findChild<QGroupBox *>("nightTimeTimesGroup"); auto *privacy=page.findChild<QLabel *>("nightTimePrivacy");
        QVERIFY(locationGroup->isHidden()); QVERIFY(!timesGroup->isHidden()); QVERIFY(privacy->isVisible()); QVERIFY(!locationGroup->isAncestorOf(privacy)); QVERIFY(privacy->text().contains("uses automatic location")); QVERIFY(!editor(page,"nightTimeLatitude")->isEnabled());
        activate(page.findChild<QComboBox *>("nightTimeSource"),0); QVERIFY(!locationGroup->isHidden()); QVERIFY(timesGroup->isHidden()); QVERIFY(privacy->isVisible()); QVERIFY(!page.findChild<QTimeEdit *>("nightTimeSunrise")->isEnabled());
        session->revert(); backend->native(NightTimeKey::Source,17); session->refresh(); QVERIFY(!locationGroup->isHidden()); QVERIFY(!timesGroup->isHidden()); QVERIFY(privacy->isVisible()); QVERIFY(!editor(page,"nightTimeLatitude")->isEnabled()); QVERIFY(!page.findChild<QTimeEdit *>("nightTimeSunrise")->isEnabled()); QCOMPARE(backend->writes,0); QCOMPARE(constructions,0);
    }
    void dormantValuesAndNativePrecisionSurviveSourceSwitchAndFocus() {
        backend->native(NightTimeKey::SunriseStart,QTime(6,12,37,125)); backend->native(NightTimeKey::TransitionDuration,uint(45)); session->refresh();
        NightTimePage page(nullptr,session,location,nullptr); page.show();
        activate(page.findChild<QComboBox *>("nightTimeSource"),1);
        auto *sunrise=page.findChild<QTimeEdit *>("nightTimeSunrise"); sunrise->setFocus(); finish(editor(page,"nightTimeSunrise"));
        auto *duration=page.findChild<QSpinBox *>("nightTimeDuration"); QVERIFY(duration->cleanText().isEmpty()); finish(editor(page,"nightTimeDuration"));
        QCOMPARE(session->staged().fields[NightTimeKey::SunriseStart].value.toTime(),QTime(6,12,37,125)); QCOMPARE(session->staged().fields[NightTimeKey::TransitionDuration].value.toUInt(),uint(45));
        QVERIFY(page.findChild<QLabel *>("nightTimeDurationDescription")->text().contains("45 seconds"));
        activate(page.findChild<QComboBox *>("nightTimeSource"),0); QCOMPARE(session->staged().intents.size(),0); QCOMPARE(backend->writes,0); QCOMPARE(constructions,0);
    }
    void rawLongDurationRemainsVisibleUntilAnExplicitMinuteReplacement() {
        backend->native(NightTimeKey::Source,1); backend->native(NightTimeKey::TransitionDuration,uint(43200)); session->refresh(); NightTimePage page(nullptr,session,location,nullptr);
        auto *spin=page.findChild<QSpinBox *>("nightTimeDuration"); QVERIFY(spin->cleanText().isEmpty()); QVERIFY(page.findChild<QLabel *>("nightTimeDurationDescription")->text().contains("43200 seconds"));
        type(editor(page,"nightTimeDuration"),QStringLiteral("600")); finish(editor(page,"nightTimeDuration")); QCOMPARE(session->staged().fields[NightTimeKey::TransitionDuration].value.toUInt(),uint(36000)); QCOMPARE(backend->writes,0);
    }
    void localeCoordinatesStageIndividuallyAndRecreateWithTheSession() {
        backend->native(NightTimeKey::Automatic,false); session->refresh();
        { NightTimePage page(nullptr,session,location,nullptr); auto *latitude=editor(page,"nightTimeLatitude"); latitude->setLocale(QLocale(QLocale::German,QLocale::Germany)); type(latitude,QStringLiteral("12,3456789012345")); finish(latitude); QCOMPARE(latitude->validator()->locale(),latitude->locale());
          QCOMPARE(session->staged().fields[NightTimeKey::Latitude].value.toDouble(),12.3456789012345); QCOMPARE(session->staged().intents.size(),1); }
        NightTimePage reopened(nullptr,session,location,nullptr); QVERIFY(reopened.findChild<QLabel *>("nightTimeLatitudeDescription")->text().contains("staged explicit")); QCOMPARE(session->staged().fields[NightTimeKey::Longitude].value.toDouble(),0.0); QCOMPARE(backend->writes,0);
    }
    void completeAndIntermediateTextSurviveVolatileRefresh() {
        backend->native(NightTimeKey::Automatic,false); session->refresh(); NightTimePage page(nullptr,session,location,nullptr);
        auto *latitude=editor(page,"nightTimeLatitude"); type(latitude,QStringLiteral("12.25")); QVERIFY(session->isDirty());
        backend->holdRead=true; backend->current.cachedLocation={true,40,20,{}}; backend->changed(); QVERIFY(session->isRefreshing()); QVERIFY(latitude->isEnabled());
        type(latitude,QStringLiteral("13.5")); finish(latitude); QCOMPARE(latitude->text(),QStringLiteral("13.5")); QCOMPARE(session->staged().fields[NightTimeKey::Latitude].value.toDouble(),12.25);
        backend->finishRead(); QCOMPARE(session->staged().fields[NightTimeKey::Latitude].value.toDouble(),13.5); QCOMPARE(latitude->text(),QStringLiteral("13.5")); QCOMPARE(backend->writes,0);
    }
    void cursorAndSelectionSurviveRefreshWithoutAnotherTextEdit() {
        backend->native(NightTimeKey::Automatic,false); session->refresh(); NightTimePage page(nullptr,session,location,nullptr);
        auto exercise=[&](QLineEdit *line,const QString &text) {
            type(line,text); line->setSelection(text.size()-1,-2);
            const int cursor=line->cursorPosition(),start=line->selectionStart(); const QString selected=line->selectedText();
            backend->holdRead=true; backend->current.cachedLocation={true,45,12,{}}; backend->changed();
            QCOMPARE(line->text(),text); QCOMPARE(line->cursorPosition(),cursor); QCOMPARE(line->selectionStart(),start); QCOMPARE(line->selectedText(),selected);
            backend->finishRead(); QCOMPARE(line->text(),text); QCOMPARE(line->cursorPosition(),cursor); QCOMPARE(line->selectionStart(),start); QCOMPARE(line->selectedText(),selected);
            line->deselect(); line->setCursorPosition(1); backend->changed(); QCOMPARE(line->cursorPosition(),1); backend->finishRead(); QCOMPARE(line->cursorPosition(),1); finish(line); backend->holdRead=false;
        };
        exercise(editor(page,"nightTimeLatitude"),QStringLiteral("12.25"));
        activate(page.findChild<QComboBox *>("nightTimeSource"),1);
        exercise(editor(page,"nightTimeDuration"),QStringLiteral("140"));
        page.findChild<QTimeEdit *>("nightTimeSunrise")->setDisplayFormat(QStringLiteral("HH:mm")); exercise(editor(page,"nightTimeSunrise"),QStringLiteral("07:15"));
        QCOMPARE(backend->writes,0); QCOMPARE(constructions,0);
    }
    void unsupportedCoordinateRepairCanRemainIntermediateDuringRefresh() {
        backend->native(NightTimeKey::Automatic,false); backend->native(NightTimeKey::Latitude,200.0); session->refresh(); NightTimePage page(nullptr,session,location,nullptr);
        auto *latitude=editor(page,"nightTimeLatitude"); QVERIFY(latitude->text().isEmpty()); type(latitude,QStringLiteral("-")); backend->holdRead=true; backend->changed(); QCOMPARE(latitude->text(),QStringLiteral("-"));
        type(latitude,QStringLiteral("-25.5")); backend->finishRead(); QCOMPARE(session->staged().fields[NightTimeKey::Latitude].value.toDouble(),-25.5); finish(latitude); QCOMPARE(backend->writes,0);
    }
    void invalidInputCannotApplyOrMasqueradeAsAStagedValue() {
        backend->native(NightTimeKey::Automatic,false); session->refresh(); NightTimePage page(nullptr,session,location,nullptr);
        type(editor(page,"nightTimeLongitude"),QStringLiteral("24")); finish(editor(page,"nightTimeLongitude")); type(editor(page,"nightTimeLatitude"),QStringLiteral("999"));
        QVERIFY(!page.findChild<QPushButton *>("nightTimeApply")->isEnabled()); QVERIFY(page.findChild<QLabel *>("nightTimeLatitudeDescription")->text().contains("not staged")); finish(editor(page,"nightTimeLatitude"));
        QCOMPARE(session->staged().fields[NightTimeKey::Latitude].value.toDouble(),0.0); QVERIFY(page.findChild<QLabel *>("nightTimeMessage")->text().contains("999")); QCOMPARE(backend->writes,0);
    }
    void deferredInputCannotCrossProviderOrSourceChanges() {
        backend->native(NightTimeKey::Automatic,false); session->refresh(); NightTimePage page(nullptr,session,location,nullptr);
        auto *latitude=editor(page,"nightTimeLatitude"); type(latitude,QStringLiteral("-")); backend->holdRead=true; backend->changed(); type(latitude,QStringLiteral("22.5")); backend->current.runtime.epoch++; backend->finishRead();
        QCOMPARE(session->staged().fields[NightTimeKey::Latitude].value.toDouble(),0.0); QVERIFY(page.findChild<QLabel *>("nightTimeMessage")->text().contains("22.5")); QVERIFY(!session->isDirty());
        backend->holdRead=false; session->revert(); type(latitude,QStringLiteral("-")); QCOMPARE(latitude->text(),QStringLiteral("-")); QVERIFY(!session->isDirty()); activate(page.findChild<QComboBox *>("nightTimeSource"),1); QCOMPARE(session->staged().fields[NightTimeKey::Latitude].value.toDouble(),0.0); QVERIFY(!latitude->isEnabled()); QCOMPARE(backend->writes,0);
    }
    void timeEditResetsOnlyEditedSecondsAndUsesNativeBoundaryValidator() {
        backend->native(NightTimeKey::Source,1); backend->native(NightTimeKey::SunriseStart,QTime(6,0,17)); backend->native(NightTimeKey::SunsetStart,QTime(18,0,22)); session->refresh(); NightTimePage page(nullptr,session,location,nullptr);
        auto *time=page.findChild<QTimeEdit *>("nightTimeSunrise"); time->setDisplayFormat(QStringLiteral("HH:mm")); type(editor(page,"nightTimeSunrise"),QStringLiteral("17:50")); finish(editor(page,"nightTimeSunrise"));
        QCOMPARE(session->staged().fields[NightTimeKey::SunriseStart].value.toTime(),QTime(17,30,22)); QCOMPARE(session->staged().fields[NightTimeKey::SunsetStart].value.toTime(),QTime(18,0,22)); QVERIFY(!session->staged().intents.contains(NightTimeKey::SunsetStart)); QCOMPARE(backend->writes,0);
    }
    void durationStagesItsBoundaryAdjustmentAtomicallyAndRejectsProtectedCompanion() {
        backend->native(NightTimeKey::Source,1); backend->native(NightTimeKey::SunsetStart,QTime(7,0)); session->refresh(); NightTimePage page(nullptr,session,location,nullptr);
        type(editor(page,"nightTimeDuration"),QStringLiteral("120")); finish(editor(page,"nightTimeDuration")); QCOMPARE(session->staged().fields[NightTimeKey::TransitionDuration].value.toUInt(),uint(7200)); QCOMPARE(session->staged().fields[NightTimeKey::SunsetStart].value.toTime(),QTime(8,0));
        session->revert(); backend->current.fields[NightTimeKey::SunsetStart].immutable=true; backend->current.fields[NightTimeKey::SunsetStart].writable=false; session->refresh();
        type(editor(page,"nightTimeDuration"),QStringLiteral("120")); finish(editor(page,"nightTimeDuration")); QCOMPARE(session->staged().fields[NightTimeKey::TransitionDuration].value.toUInt(),uint(1800)); QVERIFY(!session->isDirty()); QCOMPARE(backend->writes,0);
    }
    void typedMinuteThenSpinStepStagesTheStep() {
        backend->native(NightTimeKey::Source,1); session->refresh(); NightTimePage page(nullptr,session,location,nullptr); page.show(); auto *spin=page.findChild<QSpinBox *>("nightTimeDuration");
        type(editor(page,"nightTimeDuration"),QStringLiteral("40")); QTest::keyClick(spin,Qt::Key_Up); QCOMPARE(session->staged().fields[NightTimeKey::TransitionDuration].value.toUInt(),uint(2700)); QCOMPARE(backend->writes,0);
    }
    void mapSelectionIsAtomicAndZoomIsViewOnly() {
        NightTimePage page(nullptr,session,location,nullptr); auto *map=page.findChild<NightTimeMapWidget *>(); map->resize(640,320); map->zoomIn(); map->zoomOut(); map->resetView(); QVERIFY(!session->isDirty());
        QTest::keyClick(map,Qt::Key_Right); QCOMPARE(session->staged().fields[NightTimeKey::Longitude].value.toDouble(),1.0); QCOMPARE(session->staged().fields[NightTimeKey::Latitude].value.toDouble(),0.0); QVERIFY(!session->staged().fields[NightTimeKey::Automatic].value.toBool());
        session->revert(); backend->current.daemonFields[NightTimeKey::Longitude].immutable=true; session->refresh(); QTest::keyClick(map,Qt::Key_Right); QVERIFY(!session->isDirty()); QCOMPARE(constructions,0); QCOMPARE(backend->writes,0);
    }
    void unknownSourceAndPolicyDoNotInventReplacements() {
        backend->native(NightTimeKey::Source,17); backend->current.fields[NightTimeKey::Latitude].known=false; backend->current.daemonFields[NightTimeKey::Latitude].known=false; session->refresh(); NightTimePage page(nullptr,session,location,nullptr);
        QVERIFY(page.findChild<QComboBox *>("nightTimeSource")->currentText().contains("17")); QVERIFY(editor(page,"nightTimeLatitude")->text().isEmpty()); QVERIFY(!page.findChild<QPushButton *>("nightTimeAcquireLocation")->isEnabled()); QVERIFY(!session->isDirty()); QCOMPARE(constructions,0);
        activate(page.findChild<QComboBox *>("nightTimeSource"),1); QVERIFY(session->staged().intents.contains(NightTimeKey::Source)); QVERIFY(!session->staged().intents.contains(NightTimeKey::Latitude)); QCOMPARE(backend->writes,0);
    }
    void explicitAcquisitionIsEphemeralAndManualUseIsSeparate() {
        NightTimePage page(nullptr,session,location,nullptr); page.findChild<QPushButton *>("nightTimeAcquireLocation")->click(); QCOMPARE(constructions,1); QCOMPARE(requests,1); QVERIFY(source); source->fix();
        QVERIFY(!session->isDirty()); QVERIFY(page.findChild<QLabel *>("nightTimeLocationStatus")->text().contains("12.5 metres")); QVERIFY(page.findChild<QLabel *>("nightTimeLocationStatus")->text().contains("Sample time:"));
        QVERIFY(page.findChild<QLabel *>("nightTimeForecastDetails")->text().contains("explicit, unsaved sample")); page.findChild<QPushButton *>("nightTimeUseLocation")->click();
        QCOMPARE(session->staged().fields[NightTimeKey::Latitude].value.toDouble(),51.5); QCOMPARE(session->staged().fields[NightTimeKey::Longitude].value.toDouble(),-0.125); QVERIFY(!session->staged().fields[NightTimeKey::Automatic].value.toBool()); QCOMPARE(backend->writes,0);
    }
    void acquisitionCancelsOnHideDestructionAndCapabilityLoss() {
        auto *page=new NightTimePage(nullptr,session,location,nullptr); page->show(); page->findChild<QPushButton *>("nightTimeAcquireLocation")->click(); QVERIFY(location->isBusy()); page->hide(); QVERIFY(!location->isBusy()); QVERIFY(stops>=1); QVERIFY(!session->isDirty());
        page->show(); page->findChild<QPushButton *>("nightTimeAcquireLocation")->click(); delete page; QVERIFY(!location->isBusy());
        NightTimePage next(nullptr,session,location,nullptr); next.findChild<QPushButton *>("nightTimeAcquireLocation")->click(); backend->current.runtime.providerVerified=false; backend->changed(); QVERIFY(!location->isBusy()); QVERIFY(!next.findChild<QPushButton *>("nightTimeAcquireLocation")->isEnabled()); QCOMPARE(backend->writes,0);
    }
    void readFailureDisablesMapAcquisitionAndManualUse() {
        NightTimePage page(nullptr,session,location,nullptr); page.findChild<QPushButton *>("nightTimeAcquireLocation")->click(); source->fix(); QVERIFY(page.findChild<QPushButton *>("nightTimeUseLocation")->isEnabled());
        backend->holdRead=true; session->refresh(); backend->failRead(); QVERIFY(!page.findChild<QPushButton *>("nightTimeUseLocation")->isEnabled()); QVERIFY(!page.findChild<QPushButton *>("nightTimeAcquireLocation")->isEnabled()); QTest::keyClick(page.findChild<NightTimeMapWidget *>(),Qt::Key_Right); QVERIFY(!session->isDirty()); QCOMPARE(backend->writes,0);
    }
    void editorAndDaemonInputsStayDistinctAndCachedAgeIsUnknown() {
        backend->current.fields[NightTimeKey::Latitude].value=22.0; backend->current.fields[NightTimeKey::Latitude].globalPresent=true; backend->current.fields[NightTimeKey::Latitude].localPresent=false; backend->current.cachedLocation={true,40.0,10.0,{}}; session->refresh(); NightTimePage page(nullptr,session,location,nullptr);
        const auto detail=page.findChild<QLabel *>("nightTimeInputs")->text(); QVERIFY(detail.contains("Saved editor: 22")); QVERIFY(detail.contains("Projected daemon input: 0")); QVERIFY(page.findChild<QLabel *>("nightTimeForecastDetails")->text().contains("age is unknown")); QCOMPARE(constructions,0); QCOMPARE(backend->writes,0);
    }
    void defaultsAreDisabledAfterReadFailureAndDraftConflict() {
        backend->native(NightTimeKey::Source,1); backend->native(NightTimeKey::TransitionDuration,uint(2400)); session->refresh(); NightTimePage page(nullptr,session,location,nullptr);
        auto *defaults=page.findChild<QPushButton *>("nightTimeDefaults"); auto *sourceControl=page.findChild<QComboBox *>("nightTimeSource"); auto *duration=page.findChild<QSpinBox *>("nightTimeDuration");
        QVERIFY(defaults->isEnabled()); QVERIFY(sourceControl->actions().first()->isEnabled()); QVERIFY(duration->actions().first()->isEnabled());
        backend->holdRead=true; session->refresh(); backend->failRead(); QVERIFY(!session->isReady()); QVERIFY(!defaults->isEnabled()); QVERIFY(!sourceControl->actions().first()->isEnabled()); QVERIFY(!duration->actions().first()->isEnabled()); defaults->click(); QVERIFY(!session->isDirty()); QCOMPARE(backend->writes,0);
        backend->holdRead=false; session->revert(); QVERIFY(defaults->isEnabled()); session->selectDuration(3000); backend->native(NightTimeKey::SunriseStart,QTime(7,0)); backend->changed(); QVERIFY(session->hasConflict());
        QVERIFY(!defaults->isEnabled()); QVERIFY(!sourceControl->actions().first()->isEnabled()); QVERIFY(!duration->actions().first()->isEnabled()); defaults->click(); QCOMPARE(session->staged().fields[NightTimeKey::TransitionDuration].value.toUInt(),uint(3000)); QCOMPARE(backend->writes,0);
    }
    void threeUnknownTimeFieldsCanBeRepairedWithoutInventingCompanions() {
        backend->native(NightTimeKey::Source,1);
        for (auto key:{NightTimeKey::SunriseStart,NightTimeKey::SunsetStart,NightTimeKey::TransitionDuration}) { backend->current.fields[key].known=false; backend->current.daemonFields[key].known=false; }
        session->refresh(); NightTimePage page(nullptr,session,location,nullptr);
        QVERIFY(editor(page,"nightTimeDuration")->text().isEmpty()); type(editor(page,"nightTimeDuration"),QStringLiteral("60")); finish(editor(page,"nightTimeDuration"));
        QCOMPARE(session->staged().fields[NightTimeKey::TransitionDuration].value.toUInt(),uint(3600)); QCOMPARE(session->staged().intents.size(),1); QVERIFY(!session->staged().fields[NightTimeKey::SunriseStart].known); QVERIFY(!session->staged().fields[NightTimeKey::SunsetStart].known); QVERIFY(!session->canApply());
        page.findChild<QTimeEdit *>("nightTimeSunrise")->setDisplayFormat(QStringLiteral("HH:mm")); page.findChild<QTimeEdit *>("nightTimeSunset")->setDisplayFormat(QStringLiteral("HH:mm"));
        type(editor(page,"nightTimeSunrise"),QStringLiteral("07:00")); finish(editor(page,"nightTimeSunrise")); QCOMPARE(session->staged().intents.size(),2); QVERIFY(!session->staged().fields[NightTimeKey::SunsetStart].known); QVERIFY(!session->canApply());
        type(editor(page,"nightTimeSunset"),QStringLiteral("19:00")); finish(editor(page,"nightTimeSunset")); QCOMPARE(session->staged().fields[NightTimeKey::SunriseStart].value.toTime(),QTime(7,0)); QCOMPARE(session->staged().fields[NightTimeKey::SunsetStart].value.toTime(),QTime(19,0)); QCOMPARE(session->staged().intents.size(),3); QVERIFY(session->canApply()); QCOMPARE(backend->writes,0); QCOMPARE(constructions,0);
    }
    void defaultsAreReviewableCancelableAndStagedOnly() {
        backend->native(NightTimeKey::Source,1); backend->native(NightTimeKey::TransitionDuration,uint(2400)); session->refresh(); NightTimePage page(nullptr,session,location,nullptr);
        QTimer::singleShot(0,&page,[&page]{ auto *dialog=page.findChild<QDialog *>("nightTimeDefaultsDialog"); QVERIFY(dialog); dialog->reject(); }); page.findChild<QPushButton *>("nightTimeDefaults")->click(); QVERIFY(!session->isDirty());
        QTimer::singleShot(0,&page,[&page]{ auto *dialog=page.findChild<QDialog *>("nightTimeDefaultsDialog"); QVERIFY(dialog); bool beforeAfter=false; for (auto *label:dialog->findChildren<QLabel *>()) beforeAfter|=label->text().contains("2400 seconds → 1800 seconds"); QVERIFY(beforeAfter); dialog->accept(); }); page.findChild<QPushButton *>("nightTimeDefaults")->click(); QVERIFY(session->isDirty()); QCOMPARE(session->staged().fields[NightTimeKey::TransitionDuration].value.toUInt(),uint(1800)); QCOMPARE(backend->writes,0);
    }
    void staleDefaultsReviewCannotReplaceANewerDraft() {
        backend->native(NightTimeKey::Source,1); session->refresh(); NightTimePage page(nullptr,session,location,nullptr);
        QTimer::singleShot(0,&page,[&page,this]{ session->selectDuration(2400); auto *dialog=page.findChild<QDialog *>("nightTimeDefaultsDialog"); QVERIFY(dialog); dialog->accept(); }); page.findChild<QPushButton *>("nightTimeDefaults")->click();
        QCOMPARE(session->staged().fields[NightTimeKey::TransitionDuration].value.toUInt(),uint(2400)); QVERIFY(page.findChild<QLabel *>("nightTimeMessage")->text().contains("stale")); QCOMPARE(backend->writes,0);
    }
    void applyIsExplicitAndVerifiedSaveNeverClaimsDaemonAdoption() {
        backend->native(NightTimeKey::Source,1); session->refresh(); NightTimePage page(nullptr,session,location,nullptr); type(editor(page,"nightTimeDuration"),QStringLiteral("40")); finish(editor(page,"nightTimeDuration")); QCOMPARE(backend->writes,0);
        page.findChild<QPushButton *>("nightTimeApply")->click(); QCOMPARE(backend->writes,1); backend->complete(); QVERIFY(!session->isDirty()); QVERIFY(page.findChild<QLabel *>("nightTimeMessage")->text().contains("loaded behavior is not verified")); QCOMPARE(constructions,0);
    }
private:
    PageNightTimeBackend *backend=nullptr; NightTimeSettings *session=nullptr; NightTimeLocation *location=nullptr;
    QPointer<PageLocationSource> source; int constructions=0,requests=0,stops=0;
};
QTEST_MAIN(NightTimePageTest)
#include "night_time_page_test.moc"
