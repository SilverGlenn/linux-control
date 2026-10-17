#include "ShortcutsPage.h"
#include "settings/ShortcutsSettings.h"
#include "settings/CustomShortcutsSettings.h"
#include "settings/ShortcutImportCoordinator.h"
#include "ui/dialogs/ShortcutSchemeDialog.h"
#include "Win7Ui.h"
#include <QGroupBox>
#include <QHeaderView>
#include <KKeySequenceWidget>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace {
QString keysText(const QList<QKeySequence> &keys)
{
    QStringList text;
    for (const auto &key : keys) if (!key.isEmpty()) text.append(key.toString(QKeySequence::NativeText));
    return text.isEmpty() ? QObject::tr("None") : text.join(QStringLiteral("; "));
}
QString displayName(const QString &friendly, const QString &id)
{ return friendly.isEmpty() ? id : friendly; }
QString plainToolTip(const QString &text)
{ return QStringLiteral("<qt>%1</qt>").arg(text.toHtmlEscaped()); }
QPushButton *button(const QString &text, const char *name)
{ auto *result = new QPushButton(text); result->setObjectName(QLatin1String(name)); return result; }
bool filterItem(QTreeWidgetItem *item, const QString &query)
{
    bool visible = item->data(0, Qt::UserRole).isValid()
        && (query.isEmpty() || item->data(0, Qt::UserRole + 1).toString().contains(query, Qt::CaseInsensitive));
    for (int i = 0; i < item->childCount(); ++i) visible = filterItem(item->child(i), query) || visible;
    item->setHidden(!visible); return visible;
}
}

ShortcutsPage::ShortcutsPage(QScrollArea *sidebar, QWidget *parent)
    : ShortcutsPage(sidebar, Scope::Global, parent) {}
ShortcutsPage::ShortcutsPage(QScrollArea *sidebar, ShortcutsSettings *session, QWidget *parent)
    : ShortcutsPage(sidebar, session, Scope::Global, parent) {}
ShortcutsPage::ShortcutsPage(QScrollArea *sidebar, Scope scope, QWidget *parent)
    : ShortcutsPage(sidebar, scope == Scope::StandardApplications ? ShortcutsSettings::standardInstance() : ShortcutsSettings::instance(), scope, parent) {}
