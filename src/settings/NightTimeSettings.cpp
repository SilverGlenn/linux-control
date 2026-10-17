#include "NightTimeSettings.h"
#include "platform/kde/NightTimeBackend.h"
#include <QCoreApplication>
#include <QPointer>
#include <QTimer>
#include <cmath>
#include <limits>

namespace {
bool sameVariant(const QVariant &a, const QVariant &b)
{ return a.metaType() == b.metaType() && a == b; }
bool sameCapability(const NightTimeRuntime &a, const NightTimeRuntime &b)
{
    return a.owner == b.owner && a.epoch == b.epoch && a.version == b.version
        && a.apiVersion == b.apiVersion && a.installedVerified == b.installedVerified
        && a.serviceAvailable == b.serviceAvailable && a.providerVerified == b.providerVerified;
}
bool usable(const NightTimeRuntime &r)
{ return r.installedVerified && (!r.serviceAvailable || r.providerVerified && !r.owner.isEmpty() && r.apiVersion == 1); }
bool nativeValue(NightTimeKey key, const QVariant &v)
{
    if (key == NightTimeKey::TransitionDuration) return v.metaType().id() == QMetaType::UInt;
    return NightTimeTypes::validValue(key, v);
}
}

bool NightTimeField::operator==(const NightTimeField &o) const
{
    return sameVariant(value, o.value) && sameVariant(defaultValue, o.defaultValue)
        && known == o.known && defaultKnown == o.defaultKnown && defaultNoOp == o.defaultNoOp
        && effectivePresent == o.effectivePresent && localPresent == o.localPresent
        && inheritedPresent == o.inheritedPresent && globalPresent == o.globalPresent
        && immutable == o.immutable && writable == o.writable && resettable == o.resettable
        && reason == o.reason && resetReason == o.resetReason && revision == o.revision
        && inheritedRevision == o.inheritedRevision && globalRevision == o.globalRevision;
}

QList<NightTimeKey> NightTimeTypes::keys()
{ return {NightTimeKey::Source, NightTimeKey::Automatic, NightTimeKey::Latitude, NightTimeKey::Longitude,
          NightTimeKey::SunriseStart, NightTimeKey::SunsetStart, NightTimeKey::TransitionDuration}; }
