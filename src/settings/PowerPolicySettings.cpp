#include "PowerPolicySettings.h"
#include "PowerPolicyFields.h"
#include <QCryptographicHash>
#include "platform/kde/PowerPolicyBackend.h"
#include <QCoreApplication>
#include <QPointer>
#include <QTimer>
#include <tuple>

bool PowerPolicyKeyState::operator==(const PowerPolicyKeyState &o) const
{
    return std::tie(valueKnown, defaultKnown, effectivePresent, localPresent, inheritedPresent,
        globalPresent, immutable, writable, resettable, defaultValue, reason, defaultReason,
        resetReason, revision, inheritedRevision, globalRevision)
        == std::tie(o.valueKnown, o.defaultKnown, o.effectivePresent, o.localPresent, o.inheritedPresent,
        o.globalPresent, o.immutable, o.writable, o.resettable, o.defaultValue, o.reason, o.defaultReason,
        o.resetReason, o.revision, o.inheritedRevision, o.globalRevision)
        && defaultValue.metaType() == o.defaultValue.metaType();
}

bool PowerPolicyProfile::operator==(const PowerPolicyProfile &o) const
{
    return keys == o.keys && intents == o.intents && id == o.id && displayEnabled == o.displayEnabled
        && displaySeconds == o.displaySeconds && suspendAction == o.suspendAction
        && suspendSeconds == o.suspendSeconds && displayEditable == o.displayEditable
        && sleepEditable == o.sleepEditable && displayReason == o.displayReason
        && sleepReason == o.sleepReason
        && std::tie(idleActions, powerButtonActions, lidActions, sleepModes,
            dimEnabled, dimSeconds, brightnessEnabled, brightnessPercent,
            keyboardBrightnessEnabled, keyboardBrightnessPercent, lockedDisplaySeconds,
            lockBeforeDisplayOff, powerButtonAction, powerDownAction, lidAction, sleepMode,
            inhibitLidActionWhenExternalMonitorPresent, dimEditable, brightnessEditable,
            keyboardBrightnessEditable, lockedDisplayEditable, lockBeforeDisplayOffEditable,
            powerButtonEditable, powerDownEditable, lidEditable, lidExternalMonitorEditable,
            sleepModeEditable, dimReason, brightnessReason, keyboardBrightnessReason,
            lockedDisplayReason, lockBeforeDisplayOffReason, powerButtonReason, powerDownReason,
            lidReason, lidExternalMonitorReason, sleepModeReason)
        == std::tie(o.idleActions, o.powerButtonActions, o.lidActions, o.sleepModes,
            o.dimEnabled, o.dimSeconds, o.brightnessEnabled, o.brightnessPercent,
            o.keyboardBrightnessEnabled, o.keyboardBrightnessPercent, o.lockedDisplaySeconds,
            o.lockBeforeDisplayOff, o.powerButtonAction, o.powerDownAction, o.lidAction, o.sleepMode,
            o.inhibitLidActionWhenExternalMonitorPresent, o.dimEditable, o.brightnessEditable,
            o.keyboardBrightnessEditable, o.lockedDisplayEditable, o.lockBeforeDisplayOffEditable,
            o.powerButtonEditable, o.powerDownEditable, o.lidEditable, o.lidExternalMonitorEditable,
            o.sleepModeEditable, o.dimReason, o.brightnessReason, o.keyboardBrightnessReason,
            o.lockedDisplayReason, o.lockBeforeDisplayOffReason, o.powerButtonReason, o.powerDownReason,
            o.lidReason, o.lidExternalMonitorReason, o.sleepModeReason);
}
bool PowerPolicySnapshot::operator==(const PowerPolicySnapshot &o) const
{
    return keys == o.keys && intents == o.intents && profiles == o.profiles && serviceAvailable == o.serviceAvailable
        && unavailableReason == o.unavailableReason
        && std::tie(owner, capabilityRevision, batteryLowLevel, batteryCriticalLevel,
            batteryCriticalAction, batteryLevelsEditable, batteryCriticalActionEditable,
            batteryLevelsReason, batteryCriticalActionReason, batteryCriticalActions)
        == std::tie(o.owner, o.capabilityRevision, o.batteryLowLevel, o.batteryCriticalLevel,
            o.batteryCriticalAction, o.batteryLevelsEditable, o.batteryCriticalActionEditable,
            o.batteryLevelsReason, o.batteryCriticalActionReason, o.batteryCriticalActions);
}

