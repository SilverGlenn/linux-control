#include "NightLightPage.h"
#include "Win7Ui.h"
#include "Win7FormStyle.h"
#include "settings/NightLightSettings.h"

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QGridLayout>
#include <QHideEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QTimeZone>
#include <limits>
#include <functional>

namespace {
class NightLightTemperatureSpinBox final : public QSpinBox {
public:
    std::function<void(int)> stepped;
protected:
    void stepBy(int steps) override
    {
        const QSignalBlocker blocker(this); QSpinBox::stepBy(steps);
        // A spin step is an explicit new choice, unlike Return's correction of
        // incomplete text. Publish only its final locale-interpreted value.
        if (stepped) stepped(value());
    }
};
bool valueKnown(const NightLightSnapshot &snapshot, NightLightKey key)
{ return snapshot.fields.value(key).known || snapshot.intents.contains(key); }
QString modeName(qint64 mode)
{
    if (mode == NightLightTypes::ConstantMode) return QObject::tr("Constant");
    if (mode == NightLightTypes::DarkLightMode) return QObject::tr("Dark/Light");
    return QObject::tr("Unsupported mode (%1)").arg(mode);
}
QString valueText(const NightLightSnapshot &snapshot, NightLightKey key)
{
    const auto field = snapshot.fields.value(key);
    if (!valueKnown(snapshot,key)) return QObject::tr("Unavailable");
    if (key == NightLightKey::Active) return field.value.toBool() ? QObject::tr("Enabled") : QObject::tr("Disabled");
    if (key == NightLightKey::Mode) return modeName(field.value.toInt());
    const int temperature = field.value.toInt();
    return temperature >= NightLightTypes::MinimumTemperature && temperature <= NightLightTypes::MaximumTemperature
        ? QObject::tr("%1 K").arg(temperature) : QObject::tr("%1 K (outside the supported range)").arg(temperature);
}
QString originText(const NightLightSnapshot &snapshot, NightLightKey key)
{
    if (snapshot.intents.contains(key)) return snapshot.intents.value(key) == NightLightIntent::RestoreDefault
        ? QObject::tr("staged native default") : QObject::tr("staged explicit value");
    const auto field = snapshot.fields.value(key);
    if (!field.known) return QObject::tr("saved value unavailable");
    if (!field.effectivePresent && !field.localPresent && field.globalPresent) return QObject::tr("global override masked; native fallback");
    if (field.localPresent) return QObject::tr("local override");
    if (field.inheritedPresent) return QObject::tr("inherited value");
    if (field.globalPresent) return QObject::tr("global override");
    return QObject::tr("no saved override");
}
bool temperatureInUse(const NightLightSnapshot &draft, NightLightKey key)
{
    const auto active=draft.fields.value(NightLightKey::Active), mode=draft.fields.value(NightLightKey::Mode);
    return valueKnown(draft,NightLightKey::Active) && active.value.toBool() && valueKnown(draft,NightLightKey::Mode)
        && NightLightTypes::validValue(NightLightKey::Mode,mode.value)
        && (key==NightLightKey::NightTemperature || mode.value.toInt()==NightLightTypes::DarkLightMode);
}
QString safeToolTip(const QString &text)
{ return QStringLiteral("<qt>%1</qt>").arg(text.toHtmlEscaped().replace(QLatin1Char('\n'),QStringLiteral("<br/>"))); }
bool canRestore(const NightLightSnapshot &draft, const NightLightSnapshot &baseline, NightLightKey key)
{ auto candidate = draft; QString error; return NightLightTypes::restore(candidate,baseline,key,error) && !NightLightTypes::sameDraft(candidate,draft); }
QDateTime transitionTime(qulonglong seconds)
{
    if (!seconds || seconds > qulonglong(std::numeric_limits<qint64>::max()/1000)) return {};
    return QDateTime::fromSecsSinceEpoch(qint64(seconds),QTimeZone::systemTimeZone());
}
QString exactTime(const QDateTime &date)
{
    const QString text=date.isValid() ? date.toString(Qt::ISODateWithMs) : QString();
    return text.isEmpty() ? QObject::tr("Unavailable") : text;
}
QString transitionText(qulonglong startSeconds, uint durationMilliseconds)
{
    if (!startSeconds) return QObject::tr("No transition time reported.");
    const auto start = transitionTime(startSeconds);
    const bool validEnd = start.isValid() && start.toMSecsSinceEpoch() <= std::numeric_limits<qint64>::max()-qint64(durationMilliseconds);
    const QString end = validEnd ? exactTime(start.addMSecs(durationMilliseconds)) : QObject::tr("Unavailable");
    return QObject::tr("Start: %1\nEnd: %2\nDuration: %3 ms").arg(exactTime(start),end).arg(durationMilliseconds);
}
}

