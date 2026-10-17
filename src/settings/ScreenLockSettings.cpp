#include "ScreenLockSettings.h"
#include "platform/kde/ScreenLockBackend.h"
#include <QCoreApplication>
#include <QPointer>
#include <QTimer>
#include <cmath>
#include <cstring>
#include <limits>

namespace {
bool sameVariant(const QVariant &a, const QVariant &b)
{
    if (a.metaType() != b.metaType()) return false;
    if (a.metaType().id() == QMetaType::Double && std::isnan(a.toDouble()) && std::isnan(b.toDouble())) {
        const double av = a.toDouble(), bv = b.toDouble(); return std::memcmp(&av, &bv, sizeof(double)) == 0;
    }
    return a == b;
}
bool sameCapability(const ScreenLockRuntime &a, const ScreenLockRuntime &b)
{
    return a.owner == b.owner && a.epoch == b.epoch && a.version == b.version
        && a.installedVerified == b.installedVerified && a.serviceAvailable == b.serviceAvailable && a.providerVerified == b.providerVerified;
}
bool usable(const ScreenLockRuntime &r)
{ return r.installedVerified && (!r.serviceAvailable || r.providerVerified && !r.owner.isEmpty()); }
}
bool ScreenLockField::operator==(const ScreenLockField &o) const
{
    return sameVariant(value, o.value) && sameVariant(defaultValue, o.defaultValue)
        && known == o.known && defaultKnown == o.defaultKnown && defaultNoOp == o.defaultNoOp
        && effectivePresent == o.effectivePresent && localPresent == o.localPresent
        && inheritedPresent == o.inheritedPresent && globalPresent == o.globalPresent
        && immutable == o.immutable && writable == o.writable && resettable == o.resettable
        && reason == o.reason && resetReason == o.resetReason && revision == o.revision
        && inheritedRevision == o.inheritedRevision && globalRevision == o.globalRevision;
}
QList<ScreenLockKey> ScreenLockTypes::keys()
{ return {ScreenLockKey::Autolock, ScreenLockKey::Timeout, ScreenLockKey::Lock, ScreenLockKey::LockGrace,
    ScreenLockKey::RequirePassword, ScreenLockKey::LockOnResume, ScreenLockKey::LockOnStart, ScreenLockKey::Theme, ScreenLockKey::WallpaperPlugin}; }
