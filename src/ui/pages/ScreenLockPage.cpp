#include "ScreenLockPage.h"
#include "Win7Ui.h"
#include "Win7FormStyle.h"
#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QEvent>
#include <QGridLayout>
#include <QGroupBox>
#include <QIntValidator>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStandardItemModel>
#include <QTimer>
#include <cmath>

namespace {
constexpr int CustomChoice=-100,CurrentChoice=-101,NoPasswordChoice=-1;
class IntervalLineEdit final : public QLineEdit {
protected:
    void changeEvent(QEvent *event) override {
        QLineEdit::changeEvent(event);
        if (event->type()==QEvent::LocaleChange) if (auto *integer=qobject_cast<QIntValidator *>(const_cast<QValidator *>(validator()))) integer->setLocale(locale());
    }
};
QLabel *plain(const QString &text,const char *name=nullptr)
{ auto *label=Win7::bodyLabel(text); label->setTextFormat(Qt::PlainText); if (name) label->setObjectName(QString::fromLatin1(name)); return label; }
QString escaped(const QString &text) { return QStringLiteral("<qt>%1</qt>").arg(text.toHtmlEscaped().replace('\n',QStringLiteral("<br/>"))); }
bool known(const ScreenLockSnapshot &s,ScreenLockKey key) { return s.fields.value(key).known || s.intents.contains(key); }
bool knownBool(const ScreenLockSnapshot &s,ScreenLockKey key) { return known(s,key) && s.fields.value(key).value.metaType().id()==QMetaType::Bool; }
QString valueText(ScreenLockKey key,const ScreenLockField &f)
{
    if (!f.known) return QObject::tr("Unavailable");
    if (key==ScreenLockKey::Timeout) return QObject::tr("%1 minutes").arg(QString::number(f.value.toDouble(),'g',17));
    if (key==ScreenLockKey::LockGrace) return QObject::tr("%1 seconds").arg(f.value.toString());
    if (f.value.metaType().id()==QMetaType::Bool) return f.value.toBool()?QObject::tr("Enabled"):QObject::tr("Disabled");
    return QObject::tr("Unsupported raw value: %1").arg(f.value.toString());
}
QString origin(const ScreenLockSnapshot &s,ScreenLockKey key)
{
    if (s.intents.contains(key)) return s.intents.value(key)==ScreenLockIntent::Set?QObject::tr("staged explicit value"):QObject::tr("staged native default");
    const auto f=s.fields.value(key); if (!f.known) return QObject::tr("effective value unavailable");
    if (!f.effectivePresent && !f.localPresent && f.globalPresent) return QObject::tr("global override masked; native fallback");
    if (f.localPresent) return QObject::tr("saved local override");
    if (f.inheritedPresent) return QObject::tr("inherited value");
    if (f.globalPresent) return QObject::tr("global override");
    return QObject::tr("no saved override");
}
bool resettable(const ScreenLockSnapshot &s,const ScreenLockSnapshot &baseline,ScreenLockKey key)
{ auto next=s; QString error; return ScreenLockTypes::restore(next,baseline,key,error) && !ScreenLockTypes::sameDraft(next,s); }
void setChoiceEnabled(QComboBox *combo,int row,bool enabled)
{ if (auto *model=qobject_cast<QStandardItemModel *>(combo->model())) if (auto *item=model->item(row)) item->setEnabled(enabled); }
bool dryTimeout(const ScreenLockSnapshot &s,const ScreenLockSnapshot &baseline,double value)
{ auto next=s; QString error; return ScreenLockTypes::selectTimeout(next,baseline,value,error); }
bool dryGrace(const ScreenLockSnapshot &s,const ScreenLockSnapshot &baseline,int value)
{ auto next=s; QString error; return value==NoPasswordChoice?ScreenLockTypes::selectNoPassword(next,baseline,error):ScreenLockTypes::selectGrace(next,baseline,value,error); }
QString resetDestination(const ScreenLockSnapshot &baseline,const ScreenLockSnapshot &candidate,ScreenLockKey key)
{
    if (!candidate.intents.contains(key)) return QObject::tr("Cancel the pending edit; the saved entry stays unchanged.");
    const auto field=baseline.fields.value(key);
    const QString entry=QStringLiteral("[%1] %2").arg(ScreenLockTypes::groupName(key),ScreenLockTypes::keyName(key));
    if (field.inheritedPresent) return QObject::tr("Write the inherited native default as a typed local value in user kscreenlockerrc, %1; the inherited source is unchanged.").arg(entry);
    if (field.localPresent) return QObject::tr("Reset the local entry in user kscreenlockerrc, %1; KDE removes the override or writes a deletion mask for a global fallback.").arg(entry);
    if (field.globalPresent) return QObject::tr("Reset the global-only entry in user kdeglobals, %1; no local kscreenlockerrc value is materialized.").arg(entry);
    return QObject::tr("Apply native reset semantics in user kscreenlockerrc, %1.").arg(entry);
}
QString intervalText(int seconds)
{
    if (seconds==0) return QObject::tr("Immediately");
    if (seconds%60==0) return QObject::tr("After %1 minute(s)").arg(seconds/60);
    return QObject::tr("After %1 seconds").arg(seconds);
}
}

