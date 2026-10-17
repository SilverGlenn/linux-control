#include "ShortcutsSettings.h"
#include "platform/shortcuts/ShortcutsBackend.h"
#include "platform/shortcuts/StandardShortcutsBackend.h"
#include <QCoreApplication>
#include <QPointer>
#include <QTimer>
#include <algorithm>

namespace {
bool sharesScope(const ShortcutActionId &a, const ShortcutActionId &b)
{ return a.component != b.component || a.context == b.context; }
ShortcutChanges changes(const ShortcutsSnapshot &before, const ShortcutsSnapshot &after)
{
    ShortcutChanges result;
    for (const auto &action : after.actions) {
        const auto *previous = before.find(action.id);
        if (!previous || !ShortcutsTypes::sameKeys(previous->current, action.current)) result.insert(action.id, action.current);
    }
    return result;
}
bool sameSnapshot(const ShortcutsSnapshot &a, const ShortcutsSnapshot &b)
{
    return a.owner == b.owner && a.interfaceSignature == b.interfaceSignature
        && a.available == b.available && a.writable == b.writable && a.revision == b.revision
        && ShortcutsTypes::revision(a) == ShortcutsTypes::revision(b);
}
bool verified(const ShortcutsSnapshot &expected, const ShortcutChanges &requested, const ShortcutsSnapshot &actual)
{
    if (!actual.available || !actual.writable || actual.owner != expected.owner
        || actual.interfaceSignature != expected.interfaceSignature || actual.actions.size() != expected.actions.size()
        || !(actual.layouts == expected.layouts)) return false;
    for (const auto &before : expected.actions) {
        const auto *after = actual.find(before.id);
        const bool registered = before.registered || (before.layoutAction && !requested.value(before.id, before.current).isEmpty());
        if (!after || after->available != before.available || after->registered != registered
            || after->layoutAction != before.layoutAction || !ShortcutsTypes::sameKeys(before.defaults, after->defaults)
            || !ShortcutsTypes::sameKeys(requested.value(before.id, before.current), after->current)) return false;
    }
    return true;
}
}