QList<ScreenLockKey> ScreenLockTypes::ownedKeys()
{ return {ScreenLockKey::Autolock, ScreenLockKey::Timeout, ScreenLockKey::LockGrace, ScreenLockKey::RequirePassword, ScreenLockKey::LockOnResume}; }
bool ScreenLockTypes::owned(ScreenLockKey key) { return ownedKeys().contains(key); }
QString ScreenLockTypes::groupName(ScreenLockKey key)
{ return key == ScreenLockKey::Theme || key == ScreenLockKey::WallpaperPlugin ? QStringLiteral("Greeter") : QStringLiteral("Daemon"); }
QString ScreenLockTypes::keyName(ScreenLockKey key)
{
    switch (key) {
    case ScreenLockKey::Autolock: return QStringLiteral("Autolock");
    case ScreenLockKey::Timeout: return QStringLiteral("Timeout");
    case ScreenLockKey::Lock: return QStringLiteral("Lock");
    case ScreenLockKey::LockGrace: return QStringLiteral("LockGrace");
    case ScreenLockKey::RequirePassword: return QStringLiteral("RequirePassword");
    case ScreenLockKey::LockOnResume: return QStringLiteral("LockOnResume");
    case ScreenLockKey::LockOnStart: return QStringLiteral("LockOnStart");
    case ScreenLockKey::Theme: return QStringLiteral("Theme");
    case ScreenLockKey::WallpaperPlugin: return QStringLiteral("WallpaperPlugin");
    } return {};
}
QString ScreenLockTypes::label(ScreenLockKey key)
{
    switch (key) {
    case ScreenLockKey::Autolock: return QObject::tr("Automatic locking");
    case ScreenLockKey::Timeout: return QObject::tr("Idle timeout");
    case ScreenLockKey::LockGrace: return QObject::tr("Password grace period");
    case ScreenLockKey::RequirePassword: return QObject::tr("Require a password");
    case ScreenLockKey::LockOnResume: return QObject::tr("Lock after sleep");
    default: return keyName(key);
    }
}
QVariant ScreenLockTypes::builtinDefault(ScreenLockKey key)
{
    switch (key) {
    case ScreenLockKey::Autolock: case ScreenLockKey::Lock: case ScreenLockKey::RequirePassword: case ScreenLockKey::LockOnResume: return true;
    case ScreenLockKey::LockOnStart: return false;
    case ScreenLockKey::Timeout: return 5.0;
    case ScreenLockKey::LockGrace: return 5;
    case ScreenLockKey::Theme: return QString();
    case ScreenLockKey::WallpaperPlugin: return QStringLiteral("org.kde.image");
    } return {};
}
bool ScreenLockTypes::nativeValue(ScreenLockKey key, const QVariant &value)
{
    switch (key) {
    case ScreenLockKey::Autolock: case ScreenLockKey::Lock: case ScreenLockKey::RequirePassword:
    case ScreenLockKey::LockOnResume: case ScreenLockKey::LockOnStart: return value.metaType().id() == QMetaType::Bool;
    case ScreenLockKey::Timeout: return value.metaType().id() == QMetaType::Double;
    case ScreenLockKey::LockGrace: return value.metaType().id() == QMetaType::Int;
    case ScreenLockKey::Theme: case ScreenLockKey::WallpaperPlugin: return value.metaType().id() == QMetaType::QString;
    } return false;
}
bool ScreenLockTypes::validValue(ScreenLockKey key, const QVariant &value)
{
    if (!owned(key) || !nativeValue(key, value)) return false;
    if (key == ScreenLockKey::Timeout) return std::isfinite(value.toDouble()) && value.toDouble() >= 0 && value.toDouble() <= 9999 && std::floor(value.toDouble()) == value.toDouble();
    if (key == ScreenLockKey::LockGrace) return value.toInt() >= 0 && value.toInt() <= 599940;
    return true;
}
bool ScreenLockTypes::sameConfiguration(const ScreenLockSnapshot &a, const ScreenLockSnapshot &b)
{ return a.fields == b.fields && a.revision == b.revision && sameCapability(a.runtime, b.runtime); }
bool ScreenLockTypes::sameDraft(const ScreenLockSnapshot &a, const ScreenLockSnapshot &b)
{ return sameConfiguration(a, b) && a.intents == b.intents; }
bool ScreenLockTypes::stage(ScreenLockSnapshot &draft, const ScreenLockSnapshot &baseline, ScreenLockKey key, const QVariant &value, QString &error)
{
    error.clear();
    if (!baseline.fields.contains(key) || !draft.fields.contains(key) || !validValue(key, value)) {
        error = QObject::tr("Choose a supported Screen Locking value."); return false;
    }
    const auto &saved = baseline.fields[key];
    if (!saved.writable || saved.immutable) { error = saved.reason.isEmpty() ? QObject::tr("This preference cannot be changed.") : saved.reason; return false; }
    if (saved.known && saved.localPresent && sameVariant(saved.value, value)) { draft.fields[key] = saved; draft.intents.remove(key); return true; }
    draft.fields[key].value = value; draft.fields[key].known = true; draft.intents[key] = ScreenLockIntent::Set; return true;
}
bool ScreenLockTypes::restore(ScreenLockSnapshot &draft, const ScreenLockSnapshot &baseline, ScreenLockKey key, QString &error)
{
    error.clear();
    if (!owned(key) || !baseline.fields.contains(key) || !draft.fields.contains(key)) { error = QObject::tr("This preference is not owned by this page."); return false; }
    const auto &saved = baseline.fields[key];
    if (!saved.known || !saved.defaultKnown || !nativeValue(key, saved.defaultValue)) { error = QObject::tr("The saved value or native default is unsupported. Choose an explicit supported value."); return false; }
    if (saved.defaultNoOp || (saved.value.metaType() == saved.defaultValue.metaType() && saved.value == saved.defaultValue)) { draft.fields[key] = saved; draft.intents.remove(key); return true; }
    if (!saved.writable || saved.immutable || !saved.resettable) { error = saved.resetReason.isEmpty() ? QObject::tr("This native default cannot be restored.") : saved.resetReason; return false; }
    draft.fields[key].value = saved.defaultValue; draft.fields[key].known = true; draft.intents[key] = ScreenLockIntent::RestoreDefault; return true;
}
bool ScreenLockTypes::selectTimeout(ScreenLockSnapshot &draft, const ScreenLockSnapshot &baseline, double minutes, QString &error)
{
    auto next = draft;
    if (!stage(next, baseline, ScreenLockKey::Timeout, minutes, error)) return false;
    const auto original = baseline.fields.value(ScreenLockKey::Autolock);
    if (original.known && nativeValue(ScreenLockKey::Autolock, original.value) && original.value.toBool() == (minutes != 0)) {
        next.fields[ScreenLockKey::Autolock] = original; next.intents.remove(ScreenLockKey::Autolock);
    }
    const auto a = next.fields.value(ScreenLockKey::Autolock);
    if ((!a.known || !nativeValue(ScreenLockKey::Autolock, a.value) || a.value.toBool() != (minutes != 0))
        && !stage(next, baseline, ScreenLockKey::Autolock, minutes != 0, error)) return false;
    draft = next; return true;
}
bool ScreenLockTypes::selectGrace(ScreenLockSnapshot &draft, const ScreenLockSnapshot &baseline, int seconds, QString &error)
{
    auto next = draft;
    if (!stage(next, baseline, ScreenLockKey::LockGrace, seconds, error)) return false;
    const auto original = baseline.fields.value(ScreenLockKey::RequirePassword);
    if (original.known && nativeValue(ScreenLockKey::RequirePassword, original.value) && original.value.toBool()) {
        next.fields[ScreenLockKey::RequirePassword] = original; next.intents.remove(ScreenLockKey::RequirePassword);
    }
    const auto p = next.fields.value(ScreenLockKey::RequirePassword);
    if ((!p.known || !nativeValue(ScreenLockKey::RequirePassword, p.value) || !p.value.toBool())
        && !stage(next, baseline, ScreenLockKey::RequirePassword, true, error)) return false;
    draft = next; return true;
}
bool ScreenLockTypes::selectNoPassword(ScreenLockSnapshot &draft, const ScreenLockSnapshot &baseline, QString &error)
{ return stage(draft, baseline, ScreenLockKey::RequirePassword, false, error); }
bool ScreenLockTypes::activeBranchValid(const QMap<ScreenLockKey, ScreenLockField> &fields, QString &error)
{
    error.clear();
    const auto valid = [&](ScreenLockKey key) { const auto f = fields.value(key); return f.known && nativeValue(key, f.value); };
    for (auto key : {ScreenLockKey::Autolock, ScreenLockKey::Lock, ScreenLockKey::RequirePassword, ScreenLockKey::LockOnResume}) {
        if (!valid(key)) { error = QObject::tr("The native %1 policy is unknown. Reload is unavailable until it is supported.").arg(label(key)); return false; }
    }
    if (fields[ScreenLockKey::Autolock].value.toBool()) {
        if (!valid(ScreenLockKey::Timeout)) { error = QObject::tr("Choose a finite idle timeout before Apply."); return false; }
        const double minutes = fields[ScreenLockKey::Timeout].value.toDouble();
        if (minutes > 0) {
            // KIdleTime 6.24 narrows duration_cast<milliseconds>().count() to int.
            // A positive fractional millisecond safely truncates to zero.
            const double ms = minutes * 60000.0;
            if (!std::isfinite(ms) || ms >= double(std::numeric_limits<int>::max()) + 1.0) {
                error = QObject::tr("The resulting idle timeout exceeds KDE's supported millisecond range. Choose a shorter timeout."); return false;
            }
        }
    }
    if (fields[ScreenLockKey::Lock].value.toBool()) {
        if (!valid(ScreenLockKey::LockGrace)) { error = QObject::tr("Choose a supported password grace period before Apply."); return false; }
        const qint64 ms = qint64(fields[ScreenLockKey::LockGrace].value.toInt()) * 1000;
        if (ms < std::numeric_limits<int>::min() || ms > std::numeric_limits<int>::max()) {
            error = QObject::tr("The saved password grace period would overflow KDE's timer. Choose a supported grace period."); return false;
        }
    }
    return true;
}
bool ScreenLockTypes::requestValid(const ScreenLockSnapshot &expected, const ScreenLockSnapshot &requested, QString &error)
{
    error.clear();
    if (!usable(expected.runtime) || !sameCapability(expected.runtime, requested.runtime)) { error = QObject::tr("The installed locker contract or running service could not be verified."); return false; }
    if (!expected.intents.isEmpty() || expected.fields.size() != keys().size()) { error = QObject::tr("The native locker snapshot is incomplete."); return false; }
    for (auto key : keys()) if (!expected.fields.contains(key)) { error = QObject::tr("The native locker snapshot is incomplete."); return false; }
    auto rebuilt = expected; rebuilt.intents.clear();
    for (auto i = requested.intents.cbegin(); i != requested.intents.cend(); ++i) {
        const bool ok = i.value() == ScreenLockIntent::Set ? stage(rebuilt, expected, i.key(), requested.fields.value(i.key()).value, error)
            : i.value() == ScreenLockIntent::RestoreDefault && restore(rebuilt, expected, i.key(), error);
        if (!ok) { if (error.isEmpty()) error = QObject::tr("An invalid Screen Locking operation was requested."); return false; }
    }
    if (!sameDraft(rebuilt, requested)) { error = QObject::tr("The request changed unowned preferences or capability metadata."); return false; }
    return activeBranchValid(requested.fields, error);
}
bool ScreenLockTypes::matchesRequest(const ScreenLockSnapshot &expected, const ScreenLockSnapshot &requested, const ScreenLockSnapshot &actual, QString &error)
{
    if (!requestValid(expected, requested, error)) return false;
    if (!sameCapability(expected.runtime, actual.runtime) || !actual.intents.isEmpty() || actual.fields.size() != expected.fields.size()) {
        error = QObject::tr("Locker identity or capabilities changed during Apply."); return false;
    }
    for (auto key : keys()) {
        if (!actual.fields.contains(key) || !expected.fields.contains(key)) { error = QObject::tr("The saved locker configuration is incomplete."); return false; }
        const auto before = expected.fields[key], after = actual.fields[key];
        if (!requested.intents.contains(key)) {
            if (!(before == after)) { error = QObject::tr("An untouched locker preference changed during Apply."); return false; }
            continue;
        }
        if (before.defaultKnown != after.defaultKnown || !sameVariant(before.defaultValue, after.defaultValue)
            || before.inheritedPresent != after.inheritedPresent || before.inheritedRevision != after.inheritedRevision
            || before.immutable != after.immutable || before.writable != after.writable) {
            error = QObject::tr("Locker defaults or policy changed during Apply."); return false;
        }
        const bool explicitValue = requested.intents[key] == ScreenLockIntent::Set || before.inheritedPresent;
        if (explicitValue) {
            if (!after.known || !after.localPresent || !after.effectivePresent || !sameVariant(after.value, requested.fields[key].value)
                || before.globalPresent != after.globalPresent || before.globalRevision != after.globalRevision) {
                error = QObject::tr("The explicit locker preference could not be verified after saving."); return false;
            }
        } else if (!after.known || !sameVariant(after.value, requested.fields[key].value) || after.localPresent || after.effectivePresent
                   || (before.localPresent ? before.globalPresent != after.globalPresent || before.globalRevision != after.globalRevision : after.globalPresent)) {
            error = QObject::tr("The locker override could not be verified as removed."); return false;
        }
    }
    return true;
}

