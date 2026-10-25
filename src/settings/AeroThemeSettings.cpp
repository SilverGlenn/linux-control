#include "AeroThemeSettings.h"
#include "AeroThemeContract.h"
#include "platform/kde/AeroThemeBackend.h"
#include <QCoreApplication>
#include <QColor>
#include <QPointer>
#include <QTimer>

AeroThemeSettings *AeroThemeSettings::instance()
{
    static QPointer<AeroThemeSettings> session;
    if (!session)
        session = new AeroThemeSettings(new KdeAeroThemeBackend, QCoreApplication::instance());
    return session;
}

AeroThemeSettings::AeroThemeSettings(AeroThemeBackend *backend, QObject *parent)
    : SettingsSession(parent), m_backend(backend)
{
    Q_ASSERT(backend);
    backend->setParent(this);
    connect(backend, &AeroThemeBackend::readFinished, this, &AeroThemeSettings::onRead);
    connect(backend, &AeroThemeBackend::writeFinished, this, &AeroThemeSettings::onWrite);
    connect(backend, &AeroThemeBackend::operationPending, this, [this](const QString &message) {
        if (m_phase != Phase::Write || !m_busy) return;
        m_result = message;
        emit stateChanged();
    });
    connect(backend, &AeroThemeBackend::externalChange, this, &AeroThemeSettings::refresh);
    m_busy = true;
    QTimer::singleShot(0, this, &AeroThemeSettings::read);
}

bool AeroThemeSettings::canApply() const
{
    QString error;
    return m_ready && m_state.canApply()
        && AeroThemeContract::validateRequested(m_staged, error);
}

bool AeroThemeSettings::canEdit(const QString &key) const
{
    return m_ready && !m_busy && m_state.assetsSupported && m_state.runtimeSupported
        && m_state.writableKeys.contains(key) && m_state.values.contains(key)
        && !AeroThemeContract::identities().contains(key);
}

QStringList AeroThemeSettings::changeSummary() const
{
    QStringList result;
    for (const auto &field : AeroThemeContract::fields()) {
        if (field.key == QLatin1String("accentColorGroup") || field.key == QLatin1String("accentColorName")) continue;
        if (m_staged.value(field.key) != m_state.values.value(field.key)) {
            if (field.defaultValue.metaType().id() == QMetaType::QVariantMap) {
                result << tr("%1: saved palette → approved Aero palette").arg(field.label);
                continue;
            }
            const bool color = field.key == QLatin1String("customColor");
            result << tr("%1: %2 → %3").arg(color ? tr("Window color") : field.label,
                color ? QColor(m_state.values.value(field.key).toString()).name() : m_state.values.value(field.key).toString(),
                color ? QColor(m_staged.value(field.key).toString()).name() : m_staged.value(field.key).toString());
        }
    }
    return result;
}

void AeroThemeSettings::stage(const QVariantMap &values)
{
    m_staged = values;
    m_dirty = m_staged != m_state.values;
    if (!m_conflict) {
        m_error.clear();
        m_keepError = false;
        m_partialFailure = false;
    }
    m_result.clear();
    emit stateChanged();
}

void AeroThemeSettings::selectValue(const QString &key, const QVariant &value)
{
    if (!canEdit(key)) return;
    QVariantMap next = m_staged;
    next.insert(key, value);
    const auto identities = AeroThemeContract::identities();
    for (auto it = identities.cbegin(); it != identities.cend(); ++it)
        next.insert(it.key(), it.value());
    QString error;
    if (!AeroThemeContract::validateRequested(next, error)) {
        if (!m_conflict)
            m_error = tr("This customization could not be selected. %1 Restore Aero defaults to repair unsupported saved values.").arg(error);
        emit stateChanged();
        return;
    }
    const bool colorEdit = key == QLatin1String("aeroHue") || key == QLatin1String("aeroSaturation")
        || key == QLatin1String("aeroBrightness") || key == QLatin1String("aeroIntensity")
        || key == QLatin1String("customColor");
    if (colorEdit) {
        if (key == QLatin1String("customColor")) {
            const QColor color(value.toString());
            next[QStringLiteral("aeroHue")] = qMax(0, color.hsvHue());
            next[QStringLiteral("aeroSaturation")] = qRound(color.hsvSaturationF() * 100);
            next[QStringLiteral("aeroBrightness")] = qRound(color.valueF() * 100);
        }
        next[QStringLiteral("accentColorGroup")] = 0;
        next[QStringLiteral("accentColorName")] = 0;
        const QColor color = QColor::fromHsvF(next.value(QStringLiteral("aeroHue")).toInt() / 360.0,
            next.value(QStringLiteral("aeroSaturation")).toInt() / 100.0,
            next.value(QStringLiteral("aeroBrightness")).toInt() / 100.0,
            next.value(QStringLiteral("aeroIntensity")).toInt() / 255.0);
        next[QStringLiteral("customColor")] = color.name(QColor::HexArgb);
    }
    if (!AeroThemeContract::validateRequested(next, error)) return;
    stage(next);
}

