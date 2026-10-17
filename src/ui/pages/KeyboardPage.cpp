#include "KeyboardPage.h"
#include "settings/KeyboardSettings.h"
#include "Win7Ui.h"
#include "keyboard/KeyboardPreview.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTabWidget>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <algorithm>

namespace {
void field(QFormLayout *form, const QString &title, QWidget *editor, const QString &name)
{
    auto *label = Win7::label(title);
    label->setBuddy(editor);
    editor->setObjectName(name);
    editor->setAccessibleName(QString(title).remove(QLatin1Char('&')).remove(QLatin1Char(':')));
    form->addRow(label, editor);
}
void selectData(QComboBox *combo, const QVariant &value)
{
    const QSignalBlocker blocker(combo);
    int index = combo->findData(value);
    if (index < 0) {
        combo->addItem(QObject::tr("Current value: %1 (unavailable)").arg(value.toString()), value);
        index = combo->count() - 1;
    }
    combo->setCurrentIndex(index);
}
}

KeyboardPage::KeyboardPage(QScrollArea *sidebar, QWidget *parent)
    : KeyboardPage(sidebar, KeyboardSettings::instance(), parent) {}
KeyboardPage::KeyboardPage(QScrollArea *sidebar, KeyboardSettings *session, QWidget *parent)
    : QWidget(parent), m_session(session)
{
    Q_ASSERT(session);
    setObjectName(QStringLiteral("keyboardPage"));
    auto *layout = Win7::pageScaffold(this, sidebar, 20, 760);
    Win7::styleSettingsControls(this);
    setStyleSheet(styleSheet() + QStringLiteral(
        " QTabWidget::pane { border: 1px solid #ABADB3; background: #FFFFFF; }"
        " QTabBar::tab { color: #000000; background: #F0F0F0; border: 1px solid #ABADB3; padding: 6px 16px; }"
        " QTabBar::tab:selected { background: #FFFFFF; border-bottom-color: #FFFFFF; }"
        " QGroupBox, QCheckBox { color: #000000; background: transparent; }"
        " QLineEdit, QSpinBox, QDoubleSpinBox, QTreeWidget { color: #000000; background: #FFFFFF; border: 1px solid #ABADB3; selection-background-color: #CDE6F7; selection-color: #000000; }"
        " QLineEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus, QTreeWidget:focus { border-color: #3C7FB1; }"
        " QLineEdit:disabled, QSpinBox:disabled, QDoubleSpinBox:disabled { color: #777777; background: #F0F0F0; }"
        " QHeaderView::section { color: #000000; background: #F4F5F6; border: 0; border-right: 1px solid #DDDDDD; padding: 5px; }"));
    layout->addWidget(Win7::pageTitle(tr("Keyboard Properties")));
    auto *intro = Win7::bodyLabel(tr("Change character repeat, input languages, and keyboard options. Your choices are staged until you click Apply."));
    intro->setTextFormat(Qt::PlainText); layout->addWidget(intro);
    m_tabs = new QTabWidget; m_tabs->setObjectName(QStringLiteral("keyboardPropertiesTabs"));
    m_tabs->setAccessibleName(tr("Keyboard property categories")); layout->addWidget(m_tabs);

    auto *speed = new QWidget; auto *speedLayout = new QVBoxLayout(speed);
    speedLayout->setContentsMargins(16, 16, 16, 16); speedLayout->setSpacing(16);
    auto *repeatGroup = new QGroupBox(tr("Character repeat"));
    auto *repeatForm = new QFormLayout(repeatGroup); repeatForm->setVerticalSpacing(12);
    m_repeat = new QComboBox;
    field(repeatForm, tr("&Key repeat:"), m_repeat, QStringLiteral("keyboardRepeat"));
    m_delay = new QSpinBox; m_delay->setRange(100, 5000); m_delay->setSingleStep(50); m_delay->setSuffix(tr(" ms"));
    m_delay->setKeyboardTracking(false);
    field(repeatForm, tr("Repeat &delay:"), m_delay, QStringLiteral("keyboardRepeatDelay"));
    m_rate = new QDoubleSpinBox; m_rate->setRange(0.2, 200); m_rate->setDecimals(2); m_rate->setSuffix(tr(" repeats/s"));
    m_rate->setKeyboardTracking(false);
    field(repeatForm, tr("Repeat &rate:"), m_rate, QStringLiteral("keyboardRepeatRate"));
    auto *test = new QLineEdit; test->setPlaceholderText(tr("Click here and hold down a key to test your current keyboard settings"));
    field(repeatForm, tr("&Test area:"), test, QStringLiteral("keyboardTestArea"));
    speedLayout->addWidget(repeatGroup);
    auto *lockGroup = new QGroupBox(tr("Startup state")); auto *lockForm = new QFormLayout(lockGroup);
    m_numLock = new QComboBox;
    field(lockForm, tr("&Num Lock:"), m_numLock, QStringLiteral("keyboardNumLock"));
    m_numLock->setToolTip(tr("Saved startup preference; this does not change the current Num Lock state."));
    speedLayout->addWidget(lockGroup); speedLayout->addStretch();
    m_tabs->addTab(speed, tr("&Speed"));

    auto *languages = new QWidget; auto *languageLayout = new QVBoxLayout(languages);
    languageLayout->setContentsMargins(16, 16, 16, 16); languageLayout->setSpacing(10);
    m_configureLayouts = new QCheckBox(tr("&Configure keyboard layouts"));
    m_configureLayouts->setObjectName(QStringLiteral("keyboardConfigureLayouts"));
    m_configureLayouts->setAccessibleName(tr("Configure keyboard layouts")); languageLayout->addWidget(m_configureLayouts);
    auto *languageHint = Win7::bodyLabel(tr("Layouts are tried in the order shown. Add a language and variant, then move it up or down to change the order."));
    languageLayout->addWidget(languageHint);
    m_layouts = new QTreeWidget; m_layouts->setObjectName(QStringLiteral("keyboardLayouts"));
    m_layouts->setAccessibleName(tr("Ordered keyboard layouts"));
    m_layouts->setHeaderLabels({tr("Language / layout"), tr("Variant"), tr("Display label")});
    m_layouts->setRootIsDecorated(false); m_layouts->setSelectionMode(QAbstractItemView::SingleSelection);
    m_layouts->setMinimumHeight(145); m_layouts->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_layouts->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_layouts->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    languageLayout->addWidget(m_layouts);
    auto *orderButtons = new QHBoxLayout;
    m_preview = new QPushButton(tr("&Preview…")); m_preview->setObjectName(QStringLiteral("keyboardPreviewLayout"));
    m_preview->setAccessibleName(tr("Preview selected keyboard layout"));
    m_preview->setToolTip(tr("Inspect the selected layout with the staged keyboard model and options without applying changes."));
    m_remove = new QPushButton(tr("&Remove")); m_remove->setObjectName(QStringLiteral("keyboardRemoveLayout"));
    m_up = new QPushButton(tr("Move &up")); m_up->setObjectName(QStringLiteral("keyboardLayoutUp"));
    m_down = new QPushButton(tr("Move do&wn")); m_down->setObjectName(QStringLiteral("keyboardLayoutDown"));
    orderButtons->addWidget(m_preview); orderButtons->addWidget(m_remove); orderButtons->addStretch(); orderButtons->addWidget(m_up); orderButtons->addWidget(m_down);
    languageLayout->addLayout(orderButtons);
    auto *languageForm = new QFormLayout; languageForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    m_layoutChoice = new QComboBox; m_layoutChoice->setMaxVisibleItems(16);
    field(languageForm, tr("&Language / layout:"), m_layoutChoice, QStringLiteral("keyboardLayoutChoice"));
    m_variantChoice = new QComboBox; m_variantChoice->setMaxVisibleItems(16);
    field(languageForm, tr("&Variant:"), m_variantChoice, QStringLiteral("keyboardVariantChoice"));
    m_add = new QPushButton(tr("&Add layout")); m_add->setObjectName(QStringLiteral("keyboardAddLayout"));
    languageForm->addRow(QString(), m_add);
    m_displayLabel = new QLineEdit; m_displayLabel->setMaxLength(3); m_displayLabel->setPlaceholderText(tr("Use the layout's default label"));
    field(languageForm, tr("Selected layout's display la&bel:"), m_displayLabel, QStringLiteral("keyboardDisplayLabel"));
    languageLayout->addLayout(languageForm);
    auto *switchForm = new QFormLayout;
    m_policy = new QComboBox;
    field(switchForm, tr("Switching &policy:"), m_policy, QStringLiteral("keyboardSwitchingPolicy"));
    m_loop = new QSpinBox; m_loop->setRange(-1, 32); m_loop->setSpecialValueText(tr("All layouts")); m_loop->setKeyboardTracking(false);
    field(switchForm, tr("Main layout &loop:"), m_loop, QStringLiteral("keyboardLayoutLoop"));
    m_loop->setToolTip(tr("KWin on Wayland ignores the main layout loop. This saved preference is read-only on that session."));
    languageLayout->addLayout(switchForm);
    m_layoutOsd = new QCheckBox(tr("Show an on-screen &message when the keyboard layout changes"));
    m_layoutOsd->setObjectName(QStringLiteral("keyboardLayoutOsd"));
    m_layoutOsd->setAccessibleName(tr("Show an on-screen message when the keyboard layout changes"));
    m_layoutOsd->setToolTip(tr("Saves Plasma's layout notification preference. The current layout is unchanged."));
    languageLayout->addWidget(m_layoutOsd); languageLayout->addStretch();
    m_tabs->addTab(languages, tr("Input &Languages"));

    auto *advanced = new QWidget; auto *advancedLayout = new QVBoxLayout(advanced);
    advancedLayout->setContentsMargins(16, 16, 16, 16); advancedLayout->setSpacing(12);
    auto *modelForm = new QFormLayout; m_model = new QComboBox; m_model->setMaxVisibleItems(16);
    field(modelForm, tr("Keyboard &model:"), m_model, QStringLiteral("keyboardModel")); advancedLayout->addLayout(modelForm);
    m_resetOptions = new QCheckBox(tr("&Configure advanced keyboard options"));
    m_resetOptions->setObjectName(QStringLiteral("keyboardResetOptions"));
    m_resetOptions->setAccessibleName(tr("Configure advanced keyboard options")); advancedLayout->addWidget(m_resetOptions);
    auto *optionHint = Win7::bodyLabel(tr("Expand a group to choose its options. Groups that allow one choice replace the previous choice automatically."));
    advancedLayout->addWidget(optionHint);
    m_options = new QTreeWidget; m_options->setObjectName(QStringLiteral("keyboardOptions"));
    m_options->setAccessibleName(tr("Advanced XKB keyboard options")); m_options->setHeaderHidden(true);
    m_options->setMinimumHeight(260); m_options->setWordWrap(true); advancedLayout->addWidget(m_options, 1);
    m_tabs->addTab(advanced, tr("&Advanced"));

    m_runtime = Win7::bodyLabel({}); m_runtime->setTextFormat(Qt::PlainText); m_runtime->setObjectName(QStringLiteral("keyboardRuntimeStatus"));
    layout->addWidget(m_runtime);
    m_status = Win7::bodyLabel({}); m_status->setTextFormat(Qt::PlainText); m_status->setObjectName(QStringLiteral("keyboardStatus"));
    layout->addWidget(m_status);
    auto *buttons = new QHBoxLayout;
    m_defaults = new QPushButton(tr("Restore &defaults")); m_defaults->setObjectName(QStringLiteral("keyboardDefaults"));
    m_refresh = new QPushButton(tr("&Refresh")); m_refresh->setObjectName(QStringLiteral("keyboardRefresh"));
    m_revert = new QPushButton(tr("Re&vert")); m_revert->setObjectName(QStringLiteral("keyboardRevert"));
    m_apply = new QPushButton(tr("&Apply")); m_apply->setObjectName(QStringLiteral("keyboardApply"));
    buttons->addWidget(m_defaults); buttons->addWidget(m_refresh); buttons->addStretch(); buttons->addWidget(m_revert); buttons->addWidget(m_apply);
    layout->addLayout(buttons); layout->addStretch();
    connect(m_layoutChoice, &QComboBox::currentIndexChanged, this, &KeyboardPage::updateVariants);
    connect(m_layouts, &QTreeWidget::currentItemChanged, this, [this] { updateLayoutActions(); });
    connect(m_add, &QPushButton::clicked, this, &KeyboardPage::addLayout);
    connect(m_remove, &QPushButton::clicked, this, &KeyboardPage::removeLayout);
    connect(m_preview, &QPushButton::clicked, this, &KeyboardPage::previewLayout);
    connect(m_up, &QPushButton::clicked, this, [this] { moveLayout(-1); });
    connect(m_down, &QPushButton::clicked, this, [this] { moveLayout(1); });
    connect(m_displayLabel, &QLineEdit::textEdited, this, &KeyboardPage::stageLabel);
    connect(m_options, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem *item, int) { updateOptions(item); });
    connect(m_refresh, &QPushButton::clicked, session, &KeyboardSettings::refresh);
    connect(m_apply, &QPushButton::clicked, session, &KeyboardSettings::apply);
    connect(m_revert, &QPushButton::clicked, session, &KeyboardSettings::revert);
    connect(m_defaults, &QPushButton::clicked, session, &KeyboardSettings::restoreDefaults);
    connect(session, &SettingsSession::stateChanged, this, &KeyboardPage::updateState);
    m_repeat->addItem(tr("Repeat characters"), QStringLiteral("repeat"));
    m_repeat->addItem(tr("Show accent choices"), QStringLiteral("accent"));
    m_repeat->addItem(tr("Do not repeat"), QStringLiteral("nothing"));
    m_numLock->addItem(tr("On"), 0); m_numLock->addItem(tr("Off"), 1); m_numLock->addItem(tr("Leave unchanged"), 2);
    for (const auto &choice : QList<QPair<QString, QString>>{{tr("Global"), "Global"}, {tr("Per desktop"), "Desktop"}, {tr("Per application"), "WinClass"}, {tr("Per window"), "Window"}})
        m_policy->addItem(choice.first, choice.second);
    auto comboBinding = [this](QComboBox *combo, const QString &key) {
        connect(combo, &QComboBox::currentIndexChanged, this, [this, combo, key](int index) {
            if (!m_updating && index >= 0) m_session->selectValue(key, combo->itemData(index));
        });
    };
    comboBinding(m_repeat, QStringLiteral("KeyRepeat")); comboBinding(m_numLock, QStringLiteral("NumLock"));
    comboBinding(m_model, QStringLiteral("Model")); comboBinding(m_policy, QStringLiteral("SwitchMode"));
    connect(m_delay, &QSpinBox::valueChanged, this, [this](int value) { if (!m_updating) m_session->selectValue(QStringLiteral("RepeatDelay"), value); });
    connect(m_rate, &QDoubleSpinBox::valueChanged, this, [this](double value) { if (!m_updating) m_session->selectValue(QStringLiteral("RepeatRate"), value); });
    connect(m_loop, &QSpinBox::valueChanged, this, [this](int value) {
        if (m_updating) return;
        // The native loop has no one-layout or zero-layout state.
        if (value < 2) {
            value = m_session->staged().values.value(QStringLiteral("LayoutLoopCount")).toInt() == -1 ? 2 : -1;
            const QSignalBlocker blocker(m_loop); m_loop->setValue(value);
        }
        m_session->selectValue(QStringLiteral("LayoutLoopCount"), value);
    });
    connect(m_configureLayouts, &QCheckBox::toggled, this, [this](bool value) { if (!m_updating) m_session->selectValue(QStringLiteral("Use"), value); });
    connect(m_resetOptions, &QCheckBox::toggled, this, [this](bool value) { if (!m_updating) m_session->selectValue(QStringLiteral("ResetOldOptions"), value); });
    connect(m_layoutOsd, &QCheckBox::toggled, this, [this](bool value) {
        if (!m_updating) m_session->selectValue(QStringLiteral("kbdLayoutChangedEnabled"), value);
    });
    updateState();
}