ScreenLockPage::ScreenLockPage(QScrollArea *sidebar,QWidget *parent)
    : ScreenLockPage(sidebar,ScreenLockSettings::instance(),ScreenLockAppearance::readCurrent(),parent) {}
ScreenLockPage::ScreenLockPage(QScrollArea *sidebar,ScreenLockSettings *session,const ScreenLockAppearance::Evidence &appearance,QWidget *parent)
    : QWidget(parent),m_session(session),m_appearance(appearance)
{
    Q_ASSERT(session); auto *layout=Win7::pageScaffold(this,sidebar,20,780); setProperty("settingsScope",QStringLiteral("screenLock"));
    Win7::styleSettingsControls(this); Win7::styleSettingsFields(this);
    layout->addWidget(Win7::pageTitle(tr("Screen Locking"))); layout->addSpacing(8);
    layout->addWidget(plain(tr("Choose when your session locks and when a password is required. Changes stay in the draft until Apply.")));
    layout->addSpacing(12); layout->addLayout(Win7::sectionHeading(tr("Lock settings")));
    auto *form=new QGridLayout; form->setColumnStretch(1,1); form->setHorizontalSpacing(12); form->setVerticalSpacing(10);
    auto *timeoutLabel=Win7::label(tr("Lock automatically af&ter:")); m_timeout=new QComboBox; m_timeout->setObjectName(QStringLiteral("screenLockTimeout")); m_timeout->setAccessibleName(tr("Automatic screen lock timeout")); timeoutLabel->setBuddy(m_timeout); form->addWidget(timeoutLabel,0,0); form->addWidget(m_timeout,0,1);
    m_timeoutDescription=plain({},"screenLockTimeoutDescription"); form->addWidget(m_timeoutDescription,1,1);
    auto *graceLabel=Win7::label(tr("Require a &password:")); m_grace=new QComboBox; m_grace->setObjectName(QStringLiteral("screenLockGrace")); m_grace->setAccessibleName(tr("Password requirement and grace interval")); graceLabel->setBuddy(m_grace); form->addWidget(graceLabel,2,0); form->addWidget(m_grace,2,1);
    m_graceDescription=plain({},"screenLockGraceDescription"); form->addWidget(m_graceDescription,3,1);
    m_resume=new QCheckBox(tr("Lock when res&uming from sleep")); m_resume->setObjectName(QStringLiteral("screenLockResume")); m_resume->setAccessibleName(tr("Lock when resuming from sleep")); form->addWidget(m_resume,4,0,1,2); layout->addLayout(form);
    m_effect=plain({},"screenLockEffect"); layout->addWidget(m_effect);
    auto *shortcut=new QPushButton(tr("Edit lock screen &shortcut…")); shortcut->setObjectName(QStringLiteral("screenLockEditShortcut")); shortcut->setAccessibleName(tr("Edit the existing global lock screen shortcut")); auto *shortcutRow=new QHBoxLayout; shortcutRow->addWidget(shortcut); shortcutRow->addStretch(); layout->addLayout(shortcutRow);
    layout->addSpacing(12); layout->addLayout(Win7::sectionHeading(tr("Lock screen appearance")));
    m_appearanceSummary=plain({},"screenLockAppearanceSummary"); layout->addWidget(m_appearanceSummary);
    auto *appearanceButton=new QPushButton(tr("&Login and lock appearance…")); appearanceButton->setObjectName(QStringLiteral("screenLockEditAppearance")); appearanceButton->setAccessibleName(tr("Configure the shared Aero login and lock appearance")); auto *appearanceRow=new QHBoxLayout; appearanceRow->addWidget(appearanceButton); appearanceRow->addStretch(); layout->addLayout(appearanceRow);
    connect(appearanceButton,&QPushButton::clicked,this,&ScreenLockPage::appearanceRequested);
    auto *detailsToggle=new QPushButton(tr("Show configuration &details")); detailsToggle->setObjectName(QStringLiteral("screenLockDetailsToggle")); detailsToggle->setAccessibleName(tr("Show or hide screen locking configuration details")); detailsToggle->setCheckable(true); auto *detailsRow=new QHBoxLayout; detailsRow->addWidget(detailsToggle); detailsRow->addStretch(); layout->addLayout(detailsRow);
    m_details=new QWidget; m_details->setObjectName(QStringLiteral("screenLockDetails")); auto *detailsLayout=new QVBoxLayout(m_details); detailsLayout->setContentsMargins(0,0,0,0);
    m_saved=plain({},"screenLockSaved"); detailsLayout->addWidget(m_saved); m_configuration=plain({},"screenLockConfiguration"); detailsLayout->addWidget(m_configuration);
    m_appearanceText=plain({},"screenLockAppearance"); m_appearanceText->setTextInteractionFlags(Qt::TextSelectableByMouse|Qt::TextSelectableByKeyboard); detailsLayout->addWidget(m_appearanceText); layout->addWidget(m_details); m_details->setVisible(false);
    connect(detailsToggle,&QPushButton::toggled,this,[this,detailsToggle](bool expanded){ m_details->setVisible(expanded); detailsToggle->setText(expanded?tr("Hide configuration &details"):tr("Show configuration &details")); if (m_session) m_session->setProperty("screenLockDetailsExpanded",expanded); }); detailsToggle->setChecked(session->property("screenLockDetailsExpanded").toBool());
    m_changes=plain({},"screenLockChanges"); layout->addWidget(m_changes); m_message=plain({},"screenLockMessage"); layout->addWidget(m_message);
    auto *buttons=new QHBoxLayout; m_defaults=new QPushButton(tr("Restore de&faults…")); m_defaults->setObjectName(QStringLiteral("screenLockDefaults")); m_refresh=new QPushButton(tr("&Refresh")); m_refresh->setObjectName(QStringLiteral("screenLockRefresh")); m_revert=new QPushButton(tr("Re&vert")); m_revert->setObjectName(QStringLiteral("screenLockRevert")); m_apply=new QPushButton(tr("&Apply")); m_apply->setObjectName(QStringLiteral("screenLockApply")); buttons->addWidget(m_defaults); buttons->addWidget(m_refresh); buttons->addStretch(); buttons->addWidget(m_revert); buttons->addWidget(m_apply); layout->addLayout(buttons); layout->addStretch();
    defaultAction(m_timeout,ScreenLockKey::Timeout); defaultAction(m_timeout,ScreenLockKey::Autolock); defaultAction(m_grace,ScreenLockKey::LockGrace); defaultAction(m_grace,ScreenLockKey::RequirePassword); defaultAction(m_resume,ScreenLockKey::LockOnResume);
    connect(m_timeout,&QComboBox::activated,this,&ScreenLockPage::activateTimeout); connect(m_grace,&QComboBox::activated,this,&ScreenLockPage::activateGrace);
    connect(m_resume,&QCheckBox::clicked,this,[this](bool checked){ if (m_session) m_session->selectValue(ScreenLockKey::LockOnResume,checked); });
    connect(shortcut,&QPushButton::clicked,this,&ScreenLockPage::editLockShortcutRequested);
    connect(m_defaults,&QPushButton::clicked,this,&ScreenLockPage::reviewDefaults);
    connect(m_refresh,&QPushButton::clicked,this,[this]{ if (m_session) m_session->refresh(); }); connect(m_revert,&QPushButton::clicked,this,[this]{ m_inputMessage.clear(); if (m_session) m_session->revert(); }); connect(m_apply,&QPushButton::clicked,this,[this]{ if (m_session) m_session->apply(); });
    connect(session,&SettingsSession::stateChanged,this,&ScreenLockPage::updateState); connect(session,&QObject::destroyed,this,&ScreenLockPage::updateState);
    updateState();
}
SettingsSession *ScreenLockPage::settingsSession() const { return m_session; }
QList<SidebarLink> ScreenLockPage::sidebarLinks() { return {Nav::to(tr("Aero Login and Lock Appearance"),PageId::AeroLogin),Nav::to(tr("Power Options"),PageId::PowerOptions)}; }
QList<SidebarLink> ScreenLockPage::sidebarSeeAlso() { return {Nav::to(tr("Personalization"),PageId::Personalization),Nav::to(tr("Global Keyboard Shortcuts"),PageId::Shortcuts)}; }
void ScreenLockPage::defaultAction(QWidget *control,ScreenLockKey key)
{
    auto *action=new QAction(tr("Use system default — %1").arg(ScreenLockTypes::label(key)),control); action->setObjectName(QStringLiteral("screenLockFieldDefault")); action->setProperty("screenLockKey",int(key)); control->addAction(action); control->setContextMenuPolicy(Qt::ActionsContextMenu);
    connect(action,&QAction::triggered,this,[this,key]{ m_inputMessage.clear(); if (m_session) m_session->restoreDefault(key); });
    auto update=[this,action,key]{ action->setEnabled(m_session && m_session->canEdit(key) && resettable(m_session->staged(),m_session->applied(),key)); }; connect(m_session,&SettingsSession::stateChanged,action,update); update();
}
bool ScreenLockPage::currentContext(const ScreenLockSnapshot &saved,const ScreenLockSnapshot &draft) const
{
    if (!m_session || !m_session->isReady() || m_session->isBusy() || m_session->hasConflict()) return false;
    auto observed=m_session->applied(); observed.runtime=m_session->runtime();
    return ScreenLockTypes::sameConfiguration(saved,observed) && ScreenLockTypes::sameDraft(draft,m_session->staged());
}
void ScreenLockPage::activateTimeout(int index)
{
    if (m_updating || !m_session) return; const int choice=m_timeout->itemData(index).toInt(); m_inputMessage.clear();
    if (choice==CustomChoice) { if (!m_dialogPending) { m_dialogPending=true; QTimer::singleShot(0,this,[this]{ QPointer<ScreenLockPage> alive(this); customInterval(false); if (alive) m_dialogPending=false; }); } }
    else if (choice!=CurrentChoice) queueChoice(false,choice);
}
void ScreenLockPage::activateGrace(int index)
{
    if (m_updating || !m_session) return; const int choice=m_grace->itemData(index).toInt(); m_inputMessage.clear();
    if (choice==CustomChoice) { if (!m_dialogPending) { m_dialogPending=true; QTimer::singleShot(0,this,[this]{ QPointer<ScreenLockPage> alive(this); customInterval(true); if (alive) m_dialogPending=false; }); } }
    else if (choice!=CurrentChoice) queueChoice(true,choice);
}
void ScreenLockPage::queueChoice(bool grace,int choice)
{
    auto baseline=m_session->applied(); baseline.runtime=m_session->runtime(); const auto draft=m_session->staged();
    // QComboBox emits textActivated after activated. Stage only after both
    // emissions return, so observers may safely destroy this view on a change.
    QTimer::singleShot(0,this,[this,grace,choice,baseline,draft]{
        const auto key=grace?(choice==NoPasswordChoice?ScreenLockKey::RequirePassword:ScreenLockKey::LockGrace):ScreenLockKey::Timeout;
        const bool allowed=currentContext(baseline,draft) && m_session->canEdit(key)
            && (!grace || (knownBool(m_session->staged(),ScreenLockKey::Autolock) && m_session->staged().fields.value(ScreenLockKey::Autolock).value.toBool()))
            && (grace?dryGrace(m_session->staged(),m_session->applied(),choice):dryTimeout(m_session->staged(),m_session->applied(),double(choice)));
        if (!allowed) { m_inputMessage=tr("The selected %1 was not staged because the preferences, draft, provider or editing policy changed. Review the current values and choose again.").arg(grace?tr("password interval"):tr("automatic timeout")); updateState(); return; }
        QPointer<ScreenLockPage> page(this); m_inputMessage.clear();
        if (!grace) m_session->selectTimeout(double(choice));
        else if (choice==NoPasswordChoice) m_session->selectNoPassword(); else m_session->selectGrace(choice);
        if (page) updateState();
    });
}

