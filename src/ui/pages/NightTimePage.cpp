#include "NightTimePage.h"
#include "NightTimeMapWidget.h"
#include "NightTimePreviewWidget.h"
#include "Win7Ui.h"
#include "Win7FormStyle.h"
#include "settings/NightTimeForecast.h"
#include <QAction>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleValidator>
#include <QGridLayout>
#include <QGroupBox>
#include <QHideEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QTimeEdit>
#include <QTimeZone>
#include <functional>
#include <cmath>

namespace {
class MinuteSpin final : public QSpinBox {
public: std::function<void(int)> stepped;
protected: void stepBy(int steps) override { const QSignalBlocker blocker(this); QSpinBox::stepBy(steps); if (stepped) stepped(value()); }
};
class BoundaryTime final : public QTimeEdit {
public: std::function<void(QTime)> stepped;
protected: void stepBy(int steps) override { const QSignalBlocker blocker(this); QTimeEdit::stepBy(steps); if (stepped) stepped(time()); }
};
bool known(const NightTimeSnapshot &s,NightTimeKey key) { return s.fields.value(key).known || s.intents.contains(key); }
QString textValue(NightTimeKey key,const NightTimeField &field)
{
    if (!field.known) return QObject::tr("Unavailable");
    const auto value=field.value;
    switch (key) {
    case NightTimeKey::Source:
        if (value.toInt()==0) return QObject::tr("Location");
        if (value.toInt()==1) return QObject::tr("Custom times");
        return QObject::tr("Unsupported source (%1)").arg(value.toString());
    case NightTimeKey::Automatic: return value.toBool()?QObject::tr("Automatic"):QObject::tr("Manual");
    case NightTimeKey::Latitude: case NightTimeKey::Longitude: return QString::number(value.toDouble(),'g',17)+QChar(0x00b0);
    case NightTimeKey::SunriseStart: case NightTimeKey::SunsetStart: return value.toTime().isValid()?value.toTime().toString(QStringLiteral("HH:mm:ss.zzz")):QObject::tr("Invalid native time");
    case NightTimeKey::TransitionDuration: return QObject::tr("%1 seconds").arg(value.toString());
    }
    return {};
}
QString origin(const NightTimeSnapshot &s,NightTimeKey key)
{
    if (s.intents.contains(key)) return s.intents.value(key)==NightTimeIntent::Set?QObject::tr("staged explicit value"):QObject::tr("staged native default");
    const auto f=s.fields.value(key);
    if (!f.known) return QObject::tr("effective value unavailable");
    if (!f.effectivePresent && !f.localPresent && f.globalPresent) return QObject::tr("global override masked; native fallback");
    if (f.localPresent) return QObject::tr("saved local override");
    if (f.inheritedPresent) return QObject::tr("inherited value");
    if (f.globalPresent) return QObject::tr("global override");
    return QObject::tr("no saved override");
}
bool canReset(const NightTimeSnapshot &s,const NightTimeSnapshot &baseline,NightTimeKey key)
{ auto copy=s; QString error; return NightTimeTypes::restore(copy,baseline,key,error) && !NightTimeTypes::sameDraft(copy,s); }
QString tooltip(const QString &text) { return QStringLiteral("<qt>%1</qt>").arg(text.toHtmlEscaped().replace('\n',QStringLiteral("<br/>"))); }
QString exact(const QDateTime &time) { return time.isValid()?time.toString(Qt::ISODateWithMs):QObject::tr("Unavailable"); }
QLabel *plain(const QString &text,const char *name=nullptr)
{ auto *label=Win7::bodyLabel(text); label->setTextFormat(Qt::PlainText); if (name) label->setObjectName(QString::fromLatin1(name)); return label; }
bool locationAllowed(const NightTimeSettings *session,bool allowRefresh=false)
{
    if (!session || !session->isReady() || session->hasConflict() || (session->isBusy() && !(allowRefresh && session->isRefreshing()))) return false;
    const auto r=session->runtime(); return r.installedVerified && (!r.serviceAvailable || (r.providerVerified && !r.owner.isEmpty() && r.apiVersion==1));
}
QString timeFormat(const QLocale &locale)
{ QString format=locale.timeFormat(QLocale::ShortFormat); format.remove(QRegularExpression(QStringLiteral("[:.]s{1,2}"))); return format; }
}

NightTimePage::NightTimePage(QScrollArea *sidebar,QWidget *parent)
    : NightTimePage(sidebar,NightTimeSettings::instance(),new NightTimeLocation,parent) { m_location->setParent(this); }
