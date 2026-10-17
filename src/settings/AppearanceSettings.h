#pragma once

#include "SettingsSession.h"

// Authoritative effective KConfig value and the capabilities needed to apply it.
struct AppearanceState {
    QString doubleClickAction;
    bool writable = false;
    bool reloadAvailable = false;
    QString unavailableReason;
    bool canApply() const { return writable && reloadAvailable; }
};
Q_DECLARE_METATYPE(AppearanceState)

class AppearanceBackend : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    virtual void readCurrent() = 0;
    // The backend checks expected again immediately before its single-key write.
    virtual void writeAction(const QString &expected, const QString &action) = 0;
signals:
    void readFinished(bool success, const AppearanceState &state, const QString &error);
    void writeFinished(bool success, const QString &error);
    void externalChange();
};

class AppearanceSettings final : public SettingsSession {
    Q_OBJECT
public:
    static AppearanceSettings *instance();
    // Takes ownership; tests inject a backend that never touches desktop settings.
    explicit AppearanceSettings(AppearanceBackend *backend, QObject *parent = nullptr);
    QString appliedAction() const { return m_state.doubleClickAction; }
    QString stagedAction() const { return m_staged; }
    bool isReady() const { return m_ready; }
    bool canApply() const { return m_ready && m_state.canApply(); }
    QString unavailableReason() const { return m_state.unavailableReason; }
    static bool isSupportedAction(const QString &action);
public slots:
    void selectAction(const QString &action);
    void apply() override;
    void revert() override;
    void refresh();
private:
    enum class Phase { Idle, Refresh, Preflight, Write, Readback };
    void read();
    void onRead(bool success, const AppearanceState &state, const QString &error);
    void onWrite(bool success, const QString &error);
    void finish(bool completesApply = false, bool success = false);
    AppearanceBackend *m_backend;
    AppearanceState m_state;
    QString m_staged;
    QString m_requested;
    QString m_writeError;
    Phase m_phase = Phase::Idle;
    bool m_ready = false;
    bool m_pendingRefresh = false;
    bool m_writeSuccess = false;
    bool m_keepError = false;
};