SettingsSession *KeyboardPage::settingsSession() const { return m_session; }
QList<SidebarLink> KeyboardPage::sidebarLinks()
{ return {Nav::to(tr("Mouse and Touchpad"), PageId::InputDevices), Nav::to(tr("Keyboard Shortcuts"), PageId::Shortcuts)}; }
QList<SidebarLink> KeyboardPage::sidebarSeeAlso()
{ return {Nav::to(tr("Ease of Access Center"), PageId::EaseOfAccess)}; }

void KeyboardPage::updateVariants()
{
    const QSignalBlocker blocker(m_variantChoice);
    const QString previous = m_variantChoice->currentData().toString();
    m_variantChoice->clear(); m_variantChoice->addItem(tr("Default"), QString());
    if (const auto *layout = m_session->staged().registry.layout(m_layoutChoice->currentData().toString()))
        for (const auto &variant : layout->variants) m_variantChoice->addItem(variant.description, variant.id);
    const int found = m_variantChoice->findData(previous);
    m_variantChoice->setCurrentIndex(found < 0 ? 0 : found);
    updateLayoutActions();
}

void KeyboardPage::updateLayoutActions()
{
    if (m_updating) return;
    const auto &s = m_session->staged();
    const bool editable = m_session->canApply() && !m_session->isBusy();
    const bool layoutsEditable = editable && !s.immutableKeys.contains(QStringLiteral("Use"))
        && !s.immutableKeys.contains(QStringLiteral("LayoutList"))
        && !s.immutableKeys.contains(QStringLiteral("VariantList"))
        && !s.immutableKeys.contains(QStringLiteral("DisplayNames"));
    const int selected = m_layouts->indexOfTopLevelItem(m_layouts->currentItem());
    const int count = m_layouts->topLevelItemCount();
    m_add->setEnabled(layoutsEditable && s.registry.valid && m_layoutChoice->currentIndex() >= 0 && count < 32);
    m_preview->setEnabled(m_session->isReady() && selected >= 0);
    m_remove->setEnabled(layoutsEditable && selected >= 0 && count > 1);
    m_up->setEnabled(layoutsEditable && selected > 0);
    m_down->setEnabled(layoutsEditable && selected >= 0 && selected + 1 < count);
    m_layoutChoice->setEnabled(layoutsEditable && s.registry.valid); m_variantChoice->setEnabled(m_layoutChoice->isEnabled());
    m_displayLabel->setEnabled(layoutsEditable && selected >= 0);
    const QStringList labels = s.values.value(QStringLiteral("DisplayNames")).toStringList();
    const QString text = selected >= 0 ? labels.value(selected) : QString();
    if (m_displayLabel->text() != text) {
        const QSignalBlocker blocker(m_displayLabel); m_displayLabel->setMaxLength(std::max(3, int(text.size()))); m_displayLabel->setText(text);
    }
}

