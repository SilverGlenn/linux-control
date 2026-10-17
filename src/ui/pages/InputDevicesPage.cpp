#include "InputDevicesPage.h"
#include "settings/InputSettings.h"
#include "Win7Ui.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QSet>
#include <algorithm>

InputDevicesPage::InputDevicesPage(QScrollArea *sidebar, QWidget *parent)
    : InputDevicesPage(sidebar, InputSettings::instance(), parent) {}
InputDevicesPage::InputDevicesPage(QScrollArea *sidebar, InputSettings *session, QWidget *parent)
    : QWidget(parent), m_session(session)
{
    Q_ASSERT(session);
    auto *layout = Win7::pageScaffold(this, sidebar, 20, 700);
    Win7::styleSettingsControls(this);
    layout->addWidget(Win7::pageTitle(tr("Mouse and Touchpad Properties")));
    auto *deviceRow = new QHBoxLayout;
    auto *deviceLabel = Win7::label(tr("&Device:"));
    m_devices = new QComboBox; m_devices->setObjectName(QStringLiteral("inputDevices"));
    m_devices->setAccessibleName(tr("Mouse or touchpad")); deviceLabel->setBuddy(m_devices);
    deviceRow->addWidget(deviceLabel); deviceRow->addWidget(m_devices, 1);
    m_refresh = new QPushButton(tr("&Refresh")); m_refresh->setObjectName(QStringLiteral("inputRefresh"));
    deviceRow->addWidget(m_refresh); layout->addLayout(deviceRow);
    m_details = Win7::bodyLabel({}); m_details->setObjectName(QStringLiteral("inputDeviceDetails"));
    m_details->setTextFormat(Qt::PlainText); layout->addWidget(m_details);
    m_tabs = new QTabWidget; m_tabs->setObjectName(QStringLiteral("inputPropertiesTabs"));
    QMap<QString, QFormLayout *> forms;
    for (const auto &name : {QStringLiteral("Buttons"), QStringLiteral("Pointer Options"), QStringLiteral("Scrolling"), QStringLiteral("Tapping"), QStringLiteral("Device")}) {
        auto *tab = new QWidget; auto *form = new QFormLayout(tab); form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        form->setContentsMargins(16, 16, 16, 16); form->setVerticalSpacing(12); forms[name] = form;
        m_tabs->addTab(tab, tr(qPrintable(name)));
    }
    auto addMode = [this, &forms](const QString &name, const QString &tab, const QString &objectName, const QStringList &keys, bool allowNone) {
        auto *combo = new QComboBox; combo->setObjectName(objectName); combo->setAccessibleName(name);
        auto *label = Win7::label(name); label->setBuddy(combo); forms[tab]->addRow(label, combo);
        connect(combo, &QComboBox::currentIndexChanged, this, [this, combo, keys, allowNone](int index) {
            if (index < 0) return;
            const QString key = combo->itemData(index).toString();
            if (!key.isEmpty()) stage(key, true);
            else if (allowNone) {
                const auto *d = m_session->staged().device(m_selected);
                if (!d) return;
                QString active;
                for (const auto &k : keys) if (d->properties.value(k).toBool()) active = k;
                if (!active.isEmpty()) stage(active, false);
            }
        });
        return combo;
    };
    m_acceleration = addMode(tr("Pointer acceleration:"), QStringLiteral("Pointer Options"), QStringLiteral("inputAccelerationProfile"), {QStringLiteral("pointerAccelerationProfileFlat"), QStringLiteral("pointerAccelerationProfileAdaptive")}, false);
    m_clickMethod = addMode(tr("Touchpad click method:"), QStringLiteral("Buttons"), QStringLiteral("inputClickMethod"), {QStringLiteral("clickMethodAreas"), QStringLiteral("clickMethodClickfinger")}, false);
    m_scrollMethod = addMode(tr("Finger or button scrolling:"), QStringLiteral("Scrolling"), QStringLiteral("inputScrollMethod"), {QStringLiteral("scrollTwoFinger"), QStringLiteral("scrollEdge"), QStringLiteral("scrollOnButtonDown")}, true);
    for (const auto &f : InputTypes::fields()) {
        const QString key = QLatin1String(f.key);
        if (!InputTypes::coupledProperties(key).isEmpty()) continue;
        auto *row = new QWidget; auto *horizontal = new QHBoxLayout(row); horizontal->setContentsMargins(0, 0, 0, 0);
        QWidget *editor = nullptr;
        if (f.type == InputField::Boolean) {
            auto *check = new QCheckBox(tr(f.label)); editor = check; horizontal->addWidget(check);
            connect(check, &QCheckBox::toggled, this, [this, key](bool value) { stage(key, value); });
        } else {
            auto *label = Win7::label(tr(f.label)); horizontal->addWidget(label);
            if (f.type == InputField::Real) {
                auto *spin = new QDoubleSpinBox; spin->setDecimals(3); spin->setRange(f.minimum, f.maximum); spin->setSingleStep(0.1); editor = spin;
                connect(spin, &QDoubleSpinBox::valueChanged, this, [this, key](double value) { stage(key, value); });
            } else {
                auto *combo = new QComboBox; editor = combo;
                connect(combo, &QComboBox::currentIndexChanged, this, [this, combo, key](int index) { if (index >= 0) stage(key, QVariant::fromValue(combo->itemData(index).toUInt())); });
            }
            label->setBuddy(editor); horizontal->addWidget(editor, 1);
        }
        editor->setObjectName(QStringLiteral("input_") + key); editor->setAccessibleName(tr(f.label));
        m_controls[key] = editor; m_rows[key] = row; forms[QLatin1String(f.tab)]->addRow(row);
    }
    layout->addWidget(m_tabs);
    auto *scope = Win7::bodyLabel(tr("Changes are staged until Apply. Revert discards pending choices and reloads current settings. Device defaults affect the settings reported by KDE; scrolling speed keeps its current value. Disabling a device stops its input; keyboard navigation remains available."));
    scope->setTextFormat(Qt::PlainText); layout->addWidget(scope);
    m_status = Win7::bodyLabel({}); m_status->setTextFormat(Qt::PlainText); m_status->setObjectName(QStringLiteral("inputStatus")); layout->addWidget(m_status);
    auto *buttons = new QHBoxLayout;
    m_defaults = new QPushButton(tr("Restore device &defaults")); m_defaults->setObjectName(QStringLiteral("inputDefaults"));
    m_revert = new QPushButton(tr("Re&vert")); m_revert->setObjectName(QStringLiteral("inputRevert"));
    m_apply = new QPushButton(tr("&Apply")); m_apply->setObjectName(QStringLiteral("inputApply"));
    buttons->addWidget(m_defaults); buttons->addStretch(); buttons->addWidget(m_revert); buttons->addWidget(m_apply); layout->addLayout(buttons); layout->addStretch();
    connect(m_devices, &QComboBox::currentIndexChanged, this, [this](int index) { if (index >= 0) { m_selected = m_devices->itemData(index).toString(); updateState(); } });
    connect(m_refresh, &QPushButton::clicked, session, &InputSettings::refresh);
    connect(m_apply, &QPushButton::clicked, session, &InputSettings::apply);
    connect(m_revert, &QPushButton::clicked, session, &InputSettings::revert);
    connect(m_defaults, &QPushButton::clicked, this, [this] { m_session->restoreDeviceDefaults(m_selected); });
    connect(session, &SettingsSession::stateChanged, this, &InputDevicesPage::updateState);
    updateState();
}
SettingsSession *InputDevicesPage::settingsSession() const { return m_session; }
QList<SidebarLink> InputDevicesPage::sidebarLinks() { return {}; }
QList<SidebarLink> InputDevicesPage::sidebarSeeAlso() { return {}; }
void InputDevicesPage::stage(const QString &key, const QVariant &value)
{ m_session->selectProperty(m_selected, key, value); }
void InputDevicesPage::updateModes(QComboBox *combo, const QStringList &keys, bool allowNone)
{
    const QSignalBlocker blocker(combo);
    const auto *d = m_session->staged().device(m_selected);
    QStringList available;
    if (d) for (const auto &key : keys) {
        const auto *f = InputTypes::field(key);
        if (f && InputTypes::supported(*d, *f)) available << key;
    }
    combo->clear();
    if (allowNone && !available.isEmpty()) combo->addItem(tr("None (wheel scrolling is unchanged)"), QString());
    QString selected;
    for (const auto &key : available) {
        combo->addItem(tr(InputTypes::field(key)->label), key);
        if (d->properties.value(key).toBool()) selected = key;
    }
    if (available.isEmpty()) combo->addItem(tr("Not supported by this device"), QString());
    int index = combo->findData(selected);
    if (index < 0 && !available.isEmpty()) { combo->insertItem(0, tr("Current mode is not editable"), QStringLiteral("unavailable")); index = 0; }
    combo->setCurrentIndex(index < 0 ? 0 : index);
    combo->setEnabled(m_session->canApply() && !m_session->isBusy() && !available.isEmpty());
}
void InputDevicesPage::updateState()
{
    const QSignalBlocker deviceBlock(m_devices);
    QStringList current;
    for (int i = 0; i < m_devices->count(); ++i) current << m_devices->itemData(i).toString();
    QStringList next;
    for (const auto &d : m_session->staged().devices) next << d.id;
    if (current != next) {
        m_devices->clear();
        for (const auto &d : m_session->staged().devices) m_devices->addItem(d.name + (d.isTouchpad() ? tr(" (Touchpad)") : tr(" (Mouse)")), d.id);
    }
    int selected = m_devices->findData(m_selected);
    if (selected < 0 && m_devices->count()) { selected = 0; m_selected = m_devices->itemData(0).toString(); }
    m_devices->setCurrentIndex(selected);
    const auto *d = m_session->staged().device(m_selected);
    const bool editable = d && m_session->canApply() && !m_session->isBusy();
    m_details->setText(d ? tr("%1 — %2. %3").arg(d->name, d->id, m_session->isDirty() ? tr("Pending choices are shown.") : tr("Current settings are shown.")) : tr("No physical mouse or touchpad is available."));
    for (auto it = m_controls.cbegin(); it != m_controls.cend(); ++it) {
        auto *widget = it.value(); const auto *f = InputTypes::field(it.key());
        const bool supported = d && InputTypes::supported(*d, *f);
        m_rows.value(it.key())->setVisible(!(f->touchpadOnly && d && !d->isTouchpad()));
        widget->setEnabled(editable && supported);
        widget->setToolTip(supported ? QString() : tr("This setting is not supported by the selected device and audited protocol."));
        const QSignalBlocker blocker(widget);
        const QVariant value = d ? d->properties.value(it.key()) : QVariant();
        if (auto *check = qobject_cast<QCheckBox *>(widget)) check->setChecked(value.toBool());
        else if (auto *spin = qobject_cast<QDoubleSpinBox *>(widget)) {
            const double n = value.toDouble();
            // Preserve out-of-range saved values visibly; don't silently clamp them.
            spin->setRange(std::min(f->minimum, n), std::max(f->maximum, n)); spin->setValue(n);
        } else if (auto *combo = qobject_cast<QComboBox *>(widget)) {
            combo->clear(); combo->addItem(tr("No button"), QVariant::fromValue(0u));
            const uint buttons = d ? d->properties.value(QStringLiteral("supportedButtons")).toUInt() : 0;
            for (uint i = 0; i < 16; ++i) if (buttons & (1u << i)) {
                const uint code = 272 + i;
                const QString name = i == 0 ? tr("Primary button") : i == 1 ? tr("Secondary button") : i == 2 ? tr("Middle button") : tr("Additional button %1").arg(i - 2);
                combo->addItem(name, QVariant::fromValue(code));
            }
            int index = combo->findData(QVariant::fromValue(value.toUInt()));
            if (index < 0) { combo->addItem(tr("Current button (%1), not advertised").arg(value.toUInt()), QVariant::fromValue(value.toUInt())); index = combo->count() - 1; }
            combo->setCurrentIndex(index);
        }
    }
    updateModes(m_acceleration, {QStringLiteral("pointerAccelerationProfileFlat"), QStringLiteral("pointerAccelerationProfileAdaptive")}, false);
    updateModes(m_clickMethod, {QStringLiteral("clickMethodAreas"), QStringLiteral("clickMethodClickfinger")}, false);
    m_clickMethod->setVisible(d && d->isTouchpad());
    if (auto *form = qobject_cast<QFormLayout *>(m_clickMethod->parentWidget()->layout()))
        if (auto *label = form->labelForField(m_clickMethod)) label->setVisible(d && d->isTouchpad());
    m_tabs->setTabVisible(3, d && d->isTouchpad());
    updateModes(m_scrollMethod, {QStringLiteral("scrollTwoFinger"), QStringLiteral("scrollEdge"), QStringLiteral("scrollOnButtonDown")}, true);
    m_status->setText(m_session->isBusy() ? tr("Reading or applying input settings…")
        : !m_session->errorText().isEmpty() ? m_session->errorText()
        : !m_session->resultText().isEmpty() ? m_session->resultText()
        : !m_session->applied().reason.isEmpty() ? m_session->applied().reason
        : m_session->isDirty() ? tr("You have pending input changes.") : tr("Settings are up to date with the last KWin read."));
    m_apply->setEnabled(!m_session->isBusy() && m_session->isDirty() && m_session->canApply());
    m_revert->setEnabled(!m_session->isBusy()); m_refresh->setEnabled(!m_session->isBusy());
    m_defaults->setEnabled(editable); m_devices->setEnabled(!m_session->isBusy());
}
