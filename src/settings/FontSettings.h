#pragma once
#include "SettingsSession.h"
#include <QFont>
#include <array>

// KDE roles: general, fixed, small, toolbar, menu, window title.
// Empty rendering values mean no local fontconfig override (vendor defaults).
struct FontPreferences {
    std::array<QFont, 6> fonts;
    QString antialias, hinting, subpixel;
    bool operator==(const FontPreferences &other) const
    { return fonts == other.fonts && antialias == other.antialias && hinting == other.hinting && subpixel == other.subpixel; }
    bool operator!=(const FontPreferences &other) const { return !(*this == other); }
};
Q_DECLARE_METATYPE(FontPreferences)
struct FontState {
    FontPreferences preferences;
    QByteArray revision;
    bool writable = false;
    bool reloadAvailable = false;
    QString unavailableReason;
    bool canApply() const { return writable && reloadAvailable; }
};
Q_DECLARE_METATYPE(FontState)
class FontBackend : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    virtual void readCurrent() = 0;
    virtual void writePreferences(const FontState &expected, const FontPreferences &requested) = 0;
signals:
    void readFinished(bool success, const FontState &state, const QString &error);
    void writeFinished(bool success, const QString &error);
    void externalChange();
};
class FontSettings final : public SettingsSession {
    Q_OBJECT
public:
    static FontSettings *instance();
    explicit FontSettings(FontBackend *backend, QObject *parent = nullptr);
    FontPreferences appliedPreferences() const { return m_state.preferences; }
    FontPreferences stagedPreferences() const { return m_staged; }
    bool isReady() const { return m_ready; }
    bool canApply() const { return m_ready && m_state.canApply(); }
    QString unavailableReason() const { return m_state.unavailableReason; }
    static bool isSupported(const FontPreferences &preferences);
public slots:
    void selectFont(int role, const QFont &font);
    void selectRendering(const QString &antialias, const QString &hinting, const QString &subpixel);
    void apply() override;
    void revert() override;
    void refresh();
private:
    enum class Phase { Idle, Refresh, Preflight, Write, Readback };
    void select(const FontPreferences &preferences);
    void read();
    void onRead(bool success, const FontState &state, const QString &error);
    void onWrite(bool success, const QString &error);
    void finish(bool completesApply = false, bool success = false);
    FontBackend *m_backend;
    FontState m_state;
    FontPreferences m_staged, m_requested;
    QString m_writeError;
    Phase m_phase = Phase::Idle;
    bool m_ready = false, m_pendingRefresh = false, m_writeSuccess = false, m_keepError = false;
};
