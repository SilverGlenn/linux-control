#include "CustomShortcutsPage.h"
#include "settings/CustomShortcutsSettings.h"
#include "Win7Ui.h"
#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTreeWidget>

namespace {
QPushButton *button(const QString &label, const char *name) {
    auto *result = new QPushButton(label); result->setObjectName(QLatin1String(name)); return result;
}
QLabel *plain(const QString &text, const char *name) {
    auto *result = Win7::bodyLabel(text); result->setObjectName(QLatin1String(name)); result->setTextFormat(Qt::PlainText); return result;
}
QString plainToolTip(const QString &text) { return QStringLiteral("<qt>%1</qt>").arg(text.toHtmlEscaped()); }
}
CustomShortcutsPage::CustomShortcutsPage(QScrollArea *sidebar, QWidget *parent)
    : CustomShortcutsPage(sidebar, CustomShortcutsSettings::instance(), parent) {}
CustomShortcutsPage::CustomShortcutsPage(QScrollArea *sidebar, CustomShortcutsSettings *session, QWidget *parent)
    : QWidget(parent), m_session(session)
{
    Q_ASSERT(session);
    auto *layout = Win7::pageScaffold(this, sidebar, 18, 900); Win7::styleSettingsControls(this);
    setProperty("settingsPage", QStringLiteral("customShortcuts"));
    setStyleSheet(styleSheet() + QStringLiteral(
        " QGroupBox { color: #000000; background: transparent; }"
        " QTreeWidget, QLineEdit { color: #000000; background: #FFFFFF; border: 1px solid #ABADB3; selection-background-color: #CDE6F7; selection-color: #000000; }"
        " QTreeWidget:focus, QLineEdit:focus { border-color: #3C7FB1; }"
        " QHeaderView::section { color: #000000; background: #F4F5F6; border: 0; border-right: 1px solid #DDDDDD; padding: 5px; }"));
    layout->addWidget(Win7::pageTitle(tr("Custom Keyboard Shortcuts")));
    layout->addWidget(plain(tr("Add applications or commands, then click Apply to save their shortcut registrations. Assign key combinations in Global Keyboard Shortcuts afterward. Removing an application here removes its shortcut registration; the application remains installed. Commands are saved without being run."), "customShortcutsIntroduction"));
    m_entries = new QTreeWidget; m_entries->setObjectName(QStringLiteral("customShortcutsEntries"));
    m_entries->setAccessibleName(tr("Application and command shortcut registrations"));
    m_entries->setHeaderLabels({tr("Name"), tr("Type"), tr("Command / application"), tr("Status")});
    m_entries->setRootIsDecorated(false); m_entries->setSelectionMode(QAbstractItemView::SingleSelection);
    m_entries->setMinimumHeight(200);
    m_entries->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_entries->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_entries->header()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_entries->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    layout->addWidget(m_entries, 1);
    auto *entryButtons = new QHBoxLayout;
    m_register = button(tr("Register selec&ted"), "customShortcutsRegister");
    m_edit = button(tr("Edit &selected command"), "customShortcutsEdit");
    m_remove = button(tr("&Remove selected"), "customShortcutsRemove");
    m_restore = button(tr("&Keep selected"), "customShortcutsRestore");
    m_assign = button(tr("Assign shortcut &keys"), "customShortcutsAssign");
    entryButtons->addWidget(m_register); entryButtons->addWidget(m_edit); entryButtons->addWidget(m_remove); entryButtons->addWidget(m_restore);
    entryButtons->addStretch(); entryButtons->addWidget(m_assign); layout->addLayout(entryButtons);
    m_details = plain({}, "customShortcutsDetails"); layout->addWidget(m_details);
    auto *applicationGroup = new QGroupBox(tr("Add an installed application")); auto *applicationLayout = new QFormLayout(applicationGroup);
    m_filter = new QLineEdit; m_filter->setObjectName(QStringLiteral("customShortcutsApplicationFilter"));
    m_filter->setAccessibleName(tr("Find installed applications")); m_filter->setClearButtonEnabled(true);
    auto *filterLabel = Win7::label(tr("&Find applications:")); filterLabel->setBuddy(m_filter); applicationLayout->addRow(filterLabel, m_filter);
    m_applications = new QComboBox; m_applications->setObjectName(QStringLiteral("customShortcutsApplications"));
    m_applications->setAccessibleName(tr("Installed application to add")); m_applications->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    auto *applicationRow = new QHBoxLayout; applicationRow->addWidget(m_applications, 1);
    m_addApplication = button(tr("Add a&pplication"), "customShortcutsAddApplication"); applicationRow->addWidget(m_addApplication);
    auto *applicationLabel = Win7::label(tr("Application:")); applicationLabel->setBuddy(m_applications);
    applicationLayout->addRow(applicationLabel, applicationRow); layout->addWidget(applicationGroup);
    auto *commandGroup = new QGroupBox(tr("Custom command")); auto *commandLayout = new QFormLayout(commandGroup);
    m_name = new QLineEdit; m_name->setObjectName(QStringLiteral("customShortcutsName")); m_name->setAccessibleName(tr("Command name"));
    m_command = new QLineEdit; m_command->setObjectName(QStringLiteral("customShortcutsCommand")); m_command->setAccessibleName(tr("Command or script with arguments"));
    m_command->setPlaceholderText(tr("Command or script, with arguments"));
    auto *nameLabel = Win7::label(tr("&Name:")); nameLabel->setBuddy(m_name); commandLayout->addRow(nameLabel, m_name);
    auto *commandLabel = Win7::label(tr("&Command:")); commandLabel->setBuddy(m_command); commandLayout->addRow(commandLabel, m_command);
    auto *commandButtons = new QHBoxLayout;
    m_saveCommand = button(tr("Add co&mmand"), "customShortcutsSaveCommand");
    m_cancelEdit = button(tr("Cancel &editing"), "customShortcutsCancelEdit");
    commandButtons->addWidget(m_saveCommand); commandButtons->addWidget(m_cancelEdit); commandButtons->addStretch(); commandLayout->addRow(commandButtons);
    layout->addWidget(commandGroup);
    m_status = plain({}, "customShortcutsStatus"); layout->addWidget(m_status);
    auto *buttons = new QHBoxLayout;
    m_refresh = button(tr("Refres&h"), "customShortcutsRefresh"); m_revert = button(tr("Re&vert"), "customShortcutsRevert"); m_apply = button(tr("&Apply"), "customShortcutsApply");
    buttons->addWidget(m_refresh); buttons->addStretch(); buttons->addWidget(m_revert); buttons->addWidget(m_apply); layout->addLayout(buttons);
    connect(m_filter, &QLineEdit::textChanged, this, &CustomShortcutsPage::filterApplications);
    connect(m_applications, &QComboBox::currentIndexChanged, this, [this] { if (!m_updating) updateSelection(); });
    connect(m_entries, &QTreeWidget::currentItemChanged, this, [this] { if (!m_updating) updateSelection(); });
    connect(m_name, &QLineEdit::textChanged, this, &CustomShortcutsPage::updateSelection);
    connect(m_command, &QLineEdit::textChanged, this, &CustomShortcutsPage::updateSelection);
    connect(m_addApplication, &QPushButton::clicked, this, [this] { m_session->stageAddApplication(m_applications->currentData().toString()); });
    connect(m_register, &QPushButton::clicked, this, [this] { m_session->stageAddApplication(selectedComponent()); });
    connect(m_edit, &QPushButton::clicked, this, [this] {
        const auto *entry = m_session->staged().find(selectedComponent()); if (!entry || !entry->command || !entry->editable) return;
        m_editComponent = entry->component; m_name->setText(entry->name); m_command->setText(entry->exec); m_name->setFocus(); updateSelection();
    });
    connect(m_remove, &QPushButton::clicked, this, [this] { m_session->stageRemove(selectedComponent()); });
    connect(m_restore, &QPushButton::clicked, this, [this] { m_session->stageRestore(selectedComponent()); });
    connect(m_assign, &QPushButton::clicked, this, [this] { emit navigationRequested(PageId::Shortcuts); });
    connect(m_cancelEdit, &QPushButton::clicked, this, &CustomShortcutsPage::finishEditing);
    connect(m_saveCommand, &QPushButton::clicked, this, [this] {
        const bool staged = m_editComponent.isEmpty() ? m_session->stageAddCommand(m_name->text(), m_command->text())
            : m_session->stageEditCommand(m_editComponent, m_name->text(), m_command->text());
        if (staged) finishEditing();
    });
    connect(m_refresh, &QPushButton::clicked, session, &CustomShortcutsSettings::refresh);
    connect(m_revert, &QPushButton::clicked, this, [this] { finishEditing(); m_session->revert(); });
    connect(m_apply, &QPushButton::clicked, session, &CustomShortcutsSettings::apply);
    connect(session, &SettingsSession::stateChanged, this, &CustomShortcutsPage::updateState); updateState();
}
SettingsSession *CustomShortcutsPage::settingsSession() const { return m_session; }
QList<SidebarLink> CustomShortcutsPage::sidebarLinks() { return {Nav::to(tr("Global Keyboard Shortcuts"), PageId::Shortcuts), Nav::to(tr("Application Shortcuts"), PageId::StandardShortcuts)}; }
QList<SidebarLink> CustomShortcutsPage::sidebarSeeAlso() { return {Nav::to(tr("Keyboard Properties"), PageId::Keyboard)}; }
QString CustomShortcutsPage::selectedComponent() const {
    const auto *item = m_entries->currentItem(); return item ? item->data(0, Qt::UserRole).toString() : QString();
}
void CustomShortcutsPage::finishEditing() { m_editComponent.clear(); m_name->clear(); m_command->clear(); updateSelection(); }
void CustomShortcutsPage::filterApplications() {
    const auto selected = m_applications->currentData().toString(), query = m_filter->text().trimmed();
    const QSignalBlocker block(m_applications); m_applications->clear();
    for (const auto &application : m_session->staged().applications) {
        if (application.command || (!query.isEmpty() && !(application.name + QLatin1Char(' ') + application.id + QLatin1Char(' ') + application.exec).contains(query, Qt::CaseInsensitive))) continue;
        m_applications->addItem(application.name + QStringLiteral(" (%1)").arg(application.id), application.id);
    }
    const int index = m_applications->findData(selected); if (index >= 0) m_applications->setCurrentIndex(index);
    updateSelection();
}
void CustomShortcutsPage::updateState() {
    m_updating = true; const auto selected = selectedComponent(); const QSignalBlocker block(m_entries); m_entries->clear();
    QTreeWidgetItem *selection = nullptr;
    for (const auto &entry : m_session->staged().entries) {
        const auto *original = m_session->applied().find(entry.component);
        QString state = entry.pendingRemoval ? tr("Remove on Apply") : entry.pendingAddition ? tr("Add on Apply")
            : original && (entry.name != original->name || entry.exec != original->exec) ? tr("Change on Apply") : entry.registered ? tr("Registered") : tr("Not registered");
        auto *item = new QTreeWidgetItem(m_entries, {entry.name.isEmpty() ? entry.component : entry.name, entry.command ? tr("Command") : tr("Application"), entry.exec, state});
        item->setData(0, Qt::UserRole, entry.component);
        for (int column = 0; column < 4; ++column) item->setToolTip(column, plainToolTip(item->text(column)));
        item->setToolTip(0, plainToolTip(entry.component)); if (entry.component == selected) selection = item;
    }
    if (selection) m_entries->setCurrentItem(selection);
    if (!m_editComponent.isEmpty() && !m_session->staged().find(m_editComponent)) finishEditing();
    filterApplications(); m_updating = false; updateSelection();
    QString status = m_session->isBusy() ? tr("Reading or applying custom shortcuts…") : !m_session->errorText().isEmpty() ? m_session->errorText()
        : !m_session->resultText().isEmpty() ? m_session->resultText() : m_session->isDirty() ? tr("You have pending application or command changes. Click Apply to save them.")
        : !m_session->applied().reason.isEmpty() ? m_session->applied().reason : tr("Application and command shortcut registrations are up to date.");
    if (m_session->hasConflict()) status += tr("\nYour draft is preserved. Revert reloads the current registrations.");
    m_status->setText(status);
}
void CustomShortcutsPage::updateSelection() {
    const auto *entry = m_session->staged().find(selectedComponent());
    const bool editable = m_session->canApply() && !m_session->isBusy();
    m_register->setEnabled(editable && entry && !entry->registered && !entry->pendingAddition && !entry->pendingRemoval
        && m_session->applied().application(entry->component));
    m_edit->setEnabled(editable && entry && entry->command && entry->editable && !entry->pendingRemoval);
    m_remove->setEnabled(editable && entry && (entry->removable || entry->pendingAddition) && !entry->pendingRemoval);
    m_restore->setEnabled(editable && entry && entry->pendingRemoval);
    m_assign->setEnabled(entry && entry->registered && !entry->pendingAddition && !entry->pendingRemoval && !m_session->isBusy());
    const auto id = m_applications->currentData().toString();
    m_addApplication->setEnabled(editable && !id.isEmpty() && !m_session->staged().find(id));
    m_filter->setEnabled(!m_session->isBusy()); m_applications->setEnabled(editable);
    m_name->setEnabled(editable); m_command->setEnabled(editable);
    m_saveCommand->setText(m_editComponent.isEmpty() ? tr("Add co&mmand") : tr("Save co&mmand"));
    m_saveCommand->setEnabled(editable && !m_name->text().trimmed().isEmpty() && !m_command->text().trimmed().isEmpty());
    m_cancelEdit->setEnabled(!m_session->isBusy() && (!m_editComponent.isEmpty() || !m_name->text().isEmpty() || !m_command->text().isEmpty()));
    m_refresh->setEnabled(!m_session->isBusy()); m_revert->setEnabled(!m_session->isBusy()); m_apply->setEnabled(editable && m_session->isDirty());
    m_details->setText(!entry ? tr("Select a registered application or command to manage it.") : !entry->reason.isEmpty() ? entry->reason
        : entry->pendingAddition ? tr("This registration is staged. Apply before assigning shortcut keys.")
        : entry->pendingRemoval ? tr("This registration will be removed on Apply. Keep selected cancels its removal.")
        : !entry->registered ? tr("Register this saved definition before assigning shortcut keys.")
        : entry->command && !entry->editable ? tr("This command is read-only. Its saved definition is shown for reference.")
        : tr("Key combinations for this registration are managed in Global Keyboard Shortcuts."));
}
