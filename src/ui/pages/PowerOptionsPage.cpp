#include "PowerOptionsPage.h"
#include "Win7Ui.h"
#include "Win7FormStyle.h"
#include "settings/PowerPolicyFields.h"

#include <QScrollArea>
#include <QLabel>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QGridLayout>
#include <QFrame>
#include <QFont>
#include <QPalette>
#include <QRadioButton>
#include <QButtonGroup>
#include <QToolButton>
#include <QSignalBlocker>
#include <QMouseEvent>
#include <QEvent>
#include <QComboBox>
#include <QCheckBox>
#include <QSpinBox>
#include <QGroupBox>
#include <QPushButton>
#include <algorithm>
#include <QAction>
#include <QDialog>
#include <QDialogButtonBox>
#include <QPointer>
#include <QStandardItemModel>
#include <QTreeWidget>
#include <QHeaderView>
#include <QPainter>
#include <QFontMetrics>
#include <QTimer>

namespace {
// This review header must remain readable when AeroQt supplies a dark palette.
class PowerDefaultsHeader final : public QHeaderView {
public:
    explicit PowerDefaultsHeader(QWidget *parent) : QHeaderView(Qt::Horizontal,parent) {}
protected:
    void paintSection(QPainter *painter, const QRect &rect, int logicalIndex) const override
    {
        if (!rect.isValid()) return;
        painter->save(); painter->fillRect(rect,QColor("#F4F5F6")); painter->setPen(QColor("#DDDDDD"));
        painter->drawLine(rect.bottomLeft(),rect.bottomRight()); painter->drawLine(rect.topRight(),rect.bottomRight());
        painter->setPen(Qt::black); painter->setFont(font());
        const QString text=model() ? model()->headerData(logicalIndex,orientation(),Qt::DisplayRole).toString() : QString();
        painter->drawText(rect.adjusted(6,0,-6,0),Qt::AlignLeft|Qt::AlignVCenter|Qt::TextSingleLine,text); painter->restore();
    }
};
QString sourceName(const QString &id)
{
    if (id == QLatin1String("AC")) return QObject::tr("Plugged in");
    if (id == QLatin1String("Battery")) return QObject::tr("On battery");
    if (id == QLatin1String("LowBattery")) return QObject::tr("On low battery");
    return id;
}
QString plainToolTip(const QString &text)
{ return text.isEmpty() ? QString() : QStringLiteral("<qt>%1</qt>").arg(text.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br/>"))); }
enum ChoiceKind { ValueChoice, CustomChoice, DefaultChoice, NeverChoice, CurrentChoice };
constexpr int KindRole = Qt::UserRole + 1;
const PowerPolicyProfile *findProfile(const PowerPolicySnapshot &snapshot, const QString &id)
{ for (const auto &profile : snapshot.profiles) if (profile.id == id) return &profile; return nullptr; }
template<class T> bool canRestoreKey(const T &draft, const T &baseline, PowerPolicyKey key)
{ auto candidate=draft; QString error; return PowerPolicyFields::restore(candidate,baseline,key,error); }
bool canRestoreProfileKey(const PowerPolicyProfile &draft, const PowerPolicySnapshot &baseline, PowerPolicyKey key)
{ const auto *saved=findProfile(baseline,draft.id); return saved && canRestoreKey(draft,*saved,key); }
bool knownValue(const PowerPolicyKeyState &state, const QMap<PowerPolicyKey, PowerPolicyIntent> &intents, PowerPolicyKey key)
{ return state.valueKnown || intents.contains(key); }
QString provenance(const PowerPolicyKeyState &state, const QMap<PowerPolicyKey, PowerPolicyIntent> &intents, PowerPolicyKey key)
{
    if (intents.contains(key)) return intents.value(key) == PowerPolicyIntent::RestoreDefault
        ? QObject::tr("Staged: use the native editor default") : QObject::tr("Staged: explicit value");
    if (!state.valueKnown) return state.effectivePresent ? QObject::tr("Current value unavailable") : QObject::tr("Unset; effective value unavailable");
    if (state.localPresent) return QObject::tr("Current: local override");
    if (state.inheritedPresent) return QObject::tr("Current: inherited value");
    if (state.globalPresent) return QObject::tr("Current: global override");
    return state.defaultKnown ? QObject::tr("Current: native effective default (no override)") : QObject::tr("Current: no explicit override");
}
void disableItem(QComboBox *combo, int index)
{ if (auto *model = qobject_cast<QStandardItemModel *>(combo->model())) if (auto *item = model->item(index)) item->setEnabled(false); }
void renderChoice(QComboBox *combo, PowerPolicyKey key, const QVariant &value, const PowerPolicyKeyState &state,
                  const QMap<PowerPolicyKey, PowerPolicyIntent> &intents, bool restoreAvailable, const QList<PowerPolicyAction> &actions = {})
{
    const QSignalBlocker blocker(combo); combo->clear();
    const bool timeout = key == PowerPolicyKey::DisplaySeconds || key == PowerPolicyKey::IdleSeconds
        || key == PowerPolicyKey::DimSeconds || key == PowerPolicyKey::LockedDisplaySeconds;
    if (timeout) {
        if (key == PowerPolicyKey::LockedDisplaySeconds) { combo->addItem(QObject::tr("Same as unlocked"), -2); combo->addItem(QObject::tr("Immediately"), 0); }
        const QList<int> seconds = key == PowerPolicyKey::DisplaySeconds || key == PowerPolicyKey::IdleSeconds
            ? QList<int>{60,120,180,300,600,900,1200,1800,2700,3600,7200,10800,18000}
            : QList<int>{10,30,60,120,300,600,900,1800};
        for (int duration : seconds) combo->addItem(duration >= 60 && duration % 60 == 0
            ? QObject::tr("%1 minutes").arg(duration / 60) : QObject::tr("%1 seconds").arg(duration), duration);
    } else for (const auto &action : actions) combo->addItem(action.label, action.value);
    int current = knownValue(state, intents, key) ? combo->findData(value) : -1;
    if (current < 0) {
        QString text;
        const bool staged=intents.contains(key), nativeDefault=staged && intents.value(key)==PowerPolicyIntent::RestoreDefault;
        const QString origin=nativeDefault ? QObject::tr("native editor default, staged") : staged ? QObject::tr("staged custom") : QObject::tr("current");
        if (!knownValue(state, intents, key)) text = QObject::tr("Current value unavailable");
        else if (timeout && key == PowerPolicyKey::LockedDisplaySeconds && value.toInt() < 0) text = QObject::tr("Same as unlocked (%1: %2)").arg(origin).arg(value.toInt());
        else if (timeout) text=QObject::tr("%1 seconds (%2)").arg(value.toInt()).arg(origin);
        else text=nativeDefault ? QObject::tr("Native editor default (%1, staged)").arg(value.toUInt())
            : staged ? QObject::tr("Staged value (%1)").arg(value.toUInt()) : QObject::tr("Unsupported current value (%1)").arg(value.toUInt());
        combo->addItem(text, value); current = combo->count() - 1; combo->setItemData(current, CurrentChoice, KindRole); disableItem(combo,current);
    }
    combo->setCurrentIndex(current);
    if (timeout) {
        if (key != PowerPolicyKey::LockedDisplaySeconds) { combo->addItem(QObject::tr("Never (disable this action)"), -1); combo->setItemData(combo->count()-1,NeverChoice,KindRole); }
        combo->addItem(QObject::tr("Custom seconds…")); combo->setItemData(combo->count()-1,CustomChoice,KindRole);
    }
    combo->addItem(QObject::tr("Use system default")); const int reset = combo->count()-1;
    combo->setItemData(reset,DefaultChoice,KindRole);
    if (!restoreAvailable) disableItem(combo,reset);
}
QString actionName(const QList<PowerPolicyAction> &actions, uint value)
{
    for (const auto &action : actions) if (action.value == value) return action.label;
    return QObject::tr("Current action %1").arg(value);
}

}

QStringList PowerOptionsPage::availableProfiles(bool &ok) const
{
    ok = m_service && m_service->isAvailable();
    return m_service ? m_service->availableProfiles() : QStringList();
}

QString PowerOptionsPage::activeProfile() const
{
    return m_service ? m_service->activeProfile() : QString();
}

void PowerOptionsPage::setActiveProfile(const QString &profileId)
{
    if (profileId.isEmpty() || !m_ppdAvailable || !m_service
        || m_service->isBusy())
        return;
    m_service->setActiveProfile(profileId);
    // Keep the old selection visible while the daemon decides whether to apply
    // the request. The service's post-reply read selects the actual value.
    syncSelection();
}

void PowerOptionsPage::setProfileControlsEnabled(bool enabled)
{
    for (QAbstractButton *button : m_group->buttons())
        button->setEnabled(enabled && m_ppdAvailable
                           && !button->property("profileId").toString().isEmpty());
    for (auto it = m_nameToRadio.cbegin(); it != m_nameToRadio.cend(); ++it)
        if (auto *label = qobject_cast<QWidget *>(it.key()))
            label->setEnabled(enabled && m_ppdAvailable);
}

void PowerOptionsPage::updateServiceState()
{
    if (!m_service)
        return;
    m_ppdAvailable = m_service->isAvailable();
    setProfileControlsEnabled(!m_service->isBusy());
    if (m_service->isBusy())
        m_errorLabel->setText(QStringLiteral("Applying the selected power profile…"));
    else
        m_errorLabel->setText(m_service->error());
    m_errorLabel->setVisible(!m_errorLabel->text().isEmpty());
    syncSelection();
}

// Sidebar
QList<SidebarLink> PowerOptionsPage::sidebarLinks()
{
    // "Control Panel Home" is prepended by the sidebar shell itself, so it must
    // not be repeated here.
    return {
        Nav::to("Require a password when the computer wakes", PageId::ScreenLock),
        Nav::to("Choose what the power buttons do", PageId::PowerOptions),
        Nav::plain("Create a power plan"),
        Nav::to("Choose when to turn off the display", PageId::PowerOptions),
        Nav::to("Change when the computer sleeps", PageId::PowerOptions),
    };
}

QList<SidebarLink> PowerOptionsPage::sidebarSeeAlso()
{
    return {
        Nav::to("Personalization", PageId::Personalization),
        Nav::to("User Accounts", PageId::UserAccounts),
    };
}

// Page
PowerOptionsPage::PowerOptionsPage(QScrollArea *sidebar, QWidget *parent)
    : PowerOptionsPage(sidebar, PowerPolicySettings::instance(), parent)
{
}

PowerOptionsPage::PowerOptionsPage(QScrollArea *sidebar, PowerPolicySettings *session, QWidget *parent)
    : PowerOptionsPage(sidebar, session, PowerProfileService::instance(), parent)
{
}

PowerOptionsPage::PowerOptionsPage(QScrollArea *sidebar, PowerPolicySettings *session,
                                 PowerProfileService *profilesService, QWidget *parent)
    : QWidget(parent), m_policy(session)
{
    Q_ASSERT(session);
    Q_ASSERT(profilesService);
    m_service = profilesService;
    bool ok = false;
    QStringList profiles = availableProfiles(ok);
    m_ppdAvailable = ok && !profiles.isEmpty();

    // Fixed-width content column, left-aligned with the rest filled white,
    // matching the Linux Firewall page so this screen sits at the same width
    // as the app's other detail pages.
    auto *contentV = Win7::pageScaffold(this, sidebar, /*bottomMargin=*/20,
                                        /*fixedWidth=*/700);

    Win7::styleSettingsControls(this); Win7::styleSettingsFields(this);

    // Page title.
    contentV->addWidget(Win7::pageTitle("Select a power plan"));
    contentV->addSpacing(10);

    // Describe the supported performance-profile subset, not a plan editor.
    auto *intro = new QLabel(
        "Choose a performance profile advertised by your computer. Changes "
        "apply immediately through the system Power Profiles service. "
        "The timeout, button, lid, brightness and battery settings below are "
        "separate KDE power policies; click Apply to save those changes.");
    {
        QFont f = intro->font();
        f.setPointSize(9);
        intro->setFont(f);
    }
    intro->setWordWrap(true);
    intro->setOpenExternalLinks(false);
    {
        QPalette pal = intro->palette();
        pal.setColor(QPalette::Link, QColor("#1F4E99"));
        intro->setPalette(pal);
    }
    intro->setStyleSheet("color: #1A1A1A; background: transparent;");
    contentV->addWidget(intro);
    m_errorLabel = new QLabel;
    m_errorLabel->setObjectName(QStringLiteral("powerProfileStatus"));
    m_errorLabel->setWordWrap(true);
    m_errorLabel->setTextFormat(Qt::PlainText);
    m_errorLabel->setStyleSheet("color: #8A1C1C; background: transparent;");
    m_errorLabel->hide();
    contentV->addWidget(m_errorLabel);
    contentV->addSpacing(16);

    m_group = new QButtonGroup(this);
    QObject::connect(m_group, &QButtonGroup::buttonClicked, this,
        [this](QAbstractButton *btn) {
            if (!m_syncing && m_service && !m_service->isBusy())
                setActiveProfile(btn->property("profileId").toString());
        });

    // Preferred plans
    contentV->addLayout(Win7::sectionHeading("Performance profiles"));
    contentV->addSpacing(6);

    m_planList = new QVBoxLayout;
    // Indent the plan rows under the heading, as Windows does.
    m_planList->setContentsMargins(14, 0, 0, 0);
    m_planList->setSpacing(14);
    contentV->addLayout(m_planList);

    // Additional plans (collapsible). Built unconditionally so the layout
    // slots exist; the whole block is hidden below when there are none.
    m_additionalWrap = new QWidget;
    m_additionalWrap->setStyleSheet("background: transparent;");
    m_additionalBox = new QVBoxLayout(m_additionalWrap);
    m_additionalBox->setContentsMargins(14, 0, 0, 0);
    m_additionalBox->setSpacing(14);

    // The shared round Aero chevron toggles the additional-plans block: arrow
    // up when expanded (click to hide), down when collapsed (click to show).
    auto *chevron = new Win7::ChevronButton;
    chevron->setAccessibleName(QStringLiteral("Show additional plans"));

    QLabel *additionalHeadingLabel = nullptr;   // text set by applyExpanded

    contentV->addSpacing(16);

    // The heading row lives in its own host widget so it can be hidden wholesale
    // (rule included) when there are no additional plans.
    m_additionalHeader = new QWidget;
    m_additionalHeader->setStyleSheet("background: transparent;");
    {
        auto *row = Win7::sectionHeading(QString(), chevron,
                                         &additionalHeadingLabel);
        m_additionalHeader->setLayout(row);
    }
    contentV->addWidget(m_additionalHeader);
    contentV->addSpacing(6);
    contentV->addWidget(m_additionalWrap);

    auto applyExpanded = [additionalHeadingLabel, chevron, this](bool expanded) {
        m_additionalWrap->setVisible(expanded);
        additionalHeadingLabel->setText(expanded ? "Hide additional plans"
                                                  : "Show additional plans");
        chevron->setAccessibleName(additionalHeadingLabel->text());
        QSignalBlocker block(chevron);
        chevron->setChecked(expanded);
    };
    QObject::connect(chevron, &QToolButton::toggled, this, applyExpanded);

    buildPolicyControls(contentV);
    contentV->addStretch(1);

    // Populate the plan rows, then settle the expander state.
    buildPlans(m_ppdAvailable ? profiles : QStringList());

    if (m_additionalBox->count() == 0) {
        // No additional plans: hide the whole collapsible block and its heading.
        m_additionalHeader->hide();
        m_additionalWrap->hide();
    } else {
        // Expand by default when the active plan is one of the additional ones,
        // so the current selection is never hidden; otherwise start collapsed.
        const QString active = activeProfile();
        bool activeIsAdditional = false;
        for (int i = 0; i < m_additionalBox->count(); ++i) {
            if (auto *w = m_additionalBox->itemAt(i)->widget())
                if (auto *r = w->findChild<QRadioButton *>())
                    if (r->property("profileId").toString() == active)
                        activeIsAdditional = true;
        }
        applyExpanded(activeIsAdditional);
    }

    connect(m_service, &PowerProfileService::stateChanged,
            this, &PowerOptionsPage::updateServiceState);
    updateServiceState();
    connect(m_policy, &SettingsSession::stateChanged, this, &PowerOptionsPage::updatePolicyState);
    updatePolicyState();
}

void PowerOptionsPage::buildPolicyControls(QVBoxLayout *layout)
{
    layout->addSpacing(20);
    layout->addLayout(Win7::sectionHeading("Change display and idle settings"));
    auto *intro = Win7::label("Set timeouts for each power source. Advanced power settings below select the idle action, button and lid behavior, brightness, and battery policy. Changes stay staged until Apply. KDE activities and applications can inhibit idle actions.", 9, "#333333");
    intro->setWordWrap(true);
    intro->setTextFormat(Qt::PlainText);
    layout->addWidget(intro);
    auto *grid = new QGridLayout;
    grid->setContentsMargins(14, 6, 0, 0);
    grid->setHorizontalSpacing(12);
    grid->addWidget(Win7::label("Power source", 9, "#333333"), 0, 0);
    grid->addWidget(Win7::label("Turn off the display", 9, "#333333"), 0, 1);
    grid->addWidget(Win7::label("Run the idle action after", 9, "#333333"), 0, 2);
    const QStringList ids{QStringLiteral("AC"), QStringLiteral("Battery"), QStringLiteral("LowBattery")};
    const QStringList names{QStringLiteral("Plugged in"), QStringLiteral("On battery"), QStringLiteral("On low battery")};
    for (int i = 0; i < ids.size(); ++i) {
        auto *display = new QComboBox;
        auto *sleep = new QComboBox;
        auto *reason = new QLabel;
        reason->setWordWrap(true);
        reason->setTextFormat(Qt::PlainText);
        reason->setStyleSheet("color: #666666; background: transparent;");
        display->setObjectName(QStringLiteral("powerDisplayTimeout_%1").arg(ids[i]));
        sleep->setObjectName(QStringLiteral("powerSleepTimeout_%1").arg(ids[i]));
        display->setAccessibleName(names[i] + QStringLiteral(": turn off the display"));
        sleep->setAccessibleName(names[i] + QStringLiteral(": idle action timeout"));
        auto *displayEnabled = new QCheckBox(tr("Enabled"));
        displayEnabled->setObjectName(QStringLiteral("powerDisplayEnabled_%1").arg(ids[i]));
        displayEnabled->setAccessibleName(names[i] + tr(": turn off display when idle"));
        auto *displayRow = new QWidget; auto *displayLayout = new QHBoxLayout(displayRow); displayLayout->setContentsMargins(0,0,0,0);
        displayLayout->addWidget(displayEnabled); displayLayout->addWidget(display,1);
        grid->addWidget(Win7::label(names[i], 9, "#1A1A1A"), 1 + i * 2, 0);
        grid->addWidget(displayRow, 1 + i * 2, 1);
        grid->addWidget(sleep, 1 + i * 2, 2);
        grid->addWidget(reason, 2 + i * 2, 0, 1, 3);
        m_timeoutControls << TimeoutControls{ids[i], displayEnabled, display, sleep, reason};
        bindPolicyControl(displayEnabled,PowerPolicyKey::DisplayEnabled,ids[i]);
        bindPolicyControl(display,PowerPolicyKey::DisplaySeconds,ids[i]);
        bindPolicyControl(sleep,PowerPolicyKey::IdleSeconds,ids[i]);
    }
    layout->addLayout(grid);
    buildAdvancedControls(layout);
    m_policyStatus = new QLabel;
    m_policyStatus->setObjectName(QStringLiteral("powerPolicyStatus"));
    m_policyStatus->setWordWrap(true);
    m_policyStatus->setTextFormat(Qt::PlainText);
    layout->addWidget(m_policyStatus);
    auto *buttons = new QHBoxLayout;
    m_defaults = new QPushButton(tr("Restore defaults…")); m_defaults->setObjectName(QStringLiteral("powerPolicyDefaults"));
    m_defaults->setAccessibleName(tr("Review native power policy defaults before staging")); buttons->addWidget(m_defaults);
    connect(m_defaults,&QPushButton::clicked,this,&PowerOptionsPage::showDefaultsReview);
    buttons->addStretch();
    m_revert = new QPushButton(QStringLiteral("Re&vert"));
    m_apply = new QPushButton(QStringLiteral("&Apply"));
    m_revert->setObjectName(QStringLiteral("powerPolicyRevert"));
    m_apply->setObjectName(QStringLiteral("powerPolicyApply"));
    m_apply->setAccessibleName(tr("Apply staged KDE power policy settings"));
    m_revert->setAccessibleName(tr("Revert the power policy draft and reload current settings"));
    buttons->addWidget(m_revert);
    buttons->addWidget(m_apply);
    layout->addLayout(buttons);
    connect(m_apply, &QPushButton::clicked, m_policy, &PowerPolicySettings::apply);
    connect(m_revert, &QPushButton::clicked, m_policy, &PowerPolicySettings::revert);
}

QString PowerOptionsPage::selectedPolicySource() const
{ return m_policySource ? m_policySource->currentData().toString() : QString(); }

void PowerOptionsPage::buildAdvancedControls(QVBoxLayout *layout)
{
    layout->addSpacing(10);
    m_advancedToggle = new QPushButton(tr("Show advanced po&wer settings"));
    m_advancedToggle->setObjectName(QStringLiteral("powerAdvancedToggle")); m_advancedToggle->setCheckable(true);
    m_advancedToggle->setAccessibleName(tr("Show or hide advanced power settings"));
    auto *toggleRow = new QHBoxLayout; toggleRow->addWidget(m_advancedToggle); toggleRow->addStretch(); layout->addLayout(toggleRow);
    m_advanced = new QGroupBox(tr("Power buttons, lid, brightness and battery policy")); m_advanced->setObjectName(QStringLiteral("powerAdvancedSettings"));
    auto *advanced = new QVBoxLayout(m_advanced); auto *form = new QGridLayout; form->setVerticalSpacing(9); form->setColumnStretch(1, 1);
    int row = 0;
    auto add = [&](const QString &label, QWidget *widget) {
        auto *name = Win7::label(label); name->setBuddy(widget); form->addWidget(name, row, 0); form->addWidget(widget, row++, 1);
    };
    auto combo = [&](const char *name, const QString &accessible) {
        auto *control = new QComboBox; control->setObjectName(QLatin1String(name)); control->setAccessibleName(accessible);
        control->setProperty("policyAccessibleLabel", accessible); return control;
    };
    m_policySource = combo("powerPolicySource", tr("Power source for advanced settings")); add(tr("Power s&ource:"), m_policySource);
    m_idleAction = combo("powerIdleAction", tr("Idle action for selected power source")); add(tr("When &idle:"), m_idleAction);
    m_dim = combo("powerDimTimeout", tr("Dim display timeout for selected power source"));
    for (int seconds : {10, 30, 60, 120, 300, 600, 900, 1800}) m_dim->addItem(tr("%1 seconds").arg(seconds), seconds);
    m_dimEnabled = new QCheckBox(tr("Dim display when idle")); m_dimEnabled->setObjectName(QStringLiteral("powerDimEnabled"));
    m_dimEnabled->setAccessibleName(tr("Dim display when idle for selected power source")); form->addWidget(m_dimEnabled,row++,0,1,2);
    add(tr("&Dim display after:"), m_dim);
    m_brightnessEnabled = new QCheckBox(tr("Set display &brightness")); m_brightnessEnabled->setObjectName(QStringLiteral("powerBrightnessEnabled"));
    m_brightness = new QSpinBox; m_brightness->setObjectName(QStringLiteral("powerBrightnessPercent")); m_brightness->setRange(0, 100); m_brightness->setSuffix(tr("%"));
    m_brightness->setAccessibleName(tr("Display brightness percentage for selected power source"));
    form->addWidget(m_brightnessEnabled, row, 0); form->addWidget(m_brightness, row++, 1);
    m_keyboardBrightnessEnabled = new QCheckBox(tr("Set &keyboard brightness")); m_keyboardBrightnessEnabled->setObjectName(QStringLiteral("powerKeyboardBrightnessEnabled"));
    m_keyboardBrightness = new QSpinBox; m_keyboardBrightness->setObjectName(QStringLiteral("powerKeyboardBrightnessPercent")); m_keyboardBrightness->setRange(0, 100); m_keyboardBrightness->setSuffix(tr("%"));
    m_keyboardBrightness->setAccessibleName(tr("Keyboard brightness percentage for selected power source"));
    form->addWidget(m_keyboardBrightnessEnabled, row, 0); form->addWidget(m_keyboardBrightness, row++, 1);
    m_lockedDisplay = combo("powerLockedDisplayTimeout", tr("Display off timeout while locked for selected power source"));
    m_lockedDisplay->addItem(tr("Same as unlocked"), -2); m_lockedDisplay->addItem(tr("Immediately"), 0);
    for (int seconds : {10, 30, 60, 120, 300, 600, 900, 1800}) m_lockedDisplay->addItem(tr("%1 seconds").arg(seconds), seconds);
    add(tr("When &locked, turn off after:"), m_lockedDisplay);
    m_lockDisplay = new QCheckBox(tr("Lock be&fore turning off the display")); m_lockDisplay->setObjectName(QStringLiteral("powerLockBeforeDisplayOff")); form->addWidget(m_lockDisplay, row++, 0, 1, 2);
    m_powerButton = combo("powerButtonAction", tr("Power button action for selected power source")); add(tr("&Power button:"), m_powerButton);
    m_powerDown = combo("powerDownAction", tr("Long power-button press action for selected power source")); add(tr("When I hold the power butto&n:"), m_powerDown);
    m_lid = combo("powerLidAction", tr("Lid closing action for selected power source")); add(tr("&Closing the lid:"), m_lid);
    m_inhibitLid = new QCheckBox(tr("Ignore lid action with an &external monitor")); m_inhibitLid->setObjectName(QStringLiteral("powerLidExternalMonitor")); form->addWidget(m_inhibitLid, row++, 0, 1, 2);
    m_sleepMode = combo("powerSleepMode", tr("Sleep mode for selected power source")); add(tr("&Sleep mode:"), m_sleepMode);
    advanced->addLayout(form); advanced->addSpacing(8); advanced->addLayout(Win7::sectionHeading(tr("Battery levels and critical action")));
    auto *battery = new QGridLayout; battery->setVerticalSpacing(9); battery->setColumnStretch(1, 1);
    m_batteryLow = new QSpinBox; m_batteryCritical = new QSpinBox;
    m_batteryLow->setObjectName(QStringLiteral("powerBatteryLowLevel")); m_batteryCritical->setObjectName(QStringLiteral("powerBatteryCriticalLevel"));
    for (auto *level : {m_batteryLow, m_batteryCritical}) { level->setRange(0, 100); level->setSuffix(tr("%")); }
    m_batteryLow->setAccessibleName(tr("Low battery level percentage")); m_batteryCritical->setAccessibleName(tr("Critical battery level percentage"));
    auto *lowLabel = Win7::label(tr("Low ba&ttery level:")); lowLabel->setBuddy(m_batteryLow); battery->addWidget(lowLabel, 0, 0); battery->addWidget(m_batteryLow, 0, 1);
    auto *criticalLabel = Win7::label(tr("C&ritical battery level:")); criticalLabel->setBuddy(m_batteryCritical); battery->addWidget(criticalLabel, 1, 0); battery->addWidget(m_batteryCritical, 1, 1);
    m_criticalAction = combo("powerBatteryCriticalAction", tr("Action at critical battery level"));
    auto *actionLabel = Win7::label(tr("W&hen critical:")); actionLabel->setBuddy(m_criticalAction); battery->addWidget(actionLabel, 2, 0); battery->addWidget(m_criticalAction, 2, 1);
    advanced->addLayout(battery);
    m_advancedReason = Win7::bodyLabel({}); m_advancedReason->setObjectName(QStringLiteral("powerAdvancedReason")); m_advancedReason->setTextFormat(Qt::PlainText);
    auto *reasonArea = new QScrollArea; reasonArea->setFrameShape(QFrame::NoFrame); reasonArea->setWidgetResizable(true); reasonArea->setWidget(m_advancedReason);
    reasonArea->setMinimumHeight(45); reasonArea->setMaximumHeight(100); reasonArea->setAccessibleName(tr("Reasons power policy settings are unavailable")); advanced->addWidget(reasonArea);
    layout->addWidget(m_advanced);
    auto expanded = [this](bool visible) {
        m_advanced->setVisible(visible); m_advancedToggle->setText(visible ? tr("Hide advanced po&wer settings") : tr("Show advanced po&wer settings"));
        m_policy->setProperty("powerAdvancedExpanded", visible);
    };
    const bool visible = m_policy->property("powerAdvancedExpanded").toBool(); m_advancedToggle->setChecked(visible); expanded(visible);
    connect(m_advancedToggle, &QPushButton::toggled, this, expanded);
    connect(m_policySource, &QComboBox::currentIndexChanged, this, [this] {
        if (m_updatingPolicy) return; m_policy->setProperty("powerAdvancedSource", selectedPolicySource()); updatePolicyState();
    });
    const QString selected = QStringLiteral("@selected"), global = QStringLiteral("@global");
    bindPolicyControl(m_idleAction,PowerPolicyKey::IdleAction,selected);
    bindPolicyControl(m_dimEnabled,PowerPolicyKey::DimEnabled,selected); bindPolicyControl(m_dim,PowerPolicyKey::DimSeconds,selected);
    bindPolicyControl(m_brightnessEnabled,PowerPolicyKey::BrightnessEnabled,selected); bindPolicyControl(m_brightness,PowerPolicyKey::BrightnessPercent,selected);
    bindPolicyControl(m_keyboardBrightnessEnabled,PowerPolicyKey::KeyboardBrightnessEnabled,selected); bindPolicyControl(m_keyboardBrightness,PowerPolicyKey::KeyboardBrightnessPercent,selected);
    bindPolicyControl(m_lockedDisplay,PowerPolicyKey::LockedDisplaySeconds,selected); bindPolicyControl(m_lockDisplay,PowerPolicyKey::LockBeforeDisplayOff,selected);
    bindPolicyControl(m_powerButton,PowerPolicyKey::PowerButtonAction,selected); bindPolicyControl(m_powerDown,PowerPolicyKey::PowerDownAction,selected);
    bindPolicyControl(m_lid,PowerPolicyKey::LidAction,selected); bindPolicyControl(m_inhibitLid,PowerPolicyKey::LidExternalMonitorInhibition,selected);
    bindPolicyControl(m_sleepMode,PowerPolicyKey::SleepMode,selected);
    bindPolicyControl(m_batteryLow,PowerPolicyKey::BatteryLowLevel,global); bindPolicyControl(m_batteryCritical,PowerPolicyKey::BatteryCriticalLevel,global);
    bindPolicyControl(m_criticalAction,PowerPolicyKey::BatteryCriticalAction,global);
}

QString PowerOptionsPage::controlSource(QWidget *control) const
{
    const QString source = control->property("policySource").toString();
    return source == QLatin1String("@selected") ? selectedPolicySource() : source;
}
void PowerOptionsPage::stageControlValue(QWidget *control, const QVariant &value)
{
    const auto key = PowerPolicyKey(control->property("policyKey").toInt()); const auto source = controlSource(control);
    const auto *profile=findProfile(m_policy->staged(),source); QString error;
    const bool valid=source==QLatin1String("@global") ? PowerPolicyFields::validSet(m_policy->staged(),key,value,error)
        : profile && PowerPolicyFields::validSet(*profile,key,value,error);
    if (!valid) {
        // A raw saved percentage may be outside the editor domain. Keep it
        // visible, but never leave a rejected new value looking staged.
        updatePolicyState();
        if (!error.isEmpty()) { m_policyStatus->setText(error); m_policyStatus->setStyleSheet(QStringLiteral("color: #8A1C1C; background: transparent;")); m_policyStatus->show(); }
        return;
    }
    if (source == QLatin1String("@global")) m_policy->selectGlobalValue(key,value);
    else m_policy->selectValue(source,key,value);
}
void PowerOptionsPage::restoreControlDefault(QWidget *control)
{
    const auto key = PowerPolicyKey(control->property("policyKey").toInt()); const auto source = controlSource(control);
    if (source == QLatin1String("@global")) m_policy->restoreGlobalKeyDefault(key);
    else m_policy->restoreKeyDefault(source,key);
}
void PowerOptionsPage::bindPolicyControl(QWidget *control, PowerPolicyKey key, const QString &source)
{
    control->setProperty("policyKey",int(key)); control->setProperty("policySource",source);
    control->setContextMenuPolicy(Qt::ActionsContextMenu);
    auto *restore = new QAction(tr("Use system default"),control); restore->setObjectName(QStringLiteral("policyRestoreDefault")); control->addAction(restore);
    connect(restore,&QAction::triggered,this,[this,control] { restoreControlDefault(control); });
    if (auto *combo = qobject_cast<QComboBox *>(control)) {
        connect(combo,&QComboBox::activated,this,[this,combo](int index) { if (!m_updatingPolicy) activatePolicyChoice(combo,index); });
    } else if (auto *check = qobject_cast<QCheckBox *>(control)) {
        connect(check,&QCheckBox::stateChanged,this,[this,check](int state) {
            if (!m_updatingPolicy && state != Qt::PartiallyChecked) stageControlValue(check,state == Qt::Checked);
        });
    } else if (auto *spin = qobject_cast<QSpinBox *>(control)) {
        connect(spin,&QSpinBox::valueChanged,this,[this,spin] { if (!m_updatingPolicy) spin->setProperty("policyUserEdited",true); });
        connect(spin,&QSpinBox::editingFinished,this,[this,spin] {
            if (!m_updatingPolicy && spin->property("policyUserEdited").toBool()) {
                spin->setProperty("policyUserEdited",false); stageControlValue(spin,spin->value());
            }
        });
    }
    if (!qobject_cast<QComboBox *>(control)) {
        auto *explicitValue = new QAction(tr("Use this value explicitly"),control); explicitValue->setObjectName(QStringLiteral("policyExplicitValue")); control->addAction(explicitValue);
        connect(explicitValue,&QAction::triggered,this,[this,control] {
            if (auto *check = qobject_cast<QCheckBox *>(control)) stageControlValue(control,check->isChecked());
            else if (auto *spin = qobject_cast<QSpinBox *>(control)) stageControlValue(control,spin->value());
        });
    }
}
void PowerOptionsPage::activatePolicyChoice(QComboBox *control, int index)
{
    if (index < 0 || !control->isEnabled()) return;
    if (!(control->model()->flags(control->model()->index(index,0)) & Qt::ItemIsEnabled)) { updatePolicyState(); return; }
    const auto kind = ChoiceKind(control->itemData(index,KindRole).toInt());
    if (kind == CurrentChoice) return;
    if (kind == CustomChoice) {
        // Qt emits another combo signal after activated; open after that returns.
        const QPointer<QComboBox> combo(control); const QPointer<PowerPolicySettings> session(m_policy);
        const QString source = controlSource(control); const auto before = PowerPolicyFields::revision(m_policy->applied()), draft = PowerPolicyFields::revision(m_policy->staged());
        QTimer::singleShot(0,this,[this,combo,session,source,before,draft] {
            if (!session || !combo) return;
            if (session->isBusy() || session->hasConflict() || controlSource(combo) != source
                || PowerPolicyFields::revision(session->applied()) != before || PowerPolicyFields::revision(session->staged()) != draft) { updatePolicyState(); return; }
            editCustomTimeout(combo);
        }); return;
    }
    if (kind == DefaultChoice) { restoreControlDefault(control); return; }
    if (kind == NeverChoice) {
        const auto key = PowerPolicyKey(control->property("policyKey").toInt());
        const auto target = key == PowerPolicyKey::DisplaySeconds ? PowerPolicyKey::DisplayEnabled
            : key == PowerPolicyKey::DimSeconds ? PowerPolicyKey::DimEnabled : PowerPolicyKey::IdleAction;
        m_policy->selectValue(controlSource(control),target,target == PowerPolicyKey::IdleAction ? QVariant(0u) : QVariant(false)); return;
    }
    stageControlValue(control,control->itemData(index));
}
void PowerOptionsPage::editCustomTimeout(QComboBox *control)
{
    const auto source = controlSource(control); const auto key = PowerPolicyKey(control->property("policyKey").toInt());
    const auto *profile = findProfile(m_policy->staged(),source); if (!profile) return;
    const auto baseline = PowerPolicyFields::revision(m_policy->applied()), draft = PowerPolicyFields::revision(m_policy->staged());
    const auto state = profile->keys.value(key); const int current = PowerPolicyFields::value(*profile,key).toInt();
    auto *dialog = new QDialog(this); const QPointer<QDialog> dialogGuard(dialog); const QPointer<PowerOptionsPage> pageGuard(this);
    const QPointer<PowerPolicySettings> sessionGuard(m_policy);
    dialog->setObjectName(QStringLiteral("powerCustomTimeoutDialog")); dialog->setWindowTitle(tr("Custom Power Timeout"));
    Win7::styleSettingsDialog(dialog); Win7::styleSettingsFields(dialog); auto *layout = new QVBoxLayout(dialog);
    const QString currentText = !knownValue(state,profile->intents,key) ? tr("unavailable")
        : key == PowerPolicyKey::LockedDisplaySeconds && current < 0 ? tr("Same as unlocked (native value %1)").arg(current)
        : key == PowerPolicyKey::LockedDisplaySeconds && current == 0 ? tr("Immediately (0 seconds)") : tr("%1 seconds").arg(current);
    auto *intro = Win7::bodyLabel(tr("%1 — %2. Current draft value: %3. %4. Only the chosen setting is staged; Apply saves the policy.")
        .arg(sourceName(source),PowerPolicyFields::label(key),currentText,provenance(state,profile->intents,key)));
    intro->setObjectName(QStringLiteral("powerCustomTimeoutCurrent"));
    intro->setTextFormat(Qt::PlainText); layout->addWidget(intro);
    auto *form = new QGridLayout; auto *intent = new QComboBox; intent->setObjectName(QStringLiteral("powerCustomTimeoutIntent")); intent->setAccessibleName(tr("Timeout write intent"));
    intent->addItem(tr("Set an explicit timeout"),ValueChoice);
    if (key == PowerPolicyKey::LockedDisplaySeconds) { intent->addItem(tr("Same as unlocked (explicit)"),100); intent->addItem(tr("Immediately (explicit)"),101); }
    else {
        const auto target = key == PowerPolicyKey::DisplaySeconds ? PowerPolicyKey::DisplayEnabled : key == PowerPolicyKey::DimSeconds ? PowerPolicyKey::DimEnabled : PowerPolicyKey::IdleAction;
        if (profile->keys.value(target).writable && !profile->keys.value(target).immutable) intent->addItem(tr("Never — disable this action only"),NeverChoice);
    }
    if (canRestoreProfileKey(*profile,m_policy->applied(),key)) intent->addItem(tr("Use system default"),DefaultChoice);
    auto *intentLabel = Win7::label(tr("&Change:")); intentLabel->setBuddy(intent); form->addWidget(intentLabel,0,0); form->addWidget(intent,0,1);
    auto *seconds = new QSpinBox; seconds->setObjectName(QStringLiteral("powerCustomTimeoutSeconds")); seconds->setAccessibleName(tr("Custom timeout in seconds"));
    seconds->setRange(key == PowerPolicyKey::DisplaySeconds ? 30 : key == PowerPolicyKey::IdleSeconds ? 60 : 10,PowerPolicySettings::MaximumTimeoutSeconds);
    seconds->setValue(qBound(seconds->minimum(),current,seconds->maximum())); seconds->setSuffix(tr(" seconds"));
    auto *secondsLabel = Win7::label(tr("S&econds:")); secondsLabel->setBuddy(seconds); form->addWidget(secondsLabel,1,0); form->addWidget(seconds,1,1); layout->addLayout(form);
    auto *meaning = Win7::bodyLabel({}); meaning->setTextFormat(Qt::PlainText); meaning->setObjectName(QStringLiteral("powerCustomTimeoutMeaning")); layout->addWidget(meaning);
    auto explain = [=] {
        const int chosen = intent->currentData().toInt(); seconds->setEnabled(chosen == ValueChoice);
        meaning->setText(chosen == DefaultChoice ? tr("Use KDE's reviewed native default. This differs from an explicit Never or Same as unlocked value.")
            : chosen == NeverChoice ? tr("Disable the display, dim or idle action. Keep the saved timeout and other settings unchanged.")
            : chosen == 100 ? tr("Save Same as unlocked explicitly for the locked-display timeout. This does not restore the native editor default.")
            : chosen == 101 ? tr("Save an explicit zero-second locked-display timeout: turn off immediately. Other timeout and lock settings remain unchanged.")
            : tr("Save this value explicitly. Keep the action and its enabled setting unchanged. Valid custom timeouts are %1 to %2 seconds.").arg(seconds->minimum()).arg(seconds->maximum()));
    }; connect(intent,&QComboBox::currentIndexChanged,dialog,explain); explain();
    auto *status = Win7::bodyLabel({}); status->setObjectName(QStringLiteral("powerCustomTimeoutStatus")); status->setTextFormat(Qt::PlainText); layout->addWidget(status);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel); auto *stage = buttons->addButton(tr("&Stage change"),QDialogButtonBox::AcceptRole);
    stage->setObjectName(QStringLiteral("powerCustomTimeoutStage")); buttons->button(QDialogButtonBox::Cancel)->setObjectName(QStringLiteral("powerCustomTimeoutCancel")); layout->addWidget(buttons);
    connect(buttons,&QDialogButtonBox::rejected,dialog,&QDialog::reject);
    connect(m_policy,&QObject::destroyed,dialog,&QDialog::reject);
    auto stale = [=] {
        return !sessionGuard || sessionGuard->isBusy() || sessionGuard->hasConflict() || controlSource(control) != source
            || PowerPolicyFields::revision(sessionGuard->applied()) != baseline || PowerPolicyFields::revision(sessionGuard->staged()) != draft;
    };
    connect(m_policy,&SettingsSession::stateChanged,dialog,[=] { if (stale()) { status->setText(tr("The policy changed while this editor was open. Cancel and review the current values.")); stage->setEnabled(false); } });
    connect(m_policySource,&QComboBox::currentIndexChanged,dialog,[=] { if (stale()) { status->setText(tr("The selected power source changed. Cancel and reopen this editor.")); stage->setEnabled(false); } });
    connect(stage,&QPushButton::clicked,dialog,[=] {
        if (stale()) { status->setText(tr("This editor is stale. Cancel and review the current values.")); stage->setEnabled(false); return; }
        const int chosen = intent->currentData().toInt();
        if (chosen == DefaultChoice) restoreControlDefault(control);
        else if (chosen == NeverChoice) {
            const auto target = key == PowerPolicyKey::DisplaySeconds ? PowerPolicyKey::DisplayEnabled : key == PowerPolicyKey::DimSeconds ? PowerPolicyKey::DimEnabled : PowerPolicyKey::IdleAction;
            sessionGuard->selectValue(source,target,target == PowerPolicyKey::IdleAction ? QVariant(0u) : QVariant(false));
        } else stageControlValue(control,chosen == 100 ? -2 : chosen == 101 ? 0 : seconds->value());
        if (!dialogGuard || !sessionGuard) return;
        if (!sessionGuard->errorText().isEmpty()) status->setText(sessionGuard->errorText()); else dialog->accept();
    });
    dialog->exec(); if (dialogGuard) delete dialog; if (pageGuard && sessionGuard) updatePolicyState();
}
void PowerOptionsPage::showDefaultsReview()
{
    auto *dialog = new QDialog(this); const QPointer<QDialog> guard(dialog); const QPointer<PowerPolicySettings> session(m_policy);
    dialog->setObjectName(QStringLiteral("powerDefaultsDialog")); dialog->setWindowTitle(tr("Restore Power Policy Defaults")); dialog->resize(650,500);
    Win7::styleSettingsDialog(dialog); Win7::styleSettingsFields(dialog); auto *layout = new QVBoxLayout(dialog);
    auto *intro = Win7::bodyLabel(tr("Review native KDE defaults for the chosen scope. Unavailable defaults remain unchanged. Stage defaults changes the draft; use Apply on this page to save.")); intro->setTextFormat(Qt::PlainText); layout->addWidget(intro);
    auto *form = new QGridLayout; auto *scope = new QComboBox; scope->setObjectName(QStringLiteral("powerDefaultsScope")); scope->setAccessibleName(tr("Scope for restoring power policy defaults"));
    using Scope = PowerPolicySettings::DefaultsScope;
    for (const auto &entry : {qMakePair(tr("Selected power source"),Scope::SelectedProfile),qMakePair(tr("All power sources"),Scope::AllProfiles),qMakePair(tr("Battery levels and critical action"),Scope::Battery),qMakePair(tr("All power policies"),Scope::AllPolicies)}) scope->addItem(entry.first,int(entry.second));
    auto *source = new QComboBox; source->setObjectName(QStringLiteral("powerDefaultsSource")); source->setAccessibleName(tr("Power source for defaults review"));
    for (const auto &profile : m_policy->staged().profiles) source->addItem(sourceName(profile.id),profile.id); source->setCurrentIndex(source->findData(selectedPolicySource()));
    auto *scopeLabel = Win7::label(tr("&Scope:")); scopeLabel->setBuddy(scope); auto *sourceLabel = Win7::label(tr("Power s&ource:")); sourceLabel->setBuddy(source);
    form->addWidget(scopeLabel,0,0); form->addWidget(scope,0,1); form->addWidget(sourceLabel,1,0); form->addWidget(source,1,1); layout->addLayout(form);
    auto *rows = new QTreeWidget; rows->setHeader(new PowerDefaultsHeader(rows)); rows->setObjectName(QStringLiteral("powerDefaultsReview")); rows->setHeaderLabels({tr("Review"),tr("Setting and result")}); rows->setRootIsDecorated(false); rows->setAccessibleName(tr("Power policy defaults review"));
    rows->setWordWrap(true); rows->setTextElideMode(Qt::ElideNone); rows->setUniformRowHeights(false);
    rows->header()->setSectionResizeMode(0,QHeaderView::Fixed); rows->setColumnWidth(0,110); rows->header()->setSectionResizeMode(1,QHeaderView::Stretch);
    rows->header()->setFixedHeight(rows->header()->fontMetrics().height()+11);
    rows->setStyleSheet(rows->styleSheet()+QStringLiteral(
        " QTreeWidget#powerDefaultsReview { color: #000000; background: #FFFFFF; border: 1px solid #ABADB3; border-image: none; }"
        " QTreeWidget#powerDefaultsReview::item { color: #000000; padding: 4px; border-image: none; }"
        " QTreeWidget#powerDefaultsReview::item:selected { color: #000000; background: #CDE6F7; }"
        " QTreeWidget#powerDefaultsReview QHeaderView { color: #000000; background: #F4F5F6; border-image: none; }"
        " QTreeWidget#powerDefaultsReview QHeaderView::section { color: #000000; background: #F4F5F6; border: 0; border-right: 1px solid #DDDDDD; border-bottom: 1px solid #DDDDDD; border-image: none; padding: 5px; }"));
    auto resizeReviewRows = [rows] {
        const QFontMetrics metrics(rows->font());
        for (int row=0;row<rows->topLevelItemCount();++row) {
            auto *item=rows->topLevelItem(row); int height=metrics.height()+10;
            for (int column=0;column<2;++column) height=qMax(height,metrics.boundingRect(
                QRect(0,0,qMax(1,rows->columnWidth(column)-14),1000000),Qt::TextWordWrap|Qt::TextExpandTabs,item->text(column)).height()+10);
            for (int column=0;column<2;++column) if (item->sizeHint(column).height()!=height) item->setSizeHint(column,QSize(0,height));
        }
    };
    connect(rows->header(),&QHeaderView::sectionResized,dialog,[resizeReviewRows] { resizeReviewRows(); });
    layout->addWidget(rows,1);
    auto *status = Win7::bodyLabel({}); status->setObjectName(QStringLiteral("powerDefaultsStatus")); status->setTextFormat(Qt::PlainText); layout->addWidget(status);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel); auto *refresh = buttons->addButton(tr("&Update review"),QDialogButtonBox::ActionRole); auto *stage = buttons->addButton(tr("&Stage defaults"),QDialogButtonBox::AcceptRole);
    stage->setObjectName(QStringLiteral("powerDefaultsStage")); refresh->setObjectName(QStringLiteral("powerDefaultsUpdateReview")); buttons->button(QDialogButtonBox::Cancel)->setObjectName(QStringLiteral("powerDefaultsCancel")); layout->addWidget(buttons);
    PowerPolicySettings::DefaultsReview review;
    auto currentScope = [=] { return Scope(scope->currentData().toInt()); };
    auto update = [&] {
        source->setEnabled(currentScope() == Scope::SelectedProfile); review = session->reviewDefaults(currentScope(),source->currentData().toString()); rows->clear();
        for (const auto &group : {qMakePair(tr("Will stage"),review.changes),qMakePair(tr("Unchanged"),review.unchanged),qMakePair(tr("Unavailable"),review.unavailable)})
            for (const auto &text : group.second) {
                auto *item=new QTreeWidgetItem(rows,{group.first,text});
                const QString tooltip=plainToolTip(group.first+QStringLiteral(": ")+text);
                item->setToolTip(0,tooltip); item->setToolTip(1,tooltip);
            }
        resizeReviewRows();
        stage->setEnabled(review.hasChanges()); status->setText(review.hasChanges() ? tr("Reviewed changes are ready to stage. Nothing has been saved.") : tr("There are no available default changes in this scope."));
    };
    connect(scope,&QComboBox::currentIndexChanged,dialog,update); connect(source,&QComboBox::currentIndexChanged,dialog,update); connect(refresh,&QPushButton::clicked,dialog,update);
    connect(m_policy,&SettingsSession::stateChanged,dialog,[&] {
        if (session->reviewDefaults(currentScope(),source->currentData().toString()).revision != review.revision) {
            stage->setEnabled(false); status->setText(tr("The saved settings or draft changed. Update review before staging defaults."));
        }
    });
    connect(stage,&QPushButton::clicked,dialog,[&] {
        const auto current = session->reviewDefaults(currentScope(),source->currentData().toString());
        if (current.revision != review.revision) { stage->setEnabled(false); status->setText(tr("This defaults review is stale. Update review before staging.")); return; }
        session->stageDefaults(currentScope(),source->currentData().toString(),review.revision);
        if (!guard || !session) return;
        if (!session->errorText().isEmpty()) status->setText(session->errorText()); else dialog->accept();
    });
    connect(m_policy,&QObject::destroyed,dialog,&QDialog::reject);
    connect(buttons,&QDialogButtonBox::rejected,dialog,&QDialog::reject); update(); dialog->exec(); if (guard) delete dialog;
}