void KeyboardPage::previewLayout()
{
    if (auto *existing = findChild<KeyboardPreviewDialog *>()) {
        existing->show(); existing->raise(); existing->activateWindow(); return;
    }
    const int row = m_layouts->indexOfTopLevelItem(m_layouts->currentItem());
    const auto &values = m_session->staged().values;
    const auto layouts = values.value(QStringLiteral("LayoutList")).toStringList();
    if (row < 0 || row >= layouts.size()) return;
    const KeyboardPreview::Request request{
        values.value(QStringLiteral("Model")).toString(), layouts[row],
        values.value(QStringLiteral("VariantList")).toStringList().value(row),
        values.value(QStringLiteral("Options")).toStringList()};
    auto *dialog = new KeyboardPreviewDialog(request, this);
    dialog->setWindowModality(Qt::WindowModal); dialog->show();
}

void KeyboardPage::addLayout()
{
    const auto &values = m_session->staged().values;
    auto layouts = values.value(QStringLiteral("LayoutList")).toStringList();
    auto variants = values.value(QStringLiteral("VariantList")).toStringList();
    auto labels = values.value(QStringLiteral("DisplayNames")).toStringList();
    const QString layout = m_layoutChoice->currentData().toString(), variant = m_variantChoice->currentData().toString();
    for (int i = 0; i < layouts.size(); ++i) if (layouts[i] == layout && variants.value(i) == variant) {
        m_layouts->setCurrentItem(m_layouts->topLevelItem(i)); return;
    }
    layouts << layout; variants << variant; labels << QString();
    m_session->selectValues({{QStringLiteral("Use"), true}, {QStringLiteral("LayoutList"), layouts}, {QStringLiteral("VariantList"), variants}, {QStringLiteral("DisplayNames"), labels}});
    m_layouts->setCurrentItem(m_layouts->topLevelItem(layouts.size() - 1));
}

