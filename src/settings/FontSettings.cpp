#include "FontSettings.h"
#include "platform/kde/FontBackend.h"

#include <QCoreApplication>
#include <QPointer>
#include <QTimer>
#include <QStringList>

FontSettings *FontSettings::instance()
{
    static QPointer<FontSettings> session;
    if (!session)
        session = new FontSettings(new KdeFontBackend, QCoreApplication::instance());
    return session;
}

FontSettings::FontSettings(FontBackend *backend, QObject *parent)
    : SettingsSession(parent), m_backend(backend)
{
    Q_ASSERT(backend);
    backend->setParent(this);
    connect(backend, &FontBackend::readFinished, this, &FontSettings::onRead);
    connect(backend, &FontBackend::writeFinished, this, &FontSettings::onWrite);
    connect(backend, &FontBackend::externalChange, this, &FontSettings::refresh);
    connect(backend, &FontBackend::operationPending, this, [this](const QString &message) {
        if (!m_busy || m_phase == Phase::Idle) return;
        m_pendingMessage = message; emit stateChanged();
    });
    m_busy = true;
    QTimer::singleShot(0, this, &FontSettings::read);
}

bool FontSettings::isSupported(const FontPreferences &p)
{
    for (const auto &font : p.fonts)
        if (font.family().isEmpty() || font.pointSizeF() < 4 || font.pointSizeF() > 144)
            return false;
    return QStringList{QString(), QStringLiteral("true"), QStringLiteral("false")}.contains(p.antialias)
        && QStringList{QString(), QStringLiteral("hintnone"), QStringLiteral("hintslight"), QStringLiteral("hintmedium"), QStringLiteral("hintfull")}.contains(p.hinting)
        && QStringList{QString(), QStringLiteral("none"), QStringLiteral("rgb"), QStringLiteral("bgr"), QStringLiteral("vrgb"), QStringLiteral("vbgr")}.contains(p.subpixel);
}

QFont FontSettings::canonicalFont(QFont font)
{
    // Audited KConfig v6.24.0 src/gui/kconfiggroupgui.cpp:153-169.
    // KDE intentionally clears just these normal-weight aliases so consumers
    // can select a real bold face (QTBUG-63792/KDE bug 378523). Retaining an
    // arbitrary named face is essential; clearing all names changes intent.
    if (font.weight() == QFont::Normal && (font.styleName() == QLatin1String("Regular")
        || font.styleName() == QLatin1String("Normal") || font.styleName() == QLatin1String("Book")
        || font.styleName() == QLatin1String("Roman"))) font.setStyleName(QString());
    return font;
}

FontPreferences FontSettings::canonicalPreferences(FontPreferences preferences)
{
    for (auto &font : preferences.fonts) font = canonicalFont(font);
    return preferences;
}

void FontSettings::selectFont(int role, const QFont &font)
{
    if (role < 0 || role >= 6) return;
    auto p = m_staged;
    p.fonts[role] = font;
    select(p);
}

void FontSettings::selectRendering(const QString &aa, const QString &hint, const QString &sub)
{
    auto p = m_staged;
    p.antialias = aa; p.hinting = hint; p.subpixel = sub;
    select(p);
}

void FontSettings::select(const FontPreferences &p)
{
    const auto normalized = canonicalPreferences(p);
    if (m_busy || !m_ready || !isSupported(normalized)) return;
    m_staged = normalized;
    m_dirty = m_staged != appliedPreferences();
    if (!m_dirty) m_conflict = false;
    m_error.clear(); m_result.clear(); m_keepError = false;
    emit stateChanged();
}

