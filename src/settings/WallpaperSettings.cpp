#include "WallpaperSettings.h"
#include "platform/kde/WallpaperBackend.h"
#include <QCoreApplication>
#include <QPointer>
#include <QTimer>
#include <QUrl>

bool operator==(const WallpaperDesktop &a, const WallpaperDesktop &b)
{
    return a.id == b.id && a.screen == b.screen && a.plugin == b.plugin
        && a.image == b.image && a.fillMode == b.fillMode && a.immutability == b.immutability
        && a.writable == b.writable;
}

WallpaperSettings *WallpaperSettings::instance()
{
    static QPointer<WallpaperSettings> session;
    if (!session) session = new WallpaperSettings(new KdeWallpaperBackend, QCoreApplication::instance());
    return session;
}

WallpaperSettings::WallpaperSettings(WallpaperBackend *backend, QObject *parent)
    : SettingsSession(parent), m_backend(backend)
{
    Q_ASSERT(backend); backend->setParent(this);
    connect(backend, &WallpaperBackend::readFinished, this, &WallpaperSettings::onRead);
    connect(backend, &WallpaperBackend::externalChange, this, &WallpaperSettings::refresh);
    connect(backend, &WallpaperBackend::writeFinished, this, [this](bool success, const QString &error) {
        if (m_phase != Phase::Write) return;
        m_writeSucceeded = success; m_writeError = error; m_phase = Phase::Readback;
        m_backend->readCurrent();
    });
    m_busy = true;
    QTimer::singleShot(0, this, &WallpaperSettings::refresh);
}

const WallpaperDesktop *WallpaperSettings::desktop() const
{
    for (const auto &entry : m_state.desktops) if (entry.id == m_selected) return &entry;
    return nullptr;
}

bool WallpaperSettings::canApply() const
{
    const auto *current = desktop();
    return m_ready && current && current->screen >= 0 && current->writable;
}

bool WallpaperSettings::supportedFillMode(int mode) { return mode == 0 || mode == 1 || mode == 2 || mode == 6; }

void WallpaperSettings::resetSelection()
{
    if (!desktop()) m_selected = m_state.desktops.isEmpty() ? 0 : m_state.desktops.first().id;
    const auto *entry = desktop();
    m_image = entry ? entry->image : QString(); m_fillMode = entry ? entry->fillMode : 2;
    m_dirty = false;
}

void WallpaperSettings::updateDirty()
{
    const auto *entry = desktop();
    m_dirty = entry && (entry->image != m_image || entry->fillMode != m_fillMode
                       || (entry->plugin != QStringLiteral("org.kde.image") && !m_image.isEmpty()));
}

void WallpaperSettings::selectDesktop(quint32 id)
{
    if (m_busy || m_dirty || !m_ready) return;
    for (const auto &entry : m_state.desktops) if (entry.id == id) {
        m_selected = id; resetSelection(); m_error.clear(); m_result.clear(); m_conflict = false;
        emit stateChanged(); return;
    }
}

void WallpaperSettings::selectPicture(const QString &localPath)
{
    if (m_busy || !canApply() || localPath.isEmpty()) return;
    m_image = QUrl::fromLocalFile(localPath).toString(QUrl::FullyEncoded);
    updateDirty(); m_error.clear(); m_result.clear(); m_retainError = false; emit stateChanged();
}

void WallpaperSettings::selectFillMode(int mode)
{
    if (m_busy || !canApply() || !supportedFillMode(mode)) return;
    m_fillMode = mode; updateDirty(); m_error.clear(); m_result.clear(); m_retainError = false; emit stateChanged();
}

void WallpaperSettings::refresh()
{
    if (m_phase != Phase::Idle) { m_pendingRefresh = true; return; }
    m_pendingRefresh = false; m_phase = Phase::Refresh; m_busy = true;
    emit stateChanged(); m_backend->readCurrent();
}

void WallpaperSettings::apply()
{
    if (m_busy) return;
    if (!canApply() || m_conflict || !supportedFillMode(m_fillMode)) {
        m_error = tr("The desktop background cannot be applied. Revert to reload its current state.");
        m_retainError = true; emit stateChanged(); emit applyFinished(false); return;
    }
    if (!m_dirty) { emit applyFinished(true); return; }
    m_expected = *desktop(); m_requestedImage = m_image; m_requestedFill = m_fillMode;
    m_error.clear(); m_result.clear(); m_retainError = false; m_busy = true; m_phase = Phase::Preflight;
    emit stateChanged(); m_backend->readCurrent();
}

void WallpaperSettings::revert()
{
    if (m_busy) return;
    m_dirty = m_conflict = m_retainError = false; m_error.clear(); m_result.clear(); resetSelection(); refresh();
}

void WallpaperSettings::onRead(bool success, const WallpaperState &state, const QString &error)
{
    if (m_phase != Phase::Refresh && m_phase != Phase::Preflight && m_phase != Phase::Readback) return;
    const bool preflight = m_phase == Phase::Preflight, readback = m_phase == Phase::Readback;
    const WallpaperDesktop previous = desktop() ? *desktop() : WallpaperDesktop{};
    if (success) {
        m_state = state; m_ready = true;
        const auto *current = desktop();
        if (preflight) {
            if (current && *current == m_expected && current->writable && current->screen >= 0) {
                m_phase = Phase::Write; m_backend->writePicture(m_expected, m_requestedImage, m_requestedFill); return;
            }
            m_conflict = true; m_error = tr("This desktop changed outside Control Panel. Revert to reload it before applying.");
        } else if (readback) {
            const bool matches = current && current->screen == m_expected.screen
                && current->plugin == QStringLiteral("org.kde.image")
                && current->image == m_requestedImage && current->fillMode == m_requestedFill;
            if (m_writeSucceeded && matches) {
                m_dirty = m_conflict = m_retainError = false;
                m_result = tr("Desktop background applied; Plasma reports the selected picture and position.");
                finish(true, true); return;
            }
            updateDirty(); m_conflict = true;
            m_error = m_writeError.isEmpty() ? tr("Plasma did not confirm the selected background. Revert to inspect its current setting.") : m_writeError;
        } else if (m_dirty) {
            if (!current || !(previous == *current)) {
                m_conflict = true; m_error = tr("This desktop changed outside Control Panel. Your selection is preserved; Revert reloads the current setting.");
            }
        } else {
            resetSelection();
            if (!m_retainError) { m_conflict = false; m_error.clear(); }
        }
    } else {
        m_ready = false;
        m_error = error.isEmpty() ? tr("Plasma desktop backgrounds could not be read.") : error;
        if (preflight || readback) m_conflict = true;
    }
    if (preflight || readback) m_retainError = true;
    finish(preflight || readback, false);
}

void WallpaperSettings::finish(bool completesApply, bool success)
{
    m_phase = Phase::Idle; m_busy = false;
    if (completesApply) emit applyFinished(success);
    emit stateChanged();
    if (m_pendingRefresh) { m_pendingRefresh = false; QTimer::singleShot(0, this, &WallpaperSettings::refresh); }
}
