#include "DisplaySettings.h"
#include "display/KdeDisplayBackend.h"

#include <QCoreApplication>
#include <QPointer>
#include <cmath>

DisplaySettings *DisplaySettings::instance()
{
    static QPointer<DisplaySettings> session;
    if (!session) session = new DisplaySettings(new KdeDisplayBackend, QCoreApplication::instance());
    return session;
}

DisplaySettings::DisplaySettings(DisplayBackend *backend, QObject *parent)
    : SettingsSession(parent), m_backend(backend)
{
    Q_ASSERT(backend);
    backend->setParent(this);
    connect(backend, &DisplayBackend::readFinished, this, &DisplaySettings::readDone);
    connect(backend, &DisplayBackend::transactionFinished, this, &DisplaySettings::finish);
    connect(backend, &DisplayBackend::externalChange, this, &DisplaySettings::refresh);
    connect(backend, &DisplayBackend::previewReady, this,
        [this](const DisplaySnapshot &snapshot, int seconds) {
            if (m_phase != Phase::Preview) return;
            if (!DisplayProtocol::matchesRequested(m_snapshot, m_staged, snapshot)) {
                m_error = QStringLiteral("The display changed unexpectedly. Restoring the previous display settings…");
                m_backend->revertPreview();
                emit stateChanged();
                return;
            }
            m_confirming = true;
            m_seconds = qBound(1, seconds, 15);
            m_elapsed.start();
            m_ticks.start(200);
            emit stateChanged();
        });
    connect(&m_ticks, &QTimer::timeout, this, [this] {
        if (remainingSeconds() == 0 && !m_resolutionPending) {
            m_resolutionPending = true;
            m_backend->revertPreview();
        }
        emit stateChanged();
    });
    m_busy = true;
    QTimer::singleShot(0, this, [this] { m_backend->readCurrent(); });
}

int DisplaySettings::remainingSeconds() const
{
    return m_confirming ? qMax(0, m_seconds - int(m_elapsed.elapsed() / 1000)) : 0;
}

QString DisplaySettings::stagedMode() const
{
    const auto *output = m_staged.output(m_output);
    return output ? output->currentMode : QString();
}

QString DisplaySettings::selectedSupportText() const
{
    if (!m_backend->editRestrictionReason().isEmpty()) return m_backend->editRestrictionReason();
    if (!rollbackAvailable()) return QStringLiteral("The display rollback helper is unavailable. Display changes are disabled.");
    if (!m_ready) return QStringLiteral("The current display configuration could not be read.");
    QString error;
    DisplayProtocol::canEdit(m_snapshot, m_output, error);
    return error;
}

QString DisplaySettings::supportText() const
{
    if (!m_backend->editRestrictionReason().isEmpty()) return m_backend->editRestrictionReason();
    if (!rollbackAvailable()) return selectedSupportText();
    if (!m_ready) return QStringLiteral("The current display configuration could not be read.");
    QString error;
    DisplayProtocol::canChange(m_snapshot, m_staged, error);
    return error;
}

void DisplaySettings::adopt(const DisplaySnapshot &snapshot, bool keepDraft)
{
    m_snapshot = snapshot;
    m_ready = snapshot.isValid();
    if (!keepDraft) m_staged = snapshot;
    const DisplayOutput *output = m_staged.output(m_output);
    if (!output || !output->connected) {
        output = nullptr;
        for (const auto &candidate : m_staged.outputs)
            if (candidate.connected && candidate.enabled) { output = &candidate; break; }
        if (!output) for (const auto &candidate : m_staged.outputs)
            if (candidate.connected) { output = &candidate; break; }
        m_output = output ? output->id : 0;
    }
    m_dirty = m_staged.isValid() && (!m_ready || !DisplayProtocol::same(m_snapshot, m_staged));
}

void DisplaySettings::selectOutput(int id)
{
    if (m_busy || !m_ready) return;
    const auto *output = m_staged.output(id);
    if (!output || !output->connected) return;
    m_output = id;
    emit stateChanged();
}

void DisplaySettings::editSelected(const std::function<void(DisplayOutput &)> &edit)
{
    if (m_busy || !m_ready || m_conflict || !selectedSupportText().isEmpty()) return;
    auto requested = m_staged;
    for (auto &output : requested.outputs) {
        if (output.id != m_output) continue;
        edit(output);
        requested = DisplayProtocol::normalized(requested);
        if (!requested.isValid()) {
            m_error = QStringLiteral("This display cannot use the requested settings. Its advertised modes and layout must remain valid.");
            emit stateChanged();
            return;
        }
        m_staged = requested;
        m_dirty = !DisplayProtocol::same(m_snapshot, m_staged);
        m_error.clear(); m_result.clear();
        emit stateChanged();
        return;
    }
}

void DisplaySettings::selectMode(const QString &mode)
{
    const auto *output = m_staged.output(m_output);
    if (!output || output->followPreferredMode) return;
    for (const auto &available : output->modes) {
        if (available.id == mode) {
            editSelected([mode](auto &item) { item.currentMode = mode; });
            return;
        }
    }
}
void DisplaySettings::selectEnabled(bool enabled)
{
    editSelected([enabled](auto &item) {
        item.enabled = enabled;
        if (enabled) {
            bool found = false;
            for (const auto &mode : item.modes) if (mode.id == item.currentMode) found = true;
            if (!found && !item.modes.isEmpty()) item.currentMode = item.modes.first().id;
        }
    });
}
void DisplaySettings::selectScale(double scale)
{
    if (!DisplayProtocol::canScale(m_snapshot) || !std::isfinite(scale) || scale < DisplayProtocol::CustomMinimumScale || scale > DisplayProtocol::MaximumEditorScale) return;
    editSelected([scale](auto &item) { item.scale = scale; });
}
void DisplaySettings::selectRotation(int rotation)
{
    if (!DisplayProtocol::canRotate(m_snapshot) || (rotation != 1 && rotation != 2 && rotation != 4 && rotation != 8)) return;
    editSelected([rotation](auto &item) { item.rotation = rotation; });
}
void DisplaySettings::selectPosition(int x, int y)
{
    if (!DisplayProtocol::canPosition(m_snapshot) || x < -DisplayProtocol::MaximumPosition || y < -DisplayProtocol::MaximumPosition
        || x > DisplayProtocol::MaximumPosition || y > DisplayProtocol::MaximumPosition) return;
    editSelected([x, y](auto &item) { item.position = QPoint(x, y); });
}

