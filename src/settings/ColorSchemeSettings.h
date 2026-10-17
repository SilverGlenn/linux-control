#pragma once

#include "SettingsSession.h"

// Injection boundary: tests can report reads/writes without touching KDE.
class ColorSchemeBackend : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    virtual void readCurrent() = 0;
    virtual void applyScheme(const QString &id) = 0;
    virtual void cancel() = 0;

signals:
    void readFinished(bool success, const QString &id, const QString &error);
    void writeFinished(bool success, const QString &error);
    void externalChange();
};

class ColorSchemeSettings : public SettingsSession {
    Q_OBJECT
public:
    // Legacy read/session adapter. Production writes are retired; the shared
    // AeroThemeSettings transaction owns Personalization changes.
    static ColorSchemeSettings *instance();
    // Takes ownership of backend. It must finish each operation exactly once.
    explicit ColorSchemeSettings(ColorSchemeBackend *backend, QObject *parent = nullptr);
    QString appliedScheme() const { return m_applied; }
    QString stagedScheme() const { return m_staged; }
    bool isReady() const { return m_ready; }

public slots:
    void selectScheme(const QString &id);
    void apply() override;
    void revert() override;
    void refresh();
    void cancel();

private:
    enum class Phase { Idle, Refreshing, Preflight, Applying, Readback };
    void startRefresh();
    void onReadFinished(bool success, const QString &id, const QString &error);
    void onWriteFinished(bool success, const QString &error);
    void finishRefresh(bool completesApply = false, bool success = false);

    ColorSchemeBackend *m_backend;
    Phase m_phase = Phase::Idle;
    bool m_ready = false;
    bool m_refreshPending = false;
    bool m_writeSucceeded = false;
    bool m_hasApplyError = false;
    QString m_applied;
    QString m_staged;
    QString m_requested;
    QString m_writeError;
};
