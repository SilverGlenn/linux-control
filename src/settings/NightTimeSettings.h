#pragma once

#include "SettingsSession.h"
#include <QMap>
#include <QTime>
#include <QVariant>

enum class NightTimeKey { Source, Automatic, Latitude, Longitude, SunriseStart, SunsetStart, TransitionDuration };
enum class NightTimeIntent { Set, RestoreDefault };

struct NightTimeField {
    QVariant value, defaultValue;
    bool known = false, defaultKnown = false, defaultNoOp = false;
    bool effectivePresent = false, localPresent = false, inheritedPresent = false, globalPresent = false;
    bool immutable = false, writable = false, resettable = false;
    QString reason, resetReason;
    QByteArray revision, inheritedRevision, globalRevision;
    bool operator==(const NightTimeField &other) const;
};

struct NightTimeRuntime {
    QString owner, version, reason;
    quint64 epoch = 0;
    uint apiVersion = 0;
    // Installation/schema identity allows saving while the daemon is absent.
    // A running service must also pass the owner-pinned version/identity check.
    bool installedVerified = false, serviceAvailable = false, providerVerified = false;
};
struct NightTimeCachedLocation {
    bool available = false;
    double latitude = 0, longitude = 0;
    // Native state has no timestamp. It is never described as a fresh fix.
    QString reason;
};
struct NightTimeSnapshot {
    QMap<NightTimeKey, NightTimeField> fields;       // native editor FullConfig
    QMap<NightTimeKey, NightTimeField> daemonFields; // daemon NoGlobals input
    QMap<NightTimeKey, NightTimeIntent> intents;
    QByteArray revision;
    NightTimeRuntime runtime;
    NightTimeCachedLocation cachedLocation; // volatile, excluded from conflicts
};
Q_DECLARE_METATYPE(NightTimeSnapshot)
Q_DECLARE_METATYPE(NightTimeRuntime)

namespace NightTimeTypes {
constexpr int LocationSource = 0, TimesSource = 1;
constexpr uint MinimumDuration = 60, MaximumDuration = 36000;
QList<NightTimeKey> keys();
QString groupName(NightTimeKey key);
QString keyName(NightTimeKey key);
QString label(NightTimeKey key);
QVariant builtinDefault(NightTimeKey key);
bool validValue(NightTimeKey key, const QVariant &value);
// KConfig's native QTime writer stores whole seconds; untouched values retain
// their original precision. Finite doubles use Qt's shortest round-trip form.
QVariant persistedValue(NightTimeKey key, const QVariant &value);
bool sameConfiguration(const NightTimeSnapshot &a, const NightTimeSnapshot &b);
bool sameDraft(const NightTimeSnapshot &a, const NightTimeSnapshot &b);
bool stage(NightTimeSnapshot &draft, const NightTimeSnapshot &baseline, NightTimeKey key, const QVariant &value, QString &error);
bool restore(NightTimeSnapshot &draft, const NightTimeSnapshot &baseline, NightTimeKey key, QString &error);
QMap<NightTimeKey, NightTimeField> projectedDaemonFields(const NightTimeSnapshot &baseline, const NightTimeSnapshot &draft);
bool activeBranchValid(const QMap<NightTimeKey, NightTimeField> &fields, QString &error);
bool requestValid(const NightTimeSnapshot &expected, const NightTimeSnapshot &requested, QString &error);
bool matchesRequest(const NightTimeSnapshot &expected, const NightTimeSnapshot &requested, const NightTimeSnapshot &actual, QString &error);
QTime validatedTime(QTime input, QTime other, uint durationSeconds);
bool selectTime(NightTimeSnapshot &draft, const NightTimeSnapshot &baseline, NightTimeKey key, QTime value, QString &error);
bool selectDuration(NightTimeSnapshot &draft, const NightTimeSnapshot &baseline, uint seconds, QString &error);
bool selectManualLocation(NightTimeSnapshot &draft, const NightTimeSnapshot &baseline, double latitude, double longitude, QString &error);
QString privacyText(const NightTimeSnapshot &baseline, const NightTimeSnapshot &draft);
}

class NightTimeBackend : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    virtual void readCurrent(quint64 token) = 0;
    virtual void writeChanges(quint64 token, const NightTimeSnapshot &expected, const NightTimeSnapshot &requested) = 0;
signals:
    void readFinished(quint64 token, bool success, const NightTimeSnapshot &snapshot, const QString &error);
    // Notify is not a daemon adoption acknowledgement.
    void writeFinished(quint64 token, bool saved, bool notificationSent, const QString &error);
    void operationPending(quint64 token, const QString &message);
    void externalChange();
};

class NightTimeSettings final : public SettingsSession {
    Q_OBJECT
public:
    static NightTimeSettings *instance();
    explicit NightTimeSettings(NightTimeBackend *backend, QObject *parent = nullptr);
    const NightTimeSnapshot &applied() const { return m_applied; }
    const NightTimeSnapshot &staged() const { return m_staged; }
    const NightTimeRuntime &runtime() const { return m_runtime; }
    const NightTimeCachedLocation &cachedLocation() const { return m_cachedLocation; }
    bool isReady() const { return m_ready; }
    bool isRefreshing() const { return m_phase == Phase::Refresh; }
    bool canEdit(NightTimeKey key) const;
    bool canApply() const;
    QString applySupportText() const;
    QString privacyText() const;
    QStringList defaultsChanges() const;
    QStringList defaultsUnavailable() const;
public slots:
    void selectValue(NightTimeKey key, const QVariant &value);
    void selectTime(NightTimeKey key, QTime value);
    void selectDuration(uint seconds);
    void selectManualLocation(double latitude, double longitude);
    void restoreDefault(NightTimeKey key);
    void restoreDefaults();
    void apply() override;
    void revert() override;
    void refresh();
private:
    enum class Phase { Idle, Refresh, Revert, Preflight, Write, Readback };
    void beginRead(Phase phase);
    void onRead(quint64 token, bool success, const NightTimeSnapshot &snapshot, const QString &error);
    void onWritten(quint64 token, bool saved, bool notified, const QString &error);
    void edited();
    void finish(bool applying = false, bool success = false);
    NightTimeBackend *m_backend;
    NightTimeSnapshot m_applied, m_staged, m_requested, m_expected;
    NightTimeRuntime m_runtime;
    NightTimeCachedLocation m_cachedLocation;
    Phase m_phase = Phase::Idle;
    quint64 m_token = 0;
    bool m_ready = false, m_pendingRefresh = false, m_saved = false, m_notified = false, m_uncertain = false;
    QString m_writeError, m_pendingMessage;
};