NightLightPage::NightLightPage(QScrollArea *sidebar, QWidget *parent)
    : NightLightPage(sidebar,NightLightSettings::instance(),parent) {}
NightLightPage::NightLightPage(QScrollArea *sidebar, NightLightSettings *session, QWidget *parent)
    : QWidget(parent), m_session(session)
{
    Q_ASSERT(session);
    auto *layout = Win7::pageScaffold(this,sidebar,20,760);
    setProperty("settingsScope",QStringLiteral("nightLight"));
    Win7::styleSettingsControls(this); Win7::styleSettingsFields(this);
    setStyleSheet(styleSheet()+QStringLiteral(
        " QSlider::groove:horizontal { height: 5px; border: 1px solid #AEB8C3; border-radius: 2px; background: qlineargradient(x1:0,y1:0,x2:1,y2:0,stop:0 #EAB080,stop:1 #DBE9FB); }"
        " QSlider::handle:horizontal { width: 12px; margin: -6px 0; border: 1px solid #3C7FB1; border-radius: 2px; background: #D8EDF9; }"
        " QSlider::handle:horizontal:focus { border-color: #D79B00; }"
        " QSlider::groove:horizontal:disabled { background: #ECECEC; border-color: #C5C5C5; }"
        " QSlider::handle:horizontal:disabled { background: #E5E5E5; border-color: #B8B8B8; }"));
    layout->addWidget(Win7::pageTitle(tr("Adjust Night Light"))); layout->addSpacing(10);
    layout->addWidget(Win7::bodyLabel(tr("Choose a warmer display color for comfortable viewing. Changes stay in the draft until Apply. Preview temporarily changes the screen color for 15 seconds without saving your settings.")));
    layout->addSpacing(16); layout->addLayout(Win7::sectionHeading(tr("Night Light settings"))); layout->addSpacing(8);
    m_enabled = new QCheckBox(tr("Enable &Night Light")); m_enabled->setObjectName(QStringLiteral("nightLightEnabled")); m_enabled->setAccessibleName(tr("Enable Night Light")); layout->addWidget(m_enabled);
    auto *form = new QGridLayout; form->setColumnStretch(1,1); form->setHorizontalSpacing(12); form->setVerticalSpacing(10);
    auto *modeLabel = Win7::label(tr("&Mode:")); m_mode = new QComboBox; m_mode->setObjectName(QStringLiteral("nightLightMode")); m_mode->setAccessibleName(tr("Night Light mode")); modeLabel->setBuddy(m_mode);
    form->addWidget(modeLabel,0,0); form->addWidget(m_mode,0,1,1,3);
    auto addTemperature = [&](TemperatureControls &controls, NightLightKey key, int row, const QString &labelText, const QString &prefix) {
        controls.key = key; auto *label = Win7::label(labelText);
        controls.slider = new QSlider(Qt::Horizontal); controls.slider->setObjectName(prefix+QStringLiteral("Slider"));
        controls.slider->setRange(NightLightTypes::MinimumTemperature,NightLightTypes::MaximumTemperature); controls.slider->setSingleStep(100); controls.slider->setPageStep(500); controls.slider->setTracking(false);
        controls.slider->setAccessibleName(NightLightTypes::label(key)+tr(" slider in Kelvin"));
        controls.value = new NightLightTemperatureSpinBox; controls.value->setObjectName(prefix); controls.value->setRange(NightLightTypes::MinimumTemperature,NightLightTypes::MaximumTemperature);
        controls.value->setSuffix(tr(" K")); controls.value->setSingleStep(1); controls.value->setKeyboardTracking(false); controls.value->setMinimumWidth(100);
        controls.value->setAccessibleName(NightLightTypes::label(key)+tr(" in Kelvin")); controls.value->findChild<QLineEdit *>()->setPlaceholderText(tr("Unavailable")); label->setBuddy(controls.value);
        controls.preview = new QPushButton(key == NightLightKey::DayTemperature ? tr("Preview &day") : tr("Preview nigh&t"));
        controls.preview->setObjectName(prefix+QStringLiteral("Preview")); controls.preview->setAccessibleName(tr("Preview %1 for 15 seconds without saving").arg(NightLightTypes::label(key).toLower()));
        controls.description = Win7::bodyLabel({}); controls.description->setTextFormat(Qt::PlainText); controls.description->setObjectName(prefix+QStringLiteral("Description"));
        form->addWidget(label,row,0); form->addWidget(controls.slider,row,1); form->addWidget(controls.value,row,2); form->addWidget(controls.preview,row,3); form->addWidget(controls.description,row+1,1,1,3);
        auto *input=&controls;
        connect(controls.slider,&QSlider::sliderPressed,this,[this,input] {
            if (!beginTemperatureInput(*input)) return; input->gesture=true; input->textInput=false; input->candidateStaged=false;
        });
        connect(controls.slider,&QSlider::sliderMoved,this,[this,input](int value) {
            if (m_updating || !beginTemperatureInput(*input)) return;
            input->gesture=true; input->candidateStaged=false; input->pendingValue=value; input->pendingText=input->value->locale().toString(value)+tr(" K"); input->deferred=true; updateState();
        });
        connect(controls.slider,&QSlider::sliderReleased,this,[this,input] {
            if (m_updating || !input->inputActive) return;
            input->gesture=false; input->pendingValue=input->slider->sliderPosition(); input->pendingText=input->value->locale().toString(input->pendingValue.toInt())+tr(" K"); commitTemperatureInput(*input);
        });
        connect(controls.slider,&QSlider::valueChanged,this,[this,input](int value) {
            if (m_updating || input->slider->isSliderDown() || !beginTemperatureInput(*input)) return;
            input->textInput=false; input->candidateStaged=false; input->pendingValue=value; input->pendingText=input->value->locale().toString(value)+tr(" K"); commitTemperatureInput(*input);
        });
        static_cast<NightLightTemperatureSpinBox *>(controls.value)->stepped=[this,input](int value) {
            if (m_updating || !beginTemperatureInput(*input)) return;
            input->textInput=false; input->candidateStaged=false; input->pendingValue=value; input->pendingText=input->value->locale().toString(value)+tr(" K"); stageTemperatureInput(*input);
        };
        connect(controls.value,&QSpinBox::valueChanged,this,[this,input](int value) {
            if (m_updating || input->textInput || !beginTemperatureInput(*input)) return;
            input->candidateStaged=false; input->pendingValue=value; input->pendingText=input->value->text(); stageTemperatureInput(*input);
        });
        connect(controls.value->findChild<QLineEdit *>(),&QLineEdit::textEdited,this,[this,input] {
            if (m_updating || !beginTemperatureInput(*input)) return;
            auto *line=input->value->findChild<QLineEdit *>(); bool ok=false; const int value=input->value->locale().toInt(input->value->cleanText(),&ok);
            input->textInput=true; input->candidateStaged=false; input->pendingText=line->text(); input->cursor=line->cursorPosition(); input->selectionStart=line->selectionStart(); input->selectionLength=line->selectedText().size();
            input->pendingValue=ok && NightLightTypes::validValue(input->key,value) ? QVariant(value) : QVariant();
            input->commitRequested=false; stageTemperatureInput(*input);
        });
        connect(controls.value,&QSpinBox::editingFinished,this,[this,input] {
            if (m_updating || !m_session) return;
            if (input->inputActive) commitTemperatureInput(*input);
            else if (!valueKnown(m_session->staged(),input->key) || !NightLightTypes::validValue(input->key,m_session->staged().fields.value(input->key).value)) input->value->clear();
        });
        connect(controls.preview,&QPushButton::clicked,this,[this,key] { if (m_session) { m_previewReleaseRequested=false; m_session->previewTemperature(key); } });
        installDefaultAction(controls.value,key); installDefaultAction(controls.slider,key);
    };
    addTemperature(m_day,NightLightKey::DayTemperature,1,tr("Da&y temperature:"),QStringLiteral("nightLightDayTemperature"));
    addTemperature(m_night,NightLightKey::NightTemperature,3,tr("Ni&ght temperature:"),QStringLiteral("nightLightNightTemperature")); layout->addLayout(form);
    installDefaultAction(m_enabled,NightLightKey::Active); installDefaultAction(m_mode,NightLightKey::Mode);
    layout->addWidget(Win7::bodyLabel(tr("Lower Kelvin values are warmer; 6500 K is neutral. The color guide is illustrative. Dark/Light mode follows KDE's separate Day and Night settings. Use Day and Night to configure the shared schedule, custom times and location.")));
    m_previewStatus=Win7::bodyLabel({}); m_previewStatus->setTextFormat(Qt::PlainText); m_previewStatus->setObjectName(QStringLiteral("nightLightPreviewStatus")); layout->addWidget(m_previewStatus);
    auto *configure = new QPushButton(tr("Configure &Day and Night…"));
    configure->setObjectName(QStringLiteral("nightLightConfigureSchedule"));
    configure->setAccessibleName(tr("Configure the shared Day and Night schedule"));
    layout->addWidget(configure, 0, Qt::AlignLeft);
    connect(configure, &QPushButton::clicked, this, [this] { emit navigationRequested(PageId::NightTime); });
    m_stop=new QPushButton(tr("&Stop preview")); m_stop->setObjectName(QStringLiteral("nightLightStopPreview")); m_stop->setAccessibleName(tr("Stop the temporary Night Light preview")); layout->addWidget(m_stop,0,Qt::AlignLeft);
    layout->addSpacing(14); layout->addLayout(Win7::sectionHeading(tr("Saved settings and draft"))); layout->addSpacing(7);
    m_saved=Win7::bodyLabel({}); m_saved->setObjectName(QStringLiteral("nightLightSaved")); m_saved->setTextFormat(Qt::PlainText); layout->addWidget(m_saved);
    m_changes=Win7::bodyLabel({}); m_changes->setObjectName(QStringLiteral("nightLightChanges")); m_changes->setTextFormat(Qt::PlainText); layout->addWidget(m_changes);
    m_defaultsReview=Win7::bodyLabel({}); m_defaultsReview->setObjectName(QStringLiteral("nightLightDefaultsReview")); m_defaultsReview->setTextFormat(Qt::PlainText); layout->addWidget(m_defaultsReview);
    m_defaults=new QPushButton(tr("Restore defa&ults")); m_defaults->setObjectName(QStringLiteral("nightLightRestoreDefaults")); m_defaults->setAccessibleName(tr("Stage available Night Light defaults without applying")); layout->addWidget(m_defaults,0,Qt::AlignLeft);
    layout->addSpacing(14); layout->addLayout(Win7::sectionHeading(tr("Reported runtime status"))); layout->addSpacing(7);
    m_runtime=Win7::bodyLabel({}); m_runtime->setObjectName(QStringLiteral("nightLightRuntime")); m_runtime->setTextFormat(Qt::PlainText); layout->addWidget(m_runtime);
    auto *timeline=new QGridLayout; timeline->setColumnStretch(0,1); timeline->setColumnStretch(1,1);
    timeline->addWidget(Win7::label(tr("Previous transition")),0,0); timeline->addWidget(Win7::label(tr("Scheduled transition")),0,1);
    m_previous=Win7::bodyLabel({}); m_previous->setObjectName(QStringLiteral("nightLightPreviousTransition")); m_previous->setTextFormat(Qt::PlainText);
    m_scheduled=Win7::bodyLabel({}); m_scheduled->setObjectName(QStringLiteral("nightLightScheduledTransition")); m_scheduled->setTextFormat(Qt::PlainText);
    timeline->addWidget(m_previous,1,0); timeline->addWidget(m_scheduled,1,1); layout->addLayout(timeline);
    layout->addWidget(Win7::bodyLabel(tr("Times include the local time-zone offset. Runtime status is reported by KWin and does not verify the physical screen's appearance.")));
    m_message=Win7::bodyLabel({}); m_message->setObjectName(QStringLiteral("nightLightMessage")); m_message->setTextFormat(Qt::PlainText); layout->addWidget(m_message);
    auto *buttons=new QHBoxLayout; m_refresh=new QPushButton(tr("Re&fresh")); m_refresh->setObjectName(QStringLiteral("nightLightRefresh")); buttons->addWidget(m_refresh); buttons->addStretch();
    m_revert=new QPushButton(tr("&Revert")); m_revert->setObjectName(QStringLiteral("nightLightRevert")); m_apply=new QPushButton(tr("&Apply")); m_apply->setObjectName(QStringLiteral("nightLightApply")); buttons->addWidget(m_revert); buttons->addWidget(m_apply); layout->addLayout(buttons); layout->addStretch();
    connect(m_enabled,&QCheckBox::stateChanged,this,[this](int state) { if (!m_updating && m_session && state!=Qt::PartiallyChecked) m_session->selectValue(NightLightKey::Active,state==Qt::Checked); });
    connect(m_mode,&QComboBox::activated,this,[this](int index) { if (!m_updating && m_session && index>=0 && m_mode->itemData(index,Qt::UserRole+1).toBool()) m_session->selectValue(NightLightKey::Mode,m_mode->itemData(index)); });
    connect(m_defaults,&QPushButton::clicked,this,[this] { clearTemperatureInput(m_day); clearTemperatureInput(m_night); m_inputMessage.clear(); if (m_session) m_session->restoreDefaults(); }); connect(m_stop,&QPushButton::clicked,this,[this] { m_previewReleaseRequested=false; releasePreview(); });
    connect(m_refresh,&QPushButton::clicked,this,[this] { if (m_session && finishTemperatureInputs()) m_session->refresh(); });
    connect(m_revert,&QPushButton::clicked,this,[this] { clearTemperatureInput(m_day); clearTemperatureInput(m_night); m_inputMessage.clear(); if (m_session) m_session->revert(); });
    connect(m_apply,&QPushButton::clicked,this,[this] { if (m_session && finishTemperatureInputs()) m_session->apply(); });
    connect(session,&SettingsSession::stateChanged,this,&NightLightPage::updateState); connect(session,&QObject::destroyed,this,&NightLightPage::updateState); updateState();
}
NightLightPage::~NightLightPage() { releasePreview(); }
SettingsSession *NightLightPage::settingsSession() const { return m_session.data(); }
QList<SidebarLink> NightLightPage::sidebarLinks() { return {Nav::to("Screen Resolution",PageId::Display)}; }
QList<SidebarLink> NightLightPage::sidebarSeeAlso() { return {Nav::to("Day and Night",PageId::NightTime),Nav::to("Personalization",PageId::Personalization),Nav::to("Power Options",PageId::PowerOptions)}; }
void NightLightPage::hideEvent(QHideEvent *event) { releasePreview(); QWidget::hideEvent(event); }
void NightLightPage::releasePreview()
{
    if (m_session && m_session->previewActive() && !m_previewReleaseRequested) { m_previewReleaseRequested=true; m_session->stopPreview(); }
}
void NightLightPage::installDefaultAction(QWidget *control, NightLightKey key)
{
    auto *action=new QAction(tr("Use native default"),control); action->setObjectName(QStringLiteral("nightLightFieldDefault")); control->addAction(action); control->setContextMenuPolicy(Qt::ActionsContextMenu);
    control->setProperty("nightLightKey",int(key));
    connect(action,&QAction::triggered,this,[this,key] {
        if (key==m_day.key) clearTemperatureInput(m_day); if (key==m_night.key) clearTemperatureInput(m_night);
        m_inputMessage.clear(); if (m_session) m_session->restoreDefault(key);
    });
}
bool NightLightPage::beginTemperatureInput(TemperatureControls &controls)
{
    if (!m_session) return false;
    if (controls.inputActive) return inputContextCurrent(controls);
    if (!m_session->canEdit(controls.key) || !temperatureInUse(m_session->staged(),controls.key)) return false;
    controls.inputActive=true; controls.baseline=m_session->applied(); controls.expectedDraft=m_session->staged();
    controls.pendingText.clear(); controls.pendingValue=QVariant(); controls.textInput=controls.gesture=controls.deferred=controls.commitRequested=false;
    m_inputMessage.clear(); return true;
}
bool NightLightPage::inputContextCurrent(const TemperatureControls &controls) const
{
    if (!m_session || !controls.inputActive || !m_session->isReady() || m_session->hasConflict()) return false;
    auto observed=controls.baseline; observed.runtime=m_session->runtime();
    return NightLightTypes::sameConfiguration(controls.baseline,m_session->applied())
        && NightLightTypes::sameConfiguration(controls.baseline,observed)
        && NightLightTypes::sameDraft(controls.expectedDraft,m_session->staged())
        && temperatureInUse(m_session->staged(),controls.key)
        && (m_session->canEdit(controls.key) || m_session->isRefreshing());
}
void NightLightPage::stageTemperatureInput(TemperatureControls &controls)
{
    if (!controls.inputActive || !m_session) return;
    if (!inputContextCurrent(controls)) {
        rejectTemperatureInput(controls,tr("its saved field, provider or editing context changed"));
    } else if (!controls.pendingValue.isValid()) {
        controls.deferred=false;
    } else if (m_session->isBusy() || controls.gesture) {
        controls.deferred=true;
    } else {
        auto expected=controls.expectedDraft; QString error;
        if (!NightLightTypes::stage(expected,controls.baseline,controls.key,controls.pendingValue,error)) rejectTemperatureInput(controls,error);
        else {
            // Publish our own expected draft before synchronous stateChanged.
            controls.expectedDraft=expected; m_ownInputCommit=true;
            m_session->selectValue(controls.key,controls.pendingValue); m_ownInputCommit=false;
            if (!m_session || !NightLightTypes::sameDraft(expected,m_session->staged())) rejectTemperatureInput(controls,tr("the controller could not stage that value"));
            else { controls.deferred=false; controls.candidateStaged=true; }
        }
    }
    if (!m_updating) updateState();
}
void NightLightPage::commitTemperatureInput(TemperatureControls &controls)
{
    if (!controls.inputActive) return;
    controls.commitRequested=true;
    if (!controls.pendingValue.isValid()) rejectTemperatureInput(controls,tr("enter a complete temperature from %1 to %2 K").arg(NightLightTypes::MinimumTemperature).arg(NightLightTypes::MaximumTemperature));
    else {
        stageTemperatureInput(controls);
        if (controls.inputActive && !controls.deferred) clearTemperatureInput(controls);
    }
    if (!m_updating) updateState();
}
void NightLightPage::rejectTemperatureInput(TemperatureControls &controls, const QString &reason)
{
    const QString value=controls.pendingText.isEmpty() ? tr("(empty)") : controls.pendingText;
    m_inputMessage=controls.candidateStaged
        ? tr("Editing %1 input '%2' ended: %3. Review the saved settings and current draft.").arg(NightLightTypes::label(controls.key),value,reason)
        : tr("%1 input '%2' was not staged: %3. The current draft is shown.").arg(NightLightTypes::label(controls.key),value,reason);
    clearTemperatureInput(controls);
}
void NightLightPage::clearTemperatureInput(TemperatureControls &controls)
{
    controls.inputActive=controls.textInput=controls.gesture=controls.deferred=controls.commitRequested=controls.candidateStaged=false;
    controls.pendingValue=QVariant(); controls.pendingText.clear(); controls.cursor=controls.selectionStart=-1; controls.selectionLength=0;
    const QSignalBlocker blocker(controls.slider); if (controls.slider->isSliderDown()) controls.slider->setSliderDown(false);
}
bool NightLightPage::finishTemperatureInputs()
{
    bool valid=true;
    for (auto *controls:{&m_day,&m_night}) if (controls->inputActive) {
        if (!controls->pendingValue.isValid() || controls->gesture) valid=false;
        commitTemperatureInput(*controls);
        if (controls->inputActive) valid=false;
    }
    return valid;
}