QString NightTimeTypes::groupName(NightTimeKey key)
{
    switch (key) {
    case NightTimeKey::Source: return QStringLiteral("General");
    case NightTimeKey::Automatic: case NightTimeKey::Latitude: case NightTimeKey::Longitude: return QStringLiteral("Location");
    case NightTimeKey::SunriseStart: case NightTimeKey::SunsetStart: case NightTimeKey::TransitionDuration: return QStringLiteral("Times");
    } return {};
}
QString NightTimeTypes::keyName(NightTimeKey key)
{
    switch (key) {
    case NightTimeKey::Source: return QStringLiteral("Source");
    case NightTimeKey::Automatic: return QStringLiteral("Automatic");
    case NightTimeKey::Latitude: return QStringLiteral("Latitude");
    case NightTimeKey::Longitude: return QStringLiteral("Longitude");
    case NightTimeKey::SunriseStart: return QStringLiteral("SunriseStart");
    case NightTimeKey::SunsetStart: return QStringLiteral("SunsetStart");
    case NightTimeKey::TransitionDuration: return QStringLiteral("TransitionDuration");
    } return {};
}
QString NightTimeTypes::label(NightTimeKey key)
{
    switch (key) {
    case NightTimeKey::Source: return QObject::tr("Schedule source");
    case NightTimeKey::Automatic: return QObject::tr("Automatic location");
    case NightTimeKey::Latitude: return QObject::tr("Latitude");
    case NightTimeKey::Longitude: return QObject::tr("Longitude");
    case NightTimeKey::SunriseStart: return QObject::tr("Day transition starts");
    case NightTimeKey::SunsetStart: return QObject::tr("Night transition starts");
    case NightTimeKey::TransitionDuration: return QObject::tr("Transition duration");
    } return {};
}
QVariant NightTimeTypes::builtinDefault(NightTimeKey key)
{
    switch (key) {
    case NightTimeKey::Source: return LocationSource;
    case NightTimeKey::Automatic: return true;
    case NightTimeKey::Latitude: case NightTimeKey::Longitude: return 0.0;
    case NightTimeKey::SunriseStart: return QTime(6, 0);
    case NightTimeKey::SunsetStart: return QTime(18, 0);
    case NightTimeKey::TransitionDuration: return uint(1800);
    } return {};
}
bool NightTimeTypes::validValue(NightTimeKey key, const QVariant &value)
{
    switch (key) {
    case NightTimeKey::Source: return value.metaType().id() == QMetaType::Int && (value.toInt() == LocationSource || value.toInt() == TimesSource);
    case NightTimeKey::Automatic: return value.metaType().id() == QMetaType::Bool;
    case NightTimeKey::Latitude: case NightTimeKey::Longitude:
        return value.metaType().id() == QMetaType::Double && std::isfinite(value.toDouble())
            && std::abs(value.toDouble()) <= (key == NightTimeKey::Latitude ? 90.0 : 180.0);
    case NightTimeKey::SunriseStart: case NightTimeKey::SunsetStart:
        return value.metaType().id() == QMetaType::QTime && value.toTime().isValid();
    case NightTimeKey::TransitionDuration:
        return value.metaType().id() == QMetaType::UInt && value.toUInt() >= MinimumDuration && value.toUInt() <= MaximumDuration;
    } return false;
}
bool NightTimeTypes::sameConfiguration(const NightTimeSnapshot &a, const NightTimeSnapshot &b)
{ return a.fields == b.fields && a.daemonFields == b.daemonFields && a.revision == b.revision && sameCapability(a.runtime, b.runtime); }
bool NightTimeTypes::sameDraft(const NightTimeSnapshot &a, const NightTimeSnapshot &b)
{ return sameConfiguration(a, b) && a.intents == b.intents; }
QVariant NightTimeTypes::persistedValue(NightTimeKey key, const QVariant &value)
{
    if ((key == NightTimeKey::SunriseStart || key == NightTimeKey::SunsetStart)
        && value.metaType().id() == QMetaType::QTime && value.toTime().isValid()) {
        const auto time = value.toTime(); return QTime(time.hour(), time.minute(), time.second());
    }
    return value;
}
bool NightTimeTypes::stage(NightTimeSnapshot &draft, const NightTimeSnapshot &baseline, NightTimeKey key, const QVariant &value, QString &error)
{
    error.clear();
    if (!baseline.fields.contains(key) || !draft.fields.contains(key) || !validValue(key, value)) {
        error = QObject::tr("Choose a supported Day and Night value."); return false;
    }
    const auto &saved = baseline.fields[key];
    if (baseline.daemonFields.value(key).immutable) {
        error = QObject::tr("The daemon's configuration policy protects this preference."); return false;
    }
    if (!saved.writable || saved.immutable) {
        error = saved.reason.isEmpty() ? QObject::tr("This preference cannot be changed.") : saved.reason; return false;
    }
    if (saved.known && saved.localPresent && sameVariant(saved.value, value)) {
        draft.fields[key] = saved; draft.intents.remove(key); return true;
    }
    const auto serialized = persistedValue(key, value);
    draft.fields[key].value = serialized; draft.fields[key].known = true;
    if (saved.known && saved.localPresent && sameVariant(saved.value, serialized)) { draft.fields[key] = saved; draft.intents.remove(key); }
    else draft.intents[key] = NightTimeIntent::Set;
    return true;
}
bool NightTimeTypes::restore(NightTimeSnapshot &draft, const NightTimeSnapshot &baseline, NightTimeKey key, QString &error)
{
    error.clear();
    if (!baseline.fields.contains(key) || !draft.fields.contains(key)) { error = QObject::tr("The preference is unavailable."); return false; }
    const auto &saved = baseline.fields[key];
    if (!saved.known || !saved.defaultKnown || !nativeValue(key, saved.defaultValue)) {
        error = QObject::tr("The saved value or native default is unsupported. Choose an explicit supported value."); return false;
    }
    if (saved.defaultNoOp || sameVariant(saved.value, saved.defaultValue)) { draft.fields[key] = saved; draft.intents.remove(key); return true; }
    if (baseline.daemonFields.value(key).immutable) {
        error = QObject::tr("The daemon's configuration policy protects this preference."); return false;
    }
    if (!saved.writable || saved.immutable || !saved.resettable) {
        error = saved.resetReason.isEmpty() ? QObject::tr("This native default cannot be restored.") : saved.resetReason; return false;
    }
    draft.fields[key].value = saved.inheritedPresent ? persistedValue(key, saved.defaultValue) : saved.defaultValue;
    draft.fields[key].known = true; draft.intents[key] = NightTimeIntent::RestoreDefault; return true;
}
QMap<NightTimeKey, NightTimeField> NightTimeTypes::projectedDaemonFields(const NightTimeSnapshot &baseline, const NightTimeSnapshot &draft)
{
    auto fields = baseline.daemonFields;
    for (auto i = draft.intents.cbegin(); i != draft.intents.cend(); ++i) {
        if (!fields.contains(i.key()) || !baseline.fields.contains(i.key())) continue;
        auto &field = fields[i.key()];
        if (i.value() == NightTimeIntent::Set || baseline.fields[i.key()].inheritedPresent) {
            field.value = persistedValue(i.key(), draft.fields.value(i.key()).value); field.known = draft.fields.value(i.key()).known;
        } else if (baseline.fields[i.key()].localPresent) {
            field.value = field.defaultValue; field.known = field.defaultKnown;
        }
        // Removing a kdeglobals-only value leaves the daemon's NoGlobals view alone.
    }
    return fields;
}
bool NightTimeTypes::activeBranchValid(const QMap<NightTimeKey, NightTimeField> &fields, QString &error)
{
    error.clear();
    auto valid = [&fields](NightTimeKey key) { const auto f = fields.value(key); return f.known && nativeValue(key, f.value); };
    if (!valid(NightTimeKey::Source)) {
        error = QObject::tr("Choose a supported daemon schedule source before Apply. KDE cannot safely reload an unknown source."); return false;
    }
    const bool location = fields[NightTimeKey::Source].value.toInt() == LocationSource;
    if (location && !valid(NightTimeKey::Automatic)) { error = QObject::tr("The daemon's automatic-location policy is unknown. Choose it explicitly."); return false; }
    if (location && !fields[NightTimeKey::Automatic].value.toBool()) {
        if (!valid(NightTimeKey::Latitude) || !valid(NightTimeKey::Longitude)) {
            error = QObject::tr("Choose valid manual coordinates before Apply."); return false;
        }
        return true;
    }
    // Automatic location uses the custom times if a provider or usable fix is unavailable.
    if (!valid(NightTimeKey::SunriseStart) || !valid(NightTimeKey::SunsetStart) || !valid(NightTimeKey::TransitionDuration)) {
        error = QObject::tr("Choose valid transition times and duration, including the automatic-location fallback."); return false;
    }
    const uint duration = fields[NightTimeKey::TransitionDuration].value.toUInt();
    const int interval = std::abs(fields[NightTimeKey::SunriseStart].value.toTime().secsTo(fields[NightTimeKey::SunsetStart].value.toTime()));
    if (duration > uint(std::numeric_limits<int>::max()) || duration > uint(qMin(interval, 86400 - interval))) {
        error = QObject::tr("Day and night transitions overlap. Adjust their start times or shorten the duration."); return false;
    }
    return true;
}
bool NightTimeTypes::requestValid(const NightTimeSnapshot &expected, const NightTimeSnapshot &requested, QString &error)
{
    error.clear();
    if (!usable(expected.runtime) || !sameCapability(expected.runtime, requested.runtime)) {
        error = QObject::tr("The installed Day and Night contract or running service could not be verified."); return false;
    }
    auto rebuilt = expected; rebuilt.intents.clear();
    for (auto i = requested.intents.cbegin(); i != requested.intents.cend(); ++i) {
        const bool ok = i.value() == NightTimeIntent::Set
            ? stage(rebuilt, expected, i.key(), requested.fields.value(i.key()).value, error)
            : i.value() == NightTimeIntent::RestoreDefault && restore(rebuilt, expected, i.key(), error);
        if (!ok) { if (error.isEmpty()) error = QObject::tr("An invalid Day and Night operation was requested."); return false; }
    }
    if (!sameDraft(rebuilt, requested)) { error = QObject::tr("The request changed unowned preferences or capability metadata."); return false; }
    return activeBranchValid(projectedDaemonFields(expected, requested), error);
}
bool NightTimeTypes::matchesRequest(const NightTimeSnapshot &expected, const NightTimeSnapshot &requested, const NightTimeSnapshot &actual, QString &error)
{
    if (!requestValid(expected, requested, error)) return false;
    if (!sameCapability(expected.runtime, actual.runtime) || !actual.intents.isEmpty()
        || actual.fields.size() != expected.fields.size() || actual.daemonFields.size() != expected.daemonFields.size()) {
        error = QObject::tr("Day and Night service identity or capabilities changed during Apply."); return false;
    }
    const auto projected = projectedDaemonFields(expected, requested);
    for (auto key : keys()) {
        if (!actual.fields.contains(key) || !actual.daemonFields.contains(key) || !expected.fields.contains(key)) {
            error = QObject::tr("The saved Day and Night configuration is incomplete."); return false;
        }
        const auto before = expected.fields[key], after = actual.fields[key];
        if (!requested.intents.contains(key)) {
            if (!(before == after) || !(expected.daemonFields.value(key) == actual.daemonFields.value(key))) {
                error = QObject::tr("An untouched Day and Night preference changed during Apply."); return false;
            }
            continue;
        }
        if (before.defaultKnown != after.defaultKnown || !sameVariant(before.defaultValue, after.defaultValue)
            || before.inheritedPresent != after.inheritedPresent || before.inheritedRevision != after.inheritedRevision
            || before.immutable != after.immutable || before.writable != after.writable) {
            error = QObject::tr("Day and Night defaults or policy changed during Apply."); return false;
        }
        const bool explicitValue = requested.intents[key] == NightTimeIntent::Set || before.inheritedPresent;
        if (explicitValue) {
            if (!after.known || !after.localPresent || !after.effectivePresent || !sameVariant(after.value, requested.fields[key].value)
                || before.globalPresent != after.globalPresent || before.globalRevision != after.globalRevision) {
                error = QObject::tr("The explicit Day and Night preference could not be verified after saving."); return false;
            }
        } else if (!after.known || !sameVariant(after.value, requested.fields[key].value)
                   || after.localPresent || after.effectivePresent
                   || (before.localPresent ? before.globalPresent != after.globalPresent || before.globalRevision != after.globalRevision : after.globalPresent)) {
            error = QObject::tr("The Day and Night override could not be verified as removed."); return false;
        }
        const auto daemonBefore = expected.daemonFields.value(key), daemonAfter = actual.daemonFields.value(key);
        if (daemonAfter.known != projected.value(key).known || !sameVariant(daemonAfter.value, projected.value(key).value)
            || daemonBefore.defaultKnown != daemonAfter.defaultKnown || !sameVariant(daemonBefore.defaultValue, daemonAfter.defaultValue)
            || daemonBefore.inheritedRevision != daemonAfter.inheritedRevision || daemonBefore.immutable != daemonAfter.immutable) {
            error = QObject::tr("The daemon's saved configuration differs from the reviewed change."); return false;
        }
    }
    return true;
}
QTime NightTimeTypes::validatedTime(QTime input, QTime other, uint durationSeconds)
{
    if (!input.isValid() || !other.isValid() || durationSeconds > uint(std::numeric_limits<int>::max())) return input;
    const int interval = std::abs(input.secsTo(other));
    if (durationSeconds < uint(qMin(interval, 86400 - interval))) return input;
    return other.addSecs((input < other ? -1 : 1) * int(durationSeconds));
}
bool NightTimeTypes::selectTime(NightTimeSnapshot &draft, const NightTimeSnapshot &baseline, NightTimeKey key, QTime value, QString &error)
{
    if (key != NightTimeKey::SunriseStart && key != NightTimeKey::SunsetStart) { error = QObject::tr("Choose a transition start time."); return false; }
    const auto duration = draft.fields.value(NightTimeKey::TransitionDuration);
    const auto other = draft.fields.value(key == NightTimeKey::SunriseStart ? NightTimeKey::SunsetStart : NightTimeKey::SunriseStart);
    // A malformed tuple must remain repairable without guessing companions.
    // Native validation leaves the edited input alone when its companion is
    // invalid. Apply still rejects an incomplete resulting active branch.
    const bool canAdjust = duration.known && nativeValue(NightTimeKey::TransitionDuration, duration.value)
        && duration.value.toUInt() <= uint(std::numeric_limits<int>::max());
    return stage(draft, baseline, key, canAdjust
        ? validatedTime(value, other.known ? other.value.toTime() : QTime(), duration.value.toUInt()) : value, error);
}
bool NightTimeTypes::selectDuration(NightTimeSnapshot &draft, const NightTimeSnapshot &baseline, uint seconds, QString &error)
{
    auto candidate = draft;
    if (!stage(candidate, baseline, NightTimeKey::TransitionDuration, seconds, error)) return false;
    const auto morning = candidate.fields.value(NightTimeKey::SunriseStart), evening = candidate.fields.value(NightTimeKey::SunsetStart);
    if (!morning.known || !evening.known || !morning.value.toTime().isValid() || !evening.value.toTime().isValid()) {
        draft = candidate; return true;
    }
    const auto adjusted = validatedTime(evening.value.toTime(), morning.value.toTime(), seconds);
    if (adjusted != evening.value.toTime() && !stage(candidate, baseline, NightTimeKey::SunsetStart, adjusted, error)) return false;
    draft = candidate; return true;
}
bool NightTimeTypes::selectManualLocation(NightTimeSnapshot &draft, const NightTimeSnapshot &baseline, double latitude, double longitude, QString &error)
{
    auto candidate = draft;
    if (!stage(candidate, baseline, NightTimeKey::Latitude, latitude, error)
        || !stage(candidate, baseline, NightTimeKey::Longitude, longitude, error)
        || !stage(candidate, baseline, NightTimeKey::Automatic, false, error)) return false;
    draft = candidate; return true;
}
QString NightTimeTypes::privacyText(const NightTimeSnapshot &baseline, const NightTimeSnapshot &draft)
{
    const auto fields = projectedDaemonFields(baseline, draft);
    const auto source = fields.value(NightTimeKey::Source), automatic = fields.value(NightTimeKey::Automatic);
    const auto editorSource = draft.fields.value(NightTimeKey::Source), editorAutomatic = draft.fields.value(NightTimeKey::Automatic);
    const bool differentSource = editorSource.known && source.known && validValue(NightTimeKey::Source, editorSource.value)
        && validValue(NightTimeKey::Source, source.value) && !sameVariant(editorSource.value, source.value);
    const bool differentLocation = !differentSource && source.known && source.value.toInt() == LocationSource
        && editorAutomatic.known && automatic.known && validValue(NightTimeKey::Automatic, editorAutomatic.value)
        && validValue(NightTimeKey::Automatic, automatic.value) && !sameVariant(editorAutomatic.value, automatic.value);
    const QString difference = differentSource || differentLocation
        ? QObject::tr("The editor choice and daemon input differ. Select the intended source or location mode explicitly to stage a local override.\n") : QString();
    if (!source.known || !validValue(NightTimeKey::Source, source.value)
        || source.value.toInt() == LocationSource && (!automatic.known || automatic.value.metaType().id() != QMetaType::Bool))
        return difference + QObject::tr("The daemon's location policy is unknown. Choose a supported source and location policy before applying.");
    if (source.value.toInt() == LocationSource && automatic.value.toBool())
        return difference + QObject::tr("The daemon's resulting configuration uses automatic location. Applying it can make the running KDE service request location updates; the system provider may use GPS, cell towers, Wi-Fi or IP/network services. Opening this page does not request a location.");
    return difference + QObject::tr("The resulting daemon configuration uses %1. This page requests a current location only when you choose that action.")
        .arg(source.value.toInt() == TimesSource ? QObject::tr("custom times") : QObject::tr("manual coordinates"));
}


