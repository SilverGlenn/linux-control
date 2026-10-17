#pragma once

#include "SettingsSession.h"
#include <QSet>
#include <QVariantMap>
#include <QStringList>

struct AeroThemeState {
    QVariantMap values;
    QSet<QString> writableKeys;
    bool assetsSupported = false;
    bool runtimeSupported = false;
    QStringList issues;
    bool canApply() const
    {
        if (!assetsSupported || !runtimeSupported || values.isEmpty()) return false;
        for (auto it = values.cbegin(); it != values.cend(); ++it)
            if (!writableKeys.contains(it.key())) return false;
        return true;
    }
};
Q_DECLARE_METATYPE(AeroThemeState)

class AeroThemeBackend : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    virtual void readCurrent() = 0;
    virtual void writeSettings(const QVariantMap &expected, const QVariantMap &requested) = 0;
signals:
    void readFinished(bool success, const AeroThemeState &state, const QString &error);
    void writeFinished(bool success, const QString &error);
    void externalChange();
};

// Application-owned: all Aero views share the same staged values and work.
class AeroThemeSettings final : public SettingsSession {
    Q_OBJECT
public:
    static AeroThemeSettings *instance();
    explicit AeroThemeSettings(AeroThemeBackend *backend, QObject *parent = nullptr);
    const AeroThemeState &state() const { return m_state; }
    QVariantMap appliedValues() const { return m_state.values; }
    QVariantMap stagedValues() const { return m_staged; }
    QVariant appliedValue(const QString &key) const { return m_state.values.value(key); }
    QVariant stagedValue(const QString &key) const { return m_staged.value(key); }
    bool isReady() const { return m_ready; }
    bool canApply() const;
    bool canEdit(const QString &key) const;
    bool hasPartialFailure() const { return m_partialFailure; }
    QStringList changeSummary() const;
public slots:
    void selectValue(const QString &key, const QVariant &value);
    void stagedParameter(const QString &key, const QVariant &value) { selectValue(key, value); }
    void stageRestoreDefaults();
    void apply() override;
    void revert() override;
    void refresh();
private:
    enum class Phase { Idle, Refresh, Preflight, Write, Readback };
    void read();
    void onRead(bool success, const AeroThemeState &state, const QString &error);
    void onWrite(bool success, const QString &error);
    void finish(bool completesApply = false, bool success = false);
    void stage(const QVariantMap &values);
    AeroThemeBackend *m_backend;
    AeroThemeState m_state;
    QVariantMap m_staged, m_requested, m_beforeWrite;
    QString m_writeError;
    Phase m_phase = Phase::Idle;
    bool m_ready = false, m_pendingRefresh = false, m_writeSuccess = false;
    bool m_keepError = false, m_partialFailure = false;
};
