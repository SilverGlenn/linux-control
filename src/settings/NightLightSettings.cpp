#include "NightLightSettings.h"
#include "platform/kde/NightLightBackend.h"
#include <QCoreApplication>
#include <QPointer>

namespace {
bool sameVariant(const QVariant &a, const QVariant &b)
{ return a.metaType() == b.metaType() && a == b; }
bool sameCapability(const NightLightRuntime &a, const NightLightRuntime &b)
{
    return a.owner == b.owner && a.epoch == b.epoch && a.version == b.version && a.serviceAvailable == b.serviceAvailable
        && a.schemaSupported == b.schemaSupported && a.available == b.available
        && a.known == b.known && a.previewSupported == b.previewSupported;
}
bool usable(const NightLightRuntime &runtime)
{ return runtime.serviceAvailable && runtime.schemaSupported && runtime.known && runtime.available && !runtime.owner.isEmpty(); }
}

bool NightLightField::operator==(const NightLightField &o) const
{
    return sameVariant(value, o.value) && sameVariant(defaultValue, o.defaultValue)
        && known == o.known && defaultKnown == o.defaultKnown && defaultNoOp == o.defaultNoOp
        && effectivePresent == o.effectivePresent && localPresent == o.localPresent
        && inheritedPresent == o.inheritedPresent && globalPresent == o.globalPresent
        && immutable == o.immutable && writable == o.writable && resettable == o.resettable
        && reason == o.reason && resetReason == o.resetReason && revision == o.revision
        && inheritedRevision == o.inheritedRevision && globalRevision == o.globalRevision;
}

QList<NightLightKey> NightLightTypes::keys()
{ return {NightLightKey::Active, NightLightKey::Mode, NightLightKey::DayTemperature, NightLightKey::NightTemperature}; }
QString NightLightTypes::keyName(NightLightKey key)
{
    switch (key) {
    case NightLightKey::Active: return QStringLiteral("Active");
    case NightLightKey::Mode: return QStringLiteral("Mode");
    case NightLightKey::DayTemperature: return QStringLiteral("DayTemperature");
    case NightLightKey::NightTemperature: return QStringLiteral("NightTemperature");
    }
    return {};
}
QString NightLightTypes::label(NightLightKey key)
{
    switch (key) {
    case NightLightKey::Active: return QObject::tr("Night Light enabled");
    case NightLightKey::Mode: return QObject::tr("Switching mode");
    case NightLightKey::DayTemperature: return QObject::tr("Day light temperature");
    case NightLightKey::NightTemperature: return QObject::tr("Night light temperature");
    }
    return {};
}
QVariant NightLightTypes::builtinDefault(NightLightKey key)
{
    switch (key) {
    case NightLightKey::Active: return false;
    case NightLightKey::Mode: return DarkLightMode;
    case NightLightKey::DayTemperature: return 6500;
    case NightLightKey::NightTemperature: return 4500;
    }
    return {};
}
bool NightLightTypes::validValue(NightLightKey key, const QVariant &value)
{
    if (keyName(key).isEmpty()) return false;
    if (key == NightLightKey::Active) return value.metaType().id() == QMetaType::Bool;
    if (value.metaType().id() != QMetaType::Int) return false;
    if (key == NightLightKey::Mode) return value.toInt() == ConstantMode || value.toInt() == DarkLightMode;
    return value.toInt() >= MinimumTemperature && value.toInt() <= MaximumTemperature;
}
bool NightLightTypes::sameConfiguration(const NightLightSnapshot &a, const NightLightSnapshot &b)
{ return a.fields == b.fields && a.revision == b.revision && sameCapability(a.runtime, b.runtime); }
bool NightLightTypes::sameDraft(const NightLightSnapshot &a, const NightLightSnapshot &b)
{ return sameConfiguration(a, b) && a.intents == b.intents; }