NightTimeSettings *NightTimeSettings::instance()
{
    static QPointer<NightTimeSettings> session;
    if (!session) session = new NightTimeSettings(new KdeNightTimeBackend, QCoreApplication::instance());
    return session;
}
NightTimeSettings::NightTimeSettings(NightTimeBackend *backend, QObject *parent)
    : SettingsSession(parent), m_backend(backend)
{
    Q_ASSERT(backend); backend->setParent(this);
    connect(backend, &NightTimeBackend::readFinished, this, &NightTimeSettings::onRead);
    connect(backend, &NightTimeBackend::writeFinished, this, &NightTimeSettings::onWritten);
    connect(backend, &NightTimeBackend::operationPending, this, [this](quint64 token, const QString &message) {
        if (token != m_token || m_phase != Phase::Write) return;
        m_pendingMessage = message;
        emit stateChanged();
    });
    connect(backend, &NightTimeBackend::externalChange, this, &NightTimeSettings::refresh);
    m_busy = true; QTimer::singleShot(0, this, [this] { beginRead(Phase::Refresh); });
}
bool NightTimeSettings::canEdit(NightTimeKey key) const
{
    return m_ready && !m_busy && !m_conflict && !m_uncertain && usable(m_runtime)
        && m_applied.fields.contains(key) && m_applied.fields[key].writable && !m_applied.fields[key].immutable
        && !m_applied.daemonFields.value(key).immutable;
}
QString NightTimeSettings::applySupportText() const
{
    if (m_busy) return m_pendingMessage.isEmpty() ? tr("Day and Night preferences are being checked or saved.") : m_pendingMessage;
    if (m_conflict || m_uncertain) return tr("Day and Night changed or the last result is uncertain. Revert successfully before applying.");
    if (!m_ready || !usable(m_runtime)) return m_runtime.reason.isEmpty() ? tr("The installed Day and Night contract is unavailable.") : m_runtime.reason;
    QString error;
    if (!NightTimeTypes::requestValid(m_applied, m_staged, error)) return error;
    return {};
}
bool NightTimeSettings::canApply() const { return applySupportText().isEmpty(); }
QString NightTimeSettings::privacyText() const { return NightTimeTypes::privacyText(m_applied, m_staged); }
void NightTimeSettings::edited()
{
    m_dirty = !m_staged.intents.isEmpty(); m_error.clear(); m_result.clear(); emit stateChanged();
}
void NightTimeSettings::selectValue(NightTimeKey key, const QVariant &value)
{
    if (!canEdit(key)) return;
    QString error;
    if (!NightTimeTypes::stage(m_staged, m_applied, key, value, error)) { m_error = error; emit stateChanged(); return; }
    edited();
}
void NightTimeSettings::selectTime(NightTimeKey key, QTime value)
{
    if (!canEdit(key)) return;
    QString error;
    if (!NightTimeTypes::selectTime(m_staged, m_applied, key, value, error)) { m_error = error; emit stateChanged(); return; }
    edited();
}
void NightTimeSettings::selectDuration(uint seconds)
{
    if (!canEdit(NightTimeKey::TransitionDuration)) return;
    QString error;
    if (!NightTimeTypes::selectDuration(m_staged, m_applied, seconds, error)) { m_error = error; emit stateChanged(); return; }
    edited();
}
void NightTimeSettings::selectManualLocation(double latitude, double longitude)
{
    if (!canEdit(NightTimeKey::Latitude) || !canEdit(NightTimeKey::Longitude) || !canEdit(NightTimeKey::Automatic)) return;
    QString error;
    if (!NightTimeTypes::selectManualLocation(m_staged, m_applied, latitude, longitude, error)) { m_error = error; emit stateChanged(); return; }
    edited();
}
void NightTimeSettings::restoreDefault(NightTimeKey key)
{
    if (!canEdit(key)) return;
    QString error;
    if (!NightTimeTypes::restore(m_staged, m_applied, key, error)) { m_error = error; emit stateChanged(); return; }
    edited();
}
QStringList NightTimeSettings::defaultsChanges() const
{
    QStringList result; auto draft = m_staged;
    for (auto key : NightTimeTypes::keys()) {
        const auto before = draft; QString error;
        if (NightTimeTypes::restore(draft, m_applied, key, error) && !NightTimeTypes::sameDraft(before, draft)) result << NightTimeTypes::label(key);
    }
    return result;
}
QStringList NightTimeSettings::defaultsUnavailable() const
{
    QStringList result; auto draft = m_staged;
    for (auto key : NightTimeTypes::keys()) {
        QString error;
        if (!NightTimeTypes::restore(draft, m_applied, key, error)) result << tr("%1: %2").arg(NightTimeTypes::label(key), error);
    }
    return result;
}
void NightTimeSettings::restoreDefaults()
{
    if (!m_ready || m_busy || m_conflict || m_uncertain) return;
    auto draft = m_staged; QStringList unavailable;
    for (auto key : NightTimeTypes::keys()) {
        QString error;
        if (!NightTimeTypes::restore(draft, m_applied, key, error)) unavailable << tr("%1: %2").arg(NightTimeTypes::label(key), error);
    }
    m_staged = draft; QPointer<NightTimeSettings> guard(this); edited();
    if (guard && !unavailable.isEmpty()) { m_error = unavailable.join(QLatin1Char('\n')); emit stateChanged(); }
}
void NightTimeSettings::beginRead(Phase phase)
{
    m_phase = phase; m_busy = true; m_pendingRefresh = false; m_pendingMessage.clear();
    QPointer<NightTimeSettings> guard(this); emit stateChanged();
    if (guard) m_backend->readCurrent(++m_token);
}
void NightTimeSettings::refresh()
{
    if (m_busy) { m_pendingRefresh = true; return; }
    beginRead(Phase::Refresh);
}
void NightTimeSettings::revert()
{
    if (m_busy) return;
    beginRead(Phase::Revert);
}
void NightTimeSettings::apply()
{
    if (m_busy) return;
    const auto reason = applySupportText();
    if (!reason.isEmpty()) { m_error = reason; QPointer<NightTimeSettings> guard(this); emit stateChanged(); if (guard) emit applyFinished(false); return; }
    if (!m_dirty) { emit applyFinished(true); return; }
    m_requested = m_staged; m_expected = m_applied; m_error.clear(); m_result.clear(); beginRead(Phase::Preflight);
}
void NightTimeSettings::onRead(quint64 token, bool success, const NightTimeSnapshot &snapshot, const QString &error)
{
    if (token != m_token || (m_phase != Phase::Refresh && m_phase != Phase::Revert
        && m_phase != Phase::Preflight && m_phase != Phase::Readback)) return;
    const auto phase = m_phase;
    if (success) {
        m_runtime = snapshot.runtime; m_cachedLocation = snapshot.cachedLocation; m_ready = true;
    }
    else { m_ready = false; m_runtime.providerVerified = false; m_runtime.installedVerified = false; m_runtime.reason = error; }
    if (phase == Phase::Preflight) {
        QString validation;
        if (success && NightTimeTypes::sameConfiguration(m_expected, snapshot)
            && NightTimeTypes::requestValid(snapshot, m_requested, validation)) {
            m_phase = Phase::Write; m_backend->writeChanges(++m_token, m_expected, m_requested); return;
        }
        m_conflict = true; m_error = !success ? error : validation.isEmpty()
            ? tr("Day and Night preferences, policy or service changed before Apply. Revert before retrying.") : validation;
        finish(true, false); return;
    }
    if (phase == Phase::Readback) {
        QString verification;
        const bool matches = success && NightTimeTypes::matchesRequest(m_expected, m_requested, snapshot, verification);
        const bool applied = m_saved && m_notified && m_writeError.isEmpty() && matches;
        if (applied) {
            m_applied = m_staged = snapshot; m_dirty = m_conflict = m_uncertain = false; m_error.clear();
            m_result = snapshot.runtime.serviceAvailable
                ? tr("Saved Day and Night preferences verified in both KDE configuration views. KDE was notified. The service exposes no settings reload acknowledgement; loaded behavior is not verified.")
                : tr("Saved Day and Night preferences verified. The service is not running. Its next activation may migrate legacy settings before reading configuration; adoption is unverified. No service was started.");
        } else {
            m_conflict = m_uncertain = true;
            m_error = !m_writeError.isEmpty() ? m_writeError : !success ? error : !matches ? verification
                : !m_notified ? tr("Preferences may be saved, but KDE notification failed. Revert before retrying.")
                : tr("The Day and Night result is uncertain. Revert before retrying.");
            m_result.clear();
        }
        finish(true, applied); return;
    }
    if (success && (phase == Phase::Revert || !m_dirty && !m_uncertain && !m_conflict)) {
        m_applied = m_staged = snapshot; m_dirty = m_conflict = m_uncertain = false; m_error.clear(); m_result.clear();
    } else if (success) {
        if (!NightTimeTypes::sameConfiguration(m_applied, snapshot)) {
            m_conflict = true; m_error = tr("Day and Night preferences, policy or service changed outside Control Panel. Your draft is retained; Revert before applying.");
        }
    } else {
        if (m_dirty || m_uncertain || phase == Phase::Revert) m_conflict = true;
        m_error = error.isEmpty() ? tr("Day and Night settings could not be read. The draft is retained.") : error;
    }
    finish();
}
void NightTimeSettings::onWritten(quint64 token, bool saved, bool notified, const QString &error)
{
    if (token != m_token || m_phase != Phase::Write) return;
    m_saved = saved; m_notified = notified; m_writeError = error; beginRead(Phase::Readback);
}
void NightTimeSettings::finish(bool applying, bool success)
{
    m_busy = false; m_phase = Phase::Idle;
    QPointer<NightTimeSettings> guard(this); emit stateChanged();
    if (!guard) return;
    if (applying) emit applyFinished(success);
    if (guard && m_pendingRefresh) QTimer::singleShot(0, this, [this] { if (!m_busy && m_pendingRefresh) beginRead(Phase::Refresh); });
}