NightTimePage::NightTimePage(QScrollArea *sidebar,NightTimeSettings *session,NightTimeLocation *location,QWidget *parent)
    : QWidget(parent),m_session(session),m_location(location)
{
    Q_ASSERT(session); Q_ASSERT(location);
    auto *layout=Win7::pageScaffold(this,sidebar,20,800);
    setProperty("settingsScope",QStringLiteral("nightTime"));
    Win7::styleSettingsControls(this); Win7::styleSettingsFields(this);
    setStyleSheet(styleSheet()+QStringLiteral(
        " QLineEdit#nightTimeLatitude, QLineEdit#nightTimeLongitude { color: #000000; background: #FFFFFF;"
        " border: 1px solid #ABADB3; border-radius: 2px; border-image: none; padding: 4px 6px;"
        " selection-background-color: #CDE6F7; selection-color: #000000; }"
        " QLineEdit#nightTimeLatitude:focus, QLineEdit#nightTimeLongitude:focus { border-color: #3C7FB1; }"
        " QLineEdit#nightTimeLatitude:disabled, QLineEdit#nightTimeLongitude:disabled { color: #777777; background: #F0F0F0; border-color: #C5C5C5; }"
        " QTimeEdit { color: #000000; background: #FFFFFF; border: 1px solid #ABADB3; border-radius: 2px;"
        " border-image: none; padding: 3px 20px 3px 6px; selection-background-color: #CDE6F7; selection-color: #000000; }"
        " QTimeEdit:focus { border-color: #3C7FB1; }"
        " QTimeEdit:disabled { color: #777777; background: #F0F0F0; border-color: #C5C5C5; }"
        " QTimeEdit::up-button { subcontrol-origin: border; subcontrol-position: top right; width: 18px;"
        " border-left: 1px solid #D4D4D4; background: #F1F1F1; border-image: none; }"
        " QTimeEdit::down-button { subcontrol-origin: border; subcontrol-position: bottom right; width: 18px;"
        " border-left: 1px solid #D4D4D4; background: #E8E8E8; border-image: none; }"
        " QTimeEdit::up-arrow { image: url(:/ControlPanel/fields/spin-up.svg); width: 7px; height: 4px; }"
        " QTimeEdit::down-arrow { image: url(:/ControlPanel/fields/spin-down.svg); width: 7px; height: 4px; }"
        " QRadioButton { color: #000000; background: transparent; spacing: 6px; border-image: none; }"
        " QRadioButton:disabled { color: #777777; }"
        " QRadioButton::indicator { width: 13px; height: 13px; border: 1px solid #8E8F8F; border-radius: 7px; border-image: none; background: #FFFFFF; image: none; }"
        " QRadioButton::indicator:checked { background: qradialgradient(cx:0.5,cy:0.5,radius:0.5,fx:0.5,fy:0.5,stop:0 #244967,stop:0.48 #244967,stop:0.52 #FFFFFF,stop:1 #FFFFFF); }"
        " QRadioButton::indicator:disabled { border-color: #B8B8B8; background: #F0F0F0; }"
        " QRadioButton::indicator:checked:disabled { background: qradialgradient(cx:0.5,cy:0.5,radius:0.5,fx:0.5,fy:0.5,stop:0 #92999E,stop:0.48 #92999E,stop:0.52 #F0F0F0,stop:1 #F0F0F0); }"));
    layout->addWidget(Win7::pageTitle(tr("Day and Night"))); layout->addSpacing(8);
    layout->addWidget(plain(tr("Choose when the day and night periods begin. Changes remain in the draft until Apply. The cycle below is calculated from the daemon's projected inputs; it is not an acknowledgement that the running service adopted them.")));
    auto *sourceForm=new QGridLayout; auto *sourceLabel=Win7::label(tr("Schedule &source:"));
    m_source=new QComboBox; m_source->setObjectName(QStringLiteral("nightTimeSource")); m_source->setAccessibleName(tr("Schedule source")); sourceLabel->setBuddy(m_source);
    sourceForm->addWidget(sourceLabel,0,0); sourceForm->addWidget(m_source,0,1); sourceForm->setColumnStretch(1,1); layout->addLayout(sourceForm);
    m_privacy=plain({},"nightTimePrivacy"); layout->addWidget(m_privacy);
    m_preview=new NightTimePreviewWidget; layout->addWidget(m_preview);
    m_forecast=plain({},"nightTimeForecastDetails"); layout->addWidget(m_forecast);

    m_locationGroup=new QGroupBox(tr("Location")); m_locationGroup->setObjectName(QStringLiteral("nightTimeLocationGroup")); auto *locationLayout=new QVBoxLayout(m_locationGroup);
    auto *radios=new QHBoxLayout; m_automatic=new QRadioButton(tr("&Automatic location")); m_automatic->setObjectName(QStringLiteral("nightTimeAutomatic")); m_automatic->setAccessibleName(tr("Automatic location"));
    m_manual=new QRadioButton(tr("&Manual location")); m_manual->setObjectName(QStringLiteral("nightTimeManual")); m_manual->setAccessibleName(tr("Manual location")); radios->addWidget(m_automatic); radios->addWidget(m_manual); radios->addStretch(); locationLayout->addLayout(radios);
    auto *coordinateForm=new QGridLayout; coordinateForm->setColumnStretch(1,1);
    auto addCoordinate=[&](Editor &editor,NightTimeKey key,int row,const QString &caption,const char *name) {
        editor.key=key; editor.line=new QLineEdit; editor.control=editor.line; editor.line->setObjectName(QString::fromLatin1(name)); editor.line->setAccessibleName(NightTimeTypes::label(key));
        auto *validator=new QDoubleValidator(key==NightTimeKey::Latitude?-90:-180,key==NightTimeKey::Latitude?90:180,17,editor.line); validator->setNotation(QDoubleValidator::ScientificNotation); editor.line->setValidator(validator);
        editor.line->setPlaceholderText(tr("Enter a replacement coordinate")); auto *label=Win7::label(caption); label->setBuddy(editor.line); coordinateForm->addWidget(label,row,0); coordinateForm->addWidget(editor.line,row,1);
        editor.description=plain({},(QByteArray(name)+"Description").constData()); coordinateForm->addWidget(editor.description,row+1,1); defaultAction(editor.control,key);
    };
    addCoordinate(m_latitude,NightTimeKey::Latitude,0,tr("La&titude:"),"nightTimeLatitude"); addCoordinate(m_longitude,NightTimeKey::Longitude,2,tr("L&ongitude:"),"nightTimeLongitude"); locationLayout->addLayout(coordinateForm);
    m_map=new NightTimeMapWidget; m_map->setObjectName(QStringLiteral("nightTimeMap")); locationLayout->addWidget(m_map);
    auto *mapButtons=new QHBoxLayout;
    auto *zoomIn=new QPushButton(tr("Zoom &in")),*zoomOut=new QPushButton(tr("Zoom o&ut")),*resetMap=new QPushButton(tr("Reset map &view"));
    zoomIn->setObjectName(QStringLiteral("nightTimeMapZoomIn")); zoomOut->setObjectName(QStringLiteral("nightTimeMapZoomOut")); resetMap->setObjectName(QStringLiteral("nightTimeMapReset"));
    mapButtons->addWidget(zoomIn); mapButtons->addWidget(zoomOut); mapButtons->addWidget(resetMap); mapButtons->addStretch(); locationLayout->addLayout(mapButtons);
    locationLayout->addWidget(plain(tr("Choose a point to stage both coordinates and Manual location together. Arrow keys choose a point; Shift moves 10 degrees, Alt moves 0.1 degrees. Drag to pan. Zoom and pan do not change preferences.")));
    locationLayout->addWidget(plain(tr("Offline world map: TUBS / Wikimedia Commons, modified; Creative Commons Attribution-ShareAlike 3.0 (CC BY-SA 3.0)."),"nightTimeMapAttribution"));
    auto *acquireButtons=new QHBoxLayout; m_acquire=new QPushButton(tr("Get location &once")); m_acquire->setObjectName(QStringLiteral("nightTimeAcquireLocation")); m_cancel=new QPushButton(tr("&Cancel location request")); m_cancel->setObjectName(QStringLiteral("nightTimeCancelLocation")); m_use=new QPushButton(tr("Use this location manuall&y")); m_use->setObjectName(QStringLiteral("nightTimeUseLocation"));
    acquireButtons->addWidget(m_acquire); acquireButtons->addWidget(m_cancel); acquireButtons->addWidget(m_use); acquireButtons->addStretch(); locationLayout->addLayout(acquireButtons);
    m_locationStatus=plain({},"nightTimeLocationStatus"); locationLayout->addWidget(m_locationStatus);
    locationLayout->addWidget(plain(tr("Opening this page does not request location. Get location once explicitly starts the system location provider for up to 30 seconds. The sample stays in this view only; using it manually is a separate staged change. Cancellation stops local tracking and requests provider stop; Qt provides no stop acknowledgement.")));
    layout->addWidget(m_locationGroup);

    m_timesGroup=new QGroupBox(tr("Custom times")); m_timesGroup->setObjectName(QStringLiteral("nightTimeTimesGroup")); auto *timesForm=new QGridLayout(m_timesGroup); timesForm->setColumnStretch(1,1);
    auto addTime=[&](Editor &editor,NightTimeKey key,int row,const QString &caption,const char *name) {
        editor.key=key; auto *control=new BoundaryTime; control->setDisplayFormat(timeFormat(control->locale())); control->setKeyboardTracking(false); control->setObjectName(QString::fromLatin1(name)); control->setAccessibleName(NightTimeTypes::label(key)); editor.control=control; editor.line=control->findChild<QLineEdit *>();
        auto *label=Win7::label(caption); label->setBuddy(control); timesForm->addWidget(label,row,0); timesForm->addWidget(control,row,1); editor.description=plain({},(QByteArray(name)+"Description").constData()); timesForm->addWidget(editor.description,row+1,1);
        auto *entry=&editor; control->stepped=[this,entry,control](QTime value) { if (!m_updating) input(*entry,control->locale().toString(QTime(value.hour(),value.minute()),control->displayFormat())); }; defaultAction(control,key);
    };
    addTime(m_sunrise,NightTimeKey::SunriseStart,0,tr("&Day begins:"),"nightTimeSunrise"); addTime(m_sunset,NightTimeKey::SunsetStart,2,tr("&Night begins:"),"nightTimeSunset");
    m_duration.key=NightTimeKey::TransitionDuration; auto *duration=new MinuteSpin; duration->setObjectName(QStringLiteral("nightTimeDuration")); duration->setAccessibleName(tr("Transition duration in minutes")); duration->setRange(1,600); duration->setSingleStep(5); duration->setKeyboardTracking(false); m_duration.control=duration; m_duration.line=duration->findChild<QLineEdit *>(); m_duration.line->setPlaceholderText(tr("Replacement in minutes"));
    auto *durationLabel=Win7::label(tr("Transition &length (minutes):")); durationLabel->setBuddy(duration); timesForm->addWidget(durationLabel,4,0); timesForm->addWidget(duration,4,1); m_duration.description=plain({},"nightTimeDurationDescription"); timesForm->addWidget(m_duration.description,5,1); defaultAction(duration,m_duration.key);
    duration->stepped=[this,duration](int value) { if (!m_updating) input(m_duration,duration->locale().toString(value)); };
    timesForm->addWidget(plain(tr("Editing a boundary sets its seconds to zero and adjusts only that boundary if needed. Editing the transition length may adjust Night begins atomically. Untouched second precision is preserved.")),6,0,1,2); layout->addWidget(m_timesGroup);

    m_changes=plain({},"nightTimeChanges"); layout->addWidget(m_changes);
    auto *detailsToggle=new QPushButton(tr("Show saved and daemon &inputs")); detailsToggle->setObjectName(QStringLiteral("nightTimeDetailsToggle")); detailsToggle->setCheckable(true); layout->addWidget(detailsToggle);
    m_inputs=plain({},"nightTimeInputs"); m_inputs->setVisible(false); layout->addWidget(m_inputs);
    m_defaultsReview=plain({},"nightTimeDefaultsReview"); m_defaultsReview->setVisible(false); layout->addWidget(m_defaultsReview);
    m_message=plain({},"nightTimeMessage"); layout->addWidget(m_message);
    auto *buttons=new QHBoxLayout; m_defaults=new QPushButton(tr("Restore de&faults…")); m_defaults->setObjectName(QStringLiteral("nightTimeDefaults")); m_refresh=new QPushButton(tr("&Refresh")); m_refresh->setObjectName(QStringLiteral("nightTimeRefresh")); m_revert=new QPushButton(tr("Re&vert")); m_revert->setObjectName(QStringLiteral("nightTimeRevert")); m_apply=new QPushButton(tr("A&pply")); m_apply->setObjectName(QStringLiteral("nightTimeApply"));
    buttons->addWidget(m_defaults); buttons->addWidget(m_refresh); buttons->addStretch(); buttons->addWidget(m_revert); buttons->addWidget(m_apply); layout->addLayout(buttons); layout->addStretch();
    connect(detailsToggle,&QPushButton::toggled,this,[this](bool checked){ m_inputs->setVisible(checked); m_defaultsReview->setVisible(checked); if (m_session) m_session->setProperty("nightTimeDetailsExpanded",checked); });
    detailsToggle->setChecked(session->property("nightTimeDetailsExpanded").toBool());
    connect(m_source,&QComboBox::activated,this,[this](int index){ if (m_updating || !m_session || !m_source->itemData(index).isValid()) return; clearInputs(); cancelLocation(); m_session->selectValue(NightTimeKey::Source,m_source->itemData(index)); });
    connect(m_automatic,&QRadioButton::clicked,this,[this]{ if (m_session) { clearInputs(); cancelLocation(); m_session->selectValue(NightTimeKey::Automatic,true); } });
    connect(m_manual,&QRadioButton::clicked,this,[this]{ if (m_session) { clearInputs(); cancelLocation(); m_session->selectValue(NightTimeKey::Automatic,false); } });
    defaultAction(m_source,NightTimeKey::Source); defaultAction(m_automatic,NightTimeKey::Automatic); defaultAction(m_manual,NightTimeKey::Automatic);
    connect(zoomIn,&QPushButton::clicked,m_map,&NightTimeMapWidget::zoomIn); connect(zoomOut,&QPushButton::clicked,m_map,&NightTimeMapWidget::zoomOut); connect(resetMap,&QPushButton::clicked,m_map,&NightTimeMapWidget::resetView);
    connect(m_map,&NightTimeMapWidget::coordinateSelected,this,[this](double latitude,double longitude){ if (m_session) { clearInputs(); cancelLocation(); m_session->selectManualLocation(latitude,longitude); } });
    for (auto *entry:editors()) {
        connect(entry->line,&QLineEdit::textEdited,this,[this,entry](const QString &text){ if (!m_updating) input(*entry,text); });
        connect(entry->line,&QLineEdit::editingFinished,this,[this,entry]{ if (!m_updating) commitInput(*entry); });
    }
    connect(m_acquire,&QPushButton::clicked,this,&NightTimePage::requestLocation); connect(m_cancel,&QPushButton::clicked,this,[this]{ cancelLocation(); m_locationMessage=tr("Location request cancelled. No preferences were changed."); updateState(); });
    connect(m_use,&QPushButton::clicked,this,[this]{ if (m_session && m_haveFix) { clearInputs(); m_session->selectManualLocation(m_fix.latitude,m_fix.longitude); } });
    connect(location,&NightTimeLocation::busyChanged,this,[this]{ updateState(); });
    connect(location,&QObject::destroyed,this,[this]{ m_requestActive=false; m_locationMessage=tr("The location acquisition service is no longer available."); updateState(); });
    connect(location,&NightTimeLocation::finished,this,[this](quint64 token,bool success,const NightTimeLocationFix &fix,const QString &error){
        if (!m_requestActive || token!=m_locationToken || !m_session) return;
        m_requestActive=false;
        auto observed=m_session->applied(); observed.runtime=m_session->runtime();
        if (!locationAllowed(m_session,true) || !NightTimeTypes::sameConfiguration(m_locationBaseline,observed) || !NightTimeTypes::sameDraft(m_locationDraft,m_session->staged()) || m_locationBaseline.runtime.owner!=m_session->runtime().owner || m_locationBaseline.runtime.epoch!=m_session->runtime().epoch) { m_locationMessage=tr("Location result discarded because the saved settings, draft or provider changed."); updateState(); return; }
        m_haveFix=success && fix.timestamp.isValid() && std::isfinite(fix.latitude) && std::isfinite(fix.longitude) && qAbs(fix.latitude)<=90 && qAbs(fix.longitude)<=180;
        if (m_haveFix) { m_fix=fix; m_locationMessage=tr("One-shot sample: %1°, %2°\nSample time: %3\nHorizontal accuracy: %4. This sample has not been saved.").arg(QString::number(fix.latitude,'g',17),QString::number(fix.longitude,'g',17),exact(fix.timestamp),fix.horizontalAccuracy>=0?tr("%1 metres").arg(QString::number(fix.horizontalAccuracy,'g',17)):tr("unavailable")); }
        else m_locationMessage=error.isEmpty()?tr("The location provider returned no valid sample."):error;
        updateState();
    });
    connect(m_refresh,&QPushButton::clicked,this,[this]{ if (m_session && finishInputs()) m_session->refresh(); });
    connect(m_revert,&QPushButton::clicked,this,[this]{ if (m_session) { clearInputs(); cancelLocation(); m_session->revert(); } });
    connect(m_apply,&QPushButton::clicked,this,[this]{ if (m_session && finishInputs()) m_session->apply(); });
    connect(m_defaults,&QPushButton::clicked,this,[this]{
        if (!m_session || !m_session->isReady() || m_session->isBusy() || m_session->hasConflict()) return; clearInputs(); const auto baseline=m_session->applied(),draft=m_session->staged();
        QStringList reviewLines;
        for (auto key:NightTimeTypes::keys()) { auto reset=draft; QString error; if (NightTimeTypes::restore(reset,baseline,key,error) && !NightTimeTypes::sameDraft(reset,draft)) reviewLines<<tr("%1: %2 → %3").arg(NightTimeTypes::label(key),textValue(key,draft.fields.value(key)),textValue(key,reset.fields.value(key))); }
        const QString review=(reviewLines+m_session->defaultsUnavailable()).join('\n');
        auto *dialog=new QDialog(this); QPointer<QDialog> guard(dialog); QPointer<NightTimePage> page(this); dialog->setObjectName(QStringLiteral("nightTimeDefaultsDialog")); dialog->setWindowTitle(tr("Review Day and Night defaults")); dialog->resize(580,340); Win7::styleSettingsControls(dialog); Win7::styleSettingsFields(dialog);
        auto *box=new QVBoxLayout(dialog); box->addWidget(plain(tr("These changes will be staged only. Review them before applying:\n\n%1").arg(review))); auto *actions=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel); actions->button(QDialogButtonBox::Ok)->setText(tr("Stage defaults")); box->addWidget(actions); connect(actions,&QDialogButtonBox::accepted,dialog,&QDialog::accept); connect(actions,&QDialogButtonBox::rejected,dialog,&QDialog::reject);
        const int result=dialog->exec(); if (!page) return; if (guard) guard->deleteLater(); if (result!=QDialog::Accepted || !m_session) return;
        auto observed=m_session->applied(); observed.runtime=m_session->runtime();
        if (!NightTimeTypes::sameConfiguration(baseline,observed) || !NightTimeTypes::sameDraft(draft,m_session->staged()) || baseline.runtime.owner!=m_session->runtime().owner || baseline.runtime.epoch!=m_session->runtime().epoch || !m_session->isReady() || m_session->isBusy() || m_session->hasConflict()) { m_inputMessage=tr("The defaults review is stale. Review the current settings again."); updateState(); return; }
        m_session->restoreDefaults();
    });
    connect(session,&SettingsSession::stateChanged,this,&NightTimePage::updateState);
    connect(session,&QObject::destroyed,this,[this]{ cancelLocation(); updateState(); });
    updateState();
}
NightTimePage::~NightTimePage() { cancelLocation(); }
void NightTimePage::hideEvent(QHideEvent *event) { cancelLocation(); QWidget::hideEvent(event); }
SettingsSession *NightTimePage::settingsSession() const { return m_session; }
QList<SidebarLink> NightTimePage::sidebarLinks() { return {Nav::to(tr("Night Light"),PageId::NightLight)}; }
QList<SidebarLink> NightTimePage::sidebarSeeAlso() { return {Nav::to(tr("Screen Resolution"),PageId::Display),Nav::to(tr("Personalization"),PageId::Personalization)}; }
QList<NightTimePage::Editor *> NightTimePage::editors() { return {&m_latitude,&m_longitude,&m_sunrise,&m_sunset,&m_duration}; }

