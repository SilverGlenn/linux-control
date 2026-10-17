#pragma once

#include "SettingsSession.h"

struct AeroEffectState {
    int blurStrength = 3;
    bool writable = false;
    bool pluginSupported = false;
    bool runtimeSupported = false;
    bool effectLoaded = false;
    QString unavailableReason;
    bool canApply() const { return writable && pluginSupported && runtimeSupported && effectLoaded; }
};
Q_DECLARE_METATYPE(AeroEffectState)

class AeroEffectBackend : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    virtual void readCurrent() = 0;
    virtual void writeStrength(int expected, int requested) = 0;
signals:
    void readFinished(bool success, const AeroEffectState &state, const QString &error);
    void writeFinished(bool success, const QString &error);
    void externalChange();
};

// Shared independently of the page: Apply finishes only after saved readback.
class AeroEffectSettings final : public SettingsSession {
    Q_OBJECT
public:
    static AeroEffectSettings *instance();
    explicit AeroEffectSettings(AeroEffectBackend *backend, QObject *parent = nullptr);
    AeroEffectState state() const { return m_state; }
    int appliedStrength() const { return m_state.blurStrength; }
    int stagedStrength() const { return m_staged; }
    bool isReady() const { return m_ready; }
    bool canApply() const { return m_ready && m_state.canApply(); }
    static bool supportedStrength(int value) { return value >= 2 && value <= 15; }
public slots:
    void selectStrength(int strength);
    void apply() override;
    void revert() override;
    void refresh();
private:
    enum class Phase { Idle, Refresh, Preflight, Write, Readback };
    void read();
    void onRead(bool success, const AeroEffectState &state, const QString &error);
    void onWrite(bool success, const QString &error);
    void finish(bool completesApply = false, bool success = false);
    AeroEffectBackend *m_backend;
    AeroEffectState m_state;
    int m_staged = 3;
    int m_requested = 3;
    Phase m_phase = Phase::Idle;
    bool m_ready = false, m_pendingRefresh = false, m_keepError = false, m_writeSuccess = false;
    QString m_writeError;
};