ShortcutsSettings *ShortcutsSettings::instance()
{
    static QPointer<ShortcutsSettings> session;
    if (!session) session = new ShortcutsSettings(new KdeShortcutsBackend, QCoreApplication::instance());
    return session;
}
ShortcutsSettings *ShortcutsSettings::standardInstance()
{
    static QPointer<ShortcutsSettings> session;
    if (!session) session = new ShortcutsSettings(new KdeStandardShortcutsBackend, QCoreApplication::instance());
    return session;
}
ShortcutsSettings::ShortcutsSettings(ShortcutsBackend *backend, QObject *parent)
    : SettingsSession(parent), m_backend(backend)
{
    Q_ASSERT(backend); backend->setParent(this);
    qRegisterMetaType<ShortcutActionId>(); qRegisterMetaType<ShortcutsSnapshot>(); qRegisterMetaType<ShortcutsOutcome>();
    connect(backend, &ShortcutsBackend::readFinished, this, &ShortcutsSettings::onRead);
    connect(backend, &ShortcutsBackend::writeFinished, this, &ShortcutsSettings::onWritten);
    connect(backend, &ShortcutsBackend::externalChange, this, &ShortcutsSettings::refresh);
    m_busy = true; QTimer::singleShot(0, this, &ShortcutsSettings::read);
}
bool ShortcutsSettings::canApply() const
{ return m_ready && m_applied.writable && !m_conflict && (m_lockedOwner.isEmpty() || m_lockedOwner != m_applied.owner); }
QList<ShortcutConflict> ShortcutsSettings::conflictsFor(const ShortcutActionId &id, const QList<QKeySequence> &keys) const
{
    QList<ShortcutConflict> result;
    for (const auto &action : m_staged.actions) {
        if (action.id == id || !sharesScope(id, action.id)) continue;
        for (const auto &existing : action.current)
            if (!existing.isEmpty() && std::any_of(keys.begin(), keys.end(), [&](const auto &key) { return !key.isEmpty() && ShortcutsTypes::conflicts(key, existing); }))
                result.append({action.id, action.friendlyName.isEmpty() ? action.id.action : action.friendlyName, existing});
    }
    return result;
}
bool ShortcutsSettings::stage(const ShortcutsSnapshot &candidate)
{
    if (m_busy || !canApply()) return false;
    QString error;
    if (!ShortcutsTypes::validate(m_applied, changes(m_applied, candidate), error)) {
        m_error = error; emit stateChanged(); return false;
    }
    commitCandidate(candidate); emit stateChanged(); return true;
}
bool ShortcutsSettings::selectShortcuts(const ShortcutActionId &id, const QList<QKeySequence> &keys, bool reassign)
{ return selectChanges({{id, keys}}, reassign); }
void ShortcutsSettings::commitCandidate(const ShortcutsSnapshot &candidate)
{
    m_staged = candidate; m_dirty = !changes(m_applied, m_staged).isEmpty();
    m_error.clear(); m_result.clear(); m_keepError = false;
}
bool ShortcutsSettings::prepareChanges(const ShortcutChanges &requested, bool reassign, ShortcutsSnapshot &candidate, QString &error) const
{
    if (m_busy || !canApply()) {
        error = m_busy ? tr("Wait for shortcut work to finish before importing changes.")
            : m_conflict ? tr("Shortcuts changed outside Control Panel. Revert before importing changes.")
            : !m_applied.reason.isEmpty() ? m_applied.reason : tr("Shortcut bindings cannot be changed at this time.");
        return false;
    }
    candidate = m_staged;
    for (auto it = requested.cbegin(); it != requested.cend(); ++it) {
        auto found = std::find_if(candidate.actions.begin(), candidate.actions.end(), [&](const auto &a) { return a.id == it.key(); });
        if (found == candidate.actions.end() || (!found->available && !ShortcutsTypes::sameKeys(found->current, it.value()))) {
            error = tr("This action is read-only or unavailable and cannot be changed."); return false;
        }
        found->current = it.value();
    }
    if (reassign) {
        for (auto &action : candidate.actions) {
            if (requested.contains(action.id)) continue;
            action.current.erase(std::remove_if(action.current.begin(), action.current.end(), [&](const auto &existing) {
                if (existing.isEmpty()) return false;
                for (auto it = requested.cbegin(); it != requested.cend(); ++it)
                    if (sharesScope(it.key(), action.id) && std::any_of(it.value().begin(), it.value().end(), [&](const auto &key) {
                        return !key.isEmpty() && ShortcutsTypes::conflicts(key, existing);
                    })) return true;
                return false;
            }), action.current.end());
        }
    }
    // Validate the complete final map. Selected imported targets never steal
    // from one another; only unselected alternatives can be reassigned.
    return ShortcutsTypes::validate(m_applied, changes(m_applied, candidate), error);
}
bool ShortcutsSettings::validateChanges(const ShortcutChanges &requested, bool reassign, QString &error) const
{ ShortcutsSnapshot candidate; return prepareChanges(requested, reassign, candidate, error); }
bool ShortcutsSettings::selectChanges(const ShortcutChanges &requested, bool reassign)
{
    ShortcutsSnapshot candidate; QString error;
    if (!prepareChanges(requested, reassign, candidate, error)) {
        m_error = error; emit stateChanged(); return false;
    }
    commitCandidate(candidate); emit stateChanged(); return true;
}
bool ShortcutsSettings::stageTogether(ShortcutsSettings *globals, const ShortcutChanges &globalChanges,
                                    ShortcutsSettings *standards, const ShortcutChanges &standardChanges,
                                    bool reassign, QString &error)
{
    if ((!globals && !globalChanges.isEmpty()) || (!standards && !standardChanges.isEmpty())
        || (globals && globals == standards)) {
        error = tr("The selected shortcut scopes require independent available sessions."); return false;
    }
    ShortcutsSnapshot globalCandidate, standardCandidate;
    if (!globalChanges.isEmpty() && !globals->prepareChanges(globalChanges, reassign, globalCandidate, error)) return false;
    if (!standardChanges.isEmpty() && !standards->prepareChanges(standardChanges, reassign, standardCandidate, error)) return false;
    // No signals, event-loop turns, or backend calls occur between commits.
    // Observers of either session see the complete two-scope draft.
    if (!globalChanges.isEmpty()) globals->commitCandidate(globalCandidate);
    if (!standardChanges.isEmpty()) standards->commitCandidate(standardCandidate);
    QPointer<ShortcutsSettings> standardGuard = standards;
    if (!globalChanges.isEmpty()) emit globals->stateChanged();
    if (!standardChanges.isEmpty() && standardGuard) emit standardGuard->stateChanged();
    return true;
}
void ShortcutsSettings::restoreDefaults(const ShortcutActionId &id)
{ if (const auto *action = m_staged.find(id)) selectShortcuts(id, action->defaults); }
void ShortcutsSettings::restoreAllDefaults()
{
    auto candidate = m_staged;
    for (auto &action : candidate.actions) if (action.available) action.current = action.defaults;
    stage(candidate);
}
void ShortcutsSettings::read()
{ m_phase = Phase::Reading; m_busy = true; m_pendingRefresh = false; emit stateChanged(); m_backend->readCurrent(++m_token); }
void ShortcutsSettings::refresh()
{ if (m_busy) m_pendingRefresh = true; else read(); }
void ShortcutsSettings::revert()
{
    if (m_busy) return;
    m_reverting = true; read();
}
void ShortcutsSettings::apply()
{
    if (m_busy) return;
    QString error; const auto requested = changes(m_applied, m_staged);
    if (!canApply() || !ShortcutsTypes::validate(m_applied, requested, error)) {
        m_error = !error.isEmpty() ? error : m_lockedOwner == m_applied.owner && !m_lockedOwner.isEmpty()
            ? tr("An earlier shortcut change is uncertain. Further writes are disabled until the shortcut service restarts.")
            : m_conflict ? tr("Shortcuts changed outside Control Panel. Revert to reload them before applying.") : m_applied.reason;
        emit stateChanged(); emit applyFinished(false); return;
    }
    if (!m_dirty) { emit applyFinished(true); return; }
    m_writeExpected = m_applied; m_requested = requested; m_phase = Phase::Writing; m_busy = true;
    m_error.clear(); m_result.clear(); emit stateChanged(); m_backend->writeChanges(++m_token, m_writeExpected, m_requested);
}
void ShortcutsSettings::onRead(quint64 token, bool success, const ShortcutsSnapshot &snapshot, const QString &error)
{
    if (m_phase != Phase::Reading || token != m_token) return;
    m_phase = Phase::Idle;
    if (success) {
        const bool changed = m_ready && !sameSnapshot(m_applied, snapshot);
        if (!snapshot.owner.isEmpty() && snapshot.owner != m_lockedOwner) m_lockedOwner.clear();
        m_applied = snapshot; m_ready = snapshot.available;
        if (m_reverting || !m_dirty) {
            m_staged = snapshot; m_dirty = m_conflict = false;
            if (m_reverting) { m_error.clear(); m_result.clear(); m_keepError = false; }
        } else if (changed) {
            m_conflict = true; m_error = tr("Shortcuts changed outside Control Panel. Your choices remain staged; Revert reloads them.");
        }
        if (!m_conflict && !m_keepError && m_lockedOwner.isEmpty()) m_error.clear();
        if (!m_lockedOwner.isEmpty() && m_lockedOwner == snapshot.owner)
            m_error = tr("An earlier shortcut change is uncertain. Further writes are disabled until the shortcut service restarts.");
    } else {
        m_ready = false; if (m_dirty) m_conflict = true;
        m_error = error.isEmpty() ? tr("Shortcut bindings could not be read.") : error;
    }
    m_reverting = false; m_busy = false; emit stateChanged();
    if (m_pendingRefresh) QTimer::singleShot(0, this, &ShortcutsSettings::refresh);
}
void ShortcutsSettings::onWritten(quint64 token, ShortcutsOutcome outcome, const ShortcutsSnapshot &snapshot, const QString &error)
{
    if (m_phase != Phase::Writing || token != m_token) return;
    m_phase = Phase::Idle; m_busy = false;
    const bool success = outcome == ShortcutsOutcome::Success && verified(m_writeExpected, m_requested, snapshot);
    if (snapshot.available) { m_applied = snapshot; m_ready = true; } else m_ready = false;
    if (success) {
        m_staged = m_applied; m_dirty = m_conflict = false; m_keepError = false; m_error.clear();
        m_result = tr("Shortcut bindings were saved and independently read back.");
    } else {
        m_dirty = !m_requested.isEmpty() || !changes(m_applied, m_staged).isEmpty(); m_conflict = true; m_keepError = true;
        if (outcome == ShortcutsOutcome::Uncertain || outcome == ShortcutsOutcome::Success) m_lockedOwner = m_writeExpected.owner;
        m_error = error.isEmpty() ? tr("Shortcut changes were not fully verified. Some bindings may have been saved. Your draft is preserved; Revert reloads current bindings.") : error;
        m_result.clear();
    }
    emit stateChanged(); emit applyFinished(success);
    if (m_pendingRefresh) QTimer::singleShot(0, this, &ShortcutsSettings::refresh);
}