void PowerOptionsPage::updateAdvancedState(bool enabled)
{
    const auto &snapshot = m_policy->staged(); const QSignalBlocker sourceBlocker(m_policySource);
    auto selected = m_policy->property("powerAdvancedSource").toString(); if (selected.isEmpty()) selected = selectedPolicySource();
    m_policySource->clear(); for (const auto &profile : snapshot.profiles) m_policySource->addItem(sourceName(profile.id), profile.id);
    int index = m_policySource->findData(selected); if (index < 0 && m_policySource->count()) index = 0; m_policySource->setCurrentIndex(index);
    m_policy->setProperty("powerAdvancedSource", selectedPolicySource());
    const auto *profile = findProfile(snapshot,selectedPolicySource());
    if (profile) {
        renderChoice(m_idleAction,PowerPolicyKey::IdleAction,profile->suspendAction,profile->keys.value(PowerPolicyKey::IdleAction),profile->intents,canRestoreProfileKey(*profile,m_policy->applied(),PowerPolicyKey::IdleAction),profile->idleActions);
        renderChoice(m_dim,PowerPolicyKey::DimSeconds,profile->dimSeconds,profile->keys.value(PowerPolicyKey::DimSeconds),profile->intents,canRestoreProfileKey(*profile,m_policy->applied(),PowerPolicyKey::DimSeconds));
        renderChoice(m_lockedDisplay,PowerPolicyKey::LockedDisplaySeconds,profile->lockedDisplaySeconds,profile->keys.value(PowerPolicyKey::LockedDisplaySeconds),profile->intents,canRestoreProfileKey(*profile,m_policy->applied(),PowerPolicyKey::LockedDisplaySeconds));
        for (const auto &binding : {qMakePair(m_powerButton,PowerPolicyKey::PowerButtonAction),qMakePair(m_powerDown,PowerPolicyKey::PowerDownAction),qMakePair(m_lid,PowerPolicyKey::LidAction),qMakePair(m_sleepMode,PowerPolicyKey::SleepMode)}) {
            const auto actions = binding.second == PowerPolicyKey::LidAction ? profile->lidActions : binding.second == PowerPolicyKey::SleepMode ? profile->sleepModes : profile->powerButtonActions;
            renderChoice(binding.first,binding.second,PowerPolicyFields::value(*profile,binding.second),profile->keys.value(binding.second),profile->intents,canRestoreProfileKey(*profile,m_policy->applied(),binding.second),actions);
        }
    }
    renderChoice(m_criticalAction,PowerPolicyKey::BatteryCriticalAction,snapshot.batteryCriticalAction,snapshot.keys.value(PowerPolicyKey::BatteryCriticalAction),snapshot.intents,canRestoreKey(snapshot,m_policy->applied(),PowerPolicyKey::BatteryCriticalAction),snapshot.batteryCriticalActions);
    QStringList details;
    for (auto *control : findChildren<QWidget *>()) {
        if (!control->property("policyKey").isValid()) continue;
        const auto key = PowerPolicyKey(control->property("policyKey").toInt()); const QString source = controlSource(control);
        const bool global = source == QLatin1String("@global"); const auto *p = global ? nullptr : findProfile(snapshot,source);
        const auto state = global ? snapshot.keys.value(key) : p ? p->keys.value(key) : PowerPolicyKeyState();
        const auto intents = global ? snapshot.intents : p ? p->intents : QMap<PowerPolicyKey,PowerPolicyIntent>();
        const QVariant value = global ? PowerPolicyFields::value(snapshot,key) : p ? PowerPolicyFields::value(*p,key) : QVariant();
        const bool known = knownValue(state,intents,key), writable = state.writable && !state.immutable;
        const QString origin = provenance(state,intents,key);
        QString description = origin;
        if (!state.reason.isEmpty()) description += QStringLiteral(". ") + state.reason;
        if (!state.defaultKnown && !state.defaultReason.isEmpty()) description += QStringLiteral(". ") + state.defaultReason;
        if (!state.resettable && !state.resetReason.isEmpty()) description += QStringLiteral(". ") + state.resetReason;
        if (auto *combo = qobject_cast<QComboBox *>(control); combo && p) {
            const auto target = key == PowerPolicyKey::DisplaySeconds ? PowerPolicyKey::DisplayEnabled : key == PowerPolicyKey::DimSeconds ? PowerPolicyKey::DimEnabled : PowerPolicyKey::IdleAction;
            for (int i=0;i<combo->count();++i) if (combo->itemData(i,KindRole).toInt() == NeverChoice
                && (!p->keys.value(target).writable || p->keys.value(target).immutable)) disableItem(combo,i);
        }
        control->setEnabled(enabled && writable); control->setToolTip(plainToolTip(description)); control->setAccessibleDescription(description);
        for (auto *action : control->actions()) {
            if (action->objectName() == QLatin1String("policyRestoreDefault")) action->setEnabled(enabled && (global ? canRestoreKey(snapshot,m_policy->applied(),key) : p && canRestoreProfileKey(*p,m_policy->applied(),key)));
            else {
                QString error;
                const bool explicitValid=global ? PowerPolicyFields::validSet(snapshot,key,value,error)
                    : p && PowerPolicyFields::validSet(*p,key,value,error);
                action->setEnabled(enabled && known && explicitValid);
            }
        }
        if (auto *check = qobject_cast<QCheckBox *>(control)) { check->setTristate(!known); check->setCheckState(!known ? Qt::PartiallyChecked : value.toBool() ? Qt::Checked : Qt::Unchecked); }
        if (auto *spin = qobject_cast<QSpinBox *>(control)) {
            spin->setRange(known ? qMin(0,value.toInt()) : -1, known ? qMax(100,value.toInt()) : 100);
            spin->setSpecialValueText(known ? QString() : tr("Unknown")); spin->setValue(known ? value.toInt() : -1); spin->setProperty("policyUserEdited",false);
        }
        if (source == selectedPolicySource() || global) details << tr("%1: %2%3").arg(PowerPolicyFields::label(key),origin,
            state.reason.isEmpty() ? QString() : QStringLiteral(". ")+state.reason);
    }
    if (!snapshot.unavailableReason.isEmpty()) details << snapshot.unavailableReason;
    m_advancedReason->setText(details.join(QLatin1Char('\n'))); m_advancedReason->setVisible(!details.isEmpty());
}

