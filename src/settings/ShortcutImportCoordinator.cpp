#include "ShortcutImportCoordinator.h"
#include "ShortcutsSettings.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QTimer>
#include <algorithm>

namespace {
bool refusal(QString *reason, const QString &text) { if (reason) *reason = text; return false; }
}

ShortcutImportCoordinator *ShortcutImportCoordinator::instance()
{
    static QPointer<ShortcutImportCoordinator> session;
    if (!session) session = new ShortcutImportCoordinator(ShortcutsSettings::instance(), ShortcutsSettings::standardInstance(),
        CustomShortcutsSettings::instance(), QCoreApplication::instance());
    return session;
}
ShortcutImportCoordinator::ShortcutImportCoordinator(ShortcutsSettings *globals, ShortcutsSettings *standards,
                                                     CustomShortcutsSettings *custom, QObject *parent)
    : QObject(parent), m_globals(globals), m_standards(standards), m_custom(custom)
{
    if (globals) {
        connect(globals, &SettingsSession::stateChanged, this, [this] { sessionChanged(true); });
        connect(globals, &QObject::destroyed, this, [this] { sessionChanged(true); });
    }
    if (standards) {
        connect(standards, &SettingsSession::stateChanged, this, [this] { sessionChanged(false); });
        connect(standards, &QObject::destroyed, this, [this] { sessionChanged(false); });
    }
    if (custom) {
        connect(custom, &SettingsSession::stateChanged, this, [this] { sessionChanged(false); });
        connect(custom, &QObject::destroyed, this, [this] { sessionChanged(false); });
        connect(custom, &SettingsSession::applyFinished, this, &ShortcutImportCoordinator::customApplied);
    }
}
ShortcutsSettings *ShortcutImportCoordinator::globalSession() const { return m_globals; }
ShortcutsSettings *ShortcutImportCoordinator::standardSession() const { return m_standards; }
CustomShortcutsSettings *ShortcutImportCoordinator::customSession() const { return m_custom; }
bool ShortcutImportCoordinator::hasPendingWork() const { return hasDocument() && phase() != Phase::Complete; }
ShortcutImportCoordinator::Phase ShortcutImportCoordinator::phase() const
{
    if (!hasDocument()) return Phase::Empty;
    if (m_refreshing) return Phase::RefreshingGlobals;
    if (m_waitCustomApply) return Phase::AwaitCustomApply;
    if (m_needsRefresh) return Phase::AwaitGlobalRefresh;
    if (m_bindingsStaged) return bindingsApplied() ? Phase::Complete : Phase::Staged;
    if (m_definitionsRequested && selectedDefinitions().size() && m_dependenciesApplied
        && m_read.document.bindings.isEmpty() && definitionsApplied(selectedDefinitions())) return Phase::Complete;
    return m_dependenciesApplied ? Phase::ReviewBindings : Phase::Review;
}
QString ShortcutImportCoordinator::statusText() const
{
    if (!m_error.isEmpty()) return m_error;
    switch (phase()) {
    case Phase::Empty: return {};
    case Phase::AwaitCustomApply: return tr("Import retained. Apply its definitions on Custom Keyboard Shortcuts to continue.");
    case Phase::AwaitGlobalRefresh: {
        QString why; canRefreshGlobals(&why);
        return why.isEmpty() ? tr("Definitions verified. Global shortcut discovery is waiting to refresh.") : why;
    }
    case Phase::RefreshingGlobals: return tr("Definitions verified. Refreshing global shortcuts; the import is retained.");
    case Phase::Staged: return tr("Imported bindings are staged. Apply them on their shortcut settings pages.");
    case Phase::Complete: return tr("Selected imported definitions and bindings match applied settings.");
    case Phase::ReviewBindings: return reviewCurrent() ? tr("Command registrations are verified. Review and stage the selected bindings.")
        : tr("Command registrations were refreshed. Update review before staging their bindings.");
    case Phase::Review: return reviewCurrent() ? tr("Shortcut import review retained. Select assignments and definitions to stage.")
        : tr("Settings changed. Update the retained import review before staging.");
    }
    return {};
}
bool ShortcutImportCoordinator::reject(const QString &error)
{ m_error = error; emit changed(); return false; }
void ShortcutImportCoordinator::resetDependencies()
{
    m_dependencies.clear(); m_dependencyOwner.clear(); m_refreshOwner.clear();
    m_definitionsRequested = m_waitCustomApply = m_dependenciesApplied = m_needsRefresh = false;
    m_refreshing = m_refreshScheduled = m_bindingsStaged = false;
}
void ShortcutImportCoordinator::discard()
{
    ++m_generation; m_source.clear(); m_sourceHash.clear(); m_path.clear(); m_read = {}; m_plan = {};
    m_migrations.clear(); m_globalChecks.clear(); m_standardChecks.clear(); m_commandChecks.clear();
    m_error.clear(); m_reassign = m_reviewValid = false; resetDependencies(); emit changed();
}
bool ShortcutImportCoordinator::loadData(const QByteArray &bytes, const QString &path)
{
    const auto result = ShortcutScheme::parse(bytes);
    if (!result.success) { m_reviewValid = false; return reject(result.error); }
    ++m_generation; m_source = bytes; m_sourceHash = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
    m_path = path; m_read = result; m_error.clear(); m_migrations.clear(); m_globalChecks.clear();
    m_standardChecks.clear(); m_commandChecks.clear(); m_reassign = false; resetDependencies();
    for (const auto &binding : result.document.bindings)
        if (binding.scope == ShortcutScheme::Scope::Global) m_globalChecks.insert(binding.id, true);
        else m_standardChecks.insert(binding.id.action, true);
    for (const auto &command : result.document.commands) m_commandChecks.insert(command.component, true);
    return review();
}
bool ShortcutImportCoordinator::loadFile(const QString &path)
{
    QFile file(path); const QFileInfo info(path);
    if (!info.isAbsolute() || !info.isFile() || info.suffix().compare(QLatin1String("kksrc"), Qt::CaseInsensitive)
        || !file.open(QIODevice::ReadOnly) || file.size() > 4 * 1024 * 1024) {
        m_reviewValid = false; return reject(tr("Select a readable local .kksrc file of at most 4 MiB. The previous import is retained."));
    }
    const auto data = file.read(4 * 1024 * 1024 + 1);
    if (file.error() != QFileDevice::NoError || data.size() > 4 * 1024 * 1024) {
        m_reviewValid = false; return reject(tr("The scheme could not be read completely. The previous import is retained."));
    }
    return loadData(data, path);
}
QString ShortcutImportCoordinator::sourceComponent(const QString &resolvedComponent) const
{
    for (auto it = m_migrations.cbegin(); it != m_migrations.cend(); ++it)
        if (it.value() == resolvedComponent) return it.key();
    return resolvedComponent;
}
ShortcutActionId ShortcutImportCoordinator::sourceId(ShortcutScheme::Scope scope, const ShortcutActionId &resolvedId) const
{
    auto source = resolvedId;
    if (scope == ShortcutScheme::Scope::Global) source.component = sourceComponent(source.component);
    return source;
}
bool ShortcutImportCoordinator::bindingSelected(ShortcutScheme::Scope scope, const ShortcutActionId &resolvedId) const
{ return scope == ShortcutScheme::Scope::Global ? m_globalChecks.value(sourceId(scope, resolvedId), false) : m_standardChecks.value(resolvedId.action, false); }
bool ShortcutImportCoordinator::commandSelected(const QString &resolvedComponent) const
{ return m_commandChecks.value(sourceComponent(resolvedComponent), false); }
bool ShortcutImportCoordinator::setBindingSelected(ShortcutScheme::Scope scope, const ShortcutActionId &resolvedId, bool selected)
{
    if (scope == ShortcutScheme::Scope::Global) {
        const auto key = sourceId(scope, resolvedId); if (!m_globalChecks.contains(key)) return false;
        if (m_globalChecks.value(key) == selected) return true; m_globalChecks[key] = selected;
    } else {
        if (!m_standardChecks.contains(resolvedId.action)) return false;
        if (m_standardChecks.value(resolvedId.action) == selected) return true; m_standardChecks[resolvedId.action] = selected;
    }
    m_bindingsStaged = false; m_error.clear(); emit changed(); return true;
}
bool ShortcutImportCoordinator::setCommandSelected(const QString &resolvedComponent, bool selected)
{
    const auto key = sourceComponent(resolvedComponent); if (!m_commandChecks.contains(key)) return false;
    if (m_commandChecks.value(key) == selected) return true;
    m_commandChecks[key] = selected; resetDependencies(); m_error.clear(); emit changed(); return true;
}
void ShortcutImportCoordinator::setReassignConflicts(bool reassign)
{
    if (m_reassign == reassign) return;
    m_reassign = reassign; m_bindingsStaged = false; m_error.clear(); emit changed();
}
bool ShortcutImportCoordinator::mapCommand(const QString &source, const QString &target)
{
    if (!hasDocument() || !m_custom || !m_commandChecks.contains(source)) return reject(tr("This imported command cannot be mapped."));
    const auto *entry = m_custom->staged().find(target);
    if (target != source && (!entry || !entry->command || !entry->editable || entry->pendingRemoval))
        return reject(tr("Map to an existing writable command, or retain the original command identity."));
    auto mappings = m_migrations;
    if (source == target) mappings.remove(source); else mappings.insert(source, target);
    const auto plan = ShortcutScheme::planImport(m_read.document, m_globals ? m_globals->staged() : ShortcutsSnapshot{},
        m_standards ? m_standards->staged() : ShortcutsSnapshot{}, mappings);
    if (!plan.valid) return reject(plan.error);
    ++m_generation; m_migrations = mappings; resetDependencies(); return review();
}
void ShortcutImportCoordinator::pinReview()
{
    m_reviewEpoch = m_epoch; m_reviewValid = true;
    m_hadGlobals = bool(m_globals); m_hadStandards = bool(m_standards); m_hadCustom = bool(m_custom);
    m_globalApplied = m_globals ? ShortcutsTypes::revision(m_globals->applied()) : QByteArray{};
    m_globalDraft = m_globals ? ShortcutsTypes::revision(m_globals->staged()) : QByteArray{};
    m_standardApplied = m_standards ? ShortcutsTypes::revision(m_standards->applied()) : QByteArray{};
    m_standardDraft = m_standards ? ShortcutsTypes::revision(m_standards->staged()) : QByteArray{};
    m_customApplied = m_custom ? CustomShortcutsTypes::revision(m_custom->applied()) : QByteArray{};
    m_customDraft = m_custom ? CustomShortcutsTypes::revision(m_custom->staged()) : QByteArray{};
}
bool ShortcutImportCoordinator::reviewCurrent() const
{
    return m_reviewValid && m_reviewEpoch == m_epoch && m_hadGlobals == bool(m_globals) && m_hadStandards == bool(m_standards)
        && m_hadCustom == bool(m_custom)
        && (!m_globals || (m_globalApplied == ShortcutsTypes::revision(m_globals->applied()) && m_globalDraft == ShortcutsTypes::revision(m_globals->staged())))
        && (!m_standards || (m_standardApplied == ShortcutsTypes::revision(m_standards->applied()) && m_standardDraft == ShortcutsTypes::revision(m_standards->staged())))
        && (!m_custom || (m_customApplied == CustomShortcutsTypes::revision(m_custom->applied()) && m_customDraft == CustomShortcutsTypes::revision(m_custom->staged())));
}
bool ShortcutImportCoordinator::review()
{
    if (!hasDocument()) return reject(tr("Load a shortcut scheme before reviewing it."));
    m_plan = ShortcutScheme::planImport(m_read.document, m_globals ? m_globals->staged() : ShortcutsSnapshot{},
        m_standards ? m_standards->staged() : ShortcutsSnapshot{}, m_migrations);
    if (!m_plan.valid) { m_reviewValid = false; return reject(m_plan.error); }
    if (m_refreshing) { m_reviewValid = false; return reject(tr("Wait for global shortcut discovery to finish before reviewing.")); }
    if (m_definitionsRequested && definitionsApplied(selectedDefinitions())) {
        m_dependencies = selectedDefinitions(); m_dependencyOwner = m_custom->applied().owner;
        m_dependenciesApplied = true; m_waitCustomApply = false;
    }
    m_error.clear(); pinReview(); emit changed(); return true;
}
QList<CustomShortcutCommand> ShortcutImportCoordinator::selectedDefinitions() const
{
    QList<CustomShortcutCommand> result;
    for (const auto &definition : m_plan.commands) if (commandSelected(definition.component)) {
        QString normalized, ignored; CustomShortcutsTypes::normalizeCommand(definition.name, definition.exec, normalized, ignored);
        result << CustomShortcutCommand{definition.component, definition.name, normalized.isEmpty() ? definition.exec : normalized};
    }
    return result;
}
bool ShortcutImportCoordinator::definitionsApplied(const QList<CustomShortcutCommand> &definitions, const QString &owner) const
{
    if (definitions.isEmpty()) return true;
    if (!m_custom || !m_custom->isReady() || m_custom->isBusy() || m_custom->hasConflict()
        || (!owner.isEmpty() && m_custom->applied().owner != owner)) return false;
    for (const auto &definition : definitions) {
        const auto *entry = m_custom->applied().find(definition.component);
        if (!entry || !entry->command || !entry->registered || entry->name != definition.name || entry->exec != definition.exec) return false;
    }
    return true;
}
void ShortcutImportCoordinator::selectedChanges(ShortcutChanges &globals, ShortcutChanges &standards, QString &error) const
{
    globals.clear(); standards.clear(); error.clear();
    for (const auto &binding : m_plan.unresolved) if (bindingSelected(binding.scope, binding.id)) {
        error = tr("Selected binding %1 / %2 / %3 is unresolved or read-only. Resolve it or clear its check explicitly.")
            .arg(binding.id.component, binding.id.context, binding.id.action); return;
    }
    for (auto it = m_plan.globals.cbegin(); it != m_plan.globals.cend(); ++it)
        if (bindingSelected(ShortcutScheme::Scope::Global, it.key())) globals.insert(it.key(), it.value());
    for (auto it = m_plan.standards.cbegin(); it != m_plan.standards.cend(); ++it)
        if (bindingSelected(ShortcutScheme::Scope::Standard, it.key())) standards.insert(it.key(), it.value());
}
bool ShortcutImportCoordinator::canStageBindings(QString *reason) const
{
    if (reason) reason->clear();
    if (!hasDocument() || !m_plan.valid) return refusal(reason, tr("No valid shortcut import is loaded."));
    if (!reviewCurrent()) return refusal(reason, tr("Settings changed. Update review before staging bindings."));
    if (m_refreshing || m_waitCustomApply || m_needsRefresh) return refusal(reason, statusText());
    if (!definitionsApplied(selectedDefinitions())) return refusal(reason, tr("Apply the selected command definitions on Custom Keyboard Shortcuts before importing their bindings."));
    if (!selectedDefinitions().isEmpty() && (!m_globals || m_globals->applied().owner != m_custom->applied().owner))
        return refusal(reason, tr("Global shortcuts and command definitions have different service owners. Refresh and review their current state."));
    ShortcutChanges globals, standards; QString error; selectedChanges(globals, standards, error);
    if (!error.isEmpty()) return refusal(reason, error);
    if (globals.isEmpty() && standards.isEmpty()) return refusal(reason, tr("Select at least one resolved binding to stage."));
    for (const auto &pair : {qMakePair(m_globals.data(), globals), qMakePair(m_standards.data(), standards)})
        if (!pair.second.isEmpty() && (!pair.first || !pair.first->validateChanges(pair.second, m_reassign, error)))
            return refusal(reason, error.isEmpty() ? tr("A selected shortcut session is unavailable.") : error);
    return true;
}
bool ShortcutImportCoordinator::canStageDefinitions(QString *reason) const
{
    if (reason) reason->clear();
    if (!hasDocument() || !m_plan.valid || !reviewCurrent()) return refusal(reason, tr("Update the import review before staging definitions."));
    if (!m_custom || m_custom->isBusy() || !m_custom->canApply()) return refusal(reason, tr("Custom shortcuts are unavailable, busy, read-only or conflicted."));
    if (m_refreshing) return refusal(reason, tr("Wait for global shortcut discovery to finish."));
    const auto definitions = selectedDefinitions();
    if (definitions.isEmpty()) return refusal(reason, tr("Select at least one command definition."));
    for (const auto &definition : definitions) {
        QString normalized, error;
        const auto *entry = m_custom->staged().find(definition.component);
        if (!CustomShortcutsTypes::validComponent(definition.component)
            || !CustomShortcutsTypes::normalizeCommand(definition.name, definition.exec, normalized, error))
            return refusal(reason, error.isEmpty() ? tr("An imported command identity is invalid.") : error);
        if (entry && (!entry->command || !entry->editable || entry->pendingRemoval))
            return refusal(reason, tr("An imported command conflicts with a protected, removed or application definition."));
        if (!entry && m_custom->staged().application(definition.component))
            return refusal(reason, tr("The imported command identity belongs to an installed application."));
    }
    return true;
}
bool ShortcutImportCoordinator::stageDefinitions()
{
    QString error; if (!canStageDefinitions(&error)) return reject(error);
    const auto definitions = selectedDefinitions(); const auto generation = m_generation;
    QList<CustomShortcutEntry> staged;
    for (const auto &definition : definitions) { CustomShortcutEntry entry; entry.component = definition.component; entry.name = definition.name; entry.exec = definition.exec; staged << entry; }
    if (!m_custom->stageCommands(staged, true)) return reject(m_custom ? m_custom->errorText() : tr("The custom shortcut session was lost. The import is retained."));
    if (m_generation != generation) return false;
    if (!m_custom) return reject(tr("The custom shortcut session was lost. The import is retained."));
    m_definitionsRequested = true; m_dependencies = definitions; m_dependencyOwner = m_custom->applied().owner;
    m_dependenciesApplied = definitionsApplied(definitions, m_dependencyOwner);
    m_waitCustomApply = !m_dependenciesApplied; m_needsRefresh = m_dependenciesApplied;
    m_error.clear(); pinReview(); emit changed(); scheduleRefresh(); return true;
}
bool ShortcutImportCoordinator::stageBindings()
{
    QString error; if (!canStageBindings(&error)) return reject(error);
    ShortcutChanges globals, standards; selectedChanges(globals, standards, error); const auto generation = m_generation;
    if (!ShortcutsSettings::stageTogether(m_globals, globals, m_standards, standards, m_reassign, error)) return reject(error);
    if (m_generation != generation) return false;
    m_bindingsStaged = true; m_error.clear(); pinReview(); emit changed(); return true;
}
bool ShortcutImportCoordinator::bindingsApplied() const
{
    ShortcutChanges globals, standards; QString error; selectedChanges(globals, standards, error);
    if (!error.isEmpty() || !definitionsApplied(selectedDefinitions())) return false;
    if (!selectedDefinitions().isEmpty() && (!m_globals || m_globals->applied().owner != m_custom->applied().owner)) return false;
    for (const auto &pair : {qMakePair(m_globals.data(), globals), qMakePair(m_standards.data(), standards)}) {
        if (pair.second.isEmpty()) continue;
        if (!pair.first || !pair.first->isReady() || pair.first->isBusy() || pair.first->hasConflict()) return false;
        for (auto it = pair.second.cbegin(); it != pair.second.cend(); ++it) {
            const auto *action = pair.first->applied().find(it.key());
            if (!action || !ShortcutsTypes::sameKeys(action->current, it.value())) return false;
        }
    }
    return true;
}
bool ShortcutImportCoordinator::canRefreshGlobals(QString *reason) const
{
    if (reason) reason->clear();
    if (!hasDocument() || !m_globals) return refusal(reason, tr("The import's global shortcut session is unavailable."));
    if (m_refreshing || m_globals->isBusy()) return refusal(reason, tr("The import is retained while global shortcut work finishes."));
    if (!m_globals->isReady()) return refusal(reason, tr("Global shortcuts could not be read. Restore their connection before continuing the import."));
    if (m_globals->isDirty() || m_globals->hasConflict()) return refusal(reason, tr("Apply or Revert the existing Global Keyboard Shortcuts draft before refreshing imported command bindings."));
    if (m_waitCustomApply) return refusal(reason, tr("Apply the imported definitions on Custom Keyboard Shortcuts first."));
    if (m_dependenciesApplied && (!definitionsApplied(m_dependencies, m_dependencyOwner)
        || m_globals->applied().owner != m_dependencyOwner)) return refusal(reason, tr("The command or global shortcut service owner changed. Refresh both settings sessions and Update review."));
    return true;
}
bool ShortcutImportCoordinator::refreshGlobals()
{
    QString error; if (!canRefreshGlobals(&error)) return reject(error);
    m_refreshing = true; m_reviewValid = false; m_refreshGeneration = m_generation;
    m_refreshOwner = m_dependenciesApplied ? m_dependencyOwner : m_globals->applied().owner;
    m_error.clear(); emit changed();
    if (m_refreshGeneration != m_generation || !m_refreshing || !m_globals) return false;
    m_globals->refresh(); return true;
}
void ShortcutImportCoordinator::finishRefresh()
{
    m_refreshing = false;
    if (m_refreshGeneration != m_generation || !hasDocument()) return;
    if (!m_globals || !m_globals->isReady() || m_globals->hasConflict() || m_globals->isDirty()
        || m_globals->applied().owner != m_refreshOwner
        || (m_dependenciesApplied && !definitionsApplied(m_dependencies, m_dependencyOwner))) {
        m_error = tr("The refreshed shortcut state or owner could not be verified. The import and its choices are retained.");
        return;
    }
    m_plan = ShortcutScheme::planImport(m_read.document, m_globals->staged(), m_standards ? m_standards->staged() : ShortcutsSnapshot{}, m_migrations);
    m_needsRefresh = false; m_reviewValid = false;
    m_error = m_plan.valid ? QString() : m_plan.error;
}
void ShortcutImportCoordinator::sessionChanged(bool global)
{
    ++m_epoch; m_reviewValid = false;
    if (global && m_refreshing && (!m_globals || !m_globals->isBusy())) finishRefresh();
    emit changed(); scheduleRefresh();
}
void ShortcutImportCoordinator::customApplied(bool success)
{
    if (!hasDocument() || !m_waitCustomApply) return;
    if (!success || !definitionsApplied(m_dependencies, m_dependencyOwner)) {
        m_error = tr("The imported command definitions were not fully verified. Their review and pending bindings are retained.");
        emit changed(); return;
    }
    m_waitCustomApply = false; m_dependenciesApplied = true; m_needsRefresh = true; m_error.clear();
    emit changed(); scheduleRefresh();
}
void ShortcutImportCoordinator::scheduleRefresh()
{
    if (!hasDocument() || !m_needsRefresh || !m_dependenciesApplied || m_refreshScheduled || m_refreshing || !canRefreshGlobals()) return;
    m_refreshScheduled = true; const auto generation = m_generation;
    QTimer::singleShot(0, this, [this, generation] {
        if (m_generation != generation) return;
        m_refreshScheduled = false;
        if (hasDocument() && m_needsRefresh && m_dependenciesApplied && canRefreshGlobals()) refreshGlobals();
    });
}
