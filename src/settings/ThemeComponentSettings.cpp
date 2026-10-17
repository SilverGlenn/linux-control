#include "ThemeComponentSettings.h"
#include "AeroThemeContract.h"
#include "platform/kde/ThemeComponentBackend.h"
#include <QCoreApplication>
#include <QPointer>
#include <QTimer>

QStringList ThemeComponentState::values() const
{
    QStringList result;
    for (const auto &field : fields)
        result << field.value;
    return result;
}

ThemeComponentSettings *ThemeComponentSettings::instance()
{
    static QPointer<ThemeComponentSettings> session;
    if (!session)
        session = new ThemeComponentSettings(new KdeThemeComponentBackend, QCoreApplication::instance());
    return session;
}

ThemeComponentSettings::ThemeComponentSettings(ThemeComponentBackend *backend, QObject *parent)
    : SettingsSession(parent), m_backend(backend)
{
    Q_ASSERT(backend);
    backend->setParent(this);
    connect(backend, &ThemeComponentBackend::readFinished, this, &ThemeComponentSettings::onRead);
    connect(backend, &ThemeComponentBackend::writeFinished, this, &ThemeComponentSettings::onWrite);
    connect(backend, &ThemeComponentBackend::externalChange, this, &ThemeComponentSettings::refresh);
    m_busy = true;
    QTimer::singleShot(0, this, &ThemeComponentSettings::read);
}

const ThemeComponentField &ThemeComponentSettings::field(ThemeComponent component) const
{
    return m_state.fields.at(static_cast<int>(component));
}

QString ThemeComponentSettings::appliedValue(ThemeComponent component) const { return field(component).value; }
QString ThemeComponentSettings::stagedValue(ThemeComponent component) const { return m_staged.at(static_cast<int>(component)); }

bool ThemeComponentSettings::containsChoice(const ThemeComponentField &field, const QString &id)
{
    for (const auto &choice : field.choices)
        if (choice.id == id)
            return true;
    return false;
}

bool ThemeComponentSettings::isApprovedChoice(ThemeComponent component, const QString &id)
{
    const auto identities = AeroThemeContract::identities();
    switch (component) {
    case ThemeComponent::Icons: return id == identities.value(QStringLiteral("iconTheme")).toString();
    case ThemeComponent::Cursors: return id == identities.value(QStringLiteral("cursorTheme")).toString();
    case ThemeComponent::ApplicationStyle: return id == identities.value(QStringLiteral("applicationStyle")).toString();
    }
    return false;
}

bool ThemeComponentSettings::canApply() const
{
    if (!m_ready)
        return false;
    for (int i = 0; i < themeComponentCount; ++i)
        if (m_staged[i] != m_state.fields[i].value
            && (!isApprovedChoice(static_cast<ThemeComponent>(i), m_staged[i])
                || !m_state.fields[i].canApply() || !containsChoice(m_state.fields[i], m_staged[i])))
            return false;
    return true;
}

void ThemeComponentSettings::selectComponent(ThemeComponent component, const QString &id)
{
    const int i = static_cast<int>(component);
    if (i < 0 || i >= themeComponentCount || m_busy || !m_ready
        || !isApprovedChoice(component, id)
        || !m_state.fields[i].canApply() || !containsChoice(m_state.fields[i], id))
        return;
    m_staged[i] = id;
    m_dirty = m_staged != m_state.values();
    // A conflict requires explicit Revert even if a selection is changed back.
    m_error.clear();
    m_result.clear();
    m_keepError = false;
    emit stateChanged();
}

void ThemeComponentSettings::apply()
{
    if (m_busy)
        return;
    if (m_conflict || !canApply()) {
        m_error = m_conflict ? tr("Personalization changed outside Control Panel. Revert before applying.")
            : tr("A selected component is unavailable or locked. Revert to reload its capabilities.");
        m_keepError = true;
        emit applyFinished(false);
        emit stateChanged();
        return;
    }
    if (!m_dirty) { emit applyFinished(true); return; }
    m_requested = m_staged;
    m_error.clear();
    m_result.clear();
    m_keepError = false;
    m_busy = true;
    m_phase = Phase::Preflight;
    emit stateChanged();
    m_backend->readCurrent();
}