void PowerOptionsPage::updatePolicyState()
{
    m_updatingPolicy = true;
    const auto &snapshot = m_policy->staged();
    const bool enabled = m_policy->isReady() && !m_policy->isBusy() && !m_policy->hasConflict() && snapshot.serviceAvailable;
    for (const auto &controls : m_timeoutControls) {
        const auto *p = findProfile(snapshot,controls.profile); QStringList summary;
        if (p) {
            renderChoice(controls.display,PowerPolicyKey::DisplaySeconds,p->displaySeconds,p->keys.value(PowerPolicyKey::DisplaySeconds),p->intents,canRestoreProfileKey(*p,m_policy->applied(),PowerPolicyKey::DisplaySeconds));
            renderChoice(controls.sleep,PowerPolicyKey::IdleSeconds,p->suspendSeconds,p->keys.value(PowerPolicyKey::IdleSeconds),p->intents,canRestoreProfileKey(*p,m_policy->applied(),PowerPolicyKey::IdleSeconds));
            const auto idle = p->keys.value(PowerPolicyKey::IdleAction);
            summary << tr("Idle action: %1. %2").arg(knownValue(idle,p->intents,PowerPolicyKey::IdleAction) ? actionName(p->idleActions,p->suspendAction) : tr("unknown"),provenance(idle,p->intents,PowerPolicyKey::IdleAction));
            for (auto key : {PowerPolicyKey::DisplayEnabled,PowerPolicyKey::DisplaySeconds,PowerPolicyKey::IdleSeconds}) {
                const auto state = p->keys.value(key); summary << tr("%1: %2%3").arg(PowerPolicyFields::label(key),provenance(state,p->intents,key),state.reason.isEmpty()?QString():QStringLiteral(". ")+state.reason);
            }
        }
        controls.reason->setText(summary.join(QStringLiteral("; "))); controls.reason->setVisible(!summary.isEmpty());
    }
    updateAdvancedState(enabled);
    const QString support = m_policy->applySupportText();
    m_apply->setEnabled(enabled && m_policy->isDirty() && support.isEmpty()); m_apply->setToolTip(plainToolTip(support));
    m_defaults->setEnabled(enabled); m_revert->setEnabled(!m_policy->isBusy());
    QString status = m_policy->isBusy() ? support
        : !m_policy->errorText().isEmpty() ? m_policy->errorText()
        : m_policy->isDirty() ? tr("Your power policy changes have not been applied.") : m_policy->resultText();
    if (!support.isEmpty() && !status.contains(support)) status += (status.isEmpty()?QString():QStringLiteral("\n"))+support;
    m_policyStatus->setText(status); m_policyStatus->setStyleSheet(m_policy->errorText().isEmpty() ? "color: #333333; background: transparent;" : "color: #8A1C1C; background: transparent;");
    m_policyStatus->setVisible(!status.isEmpty()); m_updatingPolicy = false;
}