bool NightLightTypes::stage(NightLightSnapshot &draft, const NightLightSnapshot &baseline,
                           NightLightKey key, const QVariant &value, QString &error)
{
    error.clear();
    if (!baseline.fields.contains(key) || !draft.fields.contains(key) || !validValue(key, value)) {
        error = QObject::tr("Choose a supported Night Light value."); return false;
    }
    const auto &saved = baseline.fields[key];
    if (!saved.writable || saved.immutable) {
        error = saved.reason.isEmpty() ? QObject::tr("This Night Light preference cannot be changed.") : saved.reason; return false;
    }
    draft.fields[key].value = value; draft.fields[key].known = true;
    if (saved.known && saved.localPresent && sameVariant(saved.value, value)) {
        draft.fields[key] = saved; draft.intents.remove(key);
    } else draft.intents[key] = NightLightIntent::Set;
    return true;
}
bool NightLightTypes::restore(NightLightSnapshot &draft, const NightLightSnapshot &baseline,
                             NightLightKey key, QString &error)
{
    error.clear();
    if (!baseline.fields.contains(key) || !draft.fields.contains(key)) {
        error = QObject::tr("The Night Light preference is unavailable."); return false;
    }
    const auto &saved = baseline.fields[key];
    if (!saved.known || !saved.defaultKnown) {
        error = QObject::tr("The saved value or its native default is unknown. Choose an explicit supported value instead."); return false;
    }
    // Native skeleton defaults do not remove an already-default explicit entry.
    // This also cancels a pending Set on an absent entry without writing it.
    if (saved.defaultNoOp || sameVariant(saved.value, saved.defaultValue)) {
        draft.fields[key] = saved; draft.intents.remove(key); return true;
    }
    if (!saved.resettable || !saved.writable || saved.immutable) {
        error = saved.resetReason.isEmpty() ? QObject::tr("This Night Light default cannot be restored.") : saved.resetReason; return false;
    }
    draft.fields[key].value = saved.defaultValue; draft.fields[key].known = true;
    draft.intents[key] = NightLightIntent::RestoreDefault;
    return true;
}
bool NightLightTypes::requestValid(const NightLightSnapshot &expected, const NightLightSnapshot &requested, QString &error)
{
    error.clear();
    if (!usable(expected.runtime) || !sameCapability(expected.runtime, requested.runtime)) {
        error = QObject::tr("The verified Night Light service is unavailable or changed."); return false;
    }
    NightLightSnapshot rebuilt = expected; rebuilt.intents.clear();
    for (auto i = requested.intents.cbegin(); i != requested.intents.cend(); ++i) {
        const bool ok = i.value() == NightLightIntent::Set
            ? stage(rebuilt, expected, i.key(), requested.fields.value(i.key()).value, error)
            : i.value() == NightLightIntent::RestoreDefault
                ? restore(rebuilt, expected, i.key(), error) : false;
        if (!ok) { if (error.isEmpty()) error = QObject::tr("The Night Light request has an invalid operation."); return false; }
    }
    if (!sameDraft(rebuilt, requested)) {
        error = QObject::tr("The Night Light request changed unowned values or capability metadata."); return false;
    }
    return true;
}
bool NightLightTypes::matchesRequest(const NightLightSnapshot &expected, const NightLightSnapshot &requested,
                                    const NightLightSnapshot &actual, QString &error)
{
    error.clear();
    if (!requestValid(expected, requested, error)) return false;
    if (!sameCapability(expected.runtime, actual.runtime) || !actual.intents.isEmpty()
        || actual.fields.size() != expected.fields.size()) {
        error = QObject::tr("Night Light service identity or capabilities changed during Apply."); return false;
    }
    for (const auto key : keys()) {
        if (!expected.fields.contains(key) || !actual.fields.contains(key)) {
            error = QObject::tr("The saved Night Light configuration is incomplete."); return false;
        }
        const auto &before = expected.fields[key], &after = actual.fields[key];
        if (!requested.intents.contains(key)) {
            if (!(before == after)) {
                error = QObject::tr("An untouched Night Light preference changed during Apply."); return false;
            }
            continue;
        }
        if (before.defaultKnown != after.defaultKnown || !sameVariant(before.defaultValue, after.defaultValue)
            || before.inheritedPresent != after.inheritedPresent || before.inheritedRevision != after.inheritedRevision
            || before.immutable != after.immutable || before.writable != after.writable) {
            error = QObject::tr("Night Light defaults or policy changed during Apply."); return false;
        }
        const bool set = requested.intents[key] == NightLightIntent::Set;
        const bool explicitDefault = !set && before.inheritedPresent;
        if (set || explicitDefault) {
            if (!after.known || !after.localPresent || !after.effectivePresent || !sameVariant(after.value, requested.fields[key].value)
                || before.globalPresent != after.globalPresent || before.globalRevision != after.globalRevision) {
                error = QObject::tr("The explicit Night Light value could not be verified after saving."); return false;
            }
        } else if (after.localPresent || after.effectivePresent
                   || (before.localPresent ? before.globalPresent != after.globalPresent || before.globalRevision != after.globalRevision : after.globalPresent)) {
            error = QObject::tr("The Night Light override could not be verified as removed."); return false;
        }
    }
    return true;
}
bool NightLightTypes::runtimeMatches(const NightLightSnapshot &snapshot)
{
    const auto &r = snapshot.runtime;
    if (!usable(r)) return false;
    const auto active = snapshot.fields.value(NightLightKey::Active), mode = snapshot.fields.value(NightLightKey::Mode);
    if (active.known && active.value.metaType().id() == QMetaType::Bool && r.enabled != active.value.toBool()) return false;
    if (mode.known && mode.value.metaType().id() == QMetaType::Int) {
        const uint expectedMode = validValue(NightLightKey::Mode, mode.value) ? uint(mode.value.toInt()) : uint(DarkLightMode);
        if (r.mode != expectedMode) return false;
    }
    const auto targetKey = r.mode != uint(ConstantMode) && r.daylight ? NightLightKey::DayTemperature : NightLightKey::NightTemperature;
    const auto target = snapshot.fields.value(targetKey);
    return !target.known || target.value.metaType().id() != QMetaType::Int
        || r.targetTemperature == uint(qBound(MinimumTemperature, target.value.toInt(), MaximumTemperature));
}