void ScreenLockPage::customInterval(bool grace)
{
    if (!m_session || !m_session->canEdit(grace?ScreenLockKey::LockGrace:ScreenLockKey::Timeout)) { updateState(); return; }
    const auto baseline=m_session->applied(),draft=m_session->staged(); const auto key=grace?ScreenLockKey::LockGrace:ScreenLockKey::Timeout;
    if (grace && (!knownBool(draft,ScreenLockKey::Autolock) || !draft.fields.value(ScreenLockKey::Autolock).value.toBool())) { updateState(); return; }
    auto *dialog=new QDialog(this); QPointer<QDialog> guard(dialog); QPointer<ScreenLockPage> page(this); dialog->setObjectName(QStringLiteral("screenLockCustomDialog")); dialog->setWindowTitle(grace?tr("Custom password interval"):tr("Custom automatic lock timeout")); dialog->resize(540,260);
    Win7::styleSettingsControls(dialog); Win7::styleSettingsFields(dialog);
    dialog->setStyleSheet(dialog->styleSheet()+QStringLiteral(" QLineEdit#screenLockCustomValue { color: #000000; background: #FFFFFF; border-image: none; border: 1px solid #ABADB3; border-radius: 2px; padding: 4px 6px; selection-background-color: #CDE6F7; selection-color: #000000; } QLineEdit#screenLockCustomValue:focus { border-color: #3C7FB1; }"));
    auto *layout=new QVBoxLayout(dialog); layout->addWidget(plain(tr("Current draft: %1 (%2).\nSaved: %3.\nEnter an explicit whole-number replacement. Cancel preserves every preference.").arg(valueText(key,draft.fields.value(key)),origin(draft,key),valueText(key,baseline.fields.value(key))),"screenLockCustomCurrent"));
    auto *form=new QGridLayout; auto *label=Win7::label(tr("&New interval:")); auto *number=new IntervalLineEdit; number->setLocale(dialog->locale()); number->setObjectName(QStringLiteral("screenLockCustomValue")); number->setAccessibleName(tr("Custom interval, a whole number from 1 to 9999")); auto *validator=new QIntValidator(1,9999,number); validator->setLocale(number->locale()); number->setValidator(validator); label->setBuddy(number); form->addWidget(label,0,0); form->addWidget(number,0,1);
    auto *units=new QComboBox; units->setObjectName(QStringLiteral("screenLockCustomUnit")); units->setAccessibleName(tr("Custom interval unit")); if (grace) units->addItem(tr("seconds"),1); units->addItem(tr("minutes"),60); form->addWidget(units,0,2); layout->addLayout(form);
    int proposal=grace?30:5;
    if (known(draft,key)) {
        if (grace) { const int raw=draft.fields.value(key).value.toInt(); if (raw>0 && raw<=9999) proposal=raw; else if (raw>0 && raw%60==0 && raw/60<=9999) { proposal=raw/60; units->setCurrentIndex(units->findData(60)); } }
        else { const double raw=draft.fields.value(key).value.toDouble(); if (std::isfinite(raw) && raw>=1 && raw<=9999 && std::floor(raw)==raw) proposal=int(raw); }
    }
    number->setText(number->locale().toString(proposal)); number->selectAll();
    layout->addWidget(plain(grace?tr("Seconds: 1–9999, or minutes: 1–9999 (up to 599940 seconds). Selecting an interval also stages Require password when needed. Disabling password protection affects other screen-lock entry points too."):tr("Minutes: 1–9999. Selecting a positive timeout also stages automatic locking when needed. The saved legacy lock behavior is preserved.")));
    auto *error=plain({},"screenLockCustomError"); layout->addWidget(error); auto *buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel); buttons->button(QDialogButtonBox::Ok)->setText(tr("Stage interval")); layout->addWidget(buttons); int accepted=0;
    connect(buttons,&QDialogButtonBox::rejected,dialog,&QDialog::reject);
    connect(buttons,&QDialogButtonBox::accepted,dialog,[number,units,error,dialog,&accepted]{ bool ok=false; const int value=number->locale().toInt(number->text(),&ok); if (!ok || value<1 || value>9999) { error->setText(QObject::tr("Enter a complete whole number from 1 to 9999. No preference has been staged.")); number->setFocus(); return; } accepted=value*units->currentData().toInt(); dialog->accept(); });
    const int result=dialog->exec(); if (!page) return; if (guard) guard->deleteLater();
    if (result==QDialog::Accepted) {
        if (!currentContext(baseline,draft)) m_inputMessage=tr("The custom interval '%1' was not staged because the saved preferences, draft, policy or provider changed. Review the current values and try again.").arg(grace?tr("%1 seconds").arg(accepted):tr("%1 minutes").arg(accepted/60));
        else { m_inputMessage.clear(); if (grace) m_session->selectGrace(accepted); else m_session->selectTimeout(double(accepted)/60); }
    }
    if (page) updateState();
}
void ScreenLockPage::reviewDefaults()
{
    if (!m_session || !m_session->isReady() || m_session->isBusy() || m_session->hasConflict()) return;
    const auto baseline=m_session->applied(),draft=m_session->staged(); QStringList changes;
    for (auto key:ScreenLockTypes::ownedKeys()) { auto candidate=draft; QString error; if (ScreenLockTypes::restore(candidate,baseline,key,error) && !ScreenLockTypes::sameDraft(candidate,draft)) changes<<tr("%1: %2 → %3\n  Saved source: %4. %5").arg(ScreenLockTypes::label(key),valueText(key,draft.fields.value(key)),valueText(key,candidate.fields.value(key)),origin(baseline,key),resetDestination(baseline,candidate,key)); }
    auto *dialog=new QDialog(this); QPointer<QDialog> guard(dialog); QPointer<ScreenLockPage> page(this); dialog->setObjectName(QStringLiteral("screenLockDefaultsDialog")); dialog->setWindowTitle(tr("Review Screen Locking defaults")); dialog->resize(580,340); Win7::styleSettingsControls(dialog); Win7::styleSettingsFields(dialog); auto *layout=new QVBoxLayout(dialog);
    layout->addWidget(plain(tr("Stage defaults for the five preferences managed by this page. Lock screen appearance and legacy lock behavior are preserved. Apply remains a separate action.\n\n%1\n\nUnavailable:\n%2").arg(changes.join('\n'),m_session->defaultsUnavailable().join('\n')))); auto *buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel); buttons->button(QDialogButtonBox::Ok)->setText(tr("Stage defaults")); layout->addWidget(buttons); connect(buttons,&QDialogButtonBox::accepted,dialog,&QDialog::accept); connect(buttons,&QDialogButtonBox::rejected,dialog,&QDialog::reject);
    const int result=dialog->exec(); if (!page) return; if (guard) guard->deleteLater(); if (result==QDialog::Accepted) { if (!currentContext(baseline,draft)) m_inputMessage=tr("The defaults review is stale. Review the current preferences again."); else { m_inputMessage.clear(); m_session->restoreDefaults(); } } if (page) updateState();
}
void ScreenLockPage::updateState()
{
    if (m_updating) return; m_updating=true;
    if (!m_session) { setEnabled(false); m_message->setText(tr("The settings session is no longer available.")); m_updating=false; return; }
    const auto &draft=m_session->staged(),&saved=m_session->applied();
    const bool automaticKnown=knownBool(draft,ScreenLockKey::Autolock),automatic=automaticKnown && draft.fields.value(ScreenLockKey::Autolock).value.toBool();
    const bool passwordKnown=knownBool(draft,ScreenLockKey::RequirePassword),password=passwordKnown && draft.fields.value(ScreenLockKey::RequirePassword).value.toBool();
    { const QSignalBlocker block(m_timeout); m_timeout->clear(); bool any=false;
        for (int minutes:{0,1,2,5,10,15,30}) { m_timeout->addItem(minutes==0?tr("Never"):tr("%1 minute(s)").arg(minutes),minutes); const bool enabled=m_session->canEdit(ScreenLockKey::Timeout) && dryTimeout(draft,saved,minutes); setChoiceEnabled(m_timeout,m_timeout->count()-1,enabled); any|=enabled; }
        const bool custom=m_session->canEdit(ScreenLockKey::Timeout) && dryTimeout(draft,saved,1); m_timeout->addItem(tr("Custom…"),CustomChoice); setChoiceEnabled(m_timeout,m_timeout->count()-1,custom); any|=custom;
        int selected=-1;
        if (automaticKnown && !automatic) selected=m_timeout->findData(0);
        else if (automatic && known(draft,ScreenLockKey::Timeout)) { const double raw=draft.fields.value(ScreenLockKey::Timeout).value.toDouble(); if (std::isfinite(raw) && raw>=0 && raw<=9999 && std::floor(raw)==raw) selected=m_timeout->findData(int(raw)); }
        if (selected<0) { const QString text=automaticKnown?valueText(ScreenLockKey::Timeout,draft.fields.value(ScreenLockKey::Timeout))+tr(" (%1)").arg(origin(draft,ScreenLockKey::Timeout)):tr("Automatic locking preference unavailable"); m_timeout->addItem(text,CurrentChoice); selected=m_timeout->count()-1; setChoiceEnabled(m_timeout,selected,false); }
        m_timeout->setCurrentIndex(selected); m_timeout->setEnabled(any);
    }
    { const QSignalBlocker block(m_grace); m_grace->clear(); bool any=false;
        m_grace->addItem(tr("No password required"),NoPasswordChoice); const bool noPassword=m_session->canEdit(ScreenLockKey::RequirePassword) && dryGrace(draft,saved,NoPasswordChoice); setChoiceEnabled(m_grace,0,noPassword); any|=noPassword;
        for (int seconds:{0,5,30,60,300,900}) { m_grace->addItem(intervalText(seconds),seconds); const bool enabled=m_session->canEdit(ScreenLockKey::LockGrace) && dryGrace(draft,saved,seconds); setChoiceEnabled(m_grace,m_grace->count()-1,enabled); any|=enabled; }
        const bool custom=m_session->canEdit(ScreenLockKey::LockGrace) && dryGrace(draft,saved,1); m_grace->addItem(tr("Custom…"),CustomChoice); setChoiceEnabled(m_grace,m_grace->count()-1,custom); any|=custom;
        int selected=-1;
        if (passwordKnown && !password) selected=m_grace->findData(NoPasswordChoice);
        else if (password && known(draft,ScreenLockKey::LockGrace) && draft.fields.value(ScreenLockKey::LockGrace).value.metaType().id()==QMetaType::Int && draft.fields.value(ScreenLockKey::LockGrace).value.toInt()>=0) selected=m_grace->findData(draft.fields.value(ScreenLockKey::LockGrace).value.toInt());
        if (selected<0) { m_grace->addItem(passwordKnown?valueText(ScreenLockKey::LockGrace,draft.fields.value(ScreenLockKey::LockGrace))+tr(" (%1)").arg(origin(draft,ScreenLockKey::LockGrace)):tr("Password requirement unavailable"),CurrentChoice); selected=m_grace->count()-1; setChoiceEnabled(m_grace,selected,false); }
        m_grace->setCurrentIndex(selected); m_grace->setEnabled(automatic && any);
    }
    { const QSignalBlocker block(m_resume); const bool available=knownBool(draft,ScreenLockKey::LockOnResume); m_resume->setTristate(!available); m_resume->setCheckState(!available?Qt::PartiallyChecked:draft.fields.value(ScreenLockKey::LockOnResume).value.toBool()?Qt::Checked:Qt::Unchecked); m_resume->setEnabled(m_session->canEdit(ScreenLockKey::LockOnResume)); }
    QString timeoutText=tr("Current timer: %1 (%2). Saved: %3.").arg(valueText(ScreenLockKey::Timeout,draft.fields.value(ScreenLockKey::Timeout)),origin(draft,ScreenLockKey::Timeout),valueText(ScreenLockKey::Timeout,saved.fields.value(ScreenLockKey::Timeout)));
    if (automaticKnown && !automatic) timeoutText+=tr(" Automatic locking is disabled; Never is shown while the raw timer is retained. Choosing Never explicitly stages a zero timer.");
    else if (!automaticKnown) timeoutText+=tr(" Choose an explicit timeout or Never to repair the unknown automatic-lock preference.");
    if (known(draft,ScreenLockKey::Timeout) && !ScreenLockTypes::validValue(ScreenLockKey::Timeout,draft.fields.value(ScreenLockKey::Timeout).value)) timeoutText+=tr(" This raw timeout is outside the whole-minute editor range. It is preserved until you explicitly replace it; security behavior is not normalized or tested here.");
    QString timeoutNotice=tr("Retained timeout: %1.").arg(valueText(ScreenLockKey::Timeout,draft.fields.value(ScreenLockKey::Timeout)));
    if (automaticKnown && !automatic) timeoutNotice+=tr(" Automatic locking is disabled; Never is shown while this timer is retained.");
    else if (!automaticKnown) timeoutNotice+=tr(" The automatic-lock preference is unavailable. Choose an explicit timeout or Never.");
    const bool unsupportedTimeout=!known(draft,ScreenLockKey::Timeout) || !ScreenLockTypes::validValue(ScreenLockKey::Timeout,draft.fields.value(ScreenLockKey::Timeout).value);
    if (!known(draft,ScreenLockKey::Timeout)) timeoutNotice+=tr(" The saved timeout is unavailable. Choose an explicit replacement.");
    else if (unsupportedTimeout) timeoutNotice+=tr(" This raw timeout is outside the whole-minute editor range. It is preserved until you explicitly replace it.");
    m_timeoutDescription->setText(timeoutNotice); m_timeoutDescription->setVisible(!automaticKnown || !automatic || unsupportedTimeout || m_timeout->currentData().toInt()==CurrentChoice); m_timeout->setToolTip(escaped(timeoutText));
    QString graceText=tr("Current grace interval: %1 (%2). Saved: %3.").arg(valueText(ScreenLockKey::LockGrace,draft.fields.value(ScreenLockKey::LockGrace)),origin(draft,ScreenLockKey::LockGrace),valueText(ScreenLockKey::LockGrace,saved.fields.value(ScreenLockKey::LockGrace)));
    if (passwordKnown && !password) graceText+=tr(" Password protection is disabled, including other screen-lock entry points. The raw grace interval is retained.");
    if (!automatic) graceText+=tr(" The native editor enables password interval controls only while automatic locking is enabled. Existing values are preserved.");
    if (known(draft,ScreenLockKey::LockGrace) && draft.fields.value(ScreenLockKey::LockGrace).value.toInt()<0) graceText+=tr(" The native negative grace interval is unsupported by this editor and can leave the idle grace period indefinite. Choose an explicit interval to replace it; the raw value is otherwise preserved.");
    QString graceNotice=tr("Retained grace interval: %1.").arg(valueText(ScreenLockKey::LockGrace,draft.fields.value(ScreenLockKey::LockGrace)));
    if (!passwordKnown) graceNotice+=tr(" The password requirement is unavailable. Choose an explicit setting.");
    else if (!password) graceNotice+=tr(" Password protection is disabled, including other screen-lock entry points.");
    if (!automatic) graceNotice+=tr(" Password interval controls are available while automatic locking is enabled. Existing values are preserved.");
    const bool unsupportedGrace=!known(draft,ScreenLockKey::LockGrace) || !ScreenLockTypes::validValue(ScreenLockKey::LockGrace,draft.fields.value(ScreenLockKey::LockGrace).value);
    if (known(draft,ScreenLockKey::LockGrace) && draft.fields.value(ScreenLockKey::LockGrace).value.toInt()<0) graceNotice+=tr(" The negative grace interval can leave the idle grace period indefinite. Choose an explicit interval to replace it.");
    else if (unsupportedGrace) graceNotice+=tr(" The grace interval is unavailable or outside the supported editor range. Choose an explicit interval to replace it.");
    m_graceDescription->setText(graceNotice); m_graceDescription->setVisible(!passwordKnown || !password || !automatic || unsupportedGrace || m_grace->currentData().toInt()==CurrentChoice); m_grace->setToolTip(escaped(graceText));
    const bool lockKnown=knownBool(draft,ScreenLockKey::Lock);
    m_effect->setText(!lockKnown?tr("The saved legacy lock behavior is unavailable. This page preserves it; safe Apply may remain unavailable until the native configuration can be read."):!draft.fields.value(ScreenLockKey::Lock).value.toBool()?tr("The saved legacy automatic-lock flag is disabled. An automatic timeout may blank the display without a protected lock. These controls preserve that flag; enabling automatic timing does not change it."):tr("The saved legacy automatic-lock flag is enabled. Configure acknowledgement confirms only that KDE accepted the reload request; physical locking and password behavior are not tested here."));
    m_effect->setVisible(!lockKnown || !draft.fields.value(ScreenLockKey::Lock).value.toBool());
    m_configuration->setText(timeoutText+QLatin1Char('\n')+graceText+QLatin1Char('\n')+m_effect->text()+tr("\nKDE configure acknowledgement confirms only that the reload request returned. Physical locking and authentication are not tested here. The shortcut button opens the existing global shortcut editor without assigning or invoking a shortcut."));
    QString summary=(m_appearance.approvedIdentity && m_appearance.providerVerified)?tr("Aero lock screen. Open Login and Lock Appearance to review the shared picture."):tr("The Aero lock screen identity could not be verified. Login and Lock Appearance checks the approved installation before allowing edits.");
    if (!m_appearance.backgroundAvailable) summary+=tr(" The background image could not be verified.");
    m_appearanceSummary->setText(summary);
    QStringList appearance;
    appearance<<tr("Aero appearance evidence read when this page opened; Refresh checks scalar preferences only.");
    appearance<<((m_appearance.approvedIdentity && m_appearance.providerVerified)?tr("Approved Aero lock screen appearance (read-only evidence)."):tr("The approved Aero lock screen appearance could not be fully verified."));
    if (!m_appearance.description.isEmpty()) appearance<<m_appearance.description;
    if (m_appearance.backgroundAvailable) appearance<<tr("Shared login / lock background recorded when opened: %1").arg(m_appearance.backgroundPath);
    if (!m_appearance.reason.isEmpty()) appearance<<m_appearance.reason;
    appearance<<tr("Login and Lock Appearance provides the shared picture controls through a reviewed privileged transaction. It checks installation ownership and helper availability separately."); appearance+=m_appearance.issues; m_appearanceText->setText(appearance.join('\n'));
    QStringList savedRows,changes; bool anyDefault=false;
    for (auto key:ScreenLockTypes::ownedKeys()) { savedRows<<tr("%1: %2 (%3)").arg(ScreenLockTypes::label(key),valueText(key,saved.fields.value(key)),origin(saved,key)); if (draft.intents.contains(key)) changes<<tr("%1: %2 → %3 (%4)").arg(ScreenLockTypes::label(key),valueText(key,saved.fields.value(key)),valueText(key,draft.fields.value(key)),origin(draft,key)); anyDefault|=m_session->canEdit(key) && resettable(draft,saved,key); }
    m_saved->setText(tr("Saved preferences:\n%1").arg(savedRows.join('\n'))); m_changes->setText(changes.isEmpty()?tr("No staged preference changes."):tr("Staged changes:\n%1").arg(changes.join('\n')));
    m_changes->setVisible(m_session->isDirty());
    QStringList messages; if (!m_inputMessage.isEmpty()) messages<<m_inputMessage; if (!m_session->errorText().isEmpty()) messages<<m_session->errorText(); if (!m_session->resultText().isEmpty()) messages<<m_session->resultText(); if (!m_session->applySupportText().isEmpty()) messages<<m_session->applySupportText(); if (m_session->isBusy()) messages<<tr("Reading or saving native preferences…"); m_message->setText(messages.join('\n')); m_message->setVisible(!messages.isEmpty());
    m_defaults->setEnabled(m_session->isReady() && !m_session->isBusy() && !m_session->hasConflict() && anyDefault); m_refresh->setEnabled(!m_session->isBusy()); m_revert->setEnabled(!m_session->isBusy() && (m_session->isDirty() || m_session->hasConflict() || !m_session->isReady())); m_apply->setEnabled(m_session->isDirty() && m_session->canApply()); m_updating=false;
}