bool NightTimePage::inUse(NightTimeKey key) const
{
    if (!m_session) return false; const auto &s=m_session->staged();
    if (!known(s,NightTimeKey::Source) || !NightTimeTypes::validValue(NightTimeKey::Source,s.fields.value(NightTimeKey::Source).value)) return false;
    const bool location=s.fields.value(NightTimeKey::Source).value.toInt()==NightTimeTypes::LocationSource;
    if (key==NightTimeKey::Automatic) return location;
    if (key==NightTimeKey::Latitude || key==NightTimeKey::Longitude) return location && known(s,NightTimeKey::Automatic) && !s.fields.value(NightTimeKey::Automatic).value.toBool();
    if (key==NightTimeKey::SunriseStart || key==NightTimeKey::SunsetStart || key==NightTimeKey::TransitionDuration) return !location;
    return true;
}
bool NightTimePage::contextCurrent(const Editor &e) const
{
    if (!m_session || !inUse(e.key) || m_session->hasConflict()) return false;
    if (!NightTimeTypes::sameConfiguration(e.baseline,m_session->applied()) || !NightTimeTypes::sameDraft(e.expected,m_session->staged())) return false;
    const auto r=m_session->runtime(); const auto old=e.baseline.runtime;
    if (r.owner!=old.owner || r.epoch!=old.epoch || r.version!=old.version || r.apiVersion!=old.apiVersion || r.installedVerified!=old.installedVerified || r.providerVerified!=old.providerVerified || r.serviceAvailable!=old.serviceAvailable) return false;
    const auto f=m_session->staged().fields.value(e.key),daemon=m_session->staged().daemonFields.value(e.key);
    return f.writable && !f.immutable && !daemon.immutable && (!m_session->isBusy() || m_session->isRefreshing());
}
void NightTimePage::input(Editor &e,const QString &text)
{
    if (!m_session) return;
    if (!e.active) {
        if (!m_session->canEdit(e.key) || !inUse(e.key)) { updateState(); return; }
        e.active=true; e.baseline=m_session->applied(); e.baseline.runtime=m_session->runtime(); e.expected=m_session->staged();
    }
    if (!contextCurrent(e)) { m_inputMessage=tr("The pending value '%1' was not staged because the settings or editing policy changed.").arg(text); e.active=false; updateState(); return; }
    e.text=text; e.cursor=e.line->cursorPosition(); e.selectionStart=e.line->selectionStart(); e.selectionLength=e.line->selectedText().size(); e.staged=false; e.deferred=false; e.commit=false; e.candidate={};
    if (e.key==NightTimeKey::Latitude || e.key==NightTimeKey::Longitude) {
        bool ok=false; const double value=e.control->locale().toDouble(text,&ok);
        if (ok && std::isfinite(value) && NightTimeTypes::validValue(e.key,QVariant(value))) e.candidate=value;
    } else if (e.key==NightTimeKey::TransitionDuration) {
        bool ok=false; const uint minutes=e.control->locale().toUInt(text,&ok); if (ok && minutes>=1 && minutes<=600) e.candidate=QVariant::fromValue(uint(minutes*60));
    } else {
        auto *time=qobject_cast<QTimeEdit *>(e.control); QTime parsed=time->locale().toTime(text,time->displayFormat());
        if (parsed.isValid()) e.candidate=QTime(parsed.hour(),parsed.minute(),0,0);
    }
    stageInput(e); updateState();
}
void NightTimePage::stageInput(Editor &e)
{
    // Consume the deferred request even when its text is now incomplete or its
    // context has expired. A later render must not retry the same request.
    e.deferred=false;
    if (!e.active || !m_session || !e.candidate.isValid()) return;
    if (!contextCurrent(e)) { m_inputMessage=tr("The pending value '%1' was not staged because the saved settings, draft or provider changed.").arg(e.text); e.active=false; return; }
    if (m_session->isRefreshing()) { e.deferred=true; return; }
    auto desired=m_session->staged(); QString error; bool valid=false;
    if (e.key==NightTimeKey::TransitionDuration) valid=NightTimeTypes::selectDuration(desired,m_session->applied(),e.candidate.toUInt(),error);
    else if (e.key==NightTimeKey::SunriseStart || e.key==NightTimeKey::SunsetStart) valid=NightTimeTypes::selectTime(desired,m_session->applied(),e.key,e.candidate.toTime(),error);
    else valid=NightTimeTypes::stage(desired,m_session->applied(),e.key,e.candidate,error);
    if (!valid) { m_inputMessage=tr("'%1' was not staged: %2").arg(e.text,error); e.deferred=false; return; }
    e.expected=desired; e.deferred=false; m_ownCommit=true;
    if (e.key==NightTimeKey::TransitionDuration) m_session->selectDuration(e.candidate.toUInt());
    else if (e.key==NightTimeKey::SunriseStart || e.key==NightTimeKey::SunsetStart) m_session->selectTime(e.key,e.candidate.toTime());
    else m_session->selectValue(e.key,e.candidate);
    m_ownCommit=false; e.staged=NightTimeTypes::sameDraft(e.expected,m_session->staged());
    if (e.staged) {
        m_inputMessage.clear();
        // A complete edit to another field must not make an earlier, already
        // staged text session look stale. Its saved baseline remains pinned.
        for (auto *other:editors()) if (other!=&e && other->active && other->staged) other->expected=m_session->staged();
    }
}
void NightTimePage::commitInput(Editor &e)
{
    if (!e.active || !m_session) return;
    if (!e.candidate.isValid()) { m_inputMessage=tr("'%1' was not staged. Enter a valid %2; the actual draft is shown again.").arg(e.text,NightTimeTypes::label(e.key).toLower()); e.active=false; updateState(); return; }
    e.commit=true; if (!e.staged) stageInput(e);
    if (e.deferred) { updateState(); return; }
    if (!e.staged) m_inputMessage=tr("'%1' was not staged. %2").arg(e.text,m_inputMessage);
    e.active=false; updateState();
}
bool NightTimePage::finishInputs()
{
    bool valid=true;
    for (auto *e:editors()) if (e->active) { valid&=e->candidate.isValid(); commitInput(*e); valid&=!e->active && e->staged; }
    return valid;
}
void NightTimePage::clearInputs() { for (auto *e:editors()) *e=Editor{e->key,e->control,e->line,e->description}; m_inputMessage.clear(); }
void NightTimePage::defaultAction(QWidget *control,NightTimeKey key)
{
    auto *action=new QAction(tr("Use system default"),control); control->addAction(action); control->setContextMenuPolicy(Qt::ActionsContextMenu);
    connect(action,&QAction::triggered,this,[this,key]{ if (m_session) { clearInputs(); cancelLocation(); m_session->restoreDefault(key); } });
    connect(m_session,&SettingsSession::stateChanged,action,[this,action,key]{ action->setEnabled(m_session && m_session->canEdit(key) && canReset(m_session->staged(),m_session->applied(),key)); });
    action->setEnabled(m_session && m_session->canEdit(key) && canReset(m_session->staged(),m_session->applied(),key));
}
void NightTimePage::cancelLocation()
{
    if (!m_requestActive) return; m_requestActive=false; ++m_locationToken; if (m_location) m_location->cancel();
}
void NightTimePage::requestLocation()
{
    if (!locationAllowed(m_session) || !m_location || !inUse(NightTimeKey::Automatic)) return;
    cancelLocation(); m_haveFix=false; m_requestActive=true; m_locationBaseline=m_session->applied(); m_locationBaseline.runtime=m_session->runtime(); m_locationDraft=m_session->staged();
    m_locationMessage=tr("Requesting one location sample (up to 30 seconds)… No preferences have been changed."); ++m_locationToken; updateState(); m_location->requestLocation(m_locationToken);
}