void DisplaySettings::apply()
{
    if (m_busy) return;
    QString reason;
    if (!m_ready || m_conflict || !m_backend->editRestrictionReason().isEmpty() || !rollbackAvailable()
        || !DisplayProtocol::canChange(m_snapshot, m_staged, reason)) {
        m_error = m_conflict ? QStringLiteral("The display configuration changed. Revert to reload it before applying.")
            : (!m_backend->editRestrictionReason().isEmpty() || !rollbackAvailable()) ? supportText() : reason.isEmpty()
                ? QStringLiteral("The current display configuration could not be read.") : reason;
        emit applyFinished(false); emit stateChanged();
        return;
    }
    if (!m_dirty) { emit applyFinished(true); return; }
    m_busy = true; m_phase = Phase::Preflight;
    m_error.clear(); m_result.clear();
    emit stateChanged();
    m_backend->readCurrent();
}

void DisplaySettings::readDone(bool success, const DisplaySnapshot &snapshot, const QString &error)
{
    if (m_phase != Phase::Reading && m_phase != Phase::Preflight) return;
    const bool preflight = m_phase == Phase::Preflight;
    if (m_reverting) {
        m_reverting = false;
        if (success && snapshot.isValid()) {
            adopt(snapshot, false);
            m_conflict = false;
            m_error.clear(); m_result.clear();
        } else {
            m_ready = false;
            m_conflict = m_conflict || m_dirty;
            m_error = error.isEmpty() ? QStringLiteral("The displays could not be reloaded. Your draft has been retained; retry Revert.") : error;
        }
        m_busy = false; m_phase = Phase::Idle;
        emit stateChanged(); finishPendingRefresh();
        return;
    }
    if (preflight && success && DisplayProtocol::same(m_snapshot, snapshot)) {
        m_phase = Phase::Preview;
        m_resolutionPending = false;
        m_backend->beginPreview(m_snapshot, m_staged);
        return;
    }
    const bool changed = m_ready && success && !DisplayProtocol::same(m_snapshot, snapshot);
    const bool hadDraft = m_dirty;
    if (success && snapshot.isValid()) {
        adopt(snapshot, hadDraft || m_conflict);
        m_conflict = m_conflict || (changed && hadDraft) || (preflight && changed);
        if (m_conflict) {
            m_dirty = hadDraft || m_dirty;
            m_error = QStringLiteral("The display configuration changed. Revert to reload it before applying.");
        } else m_error.clear();
    } else {
        m_ready = false;
        m_conflict = m_conflict || m_dirty;
        m_error = error.isEmpty() ? QStringLiteral("The current display configuration could not be read.") : error;
    }
    m_busy = false; m_phase = Phase::Idle;
    if (preflight) emit applyFinished(false);
    emit stateChanged();
    finishPendingRefresh();
}

void DisplaySettings::finish(bool kept, bool restored, const DisplaySnapshot &snapshot, const QString &error)
{
    if (m_phase != Phase::Preview) return;
    const bool verifiedKeep = kept && DisplayProtocol::matchesRequested(m_snapshot, m_staged, snapshot);
    const bool verifiedRestore = restored && DisplayProtocol::same(m_snapshot, snapshot);
    m_ticks.stop(); m_confirming = false; m_resolutionPending = false;
    adopt(snapshot, !verifiedKeep && !verifiedRestore);
    m_busy = false; m_phase = Phase::Idle;
    m_conflict = !verifiedKeep && !verifiedRestore;
    // A partial or uncertain transaction retains the draft until an explicit
    // Revert, even when a failed command happened to save the requested values.
    if (m_conflict) m_dirty = true;
    m_error = verifiedKeep || verifiedRestore ? QString() : error.isEmpty()
        ? QStringLiteral("The final display configuration could not be verified. Revert to reload the displays before making another change.") : error;
    m_result = verifiedKeep ? QStringLiteral("The new display settings were kept and verified.")
        : verifiedRestore ? QStringLiteral("The previous display settings were restored and verified.") : QString();
    emit applyFinished(verifiedKeep);
    emit stateChanged();
    finishPendingRefresh();
}

void DisplaySettings::keep()
{
    if (!m_confirming || m_resolutionPending) return;
    if (remainingSeconds() <= 0) { revert(); return; }
    m_resolutionPending = true;
    m_backend->keepPreview();
    emit stateChanged();
}

void DisplaySettings::revert()
{
    if (m_phase == Phase::Preview) {
        if (!m_resolutionPending) {
            m_resolutionPending = true;
            m_backend->revertPreview();
        }
        return;
    }
    if (m_busy) return;
    m_reverting = true;
    refresh();
}

void DisplaySettings::refresh()
{
    if (m_busy) { m_refreshPending = true; return; }
    m_phase = Phase::Reading; m_busy = true;
    emit stateChanged(); m_backend->readCurrent();
}

void DisplaySettings::finishPendingRefresh()
{
    if (!m_refreshPending) return;
    m_refreshPending = false;
    QTimer::singleShot(0, this, &DisplaySettings::refresh);
}