ScreenLockSettings *ScreenLockSettings::instance()
{
    static QPointer<ScreenLockSettings> session;
    if (!session) session = new ScreenLockSettings(new KdeScreenLockBackend, QCoreApplication::instance());
    return session;
}
ScreenLockSettings::ScreenLockSettings(ScreenLockBackend *backend, QObject *parent)
    : SettingsSession(parent), m_backend(backend)
{
    Q_ASSERT(backend); backend->setParent(this);
    connect(backend, &ScreenLockBackend::readFinished, this, &ScreenLockSettings::onRead);
    connect(backend, &ScreenLockBackend::writeFinished, this, &ScreenLockSettings::onWritten);
    connect(backend, &ScreenLockBackend::operationPending, this, [this](quint64 token, const QString &message) {
        if (token != m_token || m_phase != Phase::Write) return;
        m_pendingMessage = message;
        emit stateChanged();
    });
    connect(backend, &ScreenLockBackend::externalChange, this, &ScreenLockSettings::refresh);
    m_busy = true; QTimer::singleShot(0, this, [this] { beginRead(Phase::Refresh); });
}
bool ScreenLockSettings::canEdit(ScreenLockKey key) const
{
    return m_ready && !m_busy && !m_conflict && !m_uncertain && usable(m_runtime)
        && ScreenLockTypes::owned(key) && m_applied.fields.contains(key) && m_applied.fields[key].writable && !m_applied.fields[key].immutable
;
}
QString ScreenLockSettings::applySupportText() const
{
    if (m_busy) return m_pendingMessage.isEmpty() ? tr("Screen Locking preferences are being checked or saved.") : m_pendingMessage;
    if (m_conflict || m_uncertain) return tr("Screen Locking changed or the last result is uncertain. Revert successfully before applying.");
    if (!m_ready || !usable(m_runtime)) return m_runtime.reason.isEmpty() ? tr("The installed Screen Locking contract is unavailable.") : m_runtime.reason;
    QString error;
    if (!ScreenLockTypes::requestValid(m_applied, m_staged, error)) return error;
    return {};
}
bool ScreenLockSettings::canApply() const { return applySupportText().isEmpty(); }
void ScreenLockSettings::edited()
{
    m_dirty = !m_staged.intents.isEmpty(); m_error.clear(); m_result.clear(); emit stateChanged();
}
void ScreenLockSettings::selectValue(ScreenLockKey key, const QVariant &value)
{
    if (!canEdit(key)) return;
    QString error;
    if (!ScreenLockTypes::stage(m_staged, m_applied, key, value, error)) { m_error = error; emit stateChanged(); return; }
    edited();
}
void ScreenLockSettings::selectTimeout(double minutes)
{
    if (!canEdit(ScreenLockKey::Timeout)) return;
    QString error;
    if (!ScreenLockTypes::selectTimeout(m_staged, m_applied, minutes, error)) { m_error = error; emit stateChanged(); return; }
    edited();
}
void ScreenLockSettings::selectGrace(int seconds)
{
    if (!canEdit(ScreenLockKey::LockGrace)) return;
    QString error;
    if (!ScreenLockTypes::selectGrace(m_staged, m_applied, seconds, error)) { m_error = error; emit stateChanged(); return; }
    edited();
}
void ScreenLockSettings::selectNoPassword() { selectValue(ScreenLockKey::RequirePassword, false); }
void ScreenLockSettings::restoreDefault(ScreenLockKey key)
{
    if (!canEdit(key)) return;
    QString error;
    if (!ScreenLockTypes::restore(m_staged, m_applied, key, error)) { m_error = error; emit stateChanged(); return; }
    edited();
}
QStringList ScreenLockSettings::defaultsChanges() const
{
    QStringList result; auto draft = m_staged;
    for (auto key : ScreenLockTypes::ownedKeys()) {
        const auto before = draft; QString error;
        if (ScreenLockTypes::restore(draft, m_applied, key, error) && !ScreenLockTypes::sameDraft(before, draft)) result << ScreenLockTypes::label(key);
    }
    return result;
}
QStringList ScreenLockSettings::defaultsUnavailable() const
{
    QStringList result; auto draft = m_staged;
    for (auto key : ScreenLockTypes::ownedKeys()) {
        QString error;
        if (!ScreenLockTypes::restore(draft, m_applied, key, error)) result << tr("%1: %2").arg(ScreenLockTypes::label(key), error);
    }
    return result;
}
void ScreenLockSettings::restoreDefaults()
{
    if (!m_ready || m_busy || m_conflict || m_uncertain || !usable(m_runtime)) return;
    auto draft = m_staged; QStringList unavailable;
    for (auto key : ScreenLockTypes::ownedKeys()) {
        QString error;
        if (!ScreenLockTypes::restore(draft, m_applied, key, error)) unavailable << tr("%1: %2").arg(ScreenLockTypes::label(key), error);
    }
    m_staged = draft; QPointer<ScreenLockSettings> guard(this); edited();
    if (guard && !unavailable.isEmpty()) { m_error = unavailable.join(QLatin1Char('\n')); emit stateChanged(); }
}
void ScreenLockSettings::beginRead(Phase phase)
{
    m_phase = phase; m_busy = true; m_pendingRefresh = false; m_pendingMessage.clear();
    QPointer<ScreenLockSettings> guard(this); emit stateChanged();
    if (guard) m_backend->readCurrent(++m_token);
}
void ScreenLockSettings::refresh()
{
    if (m_busy) { m_pendingRefresh = true; return; }
    beginRead(Phase::Refresh);
}
void ScreenLockSettings::revert()
{
    if (m_busy) return;
    beginRead(Phase::Revert);
}
void ScreenLockSettings::apply()
{
    if (m_busy) return;
    const auto reason = applySupportText();
    if (!reason.isEmpty()) { m_error = reason; QPointer<ScreenLockSettings> guard(this); emit stateChanged(); if (guard) emit applyFinished(false); return; }
    if (!m_dirty) { emit applyFinished(true); return; }
    m_requested = m_staged; m_expected = m_applied; m_error.clear(); m_result.clear(); beginRead(Phase::Preflight);
}
void ScreenLockSettings::onRead(quint64 token, bool success, const ScreenLockSnapshot &snapshot, const QString &error)
{
    if (token != m_token || (m_phase != Phase::Refresh && m_phase != Phase::Revert
        && m_phase != Phase::Preflight && m_phase != Phase::Readback)) return;
    const auto phase = m_phase;
    if (success) {
        m_runtime = snapshot.runtime; m_ready = true;
    }
    else { m_ready = false; m_runtime.providerVerified = false; m_runtime.installedVerified = false; m_runtime.reason = error; }
    if (phase == Phase::Preflight) {
        QString validation;
        if (success && ScreenLockTypes::sameConfiguration(m_expected, snapshot)
            && ScreenLockTypes::requestValid(snapshot, m_requested, validation)) {
            m_phase = Phase::Write; m_backend->writeChanges(++m_token, m_expected, m_requested); return;
        }
        m_conflict = true; m_error = !success ? error : validation.isEmpty()
            ? tr("Screen Locking preferences, policy or service changed before Apply. Revert before retrying.") : validation;
        finish(true, false); return;
    }
    if (phase == Phase::Readback) {
        QString verification;
        const bool matches = success && ScreenLockTypes::matchesRequest(m_expected, m_requested, snapshot, verification);
        const bool applied = m_saved && (!m_expected.runtime.serviceAvailable || m_acknowledged) && m_writeError.isEmpty() && matches;
        if (applied) {
            m_applied = m_staged = snapshot; m_dirty = m_conflict = m_uncertain = false; m_error.clear();
            m_result = snapshot.runtime.serviceAvailable
                ? tr("Saved Screen Locking preferences verified. KDE acknowledged the reload request. Physical locking and password behavior have not been tested.")
                : tr("Saved Screen Locking preferences verified. The locker is not running; reload is deferred and loaded behavior is unverified. No service was started.");
        } else {
            m_conflict = m_uncertain = true;
            m_error = !m_writeError.isEmpty() ? m_writeError : !success ? error : !matches ? verification
                : !m_acknowledged ? tr("Preferences may be saved, but the locker reload was not acknowledged. Revert before retrying.")
                : tr("The Screen Locking result is uncertain. Revert before retrying.");
            m_result.clear();
        }
        finish(true, applied); return;
    }
    if (success && (phase == Phase::Revert || !m_dirty && !m_uncertain && !m_conflict)) {
        m_applied = m_staged = snapshot; m_dirty = m_conflict = m_uncertain = false; m_error.clear(); m_result.clear();
    } else if (success) {
        if (!ScreenLockTypes::sameConfiguration(m_applied, snapshot)) {
            m_conflict = true; m_error = tr("Screen Locking preferences, policy or service changed outside Control Panel. Your draft is retained; Revert before applying.");
        }
    } else {
        if (m_dirty || m_uncertain || phase == Phase::Revert) m_conflict = true;
        m_error = error.isEmpty() ? tr("Screen Locking settings could not be read. The draft is retained.") : error;
    }
    finish();
}
void ScreenLockSettings::onWritten(quint64 token, bool saved, bool acknowledged, const QString &error)
{
    if (token != m_token || m_phase != Phase::Write) return;
    m_saved = saved; m_acknowledged = acknowledged; m_writeError = error; beginRead(Phase::Readback);
}
void ScreenLockSettings::finish(bool applying, bool success)
{
    m_busy = false; m_phase = Phase::Idle;
    QPointer<ScreenLockSettings> guard(this); emit stateChanged();
    if (!guard) return;
    if (applying) emit applyFinished(success);
    if (guard && m_pendingRefresh) QTimer::singleShot(0, this, [this] { if (!m_busy && m_pendingRefresh) beginRead(Phase::Refresh); });
}