ShortcutsPage::ShortcutsPage(QScrollArea *sidebar, ShortcutsSettings *session, Scope scope, QWidget *parent)
    : QWidget(parent), m_session(session), m_scope(scope)
{
    Q_ASSERT(session);
    const bool standard = m_scope == Scope::StandardApplications;
    setProperty("shortcutScope", standard ? QStringLiteral("standard") : QStringLiteral("global"));
    auto *layout = Win7::pageScaffold(this, sidebar, 18, 900); Win7::styleSettingsControls(this);
    setStyleSheet(styleSheet() + QStringLiteral(
        " QGroupBox { color: #000000; background: transparent; }"
        " QLineEdit, KKeySequenceWidget, QTreeWidget, QListWidget { color: #000000; background: #FFFFFF; border: 1px solid #ABADB3; selection-background-color: #CDE6F7; selection-color: #000000; }"
        " QLineEdit:focus, KKeySequenceWidget:focus, QTreeWidget:focus, QListWidget:focus { border-color: #3C7FB1; }"
        " QLineEdit:disabled, KKeySequenceWidget:disabled { color: #777777; background: #F0F0F0; }"
        " QHeaderView::section { color: #000000; background: #F4F5F6; border: 0; border-right: 1px solid #DDDDDD; padding: 5px; }"));
    auto *title = Win7::pageTitle(standard ? tr("Standard Application Shortcuts") : tr("Global Keyboard Shortcuts"));
    title->setObjectName(QStringLiteral("shortcutsTitle")); layout->addWidget(title);
    auto *intro = Win7::bodyLabel(standard
        ? tr("Change common shortcuts used inside applications, such as Copy, Open, and Save. Applications choose which standard actions they support and may override these bindings. Changes are saved separately from global shortcuts when you click Apply. Applications may need to restart to adopt them.")
        : tr("Choose an application or system action to change its global shortcuts. Alternative shortcuts and sequences of up to four key combinations are supported. Changes are saved when you click Apply. Inactive applications use their bindings when started."));
    intro->setObjectName(QStringLiteral("shortcutsIntroduction")); intro->setTextFormat(Qt::PlainText); layout->addWidget(intro);
    auto *searchRow = new QHBoxLayout;
    auto *searchLabel = Win7::label(tr("&Search shortcuts:"));
    m_search = new QLineEdit; m_search->setObjectName(QStringLiteral("shortcutsSearch"));
    m_search->setAccessibleName(standard ? tr("Search standard actions, categories, and shortcut keys") : tr("Search actions, applications, and shortcut keys"));
    m_search->setPlaceholderText(standard ? tr("Search categories, actions, or keys") : tr("Search applications, actions, or keys")); m_search->setClearButtonEnabled(true);
    searchLabel->setBuddy(m_search); searchRow->addWidget(searchLabel); searchRow->addWidget(m_search, 1); layout->addLayout(searchRow);
    m_actions = new QTreeWidget; m_actions->setObjectName(QStringLiteral("shortcutsActions"));
    m_actions->setAccessibleName(standard ? tr("Standard application shortcuts by category and action") : tr("Global shortcuts by application, context, and action"));
    m_actions->setHeaderLabels({standard ? tr("Category / action") : tr("Application / action"), tr("Current shortcuts"), tr("Default shortcuts")});
    m_actions->setSelectionMode(QAbstractItemView::SingleSelection); m_actions->setMinimumHeight(250);
    m_actions->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_actions->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_actions->header()->setSectionResizeMode(2, QHeaderView::Stretch); layout->addWidget(m_actions, 1);
    m_details = new QGroupBox(tr("Selected action")); auto *details = new QVBoxLayout(m_details); details->setSpacing(8);
    m_capability = Win7::bodyLabel({}); m_capability->setObjectName(QStringLiteral("shortcutsCapability"));
    m_capability->setTextFormat(Qt::PlainText); details->addWidget(m_capability);
    auto *alternativesLabel = Win7::label(tr("Shortcut alter&natives:"));
    m_alternatives = new QListWidget; m_alternatives->setObjectName(QStringLiteral("shortcutsAlternatives"));
    m_alternatives->setAccessibleName(tr("Alternative shortcuts for the selected action"));
    m_alternatives->setMaximumHeight(100); m_alternatives->setMinimumHeight(65); alternativesLabel->setBuddy(m_alternatives);
    details->addWidget(alternativesLabel); details->addWidget(m_alternatives);
    m_defaults = Win7::bodyLabel({}); m_defaults->setObjectName(QStringLiteral("shortcutsDefaultKeys")); m_defaults->setTextFormat(Qt::PlainText); details->addWidget(m_defaults);
    auto *editRow = new QHBoxLayout; auto *editLabel = Win7::label(tr("Shortcut se&quence:"));
    m_editor = new KKeySequenceWidget; m_editor->setObjectName(QStringLiteral("shortcutsSequence"));
    m_editor->setAccessibleName(tr("Shortcut sequence, up to four key combinations"));
    // The SDK recorder inhibits shortcuts only for this recording window on
    // Wayland. Its QObject ownership releases inhibition on page destruction.
    // All assignment conflicts are resolved by our staged session below.
    m_editor->setCheckForConflictsAgainst(KKeySequenceWidget::None);
    m_editor->setMultiKeyShortcutsAllowed(true);
    m_editor->setPatterns(KKeySequenceRecorder::ModifierAndKey | KKeySequenceRecorder::Key | KKeySequenceRecorder::Modifier);
    m_editor->setClearButtonShown(true);
    m_editor->focusProxy()->setAccessibleName(tr("Record shortcut sequence, up to four key combinations"));
    m_editor->setToolTip(tr("Click to record up to four key combinations in order, then wait for recording to finish. Click Add alternative or Replace selected to stage it."));
    editLabel->setBuddy(m_editor); editRow->addWidget(editLabel); editRow->addWidget(m_editor, 1); details->addLayout(editRow);
    auto *editButtons = new QHBoxLayout;
    m_add = button(tr("Add a&lternative"), "shortcutsAdd"); m_replace = button(tr("Re&place selected"), "shortcutsReplace");
    m_remove = button(tr("Rem&ove selected"), "shortcutsRemove"); m_actionDefaults = button(tr("Action &defaults"), "shortcutsActionDefaults");
    editButtons->addWidget(m_add); editButtons->addWidget(m_replace); editButtons->addWidget(m_remove); editButtons->addStretch(); editButtons->addWidget(m_actionDefaults); details->addLayout(editButtons);
    layout->addWidget(m_details);
    m_conflicts = Win7::bodyLabel({}); m_conflicts->setObjectName(QStringLiteral("shortcutsConflicts"));
    m_conflicts->setTextFormat(Qt::PlainText); layout->addWidget(m_conflicts);
    auto *conflictButtons = new QHBoxLayout;
    m_reassign = button(tr("Reassign these &conflicting shortcuts"), "shortcutsReassign");
    m_cancel = button(tr("Cancel s&hortcut change"), "shortcutsCancelConflict");
    conflictButtons->addWidget(m_reassign); conflictButtons->addWidget(m_cancel); conflictButtons->addStretch(); layout->addLayout(conflictButtons);
    m_status = Win7::bodyLabel({}); m_status->setObjectName(QStringLiteral("shortcutsStatus")); m_status->setTextFormat(Qt::PlainText); layout->addWidget(m_status);
    auto *schemeButtons = new QHBoxLayout;
    m_import = button(tr("&Import scheme…"), "shortcutsImport"); m_export = button(tr("E&xport scheme…"), "shortcutsExport");
    m_import->setAccessibleName(tr("Review and import a shortcut scheme")); m_export->setAccessibleName(tr("Export selected staged shortcut bindings"));
    schemeButtons->addWidget(m_import); schemeButtons->addWidget(m_export); schemeButtons->addStretch(); layout->addLayout(schemeButtons);
    auto *buttons = new QHBoxLayout;
    m_allDefaults = button(tr("All de&faults"), "shortcutsAllDefaults"); m_refresh = button(tr("&Refresh"), "shortcutsRefresh");
    m_revert = button(tr("Re&vert"), "shortcutsRevert"); m_apply = button(tr("&Apply"), "shortcutsApply");
    buttons->addWidget(m_allDefaults); buttons->addWidget(m_refresh); buttons->addStretch(); buttons->addWidget(m_revert); buttons->addWidget(m_apply); layout->addLayout(buttons);
    connect(m_search, &QLineEdit::textChanged, this, &ShortcutsPage::filterTree);
    connect(m_actions, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *item) {
        if (m_updating) return;
        const bool valid = item && item->data(0, Qt::UserRole).isValid();
        const auto id = valid ? item->data(0, Qt::UserRole).value<ShortcutActionId>() : ShortcutActionId{};
        const bool changed = m_hasSelection != valid || !(m_selected == id);
        m_hasSelection = valid; m_selected = id;
        if (changed) clearProposal(); updateSelection(changed);
    });
    connect(m_alternatives, &QListWidget::currentRowChanged, this, [this](int row) {
        if (m_updating) return;
        if (const auto *action = m_session->staged().find(m_selected)) m_editor->setKeySequence(action->current.value(row));
        updateSelection();
    });
    connect(m_editor, &KKeySequenceWidget::keySequenceChanged, this, [this] { if (!m_updating) updateSelection(); });
    connect(m_editor, &KKeySequenceWidget::recordingChanged, this, [this] {
        if (m_editor->isRecording()) clearProposal();
        updateSelection();
    });
    connect(m_add, &QPushButton::clicked, this, [this] { propose(false); });
    connect(m_replace, &QPushButton::clicked, this, [this] { propose(true); });
    connect(m_remove, &QPushButton::clicked, this, [this] {
        const auto *action = m_session->staged().find(m_selected); const int row = m_alternatives->currentRow();
        if (!action || row < 0 || row >= action->current.size()) return;
        auto keys = action->current; keys.removeAt(row); clearProposal();
        if (m_session->selectShortcuts(m_selected, keys)) {
            m_alternatives->setCurrentRow(-1); m_editor->clearKeySequence();
        }
    });
    connect(m_actionDefaults, &QPushButton::clicked, this, [this] { if (const auto *action = m_session->staged().find(m_selected)) stageKeys(action->defaults); });
    connect(m_allDefaults, &QPushButton::clicked, this, [this] { clearProposal(); m_session->restoreAllDefaults(); });
    connect(m_reassign, &QPushButton::clicked, this, [this] {
        if (!m_hasProposal) return;
        const auto id = m_proposalId; const auto keys = m_proposal;
        if (m_session->selectShortcuts(id, keys, true)) clearProposal();
    });
    connect(m_cancel, &QPushButton::clicked, this, &ShortcutsPage::clearProposal);
    connect(m_refresh, &QPushButton::clicked, session, &ShortcutsSettings::refresh);
    connect(m_revert, &QPushButton::clicked, this, [this] { clearProposal(); m_session->revert(); });
    connect(m_apply, &QPushButton::clicked, session, &ShortcutsSettings::apply);
    connect(m_import, &QPushButton::clicked, this, [this] { openScheme(true); });
    connect(m_export, &QPushButton::clicked, this, [this] { openScheme(false); });
    connect(session, &SettingsSession::stateChanged, this, &ShortcutsPage::updateState); updateState();
}
SettingsSession *ShortcutsPage::settingsSession() const { return m_session; }
ShortcutsSettings *ShortcutsPage::companionSession() const { return m_companion; }
CustomShortcutsSettings *ShortcutsPage::customSession() const { return m_custom; }
ShortcutImportCoordinator *ShortcutsPage::importCoordinator() const { return m_importCoordinator; }
void ShortcutsPage::setImportCoordinator(ShortcutImportCoordinator *coordinator)
{
    if (m_importCoordinator == coordinator) return;
    if (m_importCoordinator) disconnect(m_importCoordinator, nullptr, this, nullptr);
    m_importCoordinator = coordinator;
    if (m_importCoordinator) {
        connect(m_importCoordinator, &ShortcutImportCoordinator::changed, this, &ShortcutsPage::updateState);
        connect(m_importCoordinator, &QObject::destroyed, this, &ShortcutsPage::updateState);
    }
    updateState();
}
void ShortcutsPage::setCompanionSession(ShortcutsSettings *session)
{
    if (m_companion == session) return;
    if (m_companion) disconnect(m_companion, nullptr, this, nullptr);
    m_companion = session;
    if (m_companion) {
        connect(m_companion, &SettingsSession::stateChanged, this, &ShortcutsPage::updateState);
        connect(m_companion, &QObject::destroyed, this, &ShortcutsPage::updateState);
    }
    updateState();
}
void ShortcutsPage::setCustomSession(CustomShortcutsSettings *session)
{
    if (m_custom == session) return;
    if (m_custom) disconnect(m_custom, nullptr, this, nullptr);
    m_custom = session;
    if (m_custom) {
        connect(m_custom, &SettingsSession::stateChanged, this, &ShortcutsPage::updateState);
        connect(m_custom, &QObject::destroyed, this, &ShortcutsPage::updateState);
    }
    updateState();
}
void ShortcutsPage::openScheme(bool importing)
{
    auto *global = m_scope == Scope::Global ? m_session : m_companion.data();
    auto *standard = m_scope == Scope::StandardApplications ? m_session : m_companion.data();
    auto *dialog = new ShortcutSchemeDialog(global, standard, importing ? ShortcutSchemeDialog::Mode::Import : ShortcutSchemeDialog::Mode::Export, this);
    dialog->setCustomSession(m_custom);
    if (importing) { dialog->setImportCoordinator(m_importCoordinator); dialog->setBuiltInSchemes(ShortcutScheme::builtInSchemes()); }
    connect(dialog, &ShortcutSchemeDialog::navigationRequested, this, &ShortcutsPage::navigationRequested);
    dialog->setAttribute(Qt::WA_DeleteOnClose); dialog->open();
}
QList<SidebarLink> ShortcutsPage::sidebarLinks()
{ return sidebarLinks(Scope::Global); }
QList<SidebarLink> ShortcutsPage::sidebarLinks(Scope scope)
{
    return {scope == Scope::StandardApplications ? Nav::to(tr("Global Keyboard Shortcuts"), PageId::Shortcuts)
            : Nav::to(tr("Standard Application Shortcuts"), PageId::StandardShortcuts),
        Nav::to(tr("Custom Keyboard Shortcuts"), PageId::CustomShortcuts),
        Nav::to(tr("Keyboard Properties"), PageId::Keyboard), Nav::to(tr("Mouse and Touchpad"), PageId::InputDevices)};
}
QList<SidebarLink> ShortcutsPage::sidebarSeeAlso()
{ return {Nav::to(tr("Ease of Access Center"), PageId::EaseOfAccess)}; }