void KeyboardPage::removeLayout()
{
    const int row = m_layouts->indexOfTopLevelItem(m_layouts->currentItem());
    if (row < 0) return;
    QVariantMap delta;
    for (const auto &key : {QStringLiteral("LayoutList"), QStringLiteral("VariantList"), QStringLiteral("DisplayNames")}) {
        auto list = m_session->staged().values.value(key).toStringList(); if (row < list.size()) list.removeAt(row); delta[key] = list;
    }
    const int count = delta.value(QStringLiteral("LayoutList")).toStringList().size();
    if (m_session->staged().values.value(QStringLiteral("LayoutLoopCount")).toInt() > count) delta[QStringLiteral("LayoutLoopCount")] = count >= 2 ? count : -1;
    m_session->selectValues(delta);
    m_layouts->setCurrentItem(m_layouts->topLevelItem(std::min(row, count - 1)));
}

void KeyboardPage::moveLayout(int direction)
{
    const int row = m_layouts->indexOfTopLevelItem(m_layouts->currentItem());
    const int target = row + direction;
    if (row < 0 || target < 0 || target >= m_layouts->topLevelItemCount()) return;
    QVariantMap delta;
    for (const auto &key : {QStringLiteral("LayoutList"), QStringLiteral("VariantList"), QStringLiteral("DisplayNames")}) {
        auto list = m_session->staged().values.value(key).toStringList(); if (target >= list.size() || row >= list.size()) return;
        list.swapItemsAt(row, target); delta[key] = list;
    }
    m_session->selectValues(delta); m_layouts->setCurrentItem(m_layouts->topLevelItem(target));
}