void NightLightPage::updateState()
{
    if (m_updating || m_ownInputCommit) return;
    if (!m_session) { setEnabled(false); m_message->setText(tr("The Night Light settings session is no longer available.")); return; }
    m_updating=true;
    for (auto *controls:{&m_day,&m_night}) if (controls->inputActive) {
        if (!inputContextCurrent(*controls) || (m_session->isBusy() && !m_session->isRefreshing()))
            rejectTemperatureInput(*controls,tr("the saved settings, provider, mode, policy or operation changed"));
        else if (!m_session->isBusy() && !controls->gesture && controls->deferred) {
            stageTemperatureInput(*controls);
            if (controls->inputActive && controls->commitRequested) clearTemperatureInput(*controls);
        }
    }
    const auto &saved=m_session->applied(), &draft=m_session->staged(); const auto &runtime=m_session->runtime();
    if (!m_session->previewActive()) m_previewReleaseRequested=false;
    const bool ready=m_session->isReady(), idle=ready && !m_session->isBusy() && !m_session->hasConflict();
    const auto active=draft.fields.value(NightLightKey::Active); const bool activeKnown=valueKnown(draft,NightLightKey::Active);
    { const QSignalBlocker blocker(m_enabled); m_enabled->setTristate(!activeKnown); m_enabled->setCheckState(!activeKnown?Qt::PartiallyChecked:active.value.toBool()?Qt::Checked:Qt::Unchecked); }
    m_enabled->setEnabled(m_session->canEdit(NightLightKey::Active));
    const auto mode=draft.fields.value(NightLightKey::Mode); const bool modeKnown=valueKnown(draft,NightLightKey::Mode), supportedMode=modeKnown && NightLightTypes::validValue(NightLightKey::Mode,mode.value);
    { const QSignalBlocker blocker(m_mode); m_mode->clear();
        for (int choice:{NightLightTypes::ConstantMode,NightLightTypes::DarkLightMode}) { m_mode->addItem(choice==NightLightTypes::ConstantMode?tr("Constant — use night temperature"):tr("Dark/Light — follow KDE's day/night settings"),choice); m_mode->setItemData(m_mode->count()-1,true,Qt::UserRole+1); }
        int index=modeKnown?m_mode->findData(mode.value):-1;
        if (index<0) { m_mode->addItem(modeKnown?modeName(mode.value.toInt()):tr("Current mode unavailable"),mode.value); index=m_mode->count()-1; if (auto *model=qobject_cast<QStandardItemModel *>(m_mode->model())) model->item(index)->setEnabled(false); }
        m_mode->setCurrentIndex(index);
    }
    m_mode->setEnabled(m_session->canEdit(NightLightKey::Mode));
    const bool enabled=activeKnown && active.value.toBool();
    for (auto *controls:{&m_day,&m_night}) {
        const auto field=draft.fields.value(controls->key); const bool known=valueKnown(draft,controls->key), valid=known && NightLightTypes::validValue(controls->key,field.value);
        const bool inUse=enabled && supportedMode && (controls->key==NightLightKey::NightTemperature || mode.value.toInt()==NightLightTypes::DarkLightMode);
        const bool preserving=controls->inputActive && inputContextCurrent(*controls);
        const bool editable=inUse && (m_session->canEdit(controls->key) || preserving && m_session->isRefreshing());
        const QSignalBlocker valueBlocker(controls->value), sliderBlocker(controls->slider);
        const int value=valid?field.value.toInt():NightLightTypes::builtinDefault(controls->key).toInt();
        if (!preserving || !controls->textInput) {
            controls->value->setValue(qBound(NightLightTypes::MinimumTemperature,value,NightLightTypes::MaximumTemperature));
            if (!valid) controls->value->clear();
        }
        if (!preserving || !(controls->gesture || !controls->textInput && controls->deferred)) controls->slider->setValue(qBound(NightLightTypes::MinimumTemperature,value,NightLightTypes::MaximumTemperature));
        controls->slider->setVisible(valid || preserving && controls->gesture); controls->slider->setEnabled(editable && (valid || preserving && controls->gesture)); controls->value->setEnabled(editable);
        if (preserving && controls->textInput) {
            auto *line=controls->value->findChild<QLineEdit *>(); if (line->text()!=controls->pendingText) line->setText(controls->pendingText);
            if (controls->selectionStart>=0 && controls->selectionLength>0) line->setSelection(controls->selectionStart,controls->selectionLength);
            else line->setCursorPosition(qBound(0,controls->cursor,int(line->text().size())));
        }
        controls->preview->setEnabled(inUse && m_session->canPreview(controls->key));
        QString reason=field.reason;
        if (!inUse) reason= !enabled ? tr("Temperature controls are inactive while Night Light is disabled or its enabled state is unavailable.")
            : !supportedMode ? tr("Choose a supported mode to edit its temperatures.") : tr("Day temperature is inactive in Constant mode; its saved value is preserved.");
        QString description=tr("%1 — %2%3").arg(valueText(draft,controls->key),originText(draft,controls->key),reason.isEmpty()?QString():QStringLiteral(". ")+reason);
        if (preserving && !controls->pendingValue.isValid()) description+=tr("\nTyped input is incomplete or invalid; it is not staged. Enter %1 to %2 K.").arg(NightLightTypes::MinimumTemperature).arg(NightLightTypes::MaximumTemperature);
        else if (preserving && (controls->gesture || controls->deferred)) description+=tr("\nPending input %1 is not staged yet; waiting for %2.").arg(controls->pendingText,controls->gesture?tr("the slider to be released"):tr("the settings refresh to finish"));
        controls->description->setText(description); controls->value->setToolTip(safeToolTip(description)); controls->value->setAccessibleDescription(description); controls->slider->setAccessibleDescription(description);
    }
    for (auto *control:findChildren<QWidget *>()) if (control->property("nightLightKey").isValid()) {
        const auto key=NightLightKey(control->property("nightLightKey").toInt()); const auto field=draft.fields.value(key);
        for (auto *action:control->actions()) if (action->objectName()==QLatin1String("nightLightFieldDefault")) action->setEnabled(idle && canRestore(draft,saved,key));
        if (control==m_mode || control==m_enabled) { const QString description=originText(draft,key)+(field.reason.isEmpty()?QString():QStringLiteral(". ")+field.reason); control->setToolTip(safeToolTip(description)); control->setAccessibleDescription(description); }
    }
    QStringList savedRows,changes;
    for (auto key:NightLightTypes::keys()) {
        savedRows << tr("%1: %2 (%3)").arg(NightLightTypes::label(key),ready?valueText(saved,key):tr("Unavailable"),originText(saved,key));
        if (draft.intents.contains(key)) changes << tr("%1: %2 → %3 (%4)").arg(NightLightTypes::label(key),valueText(saved,key),valueText(draft,key),originText(draft,key));
    }
    m_saved->setText(savedRows.join(QLatin1Char('\n'))); m_changes->setText(changes.isEmpty()?QString():tr("Changes to apply:\n%1").arg(changes.join(QLatin1Char('\n')))); m_changes->setVisible(!changes.isEmpty());
    const auto defaults=m_session->defaultsChanges(), unavailable=m_session->defaultsUnavailable(); QStringList defaultReview;
    if (!defaults.isEmpty()) defaultReview << tr("Restore defaults will stage:\n%1").arg(defaults.join(QLatin1Char('\n')));
    if (!unavailable.isEmpty()) defaultReview << tr("Defaults left unchanged:\n%1").arg(unavailable.join(QLatin1Char('\n')));
    m_defaultsReview->setText(defaultReview.join(QLatin1Char('\n'))); m_defaultsReview->setVisible(!defaultReview.isEmpty()); m_defaults->setEnabled(idle && !m_session->previewActive() && !defaults.isEmpty());
    QStringList observed;
    if (!runtime.known) observed << tr("Runtime observations are unavailable.");
    else {
        observed << tr("KWin reports: %1; %2; %3.").arg(runtime.available?tr("available"):tr("unavailable"),runtime.enabled?tr("enabled"):tr("disabled"),runtime.running?tr("running"):tr("not running"));
        observed << (runtime.inhibited?tr("Night Light is currently inhibited."):tr("KWin reports no inhibition."));
        observed << tr("Reported mode: %1. Reported period: %2.").arg(modeName(qint64(runtime.mode)),runtime.daylight?tr("daytime"):tr("nighttime"));
        observed << tr("Reported current temperature: %1 K. Target temperature: %2 K.").arg(runtime.currentTemperature).arg(runtime.targetTemperature);
    }
    if (!runtime.reason.isEmpty()) observed << runtime.reason; m_runtime->setText(observed.join(QLatin1Char('\n')));
    m_previous->setText(runtime.known?transitionText(runtime.previousTransitionDateTime,runtime.previousTransitionDuration):tr("Transition observations are unavailable."));
    m_scheduled->setText(runtime.known?transitionText(runtime.scheduledTransitionDateTime,runtime.scheduledTransitionDuration):tr("Transition observations are unavailable."));
    m_previewStatus->setText(m_session->previewText()); m_previewStatus->setVisible(!m_previewStatus->text().isEmpty()); m_stop->setEnabled(m_session->previewActive());
    QStringList messages; if (!m_inputMessage.isEmpty()) messages << m_inputMessage; if (m_session->isBusy()) messages << tr("Reading or applying Night Light settings…");
    if (!m_session->errorText().isEmpty()) messages << m_session->errorText();
    if (m_session->hasConflict()) messages << tr("Your draft is preserved. Revert reloads the saved settings before another Apply.");
    const auto support=m_session->applySupportText(); if (!support.isEmpty() && !messages.contains(support)) messages << support;
    if (!m_session->resultText().isEmpty()) messages << m_session->resultText(); m_message->setText(messages.join(QLatin1Char('\n'))); m_message->setVisible(!messages.isEmpty());
    m_message->setStyleSheet(m_session->hasConflict() || !m_session->errorText().isEmpty()?QStringLiteral("color: #A02C20; background: transparent;"):QStringLiteral("color: #333333; background: transparent;"));
    m_apply->setEnabled(m_session->isDirty() && m_session->canApply()); m_revert->setEnabled(!m_session->isBusy()); m_refresh->setEnabled(!m_session->isBusy()); m_updating=false;
}
