#include "AeroEffectSettings.h"
#include "platform/kde/AeroEffectBackend.h"
#include <QCoreApplication>
#include <QPointer>
#include <QTimer>

AeroEffectSettings *AeroEffectSettings::instance()
{
    static QPointer<AeroEffectSettings> session;
    if (!session)
        session = new AeroEffectSettings(new KdeAeroEffectBackend, QCoreApplication::instance());
    return session;
}

AeroEffectSettings::AeroEffectSettings(AeroEffectBackend *backend, QObject *parent)
    : SettingsSession(parent), m_backend(backend)
{
    Q_ASSERT(backend);
    backend->setParent(this);
    connect(backend, &AeroEffectBackend::readFinished, this, &AeroEffectSettings::onRead);
    connect(backend, &AeroEffectBackend::writeFinished, this, &AeroEffectSettings::onWrite);
    connect(backend, &AeroEffectBackend::externalChange, this, &AeroEffectSettings::refresh);
    m_busy = true;
    QTimer::singleShot(0, this, &AeroEffectSettings::read);
}

void AeroEffectSettings::selectStrength(int strength)
{
    if (m_busy || !canApply() || !supportedStrength(strength))
        return;
    m_staged = strength;
    m_dirty = m_staged != appliedStrength();
    m_error.clear(); m_result.clear(); m_keepError = false;
    emit stateChanged();
}

void AeroEffectSettings::apply()
{
    if (m_busy) return;
    if (!canApply() || m_conflict || !supportedStrength(m_staged)) {
        m_error = m_conflict ? tr("Aero Glass Blur changed outside Control Panel. Revert before applying.")
            : !m_state.unavailableReason.isEmpty() ? m_state.unavailableReason
            : tr("Aero Glass Blur is unavailable. Revert to reload its capabilities.");
        m_keepError = true;
        emit applyFinished(false); emit stateChanged(); return;
    }
    if (!m_dirty) { emit applyFinished(true); return; }
    m_requested = m_staged;
    m_error.clear(); m_result.clear(); m_keepError = false;
    m_busy = true; m_phase = Phase::Preflight;
    emit stateChanged(); m_backend->readCurrent();
}

void AeroEffectSettings::revert()
{
    if (m_busy) return;
    m_staged = appliedStrength(); m_dirty = m_conflict = m_keepError = false;
    m_error.clear(); m_result.clear(); read();
}

void AeroEffectSettings::refresh()
{
    if (m_busy) { m_pendingRefresh = true; return; }
    read();
}

void AeroEffectSettings::read()
{
    m_pendingRefresh = false; m_phase = Phase::Refresh; m_busy = true;
    emit stateChanged(); m_backend->readCurrent();
}

void AeroEffectSettings::onRead(bool success, const AeroEffectState &state, const QString &error)
{
    if (m_phase != Phase::Refresh && m_phase != Phase::Preflight && m_phase != Phase::Readback) return;
    if (m_phase == Phase::Preflight) {
        const bool same = success && state.blurStrength == appliedStrength();
        if (success) { m_state = state; m_ready = true; }
        if (same && state.canApply()) {
            m_phase = Phase::Write;
            m_backend->writeStrength(appliedStrength(), m_requested); return;
        }
        m_ready = success; m_dirty = m_staged != appliedStrength(); m_conflict = m_keepError = true;
        m_error = !success ? (error.isEmpty() ? tr("Aero Glass Blur could not be checked before Apply.") : error)
            : !same ? tr("Blur strength changed before Apply. Revert to reload it.")
            : state.unavailableReason;
        finish(true, false); return;
    }
    const bool readback = m_phase == Phase::Readback;
    const bool wasDirty = m_dirty;
    const bool changed = success && m_ready && state.blurStrength != appliedStrength();
    if (success) {
        m_state = state; m_ready = true;
        if (!wasDirty) m_staged = appliedStrength();
        m_dirty = m_staged != appliedStrength();
        if (!readback && changed && wasDirty) m_conflict = true;
    } else {
        m_ready = false;
        if (wasDirty) m_conflict = true;
    }
    bool applied = false;
    if (readback) {
        applied = m_writeSuccess && success && state.blurStrength == m_requested
            && state.pluginSupported && state.runtimeSupported && state.effectLoaded;
        m_conflict = m_keepError = !applied;
        if (applied) {
            m_error.clear();
            m_result = tr("Saved blur strength verified. KWin acknowledged reconfiguring Aero Glass Blur and reports it loaded. "
                          "The effect does not expose its rendered blur strength for verification.");
        } else {
            m_result.clear();
            m_error = !m_writeSuccess ? m_writeError
                : !success ? tr("The preference may be saved, but readback failed. Revert to reload it. %1").arg(error)
                : tr("The saved strength or Aero effect availability changed during Apply. Revert before retrying.");
        }
    } else if (!m_keepError) {
        m_error = success ? QString() : (error.isEmpty() ? tr("Aero Glass Blur preferences could not be read.") : error);
    }
    finish(readback, applied);
}

void AeroEffectSettings::onWrite(bool success, const QString &error)
{
    if (m_phase != Phase::Write) return;
    m_writeSuccess = success;
    m_writeError = error.isEmpty() ? tr("The preference may be saved, but Aero Glass Blur could not be reconfigured. Revert before retrying.") : error;
    m_phase = Phase::Readback; m_backend->readCurrent();
}

void AeroEffectSettings::finish(bool completesApply, bool success)
{
    m_busy = false; m_phase = Phase::Idle;
    if (completesApply) emit applyFinished(success);
    emit stateChanged();
    if (m_pendingRefresh)
        QTimer::singleShot(0, this, [this] { if (m_pendingRefresh && !m_busy) read(); });
}