void PowerOptionsPage::buildPlans(const QStringList &profiles)
{
    // Map a raw PPD profile id onto its Windows-flavoured presentation.
    auto meta = [](const QString &id) -> Plan {
        if (id == QLatin1String("balanced"))
            return { id, QStringLiteral("Balanced (recommended)"),
                     QStringLiteral("Automatically balances performance with "
                                    "energy consumption on capable hardware."),
                     false };
        if (id == QLatin1String("performance"))
            return { id, QStringLiteral("High performance"),
                     QStringLiteral("Favors performance, but may use more "
                                    "energy."),
                     false };
        if (id == QLatin1String("power-saver"))
            return { id, QStringLiteral("Power saver"),
                     QStringLiteral("Saves energy by reducing your computer's "
                                    "performance where possible."),
                     true };
        // Any other profile a custom PPD setup might expose.
        QString pretty = id;
        pretty.replace(QLatin1Char('-'), QLatin1Char(' '));
        if (!pretty.isEmpty())
            pretty[0] = pretty[0].toUpper();
        return { id, pretty,
                 QStringLiteral("A custom power plan provided by your system."),
                 true };
    };

    QList<Plan> plans;

    if (profiles.isEmpty()) {
        // Fall-back: no PPD. Show a single, selected Balanced plan with no
        // profile id (selecting it is a no-op), matching what Windows shows on
        // hardware that exposes no alternative plans.
        plans << Plan{ QString(), QStringLiteral("Balanced (unavailable)"),
                       QStringLiteral("Power Profiles service is unavailable; "
                                      "no active system plan can be read or changed."),
                       false };
    } else {
        // Preferred plans first, in Windows' order, then the additional ones.
        for (const QString &id : { QStringLiteral("balanced"),
                                   QStringLiteral("performance") })
            if (profiles.contains(id))
                plans << meta(id);
        if (profiles.contains(QLatin1String("power-saver")))
            plans << meta(QStringLiteral("power-saver"));
        for (const QString &id : profiles)
            if (id != QLatin1String("balanced")
                && id != QLatin1String("performance")
                && id != QLatin1String("power-saver"))
                plans << meta(id);
    }

    for (const Plan &plan : plans) {
        QWidget *row = buildPlanRow(plan);
        (plan.additional ? m_additionalBox : m_planList)->addWidget(row);
    }

}

