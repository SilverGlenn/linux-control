#pragma once
#include "SettingsSession.h"
#include <QMap>
#include <QVariant>

enum class ScreenLockKey { Autolock, Timeout, Lock, LockGrace, RequirePassword, LockOnResume, LockOnStart, Theme, WallpaperPlugin };
enum class ScreenLockIntent { Set, RestoreDefault };
struct ScreenLockField {
    QVariant value, defaultValue;
    bool known = false, defaultKnown = false, defaultNoOp = false;
    bool effectivePresent = false, localPresent = false, inheritedPresent = false, globalPresent = false;
    bool immutable = false, writable = false, resettable = false;
    QString reason, resetReason;
    QByteArray revision, inheritedRevision, globalRevision;
    bool operator==(const ScreenLockField &other) const;
};
struct ScreenLockRuntime {
    QString owner, version, reason;
    quint64 epoch = 0;
    bool installedVerified = false, serviceAvailable = false, providerVerified = false;
};
struct ScreenLockSnapshot {
    QMap<ScreenLockKey, ScreenLockField> fields;
    QMap<ScreenLockKey, ScreenLockIntent> intents;
    QByteArray revision;
    ScreenLockRuntime runtime;
};
Q_DECLARE_METATYPE(ScreenLockSnapshot)
Q_DECLARE_METATYPE(ScreenLockRuntime)
namespace ScreenLockTypes {
QList<ScreenLockKey> keys();
QList<ScreenLockKey> ownedKeys();
bool owned(ScreenLockKey key);
QString groupName(ScreenLockKey key);
QString keyName(ScreenLockKey key);
QString label(ScreenLockKey key);
QVariant builtinDefault(ScreenLockKey key);
bool nativeValue(ScreenLockKey key, const QVariant &value);
bool validValue(ScreenLockKey key, const QVariant &value);
bool sameConfiguration(const ScreenLockSnapshot &a, const ScreenLockSnapshot &b);
bool sameDraft(const ScreenLockSnapshot &a, const ScreenLockSnapshot &b);
bool stage(ScreenLockSnapshot &draft, const ScreenLockSnapshot &baseline, ScreenLockKey key, const QVariant &value, QString &error);
bool restore(ScreenLockSnapshot &draft, const ScreenLockSnapshot &baseline, ScreenLockKey key, QString &error);
bool selectTimeout(ScreenLockSnapshot &draft, const ScreenLockSnapshot &baseline, double minutes, QString &error);
bool selectGrace(ScreenLockSnapshot &draft, const ScreenLockSnapshot &baseline, int seconds, QString &error);
bool selectNoPassword(ScreenLockSnapshot &draft, const ScreenLockSnapshot &baseline, QString &error);
bool activeBranchValid(const QMap<ScreenLockKey, ScreenLockField> &fields, QString &error);
bool requestValid(const ScreenLockSnapshot &expected, const ScreenLockSnapshot &requested, QString &error);
bool matchesRequest(const ScreenLockSnapshot &expected, const ScreenLockSnapshot &requested, const ScreenLockSnapshot &actual, QString &error);
}
class ScreenLockBackend : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    virtual void readCurrent(quint64 token) = 0;
    virtual void writeChanges(quint64 token, const ScreenLockSnapshot &expected, const ScreenLockSnapshot &requested) = 0;
signals:
    void readFinished(quint64 token, bool success, const ScreenLockSnapshot &snapshot, const QString &error);
    // A reply acknowledges configure() returning, not physical locking or authentication.
    void writeFinished(quint64 token, bool saved, bool reloadAcknowledged, const QString &error);
    // A soft deadline is progress only: the file transaction still owns its lease.
    void operationPending(quint64 token, const QString &message);
    void externalChange();
};
class ScreenLockSettings final : public SettingsSession {
    Q_OBJECT
public:
    static ScreenLockSettings *instance();
    explicit ScreenLockSettings(ScreenLockBackend *backend, QObject *parent = nullptr);
    const ScreenLockSnapshot &applied() const { return m_applied; }
    const ScreenLockSnapshot &staged() const { return m_staged; }
    const ScreenLockRuntime &runtime() const { return m_runtime; }
    bool isReady() const { return m_ready; }
    bool isRefreshing() const { return m_phase == Phase::Refresh; }
    bool canEdit(ScreenLockKey key) const;
    bool canApply() const;
    QString applySupportText() const;
    QStringList defaultsChanges() const;
    QStringList defaultsUnavailable() const;
public slots:
    void selectValue(ScreenLockKey key, const QVariant &value);
    void selectTimeout(double minutes);
    void selectGrace(int seconds);
    void selectNoPassword();
    void restoreDefault(ScreenLockKey key);
    void restoreDefaults();
    void apply() override;
    void revert() override;
    void refresh();
private:
    enum class Phase { Idle, Refresh, Revert, Preflight, Write, Readback };
    void beginRead(Phase phase);
    void onRead(quint64 token, bool success, const ScreenLockSnapshot &snapshot, const QString &error);
    void onWritten(quint64 token, bool saved, bool acknowledged, const QString &error);
    void edited();
    void finish(bool applying = false, bool success = false);
    ScreenLockBackend *m_backend;
    ScreenLockSnapshot m_applied, m_staged, m_requested, m_expected;
    ScreenLockRuntime m_runtime;
    Phase m_phase = Phase::Idle;
    quint64 m_token = 0;
    bool m_ready = false, m_pendingRefresh = false, m_saved = false, m_acknowledged = false, m_uncertain = false;
    QString m_writeError, m_pendingMessage;
};
