#include "CustomShortcutsSettings.h"
#include "platform/shortcuts/CustomShortcutsBackend.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDataStream>
#include <QIODevice>
#include <QPointer>
#include <QSet>
#include <QTimer>
#include <QUrl>
#include <QUuid>
#include <KShell>
#include <algorithm>

const CustomShortcutEntry *CustomShortcutsSnapshot::find(const QString &component) const
{ for (const auto &entry : entries) if (entry.component == component) return &entry; return nullptr; }
const CustomShortcutApplication *CustomShortcutsSnapshot::application(const QString &id) const
{ for (const auto &entry : applications) if (entry.id == id) return &entry; return nullptr; }
bool CustomShortcutChanges::isEmpty() const
{ return addApplications.isEmpty() && addCommands.isEmpty() && editCommands.isEmpty() && removeComponents.isEmpty(); }
namespace CustomShortcutsTypes {
bool validComponent(const QString &component)
{
    return !component.isEmpty() && component.size() <= 255 && component.endsWith(QLatin1String(".desktop"))
        && std::none_of(component.cbegin(), component.cend(), [](QChar c) { return c == QLatin1Char('/') || c == QLatin1Char('\\') || c == QLatin1Char('|') || c.unicode() < 32; });
}
bool normalizeCommand(const QString &name, const QString &exec, QString &normalized, QString &error)
{
    auto singleLine = [](const QString &value) { return !value.contains(QLatin1Char('\n')) && !value.contains(QLatin1Char('\r')) && !value.contains(QChar(0)); };
    if (name.trimmed().isEmpty() || exec.trimmed().isEmpty() || name.size() > 4096 || exec.size() > 32768 || !singleLine(name) || !singleLine(exec)) {
        error = QObject::tr("A command needs a nonempty, single-line name and command."); return false;
    }
    normalized = exec;
    const QUrl url(exec);
    if (!url.scheme().isEmpty()) {
        if (!url.isLocalFile() || url.toLocalFile().isEmpty()) { error = QObject::tr("Commands accept a command line or a local file URL."); return false; }
        normalized = KShell::quoteArg(url.toLocalFile());
    }
    // Same literal-percent normalization as KCMKeys::addCommand/editCommand.
    normalized.replace(QStringLiteral("%%"), QStringLiteral("%"));
    normalized.replace(QLatin1Char('%'), QStringLiteral("%%"));
    return true;
}
QByteArray revision(const CustomShortcutsSnapshot &snapshot)
{
    QByteArray data; QDataStream stream(&data, QIODevice::WriteOnly);
    stream << snapshot.owner << snapshot.interfaceSignature << snapshot.configRevision << snapshot.inventoryRevision
           << snapshot.available << snapshot.writable << snapshot.contexts;
    auto entries = snapshot.entries;
    std::sort(entries.begin(), entries.end(), [](const auto &a, const auto &b) { return a.component < b.component; });
    for (const auto &entry : entries) stream << entry.component << entry.name << entry.exec << entry.path << entry.identity
        << entry.command << entry.registered << entry.editable << entry.removable << entry.pendingAddition << entry.pendingRemoval;
    ShortcutsSnapshot registry; registry.actions = snapshot.actions;
    stream << ShortcutsTypes::revision(registry);
    return QCryptographicHash::hash(data, QCryptographicHash::Sha256);
}
CustomShortcutChanges changes(const CustomShortcutsSnapshot &before, const CustomShortcutsSnapshot &after)
{
    CustomShortcutChanges result;
    for (const auto &entry : after.entries) {
        const auto *old = before.find(entry.component);
        if (entry.pendingRemoval) { if (old) result.removeComponents << entry.component; continue; }
        if (entry.pendingAddition) {
            if (entry.command && !old && !before.application(entry.component)) result.addCommands << CustomShortcutCommand{entry.component, entry.name, entry.exec};
            else result.addApplications << entry.component;
        }
        if (old && entry.command && (old->name != entry.name || old->exec != entry.exec))
            result.editCommands.insert(entry.component, {entry.component, entry.name, entry.exec});
    }
    return result;
}
bool validate(const CustomShortcutsSnapshot &snapshot, const CustomShortcutChanges &requested, QString &error)
{
    if (!snapshot.available || !snapshot.writable) { error = snapshot.reason.isEmpty() ? QObject::tr("Application shortcuts are read-only.") : snapshot.reason; return false; }
    QSet<QString> mutations;
    for (const auto &id : requested.removeComponents) {
        const auto *entry = snapshot.find(id);
        if (!entry || !entry->removable || mutations.contains(id)) { error = QObject::tr("A component cannot be removed safely or appears more than once."); return false; }
        mutations.insert(id);
    }
    for (const auto &id : requested.addApplications) {
        const auto *application = snapshot.application(id); const auto *entry = snapshot.find(id);
        if (!application || (entry && entry->registered) || mutations.contains(id)) { error = QObject::tr("An application is unavailable, already registered, or also staged for removal."); return false; }
        mutations.insert(id);
    }
    for (const auto &command : requested.addCommands) {
        QString normalized;
        if (!validComponent(command.component) || snapshot.find(command.component) || snapshot.application(command.component) || mutations.contains(command.component)
            || !normalizeCommand(command.name, command.exec, normalized, error)) {
            if (error.isEmpty()) error = QObject::tr("The new command's native desktop identity is invalid or already exists."); return false;
        }
        mutations.insert(command.component);
    }
    for (auto it = requested.editCommands.cbegin(); it != requested.editCommands.cend(); ++it) {
        const auto *entry = snapshot.find(it.key()); QString normalized;
        if (!entry || !entry->command || !entry->editable || requested.removeComponents.contains(it.key()) || it.value().component != it.key()
            || !normalizeCommand(it.value().name, it.value().exec, normalized, error)) {
            if (error.isEmpty()) error = QObject::tr("Only writable user command shortcut definitions can be edited."); return false;
        }
    }
    return true;
}
}
namespace {
bool fulfilled(const CustomShortcutsSnapshot &expected, const CustomShortcutChanges &requested, const CustomShortcutsSnapshot &snapshot)
{
    if (!snapshot.available || snapshot.owner != expected.owner || snapshot.interfaceSignature != expected.interfaceSignature) return false;
    QSet<QString> touched;
    for (const auto &id : requested.addApplications) touched.insert(id);
    for (const auto &command : requested.addCommands) touched.insert(command.component);
    for (auto it = requested.editCommands.cbegin(); it != requested.editCommands.cend(); ++it) touched.insert(it.key());
    for (const auto &id : requested.removeComponents) touched.insert(id);
    auto untouched = [&](const CustomShortcutsSnapshot &source) {
        CustomShortcutsSnapshot state; state.owner = source.owner; state.interfaceSignature = source.interfaceSignature;
        state.available = source.available; state.writable = source.writable;
        state.contexts = source.contexts;
        for (const auto &component : touched) state.contexts.remove(component);
        for (const auto &entry : source.entries) if (!touched.contains(entry.component)) state.entries << entry;
        for (const auto &action : source.actions) if (!touched.contains(action.id.component)) state.actions << action;
        return CustomShortcutsTypes::revision(state);
    };
    if (untouched(expected) != untouched(snapshot)) return false;
    for (const auto &id : requested.addApplications) { const auto *entry = snapshot.find(id); if (!entry || !entry->registered) return false; }
    for (const auto &command : requested.addCommands) {
        const auto *entry = snapshot.find(command.component);
        if (!entry || !entry->command || !entry->registered || entry->name != command.name || entry->exec != command.exec) return false;
    }
    for (const auto &command : requested.editCommands) {
        const auto *entry = snapshot.find(command.component);
        if (!entry || !entry->command || entry->name != command.name || entry->exec != command.exec) return false;
        const auto *before = expected.find(command.component);
        const bool registering = requested.addApplications.contains(command.component);
        if (!before || entry->registered != (before->registered || registering) || entry->path != before->path) return false;
        if (registering) continue; // The complete new action/default/context set is checked below.
        if (expected.contexts.value(command.component) != snapshot.contexts.value(command.component)) return false;
        ShortcutsSnapshot previousActions, nextActions;
        for (const auto &action : expected.actions) if (action.id.component == command.component) previousActions.actions << action;
        for (const auto &action : snapshot.actions) if (action.id.component == command.component) nextActions.actions << action;
        if (ShortcutsTypes::revision(previousActions) != ShortcutsTypes::revision(nextActions)) return false;
    }
    for (const auto &id : requested.removeComponents) {
        if (snapshot.find(id) || snapshot.contexts.contains(id)) return false;
        for (const auto &action : snapshot.actions) if (action.id.component == id) return false;
    }
    for (const auto &id : requested.addApplications) {
        const auto *app = expected.application(id);
        const auto *entry = snapshot.find(id);
        const auto edit = requested.editCommands.constFind(id);
        if (!app || !entry || entry->path != app->path) return false;
        if (snapshot.contexts.value(id) != QStringList{QStringLiteral("default")}) return false;
        if (edit == requested.editCommands.cend()) {
            if (entry->identity != app->identity || entry->name != app->name || entry->exec != app->exec) return false;
        } else if (!entry->command || entry->name != edit->name || entry->exec != edit->exec) return false;
        QList<ShortcutAction> actions;
        for (const auto &action : snapshot.actions) if (action.id.component == id) actions << action;
        if (actions.size() != app->defaults.size()) return false;
        for (const auto &source : app->defaults) {
            const auto found = std::find_if(actions.cbegin(), actions.cend(), [&](const auto &a) { return a.id == source.id; });
            if (found == actions.cend() || !ShortcutsTypes::sameKeys(found->current, source.defaults) || !ShortcutsTypes::sameKeys(found->defaults, source.defaults)) return false;
        }
    }
    for (const auto &command : requested.addCommands) {
        if (snapshot.contexts.value(command.component) != QStringList{QStringLiteral("default")}) return false;
        QList<ShortcutAction> actions;
        for (const auto &action : snapshot.actions) if (action.id.component == command.component) actions << action;
        if (actions.size() != 1 || actions.first().id.context != QLatin1String("default") || actions.first().id.action != QLatin1String("_launch")
            || !ShortcutsTypes::sameKeys(actions.first().current, {}) || !ShortcutsTypes::sameKeys(actions.first().defaults, {})) return false;
    }
    return true;
}
}
CustomShortcutsSettings *CustomShortcutsSettings::instance()
{
    static QPointer<CustomShortcutsSettings> session;
    if (!session) session = new CustomShortcutsSettings(new KdeCustomShortcutsBackend, QCoreApplication::instance());
    return session;
}
CustomShortcutsSettings::CustomShortcutsSettings(CustomShortcutsBackend *backend, QObject *parent) : SettingsSession(parent), m_backend(backend)
{
    Q_ASSERT(backend); backend->setParent(this);
    qRegisterMetaType<CustomShortcutsSnapshot>(); qRegisterMetaType<CustomShortcutChanges>(); qRegisterMetaType<ShortcutsOutcome>();
    connect(backend, &CustomShortcutsBackend::readFinished, this, &CustomShortcutsSettings::onRead);
    connect(backend, &CustomShortcutsBackend::writeFinished, this, &CustomShortcutsSettings::onWritten);
    connect(backend, &CustomShortcutsBackend::externalChange, this, &CustomShortcutsSettings::refresh);
    m_busy = true; QTimer::singleShot(0, this, &CustomShortcutsSettings::read);
}
bool CustomShortcutsSettings::canApply() const
{ return m_ready && m_applied.writable && !m_conflict && (m_lockedOwner.isEmpty() || m_lockedOwner != m_applied.owner); }
bool CustomShortcutsSettings::stage(const CustomShortcutsSnapshot &candidate)
{
    if (m_busy || !canApply()) return false;
    QString error;
    if (!CustomShortcutsTypes::validate(m_applied, CustomShortcutsTypes::changes(m_applied, candidate), error)) {
        m_error = error; emit stateChanged(); return false;
    }
    m_staged = candidate; m_dirty = !CustomShortcutsTypes::changes(m_applied, m_staged).isEmpty();
    m_error.clear(); m_result.clear(); m_keepError = false; emit stateChanged(); return true;
}
bool CustomShortcutsSettings::stageAddApplication(const QString &id)
{
    if (m_busy || !canApply()) return false;
    const auto *app = m_applied.application(id);
    if (!app) { m_error = tr("This installed application is unavailable."); emit stateChanged(); return false; }
    auto candidate = m_staged;
    auto it = std::find_if(candidate.entries.begin(), candidate.entries.end(), [&](const auto &e) { return e.component == id; });
    if (it == candidate.entries.end()) {
        CustomShortcutEntry entry; entry.component = id; entry.name = app->name; entry.exec = app->exec; entry.path = app->path;
        entry.identity = app->identity; entry.command = app->command; entry.pendingAddition = true; entry.removable = true;
        candidate.entries << entry;
    } else if (!it->registered) it->pendingAddition = true;
    else { m_error = tr("This application already has a registered shortcut component."); emit stateChanged(); return false; }
    return stage(candidate);
}
bool CustomShortcutsSettings::stageAddCommand(const QString &name, const QString &exec, const QString &component)
{
    CustomShortcutEntry definition;
    definition.component = component.isEmpty() ? QStringLiteral("net.local.") + QUuid::createUuid().toString(QUuid::Id128) + QStringLiteral(".desktop") : component;
    definition.name = name; definition.exec = exec;
    if (m_staged.find(definition.component)) { m_error = tr("This native command identity already exists."); emit stateChanged(); return false; }
    return stageCommands({definition});
}
bool CustomShortcutsSettings::stageCommands(const QList<CustomShortcutEntry> &definitions, bool registerExisting)
{
    if (m_busy || !canApply()) return false;
    auto candidate = m_staged; QSet<QString> seen;
    for (const auto &definition : definitions) {
        QString normalized, error;
        if (!CustomShortcutsTypes::validComponent(definition.component) || seen.contains(definition.component)
            || !CustomShortcutsTypes::normalizeCommand(definition.name, definition.exec, normalized, error)) {
            m_error = error.isEmpty() ? tr("Imported command identities are invalid or duplicated.") : error; emit stateChanged(); return false;
        }
        seen.insert(definition.component);
        auto it = std::find_if(candidate.entries.begin(), candidate.entries.end(), [&](const auto &e) { return e.component == definition.component; });
        if (it != candidate.entries.end()) {
            if (!it->command || !it->editable || it->pendingRemoval) { m_error = tr("An imported command conflicts with a read-only, removed, or application component."); emit stateChanged(); return false; }
            it->name = definition.name; it->exec = normalized;
            if (registerExisting && !it->registered) it->pendingAddition = true;
        } else {
            if (candidate.application(definition.component)) { m_error = tr("The imported command identity belongs to an installed application."); emit stateChanged(); return false; }
            CustomShortcutEntry entry;
            entry.component = definition.component; entry.name = definition.name; entry.exec = normalized; entry.command = true;
            entry.pendingAddition = entry.editable = entry.removable = true; candidate.entries << entry;
        }
    }
    return stage(candidate);
}
bool CustomShortcutsSettings::stageEditCommand(const QString &component, const QString &name, const QString &exec)
{
    if (!m_staged.find(component)) { m_error = tr("This command is unavailable."); emit stateChanged(); return false; }
    CustomShortcutEntry definition; definition.component = component; definition.name = name; definition.exec = exec;
    return stageCommands({definition});
}
bool CustomShortcutsSettings::stageRemove(const QString &component)
{
    auto candidate = m_staged;
    auto it = std::find_if(candidate.entries.begin(), candidate.entries.end(), [&](const auto &e) { return e.component == component; });
    if (it == candidate.entries.end() || !it->removable) { m_error = tr("This component cannot be removed safely."); emit stateChanged(); return false; }
    if (!m_applied.find(component)) candidate.entries.erase(it);
    else { it->pendingRemoval = true; it->pendingAddition = false; }
    return stage(candidate);
}
bool CustomShortcutsSettings::stageRestore(const QString &component)
{
    auto candidate = m_staged;
    for (auto &entry : candidate.entries) if (entry.component == component) { entry.pendingRemoval = false; return stage(candidate); }
    return false;
}
void CustomShortcutsSettings::read()
{ m_phase = Phase::Reading; m_busy = true; m_pendingRefresh = false; emit stateChanged(); m_backend->readCurrent(++m_token); }
void CustomShortcutsSettings::refresh()
{ if (m_busy) m_pendingRefresh = true; else read(); }
void CustomShortcutsSettings::revert()
{ if (!m_busy) { m_reverting = true; read(); } }
void CustomShortcutsSettings::apply()
{
    if (m_busy) return;
    QString error; const auto requested = CustomShortcutsTypes::changes(m_applied, m_staged);
    if (!canApply() || !CustomShortcutsTypes::validate(m_applied, requested, error)) {
        m_error = error.isEmpty() ? tr("Application shortcut state changed or is locked. Revert before applying.") : error;
        emit stateChanged(); emit applyFinished(false); return;
    }
    if (requested.isEmpty()) { emit applyFinished(true); return; }
    m_expected = m_applied; m_requested = requested; m_phase = Phase::Writing; m_busy = true;
    m_error.clear(); m_result.clear(); emit stateChanged(); m_backend->writeChanges(++m_token, m_expected, requested);
}
void CustomShortcutsSettings::onRead(quint64 token, bool success, const CustomShortcutsSnapshot &snapshot, const QString &error)
{
    if (m_phase != Phase::Reading || token != m_token) return;
    m_phase = Phase::Idle; m_busy = false;
    if (success) {
        const bool changed = m_ready && m_applied.revision != snapshot.revision;
        if (!snapshot.owner.isEmpty() && snapshot.owner != m_lockedOwner) m_lockedOwner.clear();
        m_applied = snapshot; m_ready = snapshot.available;
        if (m_reverting || !m_dirty) {
            m_staged = snapshot; m_dirty = m_conflict = false;
            if (m_reverting) { m_error.clear(); m_result.clear(); m_keepError = false; }
        } else if (changed) { m_conflict = true; m_error = tr("Application shortcuts changed externally. Your draft is preserved; Revert reloads them."); }
        if (!m_conflict && !m_keepError && m_lockedOwner.isEmpty()) m_error.clear();
        if (m_lockedOwner == snapshot.owner && !m_lockedOwner.isEmpty()) m_error = tr("A native shortcut mutation is uncertain. Further writes require a new shortcut service owner.");
    } else { m_ready = false; if (m_dirty) m_conflict = true; m_error = error.isEmpty() ? tr("Application shortcuts could not be read.") : error; }
    m_reverting = false; emit stateChanged();
    if (m_pendingRefresh) QTimer::singleShot(0, this, &CustomShortcutsSettings::refresh);
}
void CustomShortcutsSettings::onWritten(quint64 token, ShortcutsOutcome outcome, const CustomShortcutsSnapshot &snapshot, const QString &error)
{
    if (m_phase != Phase::Writing || token != m_token) return;
    m_phase = Phase::Idle; m_busy = false;
    const bool success = outcome == ShortcutsOutcome::Success && fulfilled(m_expected, m_requested, snapshot);
    if (snapshot.available) { m_applied = snapshot; m_ready = true; } else m_ready = false;
    if (success) {
        m_staged = snapshot; m_dirty = m_conflict = m_keepError = false; m_error.clear();
        m_result = tr("Native definitions and component state were read back. Assign keys in Global Shortcuts. Edited commands may require a shortcut service or session restart to be adopted.");
    } else {
        // A false/partial result can match the requested values while changing
        // context topology. Keep the reviewed draft pending until explicit Revert.
        m_dirty = !m_requested.isEmpty() || !CustomShortcutsTypes::changes(m_applied, m_staged).isEmpty(); m_conflict = m_keepError = true;
        if (outcome == ShortcutsOutcome::Uncertain || outcome == ShortcutsOutcome::Success) m_lockedOwner = m_expected.owner;
        m_error = error.isEmpty() ? tr("Application shortcut changes were not fully verified. Some changes may be saved. Your draft remains staged; Revert reloads current state.") : error;
        m_result.clear();
    }
    emit stateChanged(); emit applyFinished(success);
    if (m_pendingRefresh) QTimer::singleShot(0, this, &CustomShortcutsSettings::refresh);
}