void NightTimePage::updateState()
{
    if (m_updating || m_ownCommit) return; m_updating=true;
    if (!m_session) { setEnabled(false); m_message->setText(tr("The settings session is no longer available.")); m_updating=false; return; }
    const auto &s=m_session->staged(), &saved=m_session->applied();
    auto observed=saved; observed.runtime=m_session->runtime();
    if (m_requestActive && (!locationAllowed(m_session,true) || !NightTimeTypes::sameConfiguration(m_locationBaseline,observed) || !NightTimeTypes::sameDraft(m_locationDraft,s) || m_locationBaseline.runtime.owner!=m_session->runtime().owner || m_locationBaseline.runtime.epoch!=m_session->runtime().epoch)) { cancelLocation(); m_locationMessage=tr("Location request cancelled because its settings context changed."); }
    bool invalidPending=false;
    for (auto *e:editors()) {
        if (e->active && !contextCurrent(*e)) {
            if (!e->staged) m_inputMessage=tr("The pending value '%1' was not staged because the saved settings, draft, provider or editing policy changed.").arg(e->text);
            e->active=false; e->deferred=false;
        }
        invalidPending|=e->active && !e->candidate.isValid();
    }
    // Flush each completed deferred edit once before rendering, with signal
    // reentrancy suppressed by m_updating. Rendering never calls itself.
    for (auto *e:editors()) if (e->active && e->deferred && !m_session->isBusy()) {
        stageInput(*e); if (e->commit && !e->deferred) e->active=false;
    }
    const bool sourceKnown=known(s,NightTimeKey::Source) && NightTimeTypes::validValue(NightTimeKey::Source,s.fields.value(NightTimeKey::Source).value);
    const bool location=sourceKnown && s.fields.value(NightTimeKey::Source).value.toInt()==NightTimeTypes::LocationSource;
    { const QSignalBlocker block(m_source); m_source->clear(); m_source->addItem(tr("Location"),0); m_source->addItem(tr("Custom times"),1);
        if (!sourceKnown) { m_source->addItem(textValue(NightTimeKey::Source,s.fields.value(NightTimeKey::Source)),QVariant()); auto *model=qobject_cast<QStandardItemModel *>(m_source->model()); if (model) model->item(2)->setEnabled(false); m_source->setCurrentIndex(2); }
        else m_source->setCurrentIndex(m_source->findData(s.fields.value(NightTimeKey::Source).value.toInt())); }
    m_source->setEnabled(m_session->canEdit(NightTimeKey::Source));
    { const QSignalBlocker a(m_automatic),b(m_manual); m_automatic->setAutoExclusive(false); m_manual->setAutoExclusive(false); const bool valid=known(s,NightTimeKey::Automatic) && NightTimeTypes::validValue(NightTimeKey::Automatic,s.fields.value(NightTimeKey::Automatic).value); m_automatic->setChecked(valid && s.fields.value(NightTimeKey::Automatic).value.toBool()); m_manual->setChecked(valid && !s.fields.value(NightTimeKey::Automatic).value.toBool()); m_automatic->setAutoExclusive(true); m_manual->setAutoExclusive(true); }
    m_automatic->setEnabled(location && m_session->canEdit(NightTimeKey::Automatic)); m_manual->setEnabled(location && m_session->canEdit(NightTimeKey::Automatic));
    m_locationGroup->setVisible(!sourceKnown || location); m_timesGroup->setVisible(!sourceKnown || !location);
    m_locationGroup->setTitle(location?tr("Location"):tr("Location — inactive; values preserved")); m_timesGroup->setTitle(sourceKnown && !location?tr("Custom times"):tr("Custom times — inactive; values preserved"));
    for (auto *e:editors()) {
        const auto f=s.fields.value(e->key); const bool valid=known(s,e->key) && NightTimeTypes::validValue(e->key,f.value);
        const QSignalBlocker controlBlock(e->control), lineBlock(e->line);
        if (auto *validator=qobject_cast<QDoubleValidator *>(const_cast<QValidator *>(e->line->validator()))) validator->setLocale(e->control->locale());
        if (e->active) {
            if (e->line->text()==e->text) {
                e->cursor=e->line->cursorPosition(); e->selectionStart=e->line->selectionStart(); e->selectionLength=e->line->selectedText().size();
            } else e->line->setText(e->text);
            if (e->selectionStart>=0 && e->selectionLength>0) {
                const int start=qMin(e->selectionStart,e->text.size()), length=qMin(e->selectionLength,e->text.size()-start);
                if (e->cursor==e->selectionStart) e->line->setSelection(start+length,-length);
                else e->line->setSelection(start,length);
            } else e->line->setCursorPosition(qMin(e->cursor,e->text.size()));
        }
        else if (e==&m_latitude || e==&m_longitude) e->line->setText(valid?e->control->locale().toString(f.value.toDouble(),'g',17):QString());
        else if (e==&m_duration) { auto *spin=qobject_cast<QSpinBox *>(e->control); const uint raw=f.value.toUInt(); if (valid && raw%60==0 && raw/60>=1 && raw/60<=600) spin->setValue(int(raw/60)); else e->line->clear(); }
        else { auto *time=qobject_cast<QTimeEdit *>(e->control); if (valid) time->setTime(f.value.toTime()); else e->line->clear(); }
        e->control->setEnabled(inUse(e->key) && (m_session->canEdit(e->key) || (e->active && m_session->isRefreshing() && contextCurrent(*e))));
        QString description=tr("Current draft: %1; %2. Saved: %3.").arg(textValue(e->key,f),origin(s,e->key),textValue(e->key,saved.fields.value(e->key)));
        if (!valid && known(s,e->key)) description+=tr(" Outside the supported editor range; enter an explicit replacement.");
        if (e->active && !e->staged) description+=tr(" Pending text '%1' is not staged%2.").arg(e->text,e->deferred?tr("; waiting for the read to finish"):QString());
        if (!f.reason.isEmpty()) description+=QLatin1Char(' ')+f.reason;
        e->description->setText(description); e->control->setToolTip(tooltip(description));
    }
    auto manual=s; QString manualError; const bool manualSupported=m_session->canEdit(NightTimeKey::Latitude) && m_session->canEdit(NightTimeKey::Longitude) && m_session->canEdit(NightTimeKey::Automatic) && NightTimeTypes::selectManualLocation(manual,saved,0.0,0.0,manualError);
    m_map->setSelectionEnabled(location && !m_session->isBusy() && manualSupported); m_use->setEnabled(location && m_haveFix && !m_session->isBusy() && manualSupported);
    const bool coordinateKnown=known(s,NightTimeKey::Latitude) && known(s,NightTimeKey::Longitude) && NightTimeTypes::validValue(NightTimeKey::Latitude,s.fields.value(NightTimeKey::Latitude).value) && NightTimeTypes::validValue(NightTimeKey::Longitude,s.fields.value(NightTimeKey::Longitude).value);
    m_map->setCoordinate(coordinateKnown,s.fields.value(NightTimeKey::Latitude).value.toDouble(),s.fields.value(NightTimeKey::Longitude).value.toDouble());
    m_acquire->setEnabled(location && m_location && !m_requestActive && locationAllowed(m_session)); m_cancel->setEnabled(m_requestActive);
    m_privacy->setText(m_session->privacyText()); m_locationStatus->setText(m_locationMessage.isEmpty()?tr("No one-shot sample requested. A cached daemon location, if present, has no recorded age."):m_locationMessage);
    QStringList inputs,changes; const auto daemon=NightTimeTypes::projectedDaemonFields(saved,s);
    for (auto key:NightTimeTypes::keys()) {
        inputs<<tr("%1\n  Saved editor: %2 (%3)\n  Draft: %4 (%5)\n  Projected daemon input: %6").arg(NightTimeTypes::label(key),textValue(key,saved.fields.value(key)),origin(saved,key),textValue(key,s.fields.value(key)),origin(s,key),textValue(key,daemon.value(key)));
        if (s.intents.contains(key)) changes<<tr("%1: %2 → %3 (%4)").arg(NightTimeTypes::label(key),textValue(key,saved.fields.value(key)),textValue(key,s.fields.value(key)),origin(s,key));
    }
    m_inputs->setText(inputs.join(QStringLiteral("\n\n"))); m_changes->setText(changes.isEmpty()?tr("No staged preference changes."):tr("Staged changes:\n%1").arg(changes.join('\n')));
    m_defaultsReview->setText(tr("Available defaults:\n%1\nUnavailable defaults:\n%2").arg(m_session->defaultsChanges().join('\n'),m_session->defaultsUnavailable().join('\n')));
    bool actionableDefault=false;
    for (auto key:NightTimeTypes::keys()) actionableDefault|=m_session->canEdit(key) && canReset(s,saved,key);
    m_defaults->setEnabled(m_session->isReady() && !m_session->isBusy() && !m_session->hasConflict() && actionableDefault); m_refresh->setEnabled(!m_session->isBusy()); m_revert->setEnabled(!m_session->isBusy() && (m_session->isDirty() || m_session->hasConflict() || invalidPending)); m_apply->setEnabled(!invalidPending && m_session->isDirty() && m_session->canApply());
    QStringList messages; if (!m_inputMessage.isEmpty()) messages<<m_inputMessage; if (!m_session->errorText().isEmpty()) messages<<m_session->errorText(); if (!m_session->resultText().isEmpty()) messages<<m_session->resultText(); if (!m_session->applySupportText().isEmpty()) messages<<m_session->applySupportText();
    if (m_session->isBusy()) messages<<tr("Reading or saving native preferences…");
    m_message->setText(messages.join('\n')); updateForecast(); m_updating=false;
}