QWidget *PowerOptionsPage::buildPlanRow(const Plan &plan)
{
    // Layout: [radio] [ name-line over description ]. Built from nested box
    // layouts rather than a QGridLayout: a word-wrapped label in a grid cell
    // (especially a column-spanning one) is mis-sized by Qt's height-for-width
    // handling and gets clipped to a single line, which is what truncated the
    // descriptions to "Favors performance, but".
    auto *row = new QWidget;
    row->setStyleSheet("background: transparent;");
    auto *rowH = new QHBoxLayout(row);
    rowH->setContentsMargins(0, 0, 0, 0);
    rowH->setSpacing(6);

    auto *radio = new QRadioButton;
    radio->setAccessibleName(plan.name);
    radio->setProperty("profileId", plan.profileId);
    radio->setCursor(Qt::PointingHandCursor);
    radio->setStyleSheet("QRadioButton { background: transparent; }");
    radio->setEnabled(!plan.profileId.isEmpty());
    m_group->addButton(radio);
    rowH->addWidget(radio, 0, Qt::AlignTop);

    // The text block fills the rest of the row width.
    auto *textV = new QVBoxLayout;
    textV->setContentsMargins(0, 0, 0, 0);
    textV->setSpacing(2);

    // Name line: the plan name on the left, "Change plan settings" pinned right.
    auto *nameLine = new QHBoxLayout;
    nameLine->setContentsMargins(0, 0, 0, 0);
    nameLine->setSpacing(6);

    auto *name = new QLabel(plan.name);
    name->setTextFormat(Qt::PlainText);
    {
        QFont f = name->font();
        f.setPointSize(9);
        // The recommended plan's name is shown bold (a fixed emphasis in
        // Windows, independent of which plan is currently selected).
        f.setBold(plan.name.contains(QLatin1String("(recommended)")));
        name->setFont(f);
    }
    name->setStyleSheet("color: #1A1A1A; background: transparent;");
    name->setCursor(Qt::PointingHandCursor);
    // Clicking the name selects the plan, like the label in Windows.
    if (!plan.profileId.isEmpty()) {
        m_nameToRadio.insert(name, radio);
        name->installEventFilter(this);
    } else {
        name->setEnabled(false);
        name->setCursor(Qt::ArrowCursor);
        name->setStyleSheet("color: #777777; background: transparent;");
    }
    nameLine->addWidget(name, 0, Qt::AlignVCenter);
    nameLine->addStretch(1);

    auto *change = new QLabel("Change plan settings");
    {
        QFont f = change->font();
        f.setPointSize(9);
        change->setFont(f);
    }
    change->setEnabled(false);
    change->setCursor(Qt::ArrowCursor);
    change->setStyleSheet(
        "QLabel { color: #777777; background: transparent; }");
    nameLine->addWidget(change, 0, Qt::AlignVCenter);

    textV->addLayout(nameLine);

    auto *desc = new QLabel(plan.description);
    desc->setTextFormat(Qt::PlainText);
    {
        QFont f = desc->font();
        f.setPointSize(9);
        desc->setFont(f);
    }
    desc->setWordWrap(true);
    desc->setStyleSheet("color: #333333; background: transparent;");
    textV->addWidget(desc);

    rowH->addLayout(textV, 1);

    return row;
}

void PowerOptionsPage::syncSelection()
{
    if (!m_ppdAvailable)
        return;
    const QString active = activeProfile();
    m_syncing = true;
    // A failed/empty read or an unknown profile must not leave a stale,
    // optimistic radio selection presented as the daemon's current plan.
    m_group->setExclusive(false);
    for (QAbstractButton *btn : m_group->buttons())
        btn->setChecked(false);
    m_group->setExclusive(true);
    for (QAbstractButton *btn : m_group->buttons()) {
        if (!active.isEmpty()
            && btn->property("profileId").toString() == active) {
            btn->setChecked(true);
            break;
        }
    }
    m_syncing = false;
}

bool PowerOptionsPage::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::MouseButtonRelease) {
        auto it = m_nameToRadio.constFind(watched);
        if (it != m_nameToRadio.constEnd()) {
            if (m_service && m_service->isBusy())
                return true;
            auto *me = static_cast<QMouseEvent *>(event);
            if (me->button() == Qt::LeftButton) {
                QRadioButton *radio = it.value();
                if (!radio->isChecked()) {
                    radio->setChecked(true);
                    if (!m_syncing && (!m_service || !m_service->isBusy()))
                        setActiveProfile(radio->property("profileId").toString());
                }
                return true;
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}