NightLightSettings *NightLightSettings::instance()
{
    static QPointer<NightLightSettings> session;
    if (!session) session = new NightLightSettings(new KdeNightLightBackend, QCoreApplication::instance());
    return session;
}
NightLightSettings::NightLightSettings(NightLightBackend *backend, QObject *parent)
    : SettingsSession(parent), m_backend(backend)
{
    Q_ASSERT(backend); backend->setParent(this);
    connect(backend, &NightLightBackend::readFinished, this, &NightLightSettings::onRead);
    connect(backend, &NightLightBackend::writeFinished, this, &NightLightSettings::onWritten);
    connect(backend, &NightLightBackend::operationPending, this, [this](quint64 token, const QString &message) {
        if (token != m_token || m_phase != Phase::Write) return;
        m_pendingMessage = message;
        emit stateChanged();
    });
    connect(backend, &NightLightBackend::previewFinished, this, &NightLightSettings::onPreview);
    connect(backend, &NightLightBackend::externalChange, this, &NightLightSettings::refresh);
    m_previewTimer.setSingleShot(true); m_previewTimer.setInterval(15000);
    connect(&m_previewTimer, &QTimer::timeout, this, [this] {
        ++m_previewToken; m_previewPending = m_previewActive = m_stopAfterPreview = false;
        m_previewText = m_previewUncertain
            ? tr("The preview tracking window has elapsed. A timed-out request may still execute later; KWin expires each preview 15 seconds after execution. No settings were saved.")
            : tr("The temporary preview's 15-second tracking window has elapsed. No settings were saved.");
        emit stateChanged();
    });
    m_busy = true; QTimer::singleShot(0, this, [this] { beginRead(Phase::Refresh); });
}
bool NightLightSettings::canEdit(NightLightKey key) const
{
    return m_ready && !m_busy && !m_conflict && !m_uncertain && usable(m_runtime)
        && m_applied.fields.contains(key) && m_applied.fields[key].writable && !m_applied.fields[key].immutable;
}
QString NightLightSettings::applySupportText() const
{
    if (m_busy) return m_pendingMessage.isEmpty() ? tr("Night Light settings are being checked or saved.") : m_pendingMessage;
    if (m_conflict || m_uncertain) return tr("Night Light changed or the last result is uncertain. Revert successfully before applying.");
    if (!m_ready || !usable(m_runtime)) return m_runtime.reason.isEmpty() ? tr("A verified Night Light service is unavailable.") : m_runtime.reason;
    if (previewActive()) return tr("Stop the temporary preview before applying.");
    QString error;
    if (!NightLightTypes::requestValid(m_applied, m_staged, error)) return error;
    return {};
}
bool NightLightSettings::canApply() const { return applySupportText().isEmpty(); }
bool NightLightSettings::canPreview(NightLightKey key) const
{
    if (key != NightLightKey::DayTemperature && key != NightLightKey::NightTemperature) return false;
    if (!canEdit(key) || !m_runtime.previewSupported || m_previewPending) return false;
    const auto active = m_staged.fields.value(NightLightKey::Active), mode = m_staged.fields.value(NightLightKey::Mode);
    return active.known && active.value.toBool() && mode.known && NightLightTypes::validValue(NightLightKey::Mode, mode.value)
        && (key != NightLightKey::DayTemperature || mode.value.toInt() != NightLightTypes::ConstantMode)
        && NightLightTypes::validValue(key, m_staged.fields.value(key).value);
}
void NightLightSettings::edited()
{
    m_dirty = !m_staged.intents.isEmpty(); m_error.clear(); m_result.clear(); emit stateChanged();
}
void NightLightSettings::selectValue(NightLightKey key, const QVariant &value)
{
    if (!canEdit(key)) return;
    QString error;
    if (!NightLightTypes::stage(m_staged, m_applied, key, value, error)) { m_error = error; emit stateChanged(); return; }
    edited();
}
void NightLightSettings::restoreDefault(NightLightKey key)
{
    if (!m_ready || m_busy || m_conflict || m_uncertain) return;
    QString error;
    if (!NightLightTypes::restore(m_staged, m_applied, key, error)) { m_error = error; emit stateChanged(); return; }
    edited();
}
QStringList NightLightSettings::defaultsChanges() const
{
    QStringList result; auto draft = m_staged;
    for (const auto key : NightLightTypes::keys()) {
        const auto before = draft; QString error;
        if (NightLightTypes::restore(draft, m_applied, key, error) && !NightLightTypes::sameDraft(before, draft))
            result << NightLightTypes::label(key);
    }
    return result;
}
QStringList NightLightSettings::defaultsUnavailable() const
{
    QStringList result; auto draft = m_staged;
    for (const auto key : NightLightTypes::keys()) {
        QString error;
        if (!NightLightTypes::restore(draft, m_applied, key, error)) result << tr("%1: %2").arg(NightLightTypes::label(key), error);
    }
    return result;
}
void NightLightSettings::restoreDefaults()
{
    if (!m_ready || m_busy || m_conflict || m_uncertain) return;
    auto draft = m_staged; QStringList unavailable;
    for (const auto key : NightLightTypes::keys()) {
        QString error;
        if (!NightLightTypes::restore(draft, m_applied, key, error)) unavailable << tr("%1: %2").arg(NightLightTypes::label(key), error);
    }
    m_staged = draft; QPointer<NightLightSettings> guard(this); edited();
    if (guard && !unavailable.isEmpty()) { m_error = unavailable.join(QLatin1Char('\n')); emit stateChanged(); }
}
void NightLightSettings::previewTemperature(NightLightKey key)
{
    if (!canPreview(key)) return;
    m_previewTimer.stop();
    m_previewOwner = m_runtime.owner; m_previewEpoch = m_runtime.epoch;
    m_previewPending = true; m_stopAfterPreview = m_previewUncertain = false;
    m_previewText = tr("Requesting a temporary preview…"); QPointer<NightLightSettings> guard(this); emit stateChanged();
    if (guard) m_backend->preview(++m_previewToken, m_staged.fields[key].value.toInt(), m_previewOwner, m_previewEpoch);
}
void NightLightSettings::stopPreview()
{
    if (m_previewPending) { m_stopAfterPreview = true; return; }
    if (!m_previewActive) return;
    m_previewPending = true; m_previewText = tr("Ending the temporary preview…"); QPointer<NightLightSettings> guard(this); emit stateChanged();
    if (guard) m_backend->stopPreview(++m_previewToken, m_previewOwner, m_previewEpoch);
}
void NightLightSettings::onPreview(quint64 token, bool success, bool stopped, const QString &error)
{
    if (token != m_previewToken || !m_previewPending) return;
    m_previewPending = false;
    if (!success) m_previewUncertain = true;
    else if (stopped) m_previewUncertain = false;
    if (success && stopped) { m_previewActive = false; m_previewTimer.stop(); }
    else { m_previewActive = true; if (!m_previewTimer.isActive()) m_previewTimer.start(); }
    m_previewText = success ? (stopped ? tr("Temporary preview ended.") : tr("KWin accepted a temporary preview for up to 15 seconds. It does not save settings."))
        : (error.isEmpty() ? tr("The preview result could not be verified. A timed-out request may execute later; native previews expire 15 seconds after execution.") : error);
    if (m_stopAfterPreview && m_previewActive) { m_stopAfterPreview = false; stopPreview(); return; }
    m_stopAfterPreview = false; emit stateChanged();
}
void NightLightSettings::beginRead(Phase phase)
{
    m_phase = phase; m_busy = true; m_pendingRefresh = false; m_pendingMessage.clear();
    QPointer<NightLightSettings> guard(this); emit stateChanged();
    if (guard) m_backend->readCurrent(++m_token);
}
void NightLightSettings::refresh()
{
    if (m_busy) { m_pendingRefresh = true; return; }
    beginRead(Phase::Refresh);
}
void NightLightSettings::revert()
{
    if (m_busy) return;
    QPointer<NightLightSettings> guard(this); stopPreview(); if (guard) beginRead(Phase::Revert);
}
void NightLightSettings::apply()
{
    if (m_busy) return;
    const auto reason = applySupportText();
    if (!reason.isEmpty()) { m_error = reason; QPointer<NightLightSettings> guard(this); emit stateChanged(); if (guard) emit applyFinished(false); return; }
    if (!m_dirty) { emit applyFinished(true); return; }
    m_requested = m_staged; m_expected = m_applied; m_error.clear(); m_result.clear(); beginRead(Phase::Preflight);
}
void NightLightSettings::onRead(quint64 token, bool success, const NightLightSnapshot &snapshot, const QString &error)
{
    if (token != m_token || (m_phase != Phase::Refresh && m_phase != Phase::Revert
        && m_phase != Phase::Preflight && m_phase != Phase::Readback)) return;
    const auto phase = m_phase;
    if (success) {
        if (previewActive() && (snapshot.runtime.owner != m_previewOwner || snapshot.runtime.epoch != m_previewEpoch
                               || !snapshot.runtime.serviceAvailable)) {
            ++m_previewToken; m_previewTimer.stop(); m_previewPending = m_previewActive = m_stopAfterPreview = false;
            m_previewText = tr("The Night Light service changed; the previous preview request is no longer tracked.");
        }
        m_runtime = snapshot.runtime; m_ready = true;
    }
    else { m_ready = false; m_runtime.known = false; m_runtime.reason = error; }
    if (phase == Phase::Preflight) {
        QString validation;
        if (success && NightLightTypes::sameConfiguration(m_expected, snapshot)
            && NightLightTypes::requestValid(snapshot, m_requested, validation)) {
            m_phase = Phase::Write; m_backend->writeChanges(++m_token, m_expected, m_requested); return;
        }
        m_conflict = true; m_error = !success ? error : validation.isEmpty()
            ? tr("Night Light preferences, policy or service changed before Apply. Revert before retrying.") : validation;
        finish(true, false); return;
    }
    if (phase == Phase::Readback) {
        QString verification;
        const bool matches = success && NightLightTypes::matchesRequest(m_expected, m_requested, snapshot, verification);
        const bool observed = matches && NightLightTypes::runtimeMatches(snapshot);
        const bool applied = m_saved && m_notified && m_writeError.isEmpty() && matches && observed;
        if (applied) {
            m_applied = m_staged = snapshot; m_dirty = m_conflict = m_uncertain = false; m_error.clear();
            m_result = tr("Saved Night Light preferences verified. KDE was notified; KWin's public state matches where saved values are known. "
                          "KWin does not expose the inactive temperature or a reload acknowledgement; physical screen color is not verified.");
        } else {
            m_conflict = m_uncertain = true;
            m_error = !m_writeError.isEmpty() ? m_writeError : !success ? error : !matches ? verification
                : !m_notified ? tr("Preferences may be saved, but KDE notification failed. Revert before retrying.")
                : tr("Preferences were saved, but KWin's observable Night Light state does not match. Revert before retrying.");
            m_result.clear();
        }
        finish(true, applied); return;
    }
    if (success && (phase == Phase::Revert || !m_dirty && !m_uncertain && !m_conflict)) {
        m_applied = m_staged = snapshot; m_dirty = m_conflict = m_uncertain = false; m_error.clear(); m_result.clear();
    } else if (success) {
        if (!NightLightTypes::sameConfiguration(m_applied, snapshot)) {
            m_conflict = true; m_error = tr("Night Light preferences, policy or service changed outside Control Panel. Your draft is retained; Revert before applying.");
        }
    } else {
        if (m_dirty || m_uncertain || phase == Phase::Revert) m_conflict = true;
        m_error = error.isEmpty() ? tr("Night Light settings could not be read. The draft is retained.") : error;
    }
    finish();
}
void NightLightSettings::onWritten(quint64 token, bool saved, bool notified, const QString &error)
{
    if (token != m_token || m_phase != Phase::Write) return;
    m_saved = saved; m_notified = notified; m_writeError = error; beginRead(Phase::Readback);
}
void NightLightSettings::finish(bool applying, bool success)
{
    m_busy = false; m_phase = Phase::Idle; QPointer<NightLightSettings> guard(this); emit stateChanged();
    if (!guard) return;
    if (applying) emit applyFinished(success);
    if (guard && m_pendingRefresh) QTimer::singleShot(0, this, [this] { if (!m_busy && m_pendingRefresh) beginRead(Phase::Refresh); });
}
