#include "AppearanceSettings.h"
#include "platform/kde/AppearanceBackend.h"

#include <QCoreApplication>
#include <QPointer>
#include <QTimer>

AppearanceSettings *AppearanceSettings::instance()
{
    static QPointer<AppearanceSettings> session;
    if (!session)
        session = new AppearanceSettings(new KdeAppearanceBackend, QCoreApplication::instance());
    return session;
}

AppearanceSettings::AppearanceSettings(AppearanceBackend *backend, QObject *parent)
    : SettingsSession(parent), m_backend(backend)
{
    Q_ASSERT(backend);
    backend->setParent(this);
    connect(backend, &AppearanceBackend::readFinished, this, &AppearanceSettings::onRead);
    connect(backend, &AppearanceBackend::writeFinished, this, &AppearanceSettings::onWrite);
    connect(backend, &AppearanceBackend::externalChange, this, &AppearanceSettings::refresh);
    connect(backend, &AppearanceBackend::operationPending, this, [this](const QString &message) {
        if (m_phase != Phase::Write) return;
        m_pendingMessage = message; emit stateChanged();
    });
    m_busy = true;
    QTimer::singleShot(0, this, &AppearanceSettings::read);
}

bool AppearanceSettings::isSupportedAction(const QString &action)
{
    return action == QLatin1String("Maximize") || action == QLatin1String("Minimize")
        || action == QLatin1String("Nothing");
}

void AppearanceSettings::selectAction(const QString &action)
{
    if (m_busy || !m_ready || !isSupportedAction(action))
        return;
    m_staged = action;
    m_dirty = m_staged != appliedAction();
    if (!m_dirty)
        m_conflict = false;
    m_error.clear();
    m_result.clear();
    m_keepError = false;
    emit stateChanged();
}

void AppearanceSettings::apply()
{
    if (m_busy)
        return;
    if (!canApply() || m_conflict) {
        m_error = m_conflict ? tr("Window behavior changed outside Control Panel. Revert to reload it before applying.")
            : !m_ready ? tr("The current window behavior could not be read. Revert to retry.")
            : unavailableReason();
        m_keepError = true;
        QPointer<AppearanceSettings> guard(this);
        emit applyFinished(false);
        if (guard) emit stateChanged();
        return;
    }
    if (!m_dirty) {
        emit applyFinished(true);
        return;
    }
    m_requested = m_staged;
    m_error.clear();
    m_result.clear();
    m_keepError = false;
    m_busy = true; m_pendingMessage.clear();
    m_phase = Phase::Preflight;
    QPointer<AppearanceSettings> guard(this); emit stateChanged();
    if (guard) m_backend->readCurrent();
}

void AppearanceSettings::revert()
{
    if (m_busy)
        return;
    m_reverting = true;
    read();
}

void AppearanceSettings::refresh()
{
    if (m_busy) {
        m_pendingRefresh = true;
        return;
    }
    read();
}

void AppearanceSettings::read()
{
    m_pendingRefresh = false;
    m_phase = Phase::Refresh;
    m_busy = true; m_pendingMessage.clear();
    QPointer<AppearanceSettings> guard(this); emit stateChanged();
    if (guard) m_backend->readCurrent();
}

void AppearanceSettings::onRead(bool success, const AppearanceState &state, const QString &error)
{
    if (m_phase != Phase::Refresh && m_phase != Phase::Preflight && m_phase != Phase::Readback)
        return;
    const bool valid = success && !state.doubleClickAction.isEmpty();
    if (m_phase == Phase::Preflight) {
        if (valid && state.doubleClickAction == appliedAction() && state.canApply()) {
            m_state = state;
            m_phase = Phase::Write;
            m_backend->writeAction(appliedAction(), m_requested);
            return;
        }
        if (valid) {
            m_state = state;
            m_dirty = m_staged != appliedAction();
            m_conflict = m_dirty;
            m_error = !state.canApply() ? state.unavailableReason
                : tr("Window behavior changed before Apply. Revert to reload it.");
        } else {
            m_ready = false;
            m_conflict = true;
            m_error = error.isEmpty() ? tr("Window behavior could not be checked before applying.") : error;
        }
        m_keepError = true;
        finish(true, false);
        return;
    }
    const bool readback = m_phase == Phase::Readback;
    if (!readback && m_reverting) {
        m_reverting = false;
        if (valid) {
            m_dirty = false; m_conflict = false; m_keepError = false;
            m_error.clear(); m_result.clear();
        } else {
            m_error = error.isEmpty() ? tr("Window behavior could not be reloaded; your selection is preserved.") : error;
            m_keepError = true;
        }
    }
    const bool wasDirty = m_dirty;
    const bool changed = m_ready && valid && state.doubleClickAction != appliedAction();
    if (valid) {
        m_state = state;
        m_ready = true;
        if (!wasDirty)
            m_staged = appliedAction();
        m_dirty = m_staged != appliedAction();
        if (!readback && m_keepError && m_conflict && wasDirty) m_dirty = true;
        if (!readback && changed && wasDirty && m_dirty)
            m_conflict = true;
        if (!m_dirty && !m_keepError)
            m_conflict = false;
    } else {
        m_ready = false;
        if (wasDirty)
            m_conflict = true;
    }
    bool applied = false;
    if (readback) {
        applied = m_writeSuccess && m_writeError.isEmpty() && valid && appliedAction() == m_requested;
        m_keepError = !applied;
        if (applied) {
            m_error.clear();
            m_result = tr("Window behavior saved. The saved preference and KWin's active title-bar action were verified.");
            m_conflict = false;
        } else {
            m_staged = m_requested; m_dirty = true;
            m_result.clear();
            m_error = !m_writeSuccess || !m_writeError.isEmpty() ? m_writeError
                : !valid ? tr("The setting was saved, but readback failed. Revert to reload it. %1").arg(error)
                : tr("KDE reports a different window behavior. Your selection is still staged.");
            // Another write requires an explicit reconciliation after uncertain apply.
            m_conflict = true;
        }
    } else if (!valid && !m_keepError) {
        m_error = error.isEmpty() ? tr("The current window behavior could not be read.") : error;
    } else if (valid && !m_keepError) {
        m_error.clear();
    }
    finish(readback, applied);
}

void AppearanceSettings::onWrite(bool success, const QString &error)
{
    if (m_phase != Phase::Write)
        return;
    m_writeSuccess = success;
    m_writeError = !error.isEmpty() ? error : success ? QString() : tr("Window behavior could not be saved or reloaded.");
    m_pendingMessage.clear(); m_phase = Phase::Readback;
    m_backend->readCurrent();
}

void AppearanceSettings::finish(bool completesApply, bool success)
{
    m_busy = false;
    m_phase = Phase::Idle; m_pendingMessage.clear();
    QPointer<AppearanceSettings> guard(this);
    if (completesApply) emit applyFinished(success);
    if (!guard) return;
    emit stateChanged();
    if (!guard) return;
    if (m_pendingRefresh)
        QTimer::singleShot(0, this, [this] { if (m_pendingRefresh && !m_busy) read(); });
}