void ThemeComponentSettings::revert()
{
    if (m_busy)
        return;
    m_dirty = false;
    m_conflict = false;
    m_keepError = false;
    m_error.clear();
    m_result.clear();
    m_staged = m_state.values();
    read();
}

void ThemeComponentSettings::refresh()
{
    if (m_busy) { m_pendingRefresh = true; return; }
    read();
}

void ThemeComponentSettings::read()
{
    m_pendingRefresh = false;
    m_phase = Phase::Refresh;
    m_busy = true;
    emit stateChanged();
    m_backend->readCurrent();
}

void ThemeComponentSettings::onRead(bool success, const ThemeComponentState &state, const QString &error)
{
    if (m_phase != Phase::Refresh && m_phase != Phase::Preflight && m_phase != Phase::Readback)
        return;
    bool valid = success;
    for (const auto &field : state.fields)
        valid = valid && !field.value.isEmpty();
    if (m_phase == Phase::Preflight) {
        const bool same = valid && state.values() == m_state.values();
        if (valid) { m_state = state; m_ready = true; }
        if (same && canApply()) {
            m_phase = Phase::Write;
            m_backend->writeComponents(m_state.values(), m_requested);
            return;
        }
        m_ready = valid;
        m_dirty = m_staged != m_state.values();
        m_conflict = true;
        m_keepError = true;
        m_error = !valid ? (error.isEmpty() ? tr("Personalization could not be checked before Apply.") : error)
            : !same ? tr("Personalization changed before Apply. Revert to reload the saved preferences.")
            : tr("A selected component became unavailable or locked before Apply.");
        finish(true, false);
        return;
    }
    const bool readback = m_phase == Phase::Readback;
    const bool changed = m_ready && valid && state.values() != m_state.values();
    const bool wasDirty = m_dirty;
    if (valid) {
        m_state = state;
        m_ready = true;
        if (!wasDirty)
            m_staged = state.values();
        m_dirty = m_staged != state.values();
        if (!readback && changed && wasDirty)
            m_conflict = true;
    } else {
        m_ready = false;
        if (wasDirty)
            m_conflict = true;
    }
    bool applied = false;
    if (readback) {
        applied = m_writeSuccess && valid && state.values() == m_requested;
        m_keepError = !applied;
        m_conflict = !applied;
        if (applied) {
            m_error.clear();
            m_result = tr("Saved preferences verified by readback; KDE reload notifications were requested. "
                          "Some applications and X11 cursors may require signing out and back in.");
        } else {
            m_result.clear();
            m_error = !m_writeSuccess ? m_writeError
                : !valid ? tr("Readback failed after saving. Revert to reload the preferences. %1").arg(error)
                : tr("KDE reports different saved preferences. Your selection is preserved; Revert before retrying.");
        }
    } else if (!m_keepError) {
        m_error = valid ? QString() : (error.isEmpty() ? tr("Personalization preferences could not be read.") : error);
    }
    finish(readback, applied);
}

void ThemeComponentSettings::onWrite(bool success, const QString &error)
{
    if (m_phase != Phase::Write)
        return;
    m_writeSuccess = success;
    m_writeError = error.isEmpty() ? tr("Some preferences may have been saved, but their reload requests failed. Revert before retrying.") : error;
    m_phase = Phase::Readback;
    m_backend->readCurrent();
}

void ThemeComponentSettings::finish(bool completesApply, bool success)
{
    m_busy = false;
    m_phase = Phase::Idle;
    if (completesApply)
        emit applyFinished(success);
    emit stateChanged();
    if (m_pendingRefresh)
        QTimer::singleShot(0, this, [this] { if (m_pendingRefresh && !m_busy) read(); });
}
