#include "ShortcutSchemeDialog.h"
#include "settings/ShortcutsSettings.h"
#include "settings/CustomShortcutsSettings.h"
#include "settings/ShortcutImportCoordinator.h"
#include "Win7Ui.h"
#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScopedValueRollback>
#include <QSignalBlocker>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <algorithm>

namespace {
QString keysText(const QList<QKeySequence> &keys)
{
    QStringList result;
    for (const auto &key : keys) if (!key.isEmpty()) result.append(key.toString(QKeySequence::NativeText));
    return result.isEmpty() ? QObject::tr("None") : result.join(QStringLiteral("; "));
}
QString scopeName(ShortcutScheme::Scope scope)
{ return scope == ShortcutScheme::Scope::Global ? QObject::tr("Global shortcuts") : QObject::tr("Standard application shortcuts"); }
QString actionName(const ShortcutAction *action, const ShortcutActionId &id)
{ return action && !action->friendlyName.isEmpty() ? action->friendlyName : id.action; }
QStringList conflictDescriptions(const ShortcutsSnapshot &snapshot, const ShortcutChanges &changes)
{
    auto final = snapshot;
    for (auto &action : final.actions) if (changes.contains(action.id)) action.current = changes.value(action.id);
    QStringList result;
    for (auto next = changes.cbegin(); next != changes.cend(); ++next) {
        const auto *before = snapshot.find(next.key());
        if (before && ShortcutsTypes::sameKeys(before->current, next.value())) continue;
        for (const auto &other : final.actions) {
            if (other.id == next.key() || (other.id.component == next.key().component && other.id.context != next.key().context)) continue;
            for (const auto &occupied : other.current) for (const auto &key : next.value())
                if (ShortcutsTypes::conflicts(key, occupied)) {
                    const auto line = QObject::tr("%1 / %2 / %3: %4 overlaps %5%6")
                        .arg(other.id.component, other.id.context, actionName(&other, other.id), occupied.toString(QKeySequence::NativeText),
                            key.toString(QKeySequence::NativeText), changes.contains(other.id) ? QObject::tr(" (another selected assignment)") : QString());
                    if (!result.contains(line)) result.append(line);
                }
        }
    }
    return result;
}
QPushButton *button(const QString &text, const char *name)
{ auto *result = new QPushButton(text); result->setObjectName(QLatin1String(name)); return result; }
QString plainToolTip(const QString &text)
{ return QStringLiteral("<qt>%1</qt>").arg(text.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br/>"))); }
}

ShortcutSchemeDialog::ShortcutSchemeDialog(ShortcutsSettings *globals, ShortcutsSettings *standards, Mode mode, QWidget *parent)
    : QDialog(parent), m_globals(globals), m_standards(standards), m_mode(mode)
{
    setObjectName(QStringLiteral("shortcutSchemeDialog")); setWindowTitle(mode == Mode::Import ? tr("Import Shortcut Scheme") : tr("Export Shortcut Scheme"));
    setModal(true); resize(1000, 720); Win7::styleSettingsDialog(this);
    auto *layout = new QVBoxLayout(this); layout->setContentsMargins(18, 18, 18, 18); layout->setSpacing(10);
    m_intro = Win7::bodyLabel(mode == Mode::Import
        ? tr("Review each assignment and select what to import. Staging changes does not save preferences or run commands. Use Apply on each settings page when you are ready.")
        : tr("Select the scopes, components, or actions to export. The scheme contains your staged bindings and complete alternative sequences. Nondefault contexts use a versioned extension; Plasma imports the native default sections and ignores that extension. Export does not change preferences."));
    m_intro->setTextFormat(Qt::PlainText); layout->addWidget(m_intro);
    m_workflow = Win7::bodyLabel({}); m_workflow->setObjectName(QStringLiteral("schemeWorkflow")); m_workflow->setTextFormat(Qt::PlainText);
    m_workflow->setAccessibleName(tr("Persistent scheme import progress and next step")); m_workflow->hide(); layout->addWidget(m_workflow);
    auto *pathRow = new QHBoxLayout; auto *pathLabel = Win7::label(tr("&Scheme file:"));
    m_path = new QLineEdit; m_path->setObjectName(QStringLiteral("schemePath")); m_path->setAccessibleName(tr("Shortcut scheme file path")); pathLabel->setBuddy(m_path);
    m_browse = button(tr("&Browse…"), "schemeBrowse"); m_reviewFile = button(tr("Review &file"), "schemeReviewFile");
    pathRow->addWidget(pathLabel); pathRow->addWidget(m_path, 1); pathRow->addWidget(m_browse); pathRow->addWidget(m_reviewFile); layout->addLayout(pathRow);
    m_browse->setVisible(mode == Mode::Import); m_reviewFile->setVisible(mode == Mode::Import);
    auto *builtInRow = new QHBoxLayout; auto *builtInLabel = Win7::label(tr("&Installed scheme:"));
    m_builtIns = new QComboBox; m_builtIns->setObjectName(QStringLiteral("schemeBuiltIns")); m_builtIns->setAccessibleName(tr("Installed shortcut schemes")); builtInLabel->setBuddy(m_builtIns);
    m_reviewBuiltIn = button(tr("&Review installed"), "schemeReviewBuiltIn");
    builtInRow->addWidget(builtInLabel); builtInRow->addWidget(m_builtIns, 1); builtInRow->addWidget(m_reviewBuiltIn); layout->addLayout(builtInRow);
    builtInLabel->setVisible(mode == Mode::Import); m_builtIns->setVisible(mode == Mode::Import); m_reviewBuiltIn->setVisible(mode == Mode::Import);
    auto *nameRow = new QHBoxLayout; auto *nameLabel = Win7::label(tr("Scheme &name:"));
    m_name = new QLineEdit; m_name->setObjectName(QStringLiteral("schemeName")); m_name->setAccessibleName(tr("Exported shortcut scheme name")); nameLabel->setBuddy(m_name);
    nameRow->addWidget(nameLabel); nameRow->addWidget(m_name, 1); layout->addLayout(nameRow); nameLabel->setVisible(mode == Mode::Export); m_name->setVisible(mode == Mode::Export);
    m_tree = new QTreeWidget; m_tree->setObjectName(QStringLiteral("schemeAssignments"));
    m_tree->setAccessibleName(tr("Select shortcut scheme scopes, components, contexts, and actions"));
    m_tree->setHeaderLabels({tr("Scope / action"), tr("Current staged binding"), mode == Mode::Import ? tr("Scheme binding") : tr("Export binding"), tr("Review")});
    m_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch); m_tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_tree->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents); m_tree->header()->setSectionResizeMode(3, QHeaderView::Stretch);
    m_tree->setMinimumHeight(260); layout->addWidget(m_tree, 1);
    auto *selectButtons = new QHBoxLayout; auto *all = button(tr("Select a&ll"), "schemeSelectAll"); auto *none = button(tr("Clear c&hecks"), "schemeSelectNone");
    selectButtons->addWidget(all); selectButtons->addWidget(none); selectButtons->addStretch(); layout->addLayout(selectButtons);
    m_reassign = new QCheckBox(tr("Reassign conflicting &alternatives shown below")); m_reassign->setObjectName(QStringLiteral("schemeReassign"));
    m_reassign->setVisible(mode == Mode::Import); layout->addWidget(m_reassign);
    auto *commandButtons = new QHBoxLayout;
    m_stageCommands = button(tr("Stage command &definitions"), "schemeStageCommands");
    m_mapCommand = button(tr("Map selected co&mmand…"), "schemeMapCommand");
    m_updateReview = button(tr("&Update review"), "schemeUpdateReview");
    commandButtons->addWidget(m_stageCommands); commandButtons->addWidget(m_mapCommand); commandButtons->addWidget(m_updateReview); commandButtons->addStretch(); layout->addLayout(commandButtons);
    m_stageCommands->setVisible(mode == Mode::Import); m_mapCommand->setVisible(mode == Mode::Import);
    auto *workflowButtons = new QHBoxLayout;
    m_continueCustom = button(tr("Continue on Custom &Keyboard Shortcuts"), "schemeContinueCustom");
    m_refreshBindings = button(tr("Refresh &global bindings"), "schemeRefreshBindings");
    m_discardReview = button(tr("Discard re&view"), "schemeDiscardReview");
    m_continueCustom->setAccessibleName(tr("Close this review and continue on Custom Keyboard Shortcuts"));
    m_refreshBindings->setAccessibleName(tr("Read global shortcut bindings without applying any draft"));
    m_discardReview->setToolTip(tr("Clear the retained source, mappings, and review choices. Settings drafts remain staged."));
    workflowButtons->addWidget(m_continueCustom); workflowButtons->addWidget(m_refreshBindings); workflowButtons->addStretch(); workflowButtons->addWidget(m_discardReview);
    layout->addLayout(workflowButtons); m_continueCustom->hide(); m_refreshBindings->hide(); m_discardReview->hide();
    m_report = Win7::bodyLabel({}); m_report->setObjectName(QStringLiteral("schemeReport")); m_report->setTextFormat(Qt::PlainText);
    m_report->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    auto *reportArea = new QScrollArea; reportArea->setWidgetResizable(true); reportArea->setWidget(m_report);
    reportArea->setAccessibleName(tr("Scheme validation issues, unresolved definitions, and conflicting alternatives")); reportArea->setMinimumHeight(80); reportArea->setMaximumHeight(150); layout->addWidget(reportArea);
    m_summary = Win7::bodyLabel({}); m_summary->setObjectName(QStringLiteral("schemeSummary")); m_summary->setTextFormat(Qt::PlainText); layout->addWidget(m_summary);
    auto *buttons = new QHBoxLayout; buttons->addStretch();
    m_commit = button(mode == Mode::Import ? tr("S&tage selected changes") : tr("&Export selected…"), "schemeCommit");
    m_close = button(mode == Mode::Import ? tr("&Cancel") : tr("&Close"), "schemeCancel"); buttons->addWidget(m_commit); buttons->addWidget(m_close); layout->addLayout(buttons);
    connect(m_browse, &QPushButton::clicked, this, &ShortcutSchemeDialog::chooseFile);
    connect(m_reviewFile, &QPushButton::clicked, this, [this] { loadFile(m_path->text()); });
    connect(m_reviewBuiltIn, &QPushButton::clicked, this, [this] { if (m_builtIns->currentIndex() >= 0) loadFile(m_builtIns->currentData().toString()); });
    connect(m_tree, &QTreeWidget::itemChanged, this, [this] { if (!m_updating) { syncChoicesToCoordinator(); updateReview(); } });
    connect(m_reassign, &QCheckBox::toggled, this, [this] { if (!m_updating) { syncChoicesToCoordinator(); updateReview(); } });
    connect(all, &QPushButton::clicked, this, [this] {
        { const QSignalBlocker blocker(m_tree);
            for (const auto &row : m_rows) if (row.selectable) row.item->setCheckState(0, Qt::Checked);
            for (const auto &row : m_commands) if (row.selectable) row.item->setCheckState(0, Qt::Checked);
        }
        syncChoicesToCoordinator(); updateReview();
    });
    connect(none, &QPushButton::clicked, this, [this] {
        { const QSignalBlocker blocker(m_tree);
            for (const auto &row : m_rows) if (row.selectable) row.item->setCheckState(0, Qt::Unchecked);
            for (const auto &row : m_commands) if (row.selectable) row.item->setCheckState(0, Qt::Unchecked);
        }
        syncChoicesToCoordinator(); updateReview();
    });
    connect(m_stageCommands, &QPushButton::clicked, this, &ShortcutSchemeDialog::stageCommandDefinitions);
    connect(m_mapCommand, &QPushButton::clicked, this, &ShortcutSchemeDialog::chooseCommandMapping);
    connect(m_updateReview, &QPushButton::clicked, this, &ShortcutSchemeDialog::reviewAgain);
    connect(m_tree, &QTreeWidget::currentItemChanged, this, [this] { if (!m_updating) updateReview(); });
    connect(m_commit, &QPushButton::clicked, this, [this] { if (m_mode == Mode::Import) stageSelected(); else chooseExport(); });
    connect(m_close, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_continueCustom, &QPushButton::clicked, this, [this] {
        if (!m_importCoordinator || !m_custom) return;
        QPointer<ShortcutSchemeDialog> guard(this); reject();
        if (guard) emit navigationRequested(PageId::CustomShortcuts);
    });
    connect(m_refreshBindings, &QPushButton::clicked, this, [this] { if (m_importCoordinator) m_importCoordinator->refreshGlobals(); });
    connect(m_discardReview, &QPushButton::clicked, this, [this] { if (m_importCoordinator) m_importCoordinator->discard(); });
    for (auto *session : {globals, standards}) if (session) {
        connect(session, &SettingsSession::stateChanged, this, [this] { ++m_sessionRevision; updateReview(); });
        connect(session, &QObject::destroyed, this, [this] { ++m_sessionRevision; updateReview(); });
    }
    setBuiltInSchemes({});
    if (mode == Mode::Export) { m_loaded = true; pinBaseline(); populate(); }
    updateReview();
}
ShortcutsSettings *ShortcutSchemeDialog::globalSession() const { return m_globals; }
ShortcutsSettings *ShortcutSchemeDialog::standardSession() const { return m_standards; }
ShortcutImportCoordinator *ShortcutSchemeDialog::importCoordinator() const { return m_importCoordinator; }
void ShortcutSchemeDialog::setImportCoordinator(ShortcutImportCoordinator *coordinator)
{
    if (m_mode != Mode::Import || m_importCoordinator == coordinator) return;
    if (m_importCoordinator) disconnect(m_importCoordinator, nullptr, this, nullptr);
    m_importCoordinator = coordinator; m_coordinatorRequired = bool(coordinator);
    if (!coordinator) { if (m_loaded) review(m_read); else updateReview(); return; }
    // The coordinator's injected sessions are authoritative. Attaching a view
    // never creates providers, reparses its source, or refreshes its baseline.
    for (auto *session : {m_globals.data(), m_standards.data()}) if (session) disconnect(session, nullptr, this, nullptr);
    if (m_custom) disconnect(m_custom, nullptr, this, nullptr);
    m_globals = coordinator->globalSession(); m_standards = coordinator->standardSession(); m_custom = coordinator->customSession();
    resize(qMax(width(), 1060), qMax(height(), 840));
    connect(coordinator, &ShortcutImportCoordinator::changed, this, [this] { if (!m_syncingChoices) syncFromCoordinator(); });
    connect(coordinator, &QObject::destroyed, this, [this] {
        m_error = tr("The retained import review is no longer connected. Close this view and reopen the import from its owning window."); updateReview();
    });
    syncFromCoordinator();
}
void ShortcutSchemeDialog::syncFromCoordinator()
{
    if (!m_importCoordinator || m_syncingChoices) return;
    m_globals = m_importCoordinator->globalSession(); m_standards = m_importCoordinator->standardSession(); m_custom = m_importCoordinator->customSession();
    m_read = m_importCoordinator->readResult(); m_plan = m_importCoordinator->plan(); m_migrations = m_importCoordinator->migrations();
    m_loaded = m_importCoordinator->hasDocument(); m_error = m_importCoordinator->errorText();
    m_path->setText(m_importCoordinator->sourcePath());
    { const QSignalBlocker blocker(m_reassign); m_reassign->setChecked(m_importCoordinator->reassignConflicts()); }
    populate(); updateReview();
}
void ShortcutSchemeDialog::syncChoicesToCoordinator()
{
    if (!m_importCoordinator || m_syncingChoices || m_updating) return;
    // changed() can notify other views, but rebuilding this tree from inside
    // itemChanged would invalidate Qt's active item/check-state traversal.
    const QScopedValueRollback<bool> syncing(m_syncingChoices, true);
    for (const auto &row : m_rows) if (row.selectable && m_importCoordinator)
        m_importCoordinator->setBindingSelected(row.scope, row.id, row.item->checkState(0) == Qt::Checked);
    for (const auto &row : m_commands) if (row.selectable && m_importCoordinator)
        m_importCoordinator->setCommandSelected(row.definition.component, row.item->checkState(0) == Qt::Checked);
    if (m_importCoordinator) m_importCoordinator->setReassignConflicts(m_reassign->isChecked());
}
void ShortcutSchemeDialog::setBuiltInSchemes(const QList<ShortcutScheme::BuiltIn> &schemes)
{
    m_builtIns->clear();
    for (const auto &scheme : schemes) {
        m_builtIns->addItem(scheme.name, scheme.path);
        QStringList issues; for (const auto &issue : scheme.issues) issues.append(issue.message);
        m_builtIns->setItemData(m_builtIns->count() - 1, issues.isEmpty() ? QString() : plainToolTip(issues.join(QLatin1Char('\n'))), Qt::ToolTipRole);
    }
    m_builtIns->setEnabled(!schemes.isEmpty()); m_reviewBuiltIn->setEnabled(!schemes.isEmpty());
    m_builtIns->setToolTip(schemes.isEmpty() ? tr("No installed shortcut schemes were found.") : QString());
}
void ShortcutSchemeDialog::setCustomSession(CustomShortcutsSettings *session)
{
    if (m_coordinatorRequired) return;
    if (m_custom == session) return;
    if (m_custom) disconnect(m_custom, nullptr, this, nullptr);
    m_custom = session;
    if (m_custom) {
        connect(m_custom, &SettingsSession::stateChanged, this, [this] { ++m_sessionRevision; updateReview(); });
        connect(m_custom, &QObject::destroyed, this, [this] { ++m_sessionRevision; updateReview(); });
    }
    if (m_mode == Mode::Import && m_loaded) review(m_read);
    else { pinBaseline(); populate(); updateReview(); }
}
bool ShortcutSchemeDialog::loadData(const QByteArray &bytes)
{
    if (m_coordinatorRequired) { if (!m_importCoordinator) return false; const bool result = m_importCoordinator->loadData(bytes); syncFromCoordinator(); return result; }
    m_migrations.clear(); const auto result = ShortcutScheme::parse(bytes); review(result); return result.success && m_plan.valid;
}
bool ShortcutSchemeDialog::loadFile(const QString &path)
{
    if (m_coordinatorRequired) { if (!m_importCoordinator) return false; const bool result = m_importCoordinator->loadFile(path); syncFromCoordinator(); return result; }
    m_migrations.clear(); m_path->setText(path); const auto result = ShortcutScheme::readFile(path); review(result); return result.success && m_plan.valid;
}
void ShortcutSchemeDialog::review(const ShortcutScheme::ReadResult &result)
{
    m_read = result; m_loaded = result.success; m_error = result.error;
    m_plan = result.success ? ShortcutScheme::planImport(result.document, m_globals ? m_globals->staged() : ShortcutsSnapshot{}, m_standards ? m_standards->staged() : ShortcutsSnapshot{}, m_migrations) : ShortcutScheme::ImportPlan{};
    if (result.success && !m_plan.valid) m_error = m_plan.error;
    pinBaseline();
    m_reassign->setChecked(false); populate(); updateReview();
}
void ShortcutSchemeDialog::pinBaseline()
{
    m_reviewRevision = m_sessionRevision;
    m_reviewHadGlobals = bool(m_globals); m_reviewHadStandards = bool(m_standards); m_reviewHadCustom = bool(m_custom);
    m_globalApplied = m_globals ? ShortcutsTypes::revision(m_globals->applied()) : QByteArray{};
    m_globalDraft = m_globals ? ShortcutsTypes::revision(m_globals->staged()) : QByteArray{};
    m_standardApplied = m_standards ? ShortcutsTypes::revision(m_standards->applied()) : QByteArray{};
    m_standardDraft = m_standards ? ShortcutsTypes::revision(m_standards->staged()) : QByteArray{};
    m_customApplied = m_custom ? CustomShortcutsTypes::revision(m_custom->applied()) : QByteArray{};
    m_customDraft = m_custom ? CustomShortcutsTypes::revision(m_custom->staged()) : QByteArray{};
}
void ShortcutSchemeDialog::populate(bool preserveChecks)
{
    QVariant currentBinding, currentCommand; int currentScope = -1;
    if (auto *current = m_tree->currentItem()) {
        currentBinding = current->data(0, Qt::UserRole); currentCommand = current->data(0, Qt::UserRole + 2);
        currentScope = current->data(0, Qt::UserRole + 1).toInt();
    }
    QMap<QPair<int, ShortcutActionId>, Qt::CheckState> bindingChecks;
    QMap<QString, Qt::CheckState> commandChecks;
    if (preserveChecks) {
        for (const auto &row : m_rows) if (row.matched) bindingChecks.insert(qMakePair(int(row.scope), row.id), row.item->checkState(0));
        for (const auto &row : m_commands) if (row.selectable) commandChecks.insert(row.definition.component, row.item->checkState(0));
    }
    m_updating = true; const QSignalBlocker blocker(m_tree); m_tree->clear(); m_rows.clear(); m_commands.clear();
    QMap<int, QTreeWidgetItem *> scopes; QMap<QPair<int, QString>, QTreeWidgetItem *> components;
    auto add = [&](ShortcutScheme::Scope scope, const ShortcutActionId &id, const QList<QKeySequence> &keys, bool resolved) {
        auto *session = scope == ShortcutScheme::Scope::Global ? m_globals.data() : m_standards.data();
        const auto *action = session ? session->staged().find(id) : nullptr;
        const bool matched = resolved && action && (m_mode == Mode::Export || action->available);
        const bool selectable = matched || bool(m_importCoordinator);
        const int scopeId = int(scope); auto *scopeItem = scopes.value(scopeId);
        if (!scopeItem) {
            scopeItem = new QTreeWidgetItem(m_tree, {scopeName(scope)}); scopeItem->setFlags(scopeItem->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsAutoTristate);
            scopes.insert(scopeId, scopeItem);
        }
        const auto componentKey = qMakePair(scopeId, id.component); auto *component = components.value(componentKey);
        if (!component) {
            component = new QTreeWidgetItem(scopeItem, {action && !action->componentFriendly.isEmpty() ? action->componentFriendly : id.component});
            component->setFlags(component->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsAutoTristate); components.insert(componentKey, component);
        }
        const QString label = id.context == QLatin1String("default") || id.context.isEmpty() ? actionName(action, id) : tr("%1 / %2").arg(id.context, actionName(action, id));
        QString state = !session ? tr("Scope is not connected; assignment preserved in the review")
            : action && !action->available && m_mode == Mode::Import ? tr("Read-only or unavailable; not staged") : !action || !resolved ? tr("Unmatched action; not staged")
            : ShortcutsTypes::sameKeys(action->current, keys) ? tr("Matches current draft") : tr("Selected assignment");
        if (m_importCoordinator && !matched) state += tr(". Uncheck to exclude this assignment, or resolve it before staging bindings.");
        auto *item = new QTreeWidgetItem(component, {label, action ? keysText(action->current) : tr("Unavailable"), keysText(keys), state});
        item->setData(0, Qt::UserRole, QVariant::fromValue(id)); item->setData(0, Qt::UserRole + 1, scopeId);
        if (selectable) {
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(0, m_importCoordinator ? (m_importCoordinator->bindingSelected(scope, id) ? Qt::Checked : Qt::Unchecked)
                                                     : bindingChecks.value(qMakePair(scopeId, id), Qt::Checked));
        }
        else item->setFlags(item->flags() & ~Qt::ItemIsUserCheckable);
        for (int i = 0; i < 4; ++i) item->setToolTip(i, plainToolTip(item->text(i)));
        m_rows.append({scope, id, keys, matched, selectable, item});
        if (currentBinding.isValid() && currentScope == scopeId && currentBinding.value<ShortcutActionId>() == id) m_tree->setCurrentItem(item);
    };
    if (m_mode == Mode::Export) {
        for (const auto &pair : {qMakePair(ShortcutScheme::Scope::Global, m_globals.data()), qMakePair(ShortcutScheme::Scope::Standard, m_standards.data())})
            if (pair.second) for (const auto &action : pair.second->staged().actions) add(pair.first, action.id, action.current, true);
    } else if (m_loaded) {
        for (auto i = m_plan.globals.cbegin(); i != m_plan.globals.cend(); ++i) add(ShortcutScheme::Scope::Global, i.key(), i.value(), true);
        for (auto i = m_plan.standards.cbegin(); i != m_plan.standards.cend(); ++i) add(ShortcutScheme::Scope::Standard, i.key(), i.value(), true);
        for (const auto &binding : m_plan.unresolved) add(binding.scope, binding.id, binding.keys, false);
    }
    QList<ShortcutScheme::Command> definitions = m_mode == Mode::Import ? m_plan.commands : QList<ShortcutScheme::Command>{};
    if (m_mode == Mode::Export && m_custom)
        for (const auto &entry : m_custom->staged().entries) if (entry.command && !entry.pendingRemoval) definitions.append({entry.component, entry.name, entry.exec});
    if (!definitions.isEmpty()) {
        auto *commands = new QTreeWidgetItem(m_tree, {tr("Custom command definitions")});
        commands->setFlags(commands->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsAutoTristate);
        for (const auto &command : definitions) {
            const auto *existing = m_custom ? m_custom->staged().find(command.component) : nullptr;
            const bool applicationIdentity = m_custom && !existing && m_custom->staged().application(command.component);
            QString normalized, problem;
            const bool valid = CustomShortcutsTypes::validComponent(command.component)
                && CustomShortcutsTypes::normalizeCommand(command.name, command.exec, normalized, problem);
            const bool selectable = bool(m_importCoordinator) || m_mode == Mode::Export || (valid && m_custom && !applicationIdentity && (!existing || (existing->command && existing->editable && !existing->pendingRemoval)));
            const QString state = m_mode == Mode::Export ? tr("Export staged definition") : !m_custom ? tr("Custom session is not connected; definition retained")
                : !valid ? problem.isEmpty() ? tr("Invalid native command identity") : problem
                : applicationIdentity || (existing && (!existing->command || !existing->editable || existing->pendingRemoval)) ? tr("Existing entry cannot be edited; map to another command")
                : m_importCoordinator && existing && !existing->registered
                    ? (existing->pendingAddition ? tr("Definition and native registration are staged; Apply on Custom Keyboard Shortcuts")
                                                : tr("Stage definition and native registration together, then Apply on Custom Keyboard Shortcuts"))
                : existing && existing->name == command.name && existing->exec == normalized ? tr("Matches current definition draft")
                : existing ? tr("Edit definition; Apply remains on Custom Keyboard Shortcuts") : tr("New definition; binding remains pending until Custom Apply and Refresh");
            auto *item = new QTreeWidgetItem(commands, {tr("%1 (%2)").arg(command.name, command.component),
                existing ? tr("%1: %2").arg(existing->name, existing->exec) : tr("No definition"),
                tr("%1: %2").arg(command.name, command.exec), state});
            item->setData(0, Qt::UserRole + 2, command.component);
            if (selectable) {
                item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
                item->setCheckState(0, m_importCoordinator ? (m_importCoordinator->commandSelected(command.component) ? Qt::Checked : Qt::Unchecked)
                                                         : commandChecks.value(command.component, Qt::Checked));
            }
            else item->setFlags(item->flags() & ~Qt::ItemIsUserCheckable);
            m_commands.append({command, selectable, item});
            if (currentCommand.isValid() && currentCommand.toString() == command.component) m_tree->setCurrentItem(item);
            for (int i = 0; i < 4; ++i) item->setToolTip(i, plainToolTip(item->text(i)));
        }
    }
    m_tree->expandAll(); m_updating = false;
}
bool ShortcutSchemeDialog::baselineCurrent() const
{
    if (m_coordinatorRequired) return m_importCoordinator && m_importCoordinator->reviewCurrent();
    return m_reviewRevision == m_sessionRevision
        && m_reviewHadGlobals == bool(m_globals) && m_reviewHadStandards == bool(m_standards) && m_reviewHadCustom == bool(m_custom)
        && (!m_globals || (m_globalApplied == ShortcutsTypes::revision(m_globals->applied()) && m_globalDraft == ShortcutsTypes::revision(m_globals->staged())))
        && (!m_standards || (m_standardApplied == ShortcutsTypes::revision(m_standards->applied()) && m_standardDraft == ShortcutsTypes::revision(m_standards->staged())))
        && (!m_custom || (m_customApplied == CustomShortcutsTypes::revision(m_custom->applied()) && m_customDraft == CustomShortcutsTypes::revision(m_custom->staged())));
}
QList<ShortcutScheme::Command> ShortcutSchemeDialog::selectedCommands() const
{
    QList<ShortcutScheme::Command> result;
    for (const auto &row : m_commands) if (row.selectable && row.item->checkState(0) == Qt::Checked) result.append(row.definition);
    return result;
}
void ShortcutSchemeDialog::selectedChanges(ShortcutChanges &globals, ShortcutChanges &standards) const
{
    globals.clear(); standards.clear();
    for (const auto &row : m_rows) if (row.matched && row.item->checkState(0) == Qt::Checked)
        (row.scope == ShortcutScheme::Scope::Global ? globals : standards).insert(row.id, row.keys);
}
void ShortcutSchemeDialog::updateReview()
{
    if (m_importCoordinator) m_error = m_importCoordinator->errorText();
    ShortcutChanges globals, standards; selectedChanges(globals, standards);
    QStringList report; if (!m_error.isEmpty()) report.append(m_error);
    for (const auto &issue : m_read.issues) report.append(issue.message);
    for (const auto &issue : m_plan.issues) if (!report.contains(issue.message)) report.append(issue.message);
    for (auto mapping = m_migrations.cbegin(); mapping != m_migrations.cend(); ++mapping)
        report.append(tr("Command identity mapping: %1 → %2. Definition edits and launch bindings use the selected target.").arg(mapping.key(), mapping.value()));
    if (!m_plan.commands.isEmpty()) report.append(m_coordinatorRequired
        ? tr("Stage selected definitions, then Apply on Custom Keyboard Shortcuts. Verified Apply refreshes global bindings when that session is ready. Return here and Update review before staging bindings. Apply remains separate on each settings page.")
        : tr("Custom definitions are staged separately and never run on import. New command bindings remain pending until you Apply on Custom Keyboard Shortcuts, Refresh global shortcuts, and Update review here."));
    if (m_mode == Mode::Export) for (const auto &row : m_rows)
        if (row.matched && row.item->checkState(0) == Qt::Checked && row.scope == ShortcutScheme::Scope::Global && row.id.context != QLatin1String("default")) {
            report.append(tr("Selected nondefault contexts use a versioned extension. Plasma imports the native default sections and ignores this extension.")); break;
        }
    if (m_mode == Mode::Import) {
        if (m_globals) report.append(conflictDescriptions(m_globals->staged(), globals));
        if (m_standards) report.append(conflictDescriptions(m_standards->staged(), standards));
    }
    m_workflow->setVisible(m_coordinatorRequired); m_continueCustom->setVisible(m_coordinatorRequired);
    m_refreshBindings->setVisible(m_coordinatorRequired); m_discardReview->setVisible(m_coordinatorRequired);
    if (m_coordinatorRequired) {
        m_intro->setText(tr("Review the imported bindings and command definitions. Closing this review keeps its source, mappings, and choices so you can resume on another page. Discard review clears only this import review. Staging and Apply never run commands."));
        m_close->setText(tr("&Close review"));
        QString bindingReason, definitionReason, refreshReason;
        const bool bindingsReady = m_importCoordinator && m_importCoordinator->canStageBindings(&bindingReason);
        const bool definitionsReady = m_importCoordinator && m_importCoordinator->canStageDefinitions(&definitionReason);
        const bool refreshReady = m_importCoordinator && m_importCoordinator->canRefreshGlobals(&refreshReason);
        m_commit->setEnabled(bindingsReady); m_stageCommands->setEnabled(definitionsReady);
        m_refreshBindings->setEnabled(refreshReady); m_refreshBindings->setToolTip(plainToolTip(refreshReason));
        m_continueCustom->setEnabled(m_importCoordinator && m_custom && m_loaded && !m_plan.commands.isEmpty());
        m_discardReview->setEnabled(m_importCoordinator && m_loaded);
        m_updateReview->setEnabled(m_importCoordinator && m_loaded
            && (!m_globals || !m_globals->isBusy()) && (!m_standards || !m_standards->isBusy()) && (!m_custom || !m_custom->isBusy()));
        m_mapCommand->setEnabled(m_importCoordinator && m_custom && m_tree->currentItem()
            && m_tree->currentItem()->data(0, Qt::UserRole + 2).isValid());
        m_tree->setEnabled(bool(m_importCoordinator)); m_reassign->setEnabled(bool(m_importCoordinator));
        m_browse->setEnabled(bool(m_importCoordinator)); m_reviewFile->setEnabled(bool(m_importCoordinator));
        m_reviewBuiltIn->setEnabled(m_importCoordinator && m_builtIns->count() > 0);
        m_workflow->setText(m_importCoordinator ? m_importCoordinator->statusText() : tr("The retained import coordinator is no longer available. Its settings drafts have not been changed."));
        for (const auto &reason : {bindingReason, m_plan.commands.isEmpty() ? QString() : definitionReason})
            if (m_loaded && !reason.isEmpty() && !report.contains(reason)) report.append(reason);
        int globalCount = 0, standardCount = 0, unresolvedCount = 0;
        for (const auto &row : m_rows) if (row.item->checkState(0) == Qt::Checked) {
            if (row.scope == ShortcutScheme::Scope::Global) ++globalCount; else ++standardCount;
            if (!row.matched) ++unresolvedCount;
        }
        m_summary->setText(tr("Selected: %1 global and %2 standard bindings; %3 command definitions. %4 selected bindings still need resolution. Staging changes only drafts.")
            .arg(globalCount).arg(standardCount).arg(selectedCommands().size()).arg(unresolvedCount));
        m_report->setText(report.join(QLatin1Char('\n'))); return;
    }
    bool valid = !globals.isEmpty() || !standards.isEmpty() || (m_mode == Mode::Export && !selectedCommands().isEmpty()); QString problem;
    if (!baselineCurrent()) { valid = false; problem = tr("A session or draft changed after this review. Update review before continuing."); }
    if ((!globals.isEmpty() && !m_globals) || (!standards.isEmpty() && !m_standards)) { valid = false; problem = tr("A selected shortcut session is no longer connected."); }
    if ((!globals.isEmpty() && m_globals && m_globals->isBusy()) || (!standards.isEmpty() && m_standards && m_standards->isBusy())
        || (!selectedCommands().isEmpty() && m_custom && m_custom->isBusy())) { valid = false; problem = tr("A selected session has an active job. Finish it and Update review before continuing."); }
    if (m_mode == Mode::Import) {
        valid = valid && m_loaded && m_plan.valid;
        for (const auto &pair : {qMakePair(m_globals.data(), globals), qMakePair(m_standards.data(), standards)}) {
            if (pair.second.isEmpty()) continue;
            if (!pair.first || pair.first->isBusy() || !pair.first->canApply()) { valid = false; problem = tr("A selected shortcut session is busy, read-only, or conflicted. Finish its job or Revert before importing."); }
            else {
                QString validationError;
                if (!pair.first->validateChanges(pair.second, m_reassign->isChecked(), validationError)) { valid = false; problem = validationError; }
            }
        }
    }
    if (!problem.isEmpty()) report.append(problem);
    m_report->setText(report.join(QLatin1Char('\n')));
    m_summary->setText(m_mode == Mode::Import ? tr("Selected: %1 global and %2 standard assignments. Apply remains separate on the settings pages.").arg(globals.size()).arg(standards.size())
        : tr("Selected: %1 global and %2 standard staged assignments; %3 command definitions.").arg(globals.size()).arg(standards.size()).arg(selectedCommands().size()));
    m_commit->setEnabled(valid);
    const bool commandsReady = m_mode == Mode::Import && m_loaded && m_plan.valid && baselineCurrent()
        && m_custom && m_custom->canApply() && !m_custom->isBusy() && !selectedCommands().isEmpty();
    m_stageCommands->setEnabled(commandsReady);
    m_mapCommand->setEnabled(m_mode == Mode::Import && m_custom && m_tree->currentItem()
        && m_tree->currentItem()->data(0, Qt::UserRole + 2).isValid());
    m_updateReview->setEnabled(m_mode == Mode::Export || m_loaded);
}
bool ShortcutSchemeDialog::stageSelected()
{
    if (m_mode != Mode::Import) return false;
    if (m_coordinatorRequired) { if (!m_importCoordinator) return false; const bool result = m_importCoordinator->stageBindings(); syncFromCoordinator(); return result; }
    updateReview(); if (!m_commit->isEnabled()) return false;
    ShortcutChanges globals, standards; selectedChanges(globals, standards); QString error;
    if (!ShortcutsSettings::stageTogether(m_globals, globals, m_standards, standards, m_reassign->isChecked(), error)) {
        m_error = error; updateReview(); return false;
    }
    if (m_plan.unresolved.isEmpty() && m_plan.commands.isEmpty()) accept();
    else {
        pinBaseline(); updateReview();
        m_summary->setText(tr("Selected bindings are staged. Unresolved bindings and custom definitions remain in this review; Apply and Refresh are separate."));
    }
    return true;
}
bool ShortcutSchemeDialog::stageCommandDefinitions()
{
    if (m_coordinatorRequired) { if (!m_importCoordinator) return false; const bool result = m_importCoordinator->stageDefinitions(); syncFromCoordinator(); return result; }
    updateReview(); if (!m_stageCommands->isEnabled()) return false;
    QList<CustomShortcutEntry> definitions;
    for (const auto &command : selectedCommands()) { CustomShortcutEntry entry; entry.component = command.component; entry.name = command.name; entry.exec = command.exec; definitions.append(entry); }
    if (!m_custom->stageCommands(definitions)) { m_error = m_custom->errorText(); updateReview(); return false; }
    pinBaseline(); populate(true); updateReview();
    m_summary->setText(tr("Command definitions are staged. Apply them on Custom Keyboard Shortcuts, then Refresh global shortcuts and Update review to resolve pending bindings."));
    return true;
}
void ShortcutSchemeDialog::reviewAgain()
{
    if (m_coordinatorRequired) { if (m_importCoordinator) { m_importCoordinator->review(); syncFromCoordinator(); } return; }
    if (m_mode == Mode::Import) { if (m_loaded) review(m_read); }
    else { m_error.clear(); pinBaseline(); populate(); updateReview(); }
}
bool ShortcutSchemeDialog::mapCommand(const QString &source, const QString &target)
{
    if (m_coordinatorRequired) { if (!m_importCoordinator) return false; const bool result = m_importCoordinator->mapCommand(source, target); syncFromCoordinator(); return result; }
    if (m_mode != Mode::Import || !m_loaded || !m_custom) return false;
    bool known = false; for (const auto &command : m_read.document.commands) if (command.component == source) known = true;
    const auto *existing = m_custom->staged().find(target);
    if (!known || (target != source && (!existing || !existing->command || !existing->editable || existing->pendingRemoval))) return false;
    if (target == source) m_migrations.remove(source); else m_migrations.insert(source, target);
    review(m_read); return m_plan.valid;
}
bool ShortcutSchemeDialog::exportSelected(const QString &path, bool overwriteConfirmed)
{
    if (m_mode != Mode::Export) return false;
    if (!baselineCurrent()) { m_error = tr("The staged bindings changed. Update review before exporting."); updateReview(); return false; }
    updateReview(); if (!m_commit->isEnabled()) return false;
    if (QFileInfo::exists(path) && !overwriteConfirmed) { m_error = tr("Confirm replacing the existing scheme in the Save dialog before exporting."); updateReview(); return false; }
    ShortcutChanges globals, standards; selectedChanges(globals, standards);
    const auto commands = selectedCommands();
    if (globals.isEmpty() && standards.isEmpty() && commands.isEmpty()) { m_error = tr("Select at least one assignment or definition to export."); updateReview(); return false; }
    if ((!globals.isEmpty() && !m_globals) || (!standards.isEmpty() && !m_standards) || (!commands.isEmpty() && !m_custom)) return false;
    ShortcutScheme::Selection selection; selection.globals = !globals.isEmpty(); selection.standards = !standards.isEmpty();
    selection.globalActions = globals.keys(); selection.standardActions = standards.keys();
    auto globalSnapshot = m_globals ? m_globals->staged() : ShortcutsSnapshot{};
    // Command definitions have their own reviewed selection. Filter actions
    // first so selecting a definition cannot export an unchecked binding.
    globalSnapshot.actions.erase(std::remove_if(globalSnapshot.actions.begin(), globalSnapshot.actions.end(), [&](const auto &action) { return !globals.contains(action.id); }), globalSnapshot.actions.end());
    for (const auto &command : commands) { selection.globals = true; selection.globalComponents.append(command.component); }
    const auto result = ShortcutScheme::exportFile(path, m_name->text(), globalSnapshot, m_standards ? m_standards->staged() : ShortcutsSnapshot{}, selection, commands);
    if (!result.success) { m_error = result.error; updateReview(); return false; }
    m_path->setText(path); m_error.clear(); updateReview(); m_summary->setText(tr("Selected staged bindings were exported. No preferences were changed.")); return true;
}
void ShortcutSchemeDialog::chooseFile()
{
    QFileDialog dialog(this, tr("Review Shortcut Scheme"), m_path->text(), tr("Shortcut schemes (*.kksrc)"));
    dialog.setOption(QFileDialog::DontUseNativeDialog); dialog.setAcceptMode(QFileDialog::AcceptOpen); dialog.setFileMode(QFileDialog::ExistingFile);
    Win7::styleSettingsDialog(&dialog);
    if (dialog.exec() == QDialog::Accepted && !dialog.selectedFiles().isEmpty()) loadFile(dialog.selectedFiles().first());
}
void ShortcutSchemeDialog::chooseExport()
{
    QFileDialog dialog(this, tr("Export Shortcut Scheme"), m_path->text(), tr("Shortcut schemes (*.kksrc)"));
    dialog.setOption(QFileDialog::DontUseNativeDialog); Win7::styleSettingsDialog(&dialog);
    dialog.setAcceptMode(QFileDialog::AcceptSave); dialog.setFileMode(QFileDialog::AnyFile); dialog.setDefaultSuffix(QStringLiteral("kksrc"));
    if (dialog.exec() == QDialog::Accepted && !dialog.selectedFiles().isEmpty()) exportSelected(dialog.selectedFiles().first(), true);
}
void ShortcutSchemeDialog::chooseCommandMapping()
{
    if (!m_custom || !m_tree->currentItem()) return;
    const auto selected = m_tree->currentItem()->data(0, Qt::UserRole + 2).toString(); if (selected.isEmpty()) return;
    QString source = selected; for (auto i = m_migrations.cbegin(); i != m_migrations.cend(); ++i) if (i.value() == selected) source = i.key();
    QStringList choices{source}; for (const auto &entry : m_custom->staged().entries) if (entry.command && entry.editable && !entry.pendingRemoval && !choices.contains(entry.component)) choices.append(entry.component);
    bool accepted = false; const auto target = QInputDialog::getItem(this, tr("Map Imported Command"), tr("Choose the reviewed native command identity. Choosing the original identity retains a new definition:"), choices, choices.indexOf(selected), false, &accepted);
    if (accepted) mapCommand(source, target);
}