void FontSettings::apply()
{
    if (m_busy)
        return;
    if (!canApply() || m_conflict) {
        m_error = m_conflict ? tr("Font settings changed outside Control Panel. Revert to reload it before applying.")
            : !m_ready ? tr("The current font settings could not be read. Revert to retry.")
            : unavailableReason();
        m_keepError = true;
        QPointer<FontSettings> guard(this);
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
    QPointer<FontSettings> guard(this);
    emit stateChanged();
    if (guard) m_backend->readCurrent();
}

void FontSettings::revert()
{
    if (m_busy)
        return;
    m_reverting = true;
    read();
}

void FontSettings::refresh()
{
    if (m_busy) {
        m_pendingRefresh = true;
        return;
    }
    read();
}

void FontSettings::read()
{
    m_pendingRefresh = false;
    m_phase = Phase::Refresh;
    m_busy = true; m_pendingMessage.clear();
    QPointer<FontSettings> guard(this);
    emit stateChanged();
    if (guard) m_backend->readCurrent();
}

void FontSettings::onRead(bool success, const FontState &incoming, const QString &error)
{
    if (m_phase != Phase::Refresh && m_phase != Phase::Preflight && m_phase != Phase::Readback)
        return;
    auto state = incoming; state.preferences = canonicalPreferences(incoming.preferences);
    const bool valid = success && FontSettings::isSupported(state.preferences);
    if (m_phase == Phase::Preflight) {
        if (valid && state.preferences == appliedPreferences() && state.revision == m_state.revision && state.canApply()) {
            m_state = state;
            m_phase = Phase::Write; m_pendingMessage.clear();
            m_backend->writePreferences(m_state, m_requested);
            return;
        }
        if (valid) {
            m_state = state;
            m_dirty = m_staged != appliedPreferences();
            m_conflict = m_dirty;
            m_error = !state.canApply() ? state.unavailableReason
                : tr("Font settings changed before Apply. Revert to reload it.");
        } else {
            m_ready = false;
            m_conflict = true;
            m_error = error.isEmpty() ? tr("Font settings could not be checked before applying.") : error;
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
            m_keepError = true; m_result.clear();
            m_error = error.isEmpty() ? tr("Saved font settings could not be read. Your selection is preserved; Revert to retry.") : error;
            if (m_dirty) m_conflict = true;
        }
    }
    const bool wasDirty = m_dirty;
    const bool changed = m_ready && valid && (state.preferences != appliedPreferences() || state.revision != m_state.revision);
    if (valid) {
        m_state = state;
        m_ready = true;
        if (!wasDirty)
            m_staged = appliedPreferences();
        m_dirty = m_staged != appliedPreferences();
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
        applied = m_writeSuccess && m_writeError.isEmpty() && valid && appliedPreferences() == m_requested;
        m_keepError = !applied;
        if (applied) {
            m_error.clear();
            m_result = tr("Font settings saved and read back. KDE was asked to refresh fonts. Restart applications to use new rendering settings; some applications require signing out and back in.");
            m_conflict = false;
        } else {
            m_staged = m_requested; m_dirty = true;
            m_result.clear();
            m_error = !m_writeSuccess || !m_writeError.isEmpty() ? m_writeError
                : !valid ? tr("The setting was saved, but readback failed. Revert to reload it. %1").arg(error)
                : tr("KDE reports a different font settings. Your selection is still staged.");
            // Another write requires an explicit reconciliation after uncertain apply.
            m_conflict = true;
        }
    } else if (!valid && !m_keepError) {
        m_error = error.isEmpty() ? tr("The current font settings could not be read.") : error;
    } else if (valid && !m_keepError) {
        m_error.clear();
    }
    finish(readback, applied);
}

void FontSettings::onWrite(bool success, const QString &error)
{
    if (m_phase != Phase::Write)
        return;
    m_writeSuccess = success;
    m_writeError = !error.isEmpty() ? error : success ? QString() : tr("Font settings could not be saved or the refresh request failed.");
    m_pendingMessage.clear(); m_phase = Phase::Readback;
    m_backend->readCurrent();
}

void FontSettings::finish(bool completesApply, bool success)
{
    m_busy = false;
    m_phase = Phase::Idle; m_pendingMessage.clear();
    QPointer<FontSettings> guard(this);
    if (completesApply)
        emit applyFinished(success);
    if (!guard) return;
    emit stateChanged();
    if (!guard) return;
    if (m_pendingRefresh)
        QTimer::singleShot(0, this, [this] { if (m_pendingRefresh && !m_busy) read(); });
}