void KeyboardPage::stageLabel()
{
    if (m_updating) return;
    const int row = m_layouts->indexOfTopLevelItem(m_layouts->currentItem());
    auto labels = m_session->staged().values.value(QStringLiteral("DisplayNames")).toStringList();
    if (row < 0 || row >= labels.size()) return;
    labels[row] = m_displayLabel->text(); m_session->selectValue(QStringLiteral("DisplayNames"), labels);
}

void KeyboardPage::updateOptions(QTreeWidgetItem *item)
{
    if (m_updating || !item || !item->parent()) return;
    const QString id = item->data(0, Qt::UserRole).toString();
    if (id.isEmpty()) return;
    auto options = m_session->staged().values.value(QStringLiteral("Options")).toStringList();
    const bool checked = item->checkState(0) == Qt::Checked;
    if (checked && !item->parent()->data(0, Qt::UserRole + 1).toBool())
        for (int i = 0; i < item->parent()->childCount(); ++i) options.removeAll(item->parent()->child(i)->data(0, Qt::UserRole).toString());
    options.removeAll(id); if (checked) options << id;
    m_session->selectValue(QStringLiteral("Options"), options);
}

void KeyboardPage::updateState()
{
    m_updating = true;
    const auto &s = m_session->staged(); const auto &values = s.values;
    const bool editable = m_session->canApply() && !m_session->isBusy();
    if (!m_registryBuilt || m_registryRevision != s.registry.revision) {
        m_registryBuilt = true; m_registryRevision = s.registry.revision;
        const QString model = m_model->currentData().toString(), layout = m_layoutChoice->currentData().toString();
        const QSignalBlocker modelBlock(m_model), layoutBlock(m_layoutChoice), optionBlock(m_options);
        m_model->clear(); for (const auto &choice : s.registry.models) m_model->addItem(choice.description, choice.id);
        m_layoutChoice->clear(); for (const auto &choice : s.registry.layouts) m_layoutChoice->addItem(choice.description, choice.id);
        m_model->setCurrentIndex(m_model->findData(model));
        int index = m_layoutChoice->findData(layout); m_layoutChoice->setCurrentIndex(index < 0 && m_layoutChoice->count() ? 0 : index);
        updateVariants();
        m_options->clear();
        for (const auto &group : s.registry.optionGroups) {
            auto *parent = new QTreeWidgetItem(m_options, {group.description});
            parent->setData(0, Qt::UserRole + 1, group.allowMultiple);
            parent->setToolTip(0, group.allowMultiple ? tr("Choose any number of options in this group") : tr("Choose at most one option in this group"));
            for (const auto &option : group.options) {
                auto *child = new QTreeWidgetItem(parent, {option.description});
                child->setData(0, Qt::UserRole, option.id); child->setFlags(child->flags() | Qt::ItemIsUserCheckable);
                child->setCheckState(0, Qt::Unchecked); child->setToolTip(0, option.id);
            }
        }
    }
    const QMap<QString, QComboBox *> combos{{QStringLiteral("KeyRepeat"), m_repeat}, {QStringLiteral("NumLock"), m_numLock}, {QStringLiteral("Model"), m_model}, {QStringLiteral("SwitchMode"), m_policy}};
    for (auto it = combos.cbegin(); it != combos.cend(); ++it) {
        selectData(it.value(), values.value(it.key())); it.value()->setEnabled(editable && !s.immutableKeys.contains(it.key()));
    }
    auto spin = [this, editable, &s](QSpinBox *editor, const QString &key) {
        const QSignalBlocker blocker(editor); const int n = s.values.value(key).toInt();
        if (editor->value() != n && !editor->hasFocus()) { editor->setRange(std::min(editor->minimum(), n), std::max(editor->maximum(), n)); editor->setValue(n); }
        editor->setEnabled((editable || m_session->isBusy()) && !s.immutableKeys.contains(key));
        editor->setReadOnly(!editable);
    };
    spin(m_delay, QStringLiteral("RepeatDelay")); spin(m_loop, QStringLiteral("LayoutLoopCount"));
    {
        const QSignalBlocker blocker(m_rate); const double n = values.value(QStringLiteral("RepeatRate")).toDouble();
        if (m_rate->value() != n && !m_rate->hasFocus()) { m_rate->setRange(std::min(m_rate->minimum(), n), std::max(m_rate->maximum(), n)); m_rate->setValue(n); }
        m_rate->setEnabled((editable || m_session->isBusy()) && !s.immutableKeys.contains(QStringLiteral("RepeatRate")));
        m_rate->setReadOnly(!editable);
    }
    const bool repeats = values.value(QStringLiteral("KeyRepeat")).toString() != QLatin1String("nothing");
    m_delay->setEnabled(m_delay->isEnabled() && repeats); m_rate->setEnabled(m_rate->isEnabled() && repeats);
    for (const auto &pair : QList<QPair<QCheckBox *, QString>>{{m_configureLayouts, QStringLiteral("Use")}, {m_resetOptions, QStringLiteral("ResetOldOptions")}, {m_layoutOsd, QStringLiteral("kbdLayoutChangedEnabled")}}) {
        const QSignalBlocker blocker(pair.first); pair.first->setChecked(values.value(pair.second).toBool());
        pair.first->setEnabled(editable && !s.immutableKeys.contains(pair.second));
    }
    const auto layouts = values.value(QStringLiteral("LayoutList")).toStringList();
    const auto variants = values.value(QStringLiteral("VariantList")).toStringList();
    const auto labels = values.value(QStringLiteral("DisplayNames")).toStringList();
    QStringList shown;
    for (int i = 0; i < m_layouts->topLevelItemCount(); ++i) shown << m_layouts->topLevelItem(i)->data(0, Qt::UserRole).toString();
    QStringList wanted; for (int i = 0; i < layouts.size(); ++i) wanted << layouts[i] + QLatin1Char('\n') + variants.value(i) + QLatin1Char('\n') + labels.value(i);
    if (shown != wanted) {
        const QSignalBlocker blocker(m_layouts); const int selected = m_layouts->indexOfTopLevelItem(m_layouts->currentItem());
        m_layouts->clear();
        for (int i = 0; i < layouts.size(); ++i) {
            const auto *info = s.registry.layout(layouts[i]);
            QString variant = variants.value(i).isEmpty() ? tr("Default") : variants.value(i);
            if (info) for (const auto &choice : info->variants) if (choice.id == variants.value(i)) variant = choice.description;
            auto *item = new QTreeWidgetItem(m_layouts, {info ? info->description : tr("%1 (unavailable)").arg(layouts[i]), variant, labels.value(i)});
            item->setData(0, Qt::UserRole, wanted[i]);
        }
        m_layouts->setCurrentItem(m_layouts->topLevelItem(selected < 0 ? 0 : std::min(selected, int(layouts.size()) - 1)));
    }
    const auto options = values.value(QStringLiteral("Options")).toStringList();
    const bool optionsEditable = editable && m_resetOptions->isChecked() && !s.immutableKeys.contains(QStringLiteral("Options"));
    {
        const QSignalBlocker blocker(m_options); QStringList known;
        for (int i = m_options->topLevelItemCount() - 1; i >= 0; --i) {
            auto *group = m_options->topLevelItem(i);
            if (group->data(0, Qt::UserRole + 2).toBool()) { delete m_options->takeTopLevelItem(i); continue; }
            for (int j = 0; j < group->childCount(); ++j) {
                auto *item = group->child(j); const QString id = item->data(0, Qt::UserRole).toString(); known << id;
                item->setCheckState(0, options.contains(id) ? Qt::Checked : Qt::Unchecked);
                item->setDisabled(!optionsEditable);
            }
        }
        QTreeWidgetItem *unknown = nullptr;
        for (const auto &id : options) if (!known.contains(id)) {
            if (!unknown) { unknown = new QTreeWidgetItem(m_options, {tr("Saved options unavailable in this registry")}); unknown->setData(0, Qt::UserRole + 2, true); }
            auto *item = new QTreeWidgetItem(unknown, {id}); item->setCheckState(0, Qt::Checked); item->setDisabled(true);
        }
    }
    m_loop->setEnabled(m_loop->isEnabled() && m_configureLayouts->isChecked() && layouts.size() >= 2);
    m_policy->setEnabled(m_policy->isEnabled() && m_configureLayouts->isChecked());
    m_runtime->setText(s.runtimeStatus.isEmpty() ? tr("Saved keyboard preferences are shown. Runtime adoption has not been verified.") : s.runtimeStatus);
    QString status = m_session->isBusy() ? tr("Reading or applying keyboard settings…")
        : !m_session->errorText().isEmpty() ? m_session->errorText()
        : !m_session->resultText().isEmpty() ? m_session->resultText()
        : !s.reason.isEmpty() ? s.reason
        : m_session->isDirty() ? tr("You have pending keyboard changes.") : tr("Saved keyboard settings are up to date.");
    if (m_session->hasConflict()) status += tr("\nYour choices are preserved. Revert reloads the saved settings before another Apply.");
    if (!s.registry.valid && !s.registry.error.isEmpty()) status += QLatin1Char('\n') + s.registry.error;
    m_status->setText(status);
    m_apply->setEnabled(editable && m_session->isDirty());
    m_revert->setEnabled(!m_session->isBusy()); m_refresh->setEnabled(!m_session->isBusy());
    m_defaults->setEnabled(editable);
    m_updating = false; updateLayoutActions();
}