void NightTimePage::updateForecast()
{
    if (!m_session) return;
    using namespace NightTimeForecast;
    const auto fields=NightTimeTypes::projectedDaemonFields(m_session->applied(),m_session->staged());
    Input input; input.reference=QDateTime::currentDateTime();
    QString error; bool valid=NightTimeTypes::activeBranchValid(fields,error); QString coordinateNote;
    if (valid) {
        input.sunrise=fields.value(NightTimeKey::SunriseStart).value.toTime(); input.sunset=fields.value(NightTimeKey::SunsetStart).value.toTime(); input.transitionSeconds=fields.value(NightTimeKey::TransitionDuration).value.toInt();
        if (fields.value(NightTimeKey::Source).value.toInt()==NightTimeTypes::TimesSource) input.source=Source::CustomTimes;
        else if (fields.value(NightTimeKey::Automatic).value.toBool()) {
            input.source=Source::AutomaticLocation;
            if (m_haveFix) { input.coordinate={true,m_fix.latitude,m_fix.longitude,CoordinateOrigin::ExplicitAcquisition,m_fix.timestamp}; coordinateNote=tr("Uses the explicit, unsaved sample from %1.").arg(exact(m_fix.timestamp)); }
            else { const auto cached=m_session->cachedLocation(); input.coordinate={cached.available,cached.latitude,cached.longitude,CoordinateOrigin::DaemonCache,{}}; coordinateNote=cached.available?tr("Uses the daemon's cached coordinate; its age is unknown."):tr("Automatic coordinate unavailable; native custom-time fallback is shown."); }
        } else { input.source=Source::ManualLocation; input.coordinate={true,fields.value(NightTimeKey::Latitude).value.toDouble(),fields.value(NightTimeKey::Longitude).value.toDouble(),CoordinateOrigin::SavedManual,{}}; }
    }
    Result result; if (valid) result=calculate(input); else result.explanation=tr("Projected daemon inputs are unavailable or invalid: %1").arg(error);
    QStringList description; description<<tr("Calculated cycle — projected daemon inputs. Local date: %1. Sun path is schematic, not solar altitude.").arg(input.reference.date().toString(Qt::ISODate)); description<<result.explanation;
    if (!coordinateNote.isEmpty()) description<<coordinateNote;
    if (result.valid) {
        auto describe=[&](const QString &label,const Transition &t){ return tr("%1: %2 begins %3; ends %4.").arg(label,t.morning?tr("day transition"):tr("night transition"),exact(t.start),exact(t.end)); };
        description<<describe(tr("Previous"),result.previous)<<describe(tr("Next"),result.next);
        if (result.fallback==Fallback::PolarDay || result.fallback==Fallback::PolarNight || result.fallback==Fallback::SolarUnavailable) description<<tr("Native solar fallback uses the compiled 06:00 / 18:00 boundaries and a 30-minute transition, independently of the dormant custom times.");
    }
    m_forecast->setText(description.join('\n')); m_preview->setForecast(result,input.reference,m_forecast->text());
}