void ShortcutsPage::clearProposal()
{
    m_hasProposal = false; m_proposal.clear(); m_conflicts->clear();
    m_conflicts->hide(); m_reassign->hide(); m_cancel->hide();
}
void ShortcutsPage::stageKeys(const QList<QKeySequence> &keys)
{
    const auto conflicts = m_session->conflictsFor(m_selected, keys);
    if (conflicts.isEmpty()) { clearProposal(); m_session->selectShortcuts(m_selected, keys); return; }
    m_proposalId = m_selected; m_proposal = keys; m_hasProposal = true;
    QStringList text{m_scope == Scope::StandardApplications
        ? tr("These standard application shortcuts overlap other standard bindings. Reassigning removes only the conflicting alternatives from the actions below and stages every change together:")
        : tr("These shortcuts overlap existing bindings. Reassigning removes only the conflicting alternatives from the actions below and stages every change together:")};
    bool writable = true;
    for (const auto &conflict : conflicts) {
        const auto *action = m_session->staged().find(conflict.id);
        text.append(m_scope == Scope::StandardApplications
            ? tr("%1 / %2 — %3").arg(action ? displayName(action->componentFriendly, conflict.id.component) : conflict.id.component,
                conflict.name, conflict.sequence.toString(QKeySequence::NativeText))
            : tr("%1 / %2 / %3 — %4").arg(conflict.id.component, conflict.id.context, conflict.name, conflict.sequence.toString(QKeySequence::NativeText)));
        writable = writable && action && action->available;
    }
    if (!writable) text.append(tr("An affected action is unavailable. Choose a different shortcut; these bindings cannot be reassigned."));
    m_conflicts->setText(text.join(QLatin1Char('\n'))); m_conflicts->show(); m_reassign->show(); m_cancel->show();
    m_reassign->setEnabled(writable && m_session->canApply() && !m_session->isBusy());
}
void ShortcutsPage::propose(bool replace)
{
    const auto *action = m_session->staged().find(m_selected); if (!action || m_editor->keySequence().isEmpty()) return;
    auto keys = action->current; const int row = m_alternatives->currentRow();
    if (replace) { if (row < 0 || row >= keys.size()) return; keys[row] = m_editor->keySequence(); }
    else keys.append(m_editor->keySequence());
    stageKeys(keys);
}
void ShortcutsPage::focusExistingAction(const QString &component, const QString &action)
{
    m_focusComponent = component; m_focusAction = action;
    m_search->setText(action); updateState(); m_actions->setFocus(Qt::TabFocusReason);
}
void ShortcutsPage::filterTree()
{
    const auto query = m_search->text().trimmed();
    for (int i = 0; i < m_actions->topLevelItemCount(); ++i) filterItem(m_actions->topLevelItem(i), query);
}
void ShortcutsPage::updateState()
{
    // A refreshed catalog invalidates the list of actions the user reviewed.
    // Never carry a reassignment proposal across a session state transition.
    if (m_hasProposal) clearProposal();
    const bool standard = m_scope == Scope::StandardApplications;
    m_updating = true; const QSignalBlocker blocker(m_actions); m_actions->clear();
    QMap<QString, QTreeWidgetItem *> components; QMap<QPair<QString, QString>, QTreeWidgetItem *> contexts;
    if (!m_focusAction.isEmpty()) {
        QList<ShortcutActionId> matches;
        for (const auto &action : m_session->staged().actions)
            if (action.registered && action.id.component == m_focusComponent && action.id.action == m_focusAction) matches << action.id;
        if (matches.size() == 1) { m_selected = matches.first(); m_hasSelection = true; m_focusComponent.clear(); m_focusAction.clear(); }
    }
    QTreeWidgetItem *selected = nullptr;
    for (const auto &action : m_session->staged().actions) {
        auto *component = components.value(action.id.component);
        if (!component) {
            component = new QTreeWidgetItem(m_actions, {displayName(action.componentFriendly, action.id.component)});
            component->setToolTip(0, plainToolTip(action.id.component)); components.insert(action.id.component, component);
        }
        auto *actionParent = component;
        if (!standard) {
            const auto contextId = qMakePair(action.id.component, action.id.context); auto *context = contexts.value(contextId);
            if (!context) {
                context = new QTreeWidgetItem(component, {action.id.context.isEmpty() ? tr("Default context") : displayName(action.contextFriendly, action.id.context)});
                context->setToolTip(0, plainToolTip(action.id.context)); contexts.insert(contextId, context);
            }
            actionParent = context;
        }
        auto *item = new QTreeWidgetItem(actionParent, {displayName(action.friendlyName, action.id.action), keysText(action.current), keysText(action.defaults)});
        item->setData(0, Qt::UserRole, QVariant::fromValue(action.id));
        item->setData(0, Qt::UserRole + 1, QStringList{action.id.component, action.id.context, action.id.action, action.componentFriendly, action.contextFriendly, action.friendlyName, keysText(action.current), keysText(action.defaults)}.join(QLatin1Char(' ')));
        for (int column = 0; column < 3; ++column) item->setToolTip(column, plainToolTip(item->text(column)));
        if (m_hasSelection && action.id == m_selected) selected = item;
    }
    if (selected) m_actions->setCurrentItem(selected);
    else if (m_hasSelection) { m_hasSelection = false; clearProposal(); }
    m_actions->expandAll(); filterTree(); m_updating = false; updateSelection();
    QString text = m_session->isBusy() ? standard ? tr("Reading or applying standard application shortcuts…") : tr("Reading or applying global shortcuts…")
        : !m_session->errorText().isEmpty() ? m_session->errorText()
        : !m_session->resultText().isEmpty() ? m_session->resultText()
        : m_session->isDirty() ? standard ? tr("You have pending standard application shortcut changes.") : tr("You have pending shortcut changes.")
        : !m_session->applied().reason.isEmpty() ? m_session->applied().reason
        : standard ? tr("Standard application shortcuts are up to date.") : tr("Global shortcuts are up to date.");
    if (m_session->hasConflict()) text += tr("\nYour draft is preserved. Revert reloads the current bindings before another Apply.");
    if (standard && !m_session->resultText().isEmpty()) text += tr("\nApplications may need to restart to adopt these standard bindings.");
    if (m_companion && (m_companion->isDirty() || m_companion->isBusy()))
        text += standard ? tr("\nGlobal shortcuts have pending changes or an active job. Review them on the Global Keyboard Shortcuts page.")
            : tr("\nStandard application shortcuts have pending changes or an active job. Review them on the Standard Application Shortcuts page.");
    if (m_custom && (m_custom->isDirty() || m_custom->isBusy()))
        text += tr("\nCustom shortcut definitions have pending changes or an active job. Review them on the Custom Keyboard Shortcuts page.");
    if (m_importCoordinator && m_importCoordinator->hasDocument()) {
        text += tr("\nRetained scheme import: %1").arg(m_importCoordinator->statusText());
        if (!m_importCoordinator->errorText().isEmpty()) text += QLatin1Char('\n') + m_importCoordinator->errorText();
        m_import->setText(tr("Resume &import review…")); m_import->setAccessibleName(tr("Resume the retained shortcut scheme import review"));
    } else {
        m_import->setText(tr("&Import scheme…")); m_import->setAccessibleName(tr("Review and import a shortcut scheme"));
    }
    m_status->setText(text);
}
void ShortcutsPage::updateSelection(bool changed)
{
    const auto *action = m_hasSelection ? m_session->staged().find(m_selected) : nullptr;
    const bool recording = m_editor->isRecording();
    const bool sessionEditable = m_session->canApply() && !m_session->isBusy();
    const bool editable = action && action->available && sessionEditable;
    m_details->setTitle(action ? displayName(action->friendlyName, action->id.action) : tr("Selected action"));
    QString capability = !action ? tr("Select an action in the list above.") : !action->available
        ? tr("This action is read-only or unavailable. Its saved shortcuts are shown for reference.")
        : !m_session->applied().writable ? m_session->applied().reason : tr("Select an alternative to replace or remove it, or record another shortcut.");
    if (m_scope == Scope::Global && action && action->layoutAction) {
        const auto &adapter = m_session->staged().layouts;
        if (action->available && adapter.available) {
            capability += tr("\nApply saves the bindings and reloads keyboard-layout shortcuts. Physical shortcut invocation has not been verified.");
            if (!action->registered) capability += tr("\nThis unassigned binding will be created when you click Apply.");
        } else {
            capability = tr("This per-layout binding is shown for reference. Native registration and layout-shortcut reload are unavailable for this action.");
            if (!adapter.reason.isEmpty()) capability += QLatin1Char('\n') + adapter.reason;
            else if (adapter.available) capability += tr("\nThis action does not match an available keyboard layout.");
            if (!action->registered) capability += tr("\nThis unassigned binding cannot be created until native layout support is available.");
        }
    }
    m_capability->setText(capability);
    const int oldRow = changed ? -1 : m_alternatives->currentRow();
    m_updating = true;
    {
        const QSignalBlocker blocker(m_alternatives); m_alternatives->clear();
        if (action) for (const auto &key : action->current) m_alternatives->addItem(key.toString(QKeySequence::NativeText));
        if (oldRow >= 0 && oldRow < m_alternatives->count()) m_alternatives->setCurrentRow(oldRow);
    }
    if (changed) m_editor->clearKeySequence();
    m_updating = false;
    m_defaults->setText(action ? tr("Default shortcuts: %1").arg(keysText(action->defaults)) : QString());
    const bool selected = m_alternatives->currentRow() >= 0;
    m_editor->setEnabled(editable);
    m_actions->setEnabled(!recording); m_search->setEnabled(!recording); m_alternatives->setEnabled(action && !recording);
    m_add->setEnabled(editable && !recording && !m_editor->keySequence().isEmpty());
    m_replace->setEnabled(editable && !recording && selected && !m_editor->keySequence().isEmpty());
    m_remove->setEnabled(editable && !recording && selected); m_actionDefaults->setEnabled(editable && !recording);
    m_apply->setEnabled(sessionEditable && !recording && m_session->isDirty());
    m_revert->setEnabled(!m_session->isBusy() && !recording); m_refresh->setEnabled(!m_session->isBusy() && !recording);
    m_allDefaults->setEnabled(sessionEditable && !recording && !m_session->staged().actions.isEmpty());
    m_import->setEnabled(m_session->isReady() && !recording); m_export->setEnabled(m_session->isReady() && !recording);
    if (m_hasProposal && (!sessionEditable || recording)) m_reassign->setEnabled(false);
}