void AeroThemeSettings::stageRestoreDefaults()
{
    if (!m_ready || m_busy) return;
    QPointer<AeroThemeSettings> guard(this);
    stage(AeroThemeContract::defaults());
    if (!guard) return;
    if (!m_conflict)
        m_result = tr("Aero defaults are staged. Review the changes, then click Apply to save them.");
    emit stateChanged();
}

void AeroThemeSettings::apply()
{
    if (m_busy) return;
    if (m_conflict || !canApply()) {
        m_error = m_conflict ? tr("Aero settings changed or an earlier Apply is uncertain. Revert before applying.")
            : tr("Aero settings cannot be applied. Check the component, runtime, and policy status below.");
        m_keepError = true;
        QPointer<AeroThemeSettings> guard(this);
        emit applyFinished(false); if (!guard) return;
        emit stateChanged(); return;
    }
    if (!m_dirty) { emit applyFinished(true); return; }
    m_requested = m_staged;
    m_error.clear(); m_result.clear(); m_keepError = m_partialFailure = false;
    m_busy = true; m_phase = Phase::Preflight;
    QPointer<AeroThemeSettings> guard(this);
    const auto phase = m_phase;
    emit stateChanged(); if (!guard || m_phase != phase || !m_busy) return;
    m_backend->readCurrent();
}

void AeroThemeSettings::revert()
{
    if (m_busy) return;
    m_staged = m_state.values;
    m_dirty = m_conflict = m_keepError = m_partialFailure = false;
    m_error.clear(); m_result.clear(); read();
}

void AeroThemeSettings::refresh()
{
    if (m_busy) { m_pendingRefresh = true; return; }
    read();
}

void AeroThemeSettings::read()
{
    m_pendingRefresh = false; m_phase = Phase::Refresh; m_busy = true;
    QPointer<AeroThemeSettings> guard(this);
    const auto phase = m_phase;
    emit stateChanged(); if (!guard || m_phase != phase || !m_busy) return;
    m_backend->readCurrent();
}

void AeroThemeSettings::onRead(bool success, const AeroThemeState &state, const QString &error)
{
    if (m_phase != Phase::Refresh && m_phase != Phase::Preflight && m_phase != Phase::Readback) return;
    const bool valid = success && !state.values.isEmpty();
    if (m_phase == Phase::Preflight) {
        const bool same = valid && state.values == m_state.values;
        if (valid) { m_state = state; m_ready = true; }
        if (same && canApply()) {
            m_beforeWrite = state.values; m_phase = Phase::Write;
            m_backend->writeSettings(state.values, m_requested); return;
        }
        m_ready = valid; m_dirty = m_staged != m_state.values;
        m_conflict = m_keepError = true;
        m_error = !valid ? (error.isEmpty() ? tr("Aero settings could not be checked before Apply.") : error)
            : !same ? tr("Aero settings changed before Apply. Your staged changes are preserved; Revert to reload.")
            : tr("Aero assets, runtime support, or write policy changed before Apply. %1").arg(state.issues.join(QLatin1Char('\n')));
        finish(true, false); return;
    }
    const bool readback = m_phase == Phase::Readback;
    const bool wasDirty = m_dirty;
    const bool changed = valid && m_ready && state.values != m_state.values;
    if (valid) {
        m_state = state; m_ready = true;
        if (!wasDirty) m_staged = state.values;
        m_dirty = m_staged != state.values;
        if (!readback && changed && wasDirty) m_conflict = true;
    } else {
        m_ready = false;
        if (wasDirty) m_conflict = true;
    }
    bool applied = false;
    if (readback) {
        applied = m_writeSuccess && valid && state.values == m_requested && state.canApply();
        m_keepError = m_conflict = !applied;
        m_partialFailure = !applied && (!valid || state.values != m_beforeWrite);
        if (applied) {
            m_error.clear();
            m_result = tr("Saved Aero preferences verified by readback; runtime reload requests succeeded. "
                          "Rendered appearance is not verified. Some applications and pointers may need a new session.");
        } else {
            m_result.clear();
            m_error = !m_writeSuccess ? m_writeError
                : !valid ? tr("Readback failed after Apply. Some settings may have been saved. %1").arg(error)
                : tr("Saved Aero settings or runtime capabilities differ from the requested values.");
            m_error += tr("\nApply was not fully verified. Saved values are shown where available; staged choices are preserved. Revert before retrying.");
        }
    } else if (!m_keepError) {
        m_error = valid ? QString() : (error.isEmpty() ? tr("Aero settings could not be read.") : error);
    }
    finish(readback, applied);
}

void AeroThemeSettings::onWrite(bool success, const QString &error)
{
    if (m_phase != Phase::Write) return;
    m_writeSuccess = success;
    m_writeError = error.isEmpty() ? tr("Some Aero settings may have been saved, but runtime reload failed.") : error;
    m_phase = Phase::Readback; m_backend->readCurrent();
}

void AeroThemeSettings::finish(bool completesApply, bool success)
{
    m_busy = false; m_phase = Phase::Idle;
    QPointer<AeroThemeSettings> guard(this);
    if (completesApply) emit applyFinished(success);
    if (!guard) return;
    emit stateChanged();
    if (!guard) return;
    if (m_pendingRefresh)
        QTimer::singleShot(0, this, [this] { if (m_pendingRefresh && !m_busy) read(); });
}
