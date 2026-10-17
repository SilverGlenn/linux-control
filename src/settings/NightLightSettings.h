#pragma once

#include "SettingsSession.h"
#include <QMap>
#include <QVariant>
#include <QTimer>

enum class NightLightKey { Active, Mode, DayTemperature, NightTemperature };
enum class NightLightIntent { Set, RestoreDefault };

struct NightLightField {
    QVariant value, defaultValue;
    bool known = false, defaultKnown = false, defaultNoOp = false;
    bool effectivePresent = false, localPresent = false, inheritedPresent = false, globalPresent = false;
    bool immutable = false, writable = false, resettable = false;
    QString reason, resetReason;
    QByteArray revision, inheritedRevision, globalRevision;
    bool operator==(const NightLightField &other) const;
};

// Public runtime observations are deliberately separate from saved preferences.
// KWin does not expose both loaded temperature preferences or a reload serial.
struct NightLightRuntime {
    QString owner, version, reason;
    quint64 epoch = 0;
    bool serviceAvailable = false, schemaSupported = false, known = false;
    bool available = false, enabled = false, running = false, inhibited = false, daylight = false;
    uint mode = 0, currentTemperature = 0, targetTemperature = 0;
    qulonglong previousTransitionDateTime = 0, scheduledTransitionDateTime = 0;
    uint previousTransitionDuration = 0, scheduledTransitionDuration = 0;
    bool previewSupported = false;
};

struct NightLightSnapshot {
    QMap<NightLightKey, NightLightField> fields;
    QMap<NightLightKey, NightLightIntent> intents;
    QByteArray revision;
    NightLightRuntime runtime;
};
Q_DECLARE_METATYPE(NightLightSnapshot)
Q_DECLARE_METATYPE(NightLightRuntime)

namespace NightLightTypes {
constexpr int MinimumTemperature = 1000, MaximumTemperature = 6500;
constexpr int ConstantMode = 0, DarkLightMode = 1;
QList<NightLightKey> keys();
QString keyName(NightLightKey key);
QString label(NightLightKey key);
QVariant builtinDefault(NightLightKey key);
bool validValue(NightLightKey key, const QVariant &value);
bool sameConfiguration(const NightLightSnapshot &a, const NightLightSnapshot &b);
bool sameDraft(const NightLightSnapshot &a, const NightLightSnapshot &b);
bool stage(NightLightSnapshot &draft, const NightLightSnapshot &baseline,
           NightLightKey key, const QVariant &value, QString &error);
bool restore(NightLightSnapshot &draft, const NightLightSnapshot &baseline,
             NightLightKey key, QString &error);
bool requestValid(const NightLightSnapshot &expected, const NightLightSnapshot &requested, QString &error);
bool matchesRequest(const NightLightSnapshot &expected, const NightLightSnapshot &requested,
                    const NightLightSnapshot &actual, QString &error);
// A match proves only observable enabled/mode/target state, not causal reload
// acknowledgement, the dormant temperature, or the physical output appearance.
bool runtimeMatches(const NightLightSnapshot &snapshot);
}

class NightLightBackend : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    virtual void readCurrent(quint64 token) = 0;
    virtual void writeChanges(quint64 token, const NightLightSnapshot &expected,
                              const NightLightSnapshot &requested) = 0;
    virtual void preview(quint64 token, int temperature, const QString &expectedOwner, quint64 expectedEpoch) = 0;
    virtual void stopPreview(quint64 token, const QString &expectedOwner, quint64 expectedEpoch) = 0;
signals:
    void readFinished(quint64 token, bool success, const NightLightSnapshot &snapshot, const QString &error);
    // notificationSent means the native KConfig Notify path was used; it is not
    // an acknowledgement from KWin. The controller verifies fresh saved/runtime state.
    void writeFinished(quint64 token, bool saved, bool notificationSent, const QString &error);
    void operationPending(quint64 token, const QString &message);
    void previewFinished(quint64 token, bool success, bool stopped, const QString &error);
    void externalChange();
};

class NightLightSettings final : public SettingsSession {
    Q_OBJECT
public:
    static NightLightSettings *instance();
    explicit NightLightSettings(NightLightBackend *backend, QObject *parent = nullptr);
    const NightLightSnapshot &applied() const { return m_applied; }
    const NightLightSnapshot &staged() const { return m_staged; }
    const NightLightRuntime &runtime() const { return m_runtime; }
    bool isReady() const { return m_ready; }
    bool isRefreshing() const { return m_phase == Phase::Refresh; }
    bool canEdit(NightLightKey key) const;
    bool canApply() const;
    bool canPreview(NightLightKey key) const;
    bool previewActive() const { return m_previewActive || m_previewPending; }
    QString previewText() const { return m_previewText; }
    QString applySupportText() const;
    QStringList defaultsChanges() const;
    QStringList defaultsUnavailable() const;
public slots:
    void selectValue(NightLightKey key, const QVariant &value);
    void restoreDefault(NightLightKey key);
    void restoreDefaults();
    void previewTemperature(NightLightKey key);
    void stopPreview();
    void apply() override;
    void revert() override;
    void refresh();
private:
    enum class Phase { Idle, Refresh, Revert, Preflight, Write, Readback };
    void beginRead(Phase phase);
    void onRead(quint64 token, bool success, const NightLightSnapshot &snapshot, const QString &error);
    void onWritten(quint64 token, bool saved, bool notified, const QString &error);
    void onPreview(quint64 token, bool success, bool stopped, const QString &error);
    void finish(bool applying = false, bool success = false);
    void edited();
    NightLightBackend *m_backend;
    NightLightSnapshot m_applied, m_staged, m_requested, m_expected;
    NightLightRuntime m_runtime;
    Phase m_phase = Phase::Idle;
    quint64 m_token = 0, m_previewToken = 0;
    quint64 m_previewEpoch = 0;
    bool m_ready = false, m_pendingRefresh = false, m_saved = false, m_notified = false;
    bool m_uncertain = false, m_previewPending = false, m_previewActive = false, m_stopAfterPreview = false;
    bool m_previewUncertain = false;
    QString m_writeError, m_pendingMessage, m_previewText, m_previewOwner;
    QTimer m_previewTimer;
};