PowerPolicySettings *PowerPolicySettings::instance()
{
    static QPointer<PowerPolicySettings> session;
    if (!session)
        session = new PowerPolicySettings(new KdePowerPolicyBackend,
                                          QCoreApplication::instance());
    return session;
}
PowerPolicySettings::PowerPolicySettings(PowerPolicyBackend *backend, QObject *parent)
    : SettingsSession(parent), m_backend(backend)
{
    Q_ASSERT(backend);
    backend->setParent(this);
    connect(backend, &PowerPolicyBackend::readFinished, this, &PowerPolicySettings::readFinished);
    connect(backend, &PowerPolicyBackend::writeFinished, this, &PowerPolicySettings::writeFinished);
    connect(backend, &PowerPolicyBackend::operationPending, this, [this](const QString &message) {
        if (m_phase != Phase::Writing) return;
        m_pendingMessage = message;
        emit stateChanged();
    });
    connect(backend, &PowerPolicyBackend::externalChange, this, &PowerPolicySettings::refresh);
    m_busy = true;
    QTimer::singleShot(0, this, &PowerPolicySettings::startRefresh);
}
void PowerPolicySettings::selectDisplayTimeout(const QString &id, int seconds)
{
    selectTimeout(id, seconds, true);
}
void PowerPolicySettings::selectSleepTimeout(const QString &id, int seconds)
{
    selectTimeout(id, seconds, false);
}
namespace {
QString formattedDefault(PowerPolicyKey key, const QVariant &value, const QList<PowerPolicyAction> &choices = {})
{
    for (const auto &choice : choices) if (value.metaType().id() == QMetaType::UInt && choice.value == value.toUInt()) return choice.label;
    if (value.metaType().id() == QMetaType::Bool) return value.toBool() ? QStringLiteral("enabled") : QStringLiteral("disabled");
    if (value.metaType().id() == QMetaType::UInt) return QStringLiteral("native action %1").arg(value.toUInt());
    switch (key) {
    case PowerPolicyKey::DisplaySeconds: case PowerPolicyKey::IdleSeconds: case PowerPolicyKey::DimSeconds:
    case PowerPolicyKey::LockedDisplaySeconds:
        return value.toInt() == -2 ? QStringLiteral("same as unlocked") : value.toInt() == 0 ? QStringLiteral("immediately") : QStringLiteral("%1 seconds").arg(value.toInt());
    default: return QStringLiteral("%1%").arg(value.toInt());
    }
}
QList<PowerPolicyAction> choicesFor(const PowerPolicyProfile &p, PowerPolicyKey key)
{
    switch (key) {
    case PowerPolicyKey::IdleAction: return p.idleActions;
    case PowerPolicyKey::PowerButtonAction: case PowerPolicyKey::PowerDownAction: return p.powerButtonActions;
    case PowerPolicyKey::LidAction: return p.lidActions;
    case PowerPolicyKey::SleepMode: return p.sleepModes;
    default: return {};
    }
}
}
void PowerPolicySettings::edited()
{
    m_dirty = m_staged != m_applied;
    m_error.clear(); m_result.clear();
    m_applyError = false; m_cancelled = false;
    emit stateChanged();
}
void PowerPolicySettings::editProfile(const QString &id, const std::function<bool(PowerPolicyProfile &)> &edit)
{
    if (m_busy || !m_ready || m_conflict || !m_staged.serviceAvailable) return;
    for (auto &profile : m_staged.profiles) if (profile.id == id) { if (edit(profile)) edited(); return; }
}
void PowerPolicySettings::selectValue(const QString &id, PowerPolicyKey key, const QVariant &value)
{
    editProfile(id, [&](auto &profile) {
        for (const auto &baseline : m_applied.profiles) if (baseline.id == id) {
            QString error; return PowerPolicyFields::stage(profile, baseline, key, value, error);
        }
        return false;
    });
}
void PowerPolicySettings::selectGlobalValue(PowerPolicyKey key, const QVariant &value)
{
    if (m_busy || !m_ready || m_conflict || !m_staged.serviceAvailable) return;
    QString error;
    if (PowerPolicyFields::stage(m_staged, m_applied, key, value, error)) edited();
}
void PowerPolicySettings::restoreKeyDefault(const QString &id, PowerPolicyKey key)
{
    editProfile(id, [&](auto &profile) {
        for (const auto &baseline : m_applied.profiles) if (baseline.id == id) {
            QString error; return PowerPolicyFields::restore(profile, baseline, key, error);
        }
        return false;
    });
}
void PowerPolicySettings::restoreGlobalKeyDefault(PowerPolicyKey key)
{
    if (m_busy || !m_ready || m_conflict || !m_staged.serviceAvailable) return;
    QString error;
    if (PowerPolicyFields::restore(m_staged, m_applied, key, error)) edited();
}
void PowerPolicySettings::selectIdleAction(const QString &id, uint action) { selectValue(id,PowerPolicyKey::IdleAction,action); }
void PowerPolicySettings::selectIdleTimeout(const QString &id, int seconds) { selectValue(id,PowerPolicyKey::IdleSeconds,seconds); }
void PowerPolicySettings::selectLockedDisplayTimeout(const QString &id, int seconds) { selectValue(id,PowerPolicyKey::LockedDisplaySeconds,seconds); }
void PowerPolicySettings::selectLockBeforeDisplayOff(const QString &id, bool enabled) { selectValue(id,PowerPolicyKey::LockBeforeDisplayOff,enabled); }
void PowerPolicySettings::selectPowerButtonAction(const QString &id, uint action) { selectValue(id,PowerPolicyKey::PowerButtonAction,action); }
void PowerPolicySettings::selectPowerDownAction(const QString &id, uint action) { selectValue(id,PowerPolicyKey::PowerDownAction,action); }
void PowerPolicySettings::selectLidAction(const QString &id, uint action) { selectValue(id,PowerPolicyKey::LidAction,action); }
void PowerPolicySettings::selectLidExternalMonitorInhibition(const QString &id, bool enabled) { selectValue(id,PowerPolicyKey::LidExternalMonitorInhibition,enabled); }
void PowerPolicySettings::selectSleepMode(const QString &id, uint mode) { selectValue(id,PowerPolicyKey::SleepMode,mode); }
void PowerPolicySettings::selectBatteryCriticalAction(uint action) { selectGlobalValue(PowerPolicyKey::BatteryCriticalAction,action); }
void PowerPolicySettings::selectBatteryLevels(int low, int critical)
{
    if (m_busy || !m_ready || m_conflict || !m_staged.serviceAvailable || critical > low) return;
    auto requested = m_staged; QString error;
    if (!PowerPolicyFields::stage(requested,m_applied,PowerPolicyKey::BatteryLowLevel,low,error)
        || !PowerPolicyFields::stage(requested,m_applied,PowerPolicyKey::BatteryCriticalLevel,critical,error)) return;
    m_staged = requested; edited();
}
void PowerPolicySettings::selectBrightness(const QString &id, bool enabled, int percent)
{
    editProfile(id,[&](auto &profile) {
        for (const auto &baseline:m_applied.profiles) if (baseline.id==id) {
            auto draft=profile; QString error;
            if (!PowerPolicyFields::stage(draft,baseline,PowerPolicyKey::BrightnessEnabled,enabled,error)
                || !PowerPolicyFields::stage(draft,baseline,PowerPolicyKey::BrightnessPercent,percent,error)) return false;
            profile=draft; return true;
        }
        return false;
    });
}
void PowerPolicySettings::selectKeyboardBrightness(const QString &id, bool enabled, int percent)
{
    editProfile(id,[&](auto &profile) {
        for (const auto &baseline:m_applied.profiles) if (baseline.id==id) {
            auto draft=profile; QString error;
            if (!PowerPolicyFields::stage(draft,baseline,PowerPolicyKey::KeyboardBrightnessEnabled,enabled,error)
                || !PowerPolicyFields::stage(draft,baseline,PowerPolicyKey::KeyboardBrightnessPercent,percent,error)) return false;
            profile=draft; return true;
        }
        return false;
    });
}
void PowerPolicySettings::selectDimTimeout(const QString &id, int seconds)
{
    editProfile(id,[&](auto &profile) {
        for (const auto &baseline:m_applied.profiles) if (baseline.id==id) {
            auto draft=profile; QString error;
            if (seconds!=-1 && !PowerPolicyFields::stage(draft,baseline,PowerPolicyKey::DimSeconds,seconds,error)) return false;
            if ((seconds==-1 || !profile.keys.value(PowerPolicyKey::DimEnabled).valueKnown || !profile.dimEnabled)
                && !PowerPolicyFields::stage(draft,baseline,PowerPolicyKey::DimEnabled,seconds!=-1,error)) return false;
            profile=draft; return true;
        }
        return false;
    });
}
void PowerPolicySettings::selectTimeout(const QString &id, int seconds, bool display)
{
    if (!display) {
        if (seconds==-1) selectIdleAction(id,0u);
        else if (seconds>=60 && seconds<=MaximumTimeoutSeconds) {
            // Compatibility API for the old Sleep selector. The advanced UI
            // uses independent action and duration setters.
            editProfile(id,[&](auto &profile) {
                for (const auto &baseline:m_applied.profiles) if (baseline.id==id) {
                    auto draft=profile; QString error;
                    if (!PowerPolicyFields::stage(draft,baseline,PowerPolicyKey::IdleSeconds,seconds,error)) return false;
                    if ((!profile.keys.value(PowerPolicyKey::IdleAction).valueKnown || profile.suspendAction==0)
                        && !PowerPolicyFields::stage(draft,baseline,PowerPolicyKey::IdleAction,1u,error)) return false;
                    profile=draft; return true;
                }
                return false;
            });
        }
        return;
    }
    editProfile(id,[&](auto &profile) {
        for (const auto &baseline:m_applied.profiles) if (baseline.id==id) {
            auto draft=profile; QString error;
            if (seconds!=-1 && !PowerPolicyFields::stage(draft,baseline,PowerPolicyKey::DisplaySeconds,seconds,error)) return false;
            if ((seconds==-1 || !profile.keys.value(PowerPolicyKey::DisplayEnabled).valueKnown || !profile.displayEnabled)
                && !PowerPolicyFields::stage(draft,baseline,PowerPolicyKey::DisplayEnabled,seconds!=-1,error)) return false;
            profile=draft; return true;
        }
        return false;
    });
}
PowerPolicySettings::DefaultsReview PowerPolicySettings::reviewDefaults(DefaultsScope scope, const QString &id) const
{
    DefaultsReview review;
    QByteArray context=PowerPolicyFields::revision(m_applied)+PowerPolicyFields::revision(m_staged)
        + QByteArray::number(int(scope))+id.toUtf8()+QByteArray::number(m_ready)+QByteArray::number(m_conflict)+QByteArray::number(m_busy);
    review.revision=QCryptographicHash::hash(context,QCryptographicHash::Sha256);
    if (!m_ready || m_busy || m_conflict || !m_applied.serviceAvailable) {
        review.unavailable << QStringLiteral("Reload readable native power settings before restoring defaults."); return review;
    }
    auto inspect=[&](const auto &baseline, const auto &draft, PowerPolicyKey key, const QString &prefix, const QList<PowerPolicyAction> &choices) {
        const auto state=baseline.keys.value(key); auto requested=draft; QString error;
        const auto title=prefix+PowerPolicyFields::label(key);
        if (PowerPolicyFields::restore(requested,baseline,key,error) && !(requested==draft)) {
            if (!requested.intents.contains(key)) review.changes << title+QStringLiteral(": discard this staged override; keep the saved native setting.");
            else review.changes << title+QStringLiteral(": use KDE default (%1); %2.").arg(formattedDefault(key,state.defaultValue,choices),
                state.inheritedPresent ? QStringLiteral("save the inherited default locally")
                : state.globalPresent && !state.localPresent ? QStringLiteral("revert the user global override") : QStringLiteral("revert the saved override"));
        } else if (!state.effectivePresent || (state.valueKnown && state.defaultKnown && PowerPolicyFields::value(baseline,key)==state.defaultValue))
            review.unchanged << title+QStringLiteral(": ")+state.resetReason;
        else review.unavailable << (error.isEmpty() ? title+QStringLiteral(": ")+state.resetReason : prefix+error);
    };
    if (scope==DefaultsScope::SelectedProfile || scope==DefaultsScope::AllProfiles || scope==DefaultsScope::AllPolicies) {
        bool found=false;
        for (int i=0;i<m_applied.profiles.size();++i) {
            const auto &p=m_applied.profiles[i]; if (scope==DefaultsScope::SelectedProfile && p.id!=id) continue;
            found=true;
            const QString source=p.id==QLatin1String("AC") ? QStringLiteral("Plugged in") : p.id==QLatin1String("Battery") ? QStringLiteral("On battery") : QStringLiteral("Low battery");
            for (auto key:PowerPolicyFields::profileKeys()) inspect(p,m_staged.profiles[i],key,source+QStringLiteral(" — "),choicesFor(p,key));
        }
        if (!found) review.unavailable << QStringLiteral("The selected native power source is unavailable.");
    }
    if (scope==DefaultsScope::Battery || scope==DefaultsScope::AllPolicies)
        for (auto key:PowerPolicyFields::globalKeys()) inspect(m_applied,m_staged,key,QStringLiteral("Battery — "),key==PowerPolicyKey::BatteryCriticalAction ? m_applied.batteryCriticalActions : QList<PowerPolicyAction>{});
    return review;
}
void PowerPolicySettings::stageDefaults(DefaultsScope scope, const QString &id, const QByteArray &reviewRevision)
{
    const auto review=reviewDefaults(scope,id);
    if (reviewRevision!=review.revision) {
        m_error=QStringLiteral("The settings or staged choices changed after this defaults review. Review defaults again."); emit stateChanged(); return;
    }
    if (!review.hasChanges() || m_busy || !m_ready || m_conflict) return;
    auto requested=m_staged; QString error;
    if (scope==DefaultsScope::SelectedProfile || scope==DefaultsScope::AllProfiles || scope==DefaultsScope::AllPolicies)
        for (int i=0;i<m_applied.profiles.size();++i) {
            const auto &baseline=m_applied.profiles[i]; if (scope==DefaultsScope::SelectedProfile && baseline.id!=id) continue;
            for (auto key:PowerPolicyFields::profileKeys()) PowerPolicyFields::restore(requested.profiles[i],baseline,key,error);
        }
    if (scope==DefaultsScope::Battery || scope==DefaultsScope::AllPolicies)
        for (auto key:PowerPolicyFields::globalKeys()) PowerPolicyFields::restore(requested,m_applied,key,error);
    m_staged=requested; edited();
}
QString PowerPolicySettings::applySupportText() const
{
    if (m_busy) return m_pendingMessage.isEmpty()
        ? QStringLiteral("Native power settings are being checked or saved.") : m_pendingMessage;
    if (!m_ready || m_conflict) return QStringLiteral("Revert to reload the native power settings before applying.");
    QString error; PowerPolicyFields::requestValid(m_applied,m_staged,error); return error;
}
void PowerPolicySettings::apply()
{
    if (m_busy)
        return;
    if (!m_ready || m_conflict) {
        m_applyError = true;
        m_error = QStringLiteral("Power settings changed or could not be read. Revert to reload before applying.");
        QPointer<PowerPolicySettings> guard(this);
        emit applyFinished(false);
        if (guard) emit stateChanged();
        return;
    }
    if (!m_dirty) {
        emit applyFinished(true);
        return;
    }
    const auto support = applySupportText();
    if (!support.isEmpty()) {
        m_error = support; m_applyError = true; QPointer<PowerPolicySettings> guard(this);
        emit applyFinished(false); if (guard) emit stateChanged(); return;
    }
    m_requested = m_staged;
    m_error.clear();
    m_result.clear();
    m_cancelled = false;
    m_applyError = false;
    m_pendingMessage.clear();
    m_busy = true;
    m_phase = Phase::Preflight;
    QPointer<PowerPolicySettings> guard(this);
    emit stateChanged();
    if (guard) m_backend->readCurrent();
}
void PowerPolicySettings::revert()
{
    if (m_busy)
        return;
    m_reverting = true;
    startRefresh();
}
void PowerPolicySettings::refresh()
{
    if (m_busy) {
        m_refreshPending = true;
        return;
    }
    startRefresh();
}
void PowerPolicySettings::startRefresh()
{
    m_refreshPending = false;
    m_phase = Phase::Refreshing;
    m_busy = true;
    m_pendingMessage.clear();
    QPointer<PowerPolicySettings> guard(this);
    emit stateChanged();
    if (guard) m_backend->readCurrent();
}
void PowerPolicySettings::cancel()
{
    if (m_phase == Phase::Writing)
        m_backend->cancel();
}
void PowerPolicySettings::writeFinished(bool saved, bool acknowledged, bool cancelled, const QString &error)
{
    if (m_phase != Phase::Writing)
        return;
    m_saved = saved;
    m_acknowledged = acknowledged;
    m_cancelled = cancelled;
    m_writeError = error;
    m_pendingMessage.clear();
    m_phase = Phase::Readback;
    m_backend->readCurrent();
}
void PowerPolicySettings::readFinished(bool success, const PowerPolicySnapshot &snapshot, const QString &error)
{
    if (m_phase != Phase::Preflight && m_phase != Phase::Refreshing && m_phase != Phase::Readback)
        return;
    if (m_reverting) {
        m_reverting = false;
        if (success) {
            m_applied = m_staged = snapshot;
            m_ready = true; m_dirty = m_conflict = m_applyError = m_cancelled = false;
            m_error.clear(); m_result.clear();
        } else {
            m_ready = false;
            m_conflict = m_conflict || m_dirty;
            m_error = error.isEmpty() ? QStringLiteral("Power settings could not be reloaded. Your draft has been retained; retry Revert.") : error;
        }
        finish(); return;
    }
    if (m_phase == Phase::Preflight) {
        if (success && snapshot == m_applied && snapshot.serviceAvailable) {
            m_phase = Phase::Writing;
            m_backend->writePolicy(m_applied, m_requested);
            return;
        }
        if (success) {
            m_applied = snapshot;
            m_dirty = true;
        } else {
            m_ready = false;
        }
        m_conflict = true;
        m_applyError = true;
        m_error = error.isEmpty()
            ? QStringLiteral("Power settings or capabilities changed outside Control Panel. Revert to reload them.") : error;
        finish(true, false);
        return;
    }
    const auto previous = m_applied;
    const bool readback = m_phase == Phase::Readback;
    const bool changed = m_ready && success && snapshot != m_applied;
    const bool unresolved = m_applyError && m_conflict;
    if (success) {
        m_applied = snapshot;
        m_ready = true;
        if (!m_dirty && !unresolved)
            m_staged = snapshot;
        m_dirty = unresolved || m_staged != m_applied;
        if (!readback && changed && m_dirty)
            m_conflict = true;
        if (!m_dirty && !(m_applyError && m_conflict))
            m_conflict = false;
    } else {
        m_ready = false;
        if (m_dirty || readback)
            m_conflict = true;
    }
    bool applied = false;
    if (readback) {
        QString verificationError;
        applied = m_saved && m_acknowledged && !m_cancelled && m_writeError.isEmpty() && success
            && PowerPolicyFields::matchesRequest(previous, m_requested, snapshot, verificationError);
        m_applyError = !applied;
        if (applied) {
            m_staged = snapshot;
            m_dirty = false;
            m_conflict = false;
            m_error.clear();
            m_result = QStringLiteral("Native power policy operations saved and verified. PowerDevil acknowledged a configuration reload. Unverified effective defaults remain marked; activities and applications may inhibit idle actions.");
        } else {
            m_result.clear();
            m_error = m_cancelled
                ? QStringLiteral("Waiting for the power operation was cancelled. %1").arg(m_writeError.isEmpty()
                    ? QStringLiteral("Saved preferences or a native reload may already have changed; Revert to inspect.") : m_writeError)
                : !m_writeError.isEmpty() ? m_writeError
                : !success ? QStringLiteral("Power policy readback failed. %1").arg(error)
                : !m_acknowledged ? QStringLiteral("Power policy was saved, but PowerDevil did not acknowledge the live reload.")
                : !verificationError.isEmpty() ? verificationError
                : QStringLiteral("PowerDevil policy readback differs from your selection. Revert to reload before retrying.");
            // A saved file without runtime acknowledgement is not an applied
            // session. Require an explicit reload before the next edit/write.
            m_conflict = true;
            m_dirty = true;
        }
    } else if (!m_applyError) {
        m_error = success ? QString() : error;
        if (m_conflict)
            m_error = QStringLiteral("Power settings changed outside Control Panel. Revert to reload them.");
    }
    finish(readback, applied);
}
void PowerPolicySettings::finish(bool applying, bool success)
{
    m_phase = Phase::Idle;
    m_busy = false;
    m_pendingMessage.clear();
    QPointer<PowerPolicySettings> guard(this);
    if (applying)
        emit applyFinished(success);
    if (!guard) return;
    emit stateChanged();
    if (!guard) return;
    if (m_refreshPending)
        QTimer::singleShot(0, this, [this] {
            if (!m_busy && m_refreshPending)
                startRefresh();
        });
}
