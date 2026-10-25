#pragma once
#include "SettingsSession.h"
#include <QByteArray>
#include <QList>
#include <QSet>
#include <QVariantMap>

struct KeyboardChoice { QString id, description; };
struct KeyboardLayoutInfo {
    QString id, description;
    QList<KeyboardChoice> variants;
};
struct KeyboardOptionGroup {
    QString id, description;
    bool allowMultiple = false;
    QList<KeyboardChoice> options;
};
struct KeyboardRegistry {
    QList<KeyboardChoice> models;
    QList<KeyboardLayoutInfo> layouts;
    QList<KeyboardOptionGroup> optionGroups;
    bool valid = false;
    QString error;
    QByteArray revision;
    const KeyboardLayoutInfo *layout(const QString &id) const;
};
struct KeyboardRuntimeLayout { QString shortName, displayName, longName; };
struct KeyboardSnapshot {
    QString owner, version, reason, runtimeStatus;
    QVariantMap values;
    QSet<QString> immutableKeys;
    bool available = false, writable = false, reloadAvailable = false;
    QList<KeyboardRuntimeLayout> runtimeLayouts;
    bool runtimeLayoutsVerified = false;
    KeyboardRegistry registry;
    QByteArray revision;
};
enum class KeyboardOutcome { Success, Failed, Partial, Uncertain };
namespace KeyboardTypes {
QVariantMap defaults();
KeyboardRegistry readRegistry(const QString &path, const QString &extrasPath = {});
KeyboardRegistry readRegistryData(const QByteArray &primary, const QByteArray &extras = {}, bool extrasPresent = false);
bool sameValues(const QVariantMap &a, const QVariantMap &b);
QStringList changedKeys(const QVariantMap &a, const QVariantMap &b);
bool validate(const KeyboardSnapshot &snapshot, const QVariantMap &requested, QString &error);
}
Q_DECLARE_METATYPE(KeyboardSnapshot)
Q_DECLARE_METATYPE(KeyboardOutcome)
class KeyboardBackend : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    virtual void cancel() {}
    virtual void readCurrent(quint64 token) = 0;
    virtual void writePreferences(quint64 token, const KeyboardSnapshot &expected, const QVariantMap &requested) = 0;
signals:
    void readFinished(quint64 token, bool success, const KeyboardSnapshot &snapshot, const QString &error);
    void writeFinished(quint64 token, KeyboardOutcome outcome, const KeyboardSnapshot &snapshot, const QString &error);
    void operationPending(const QString &message);
    void externalChange();
};
class KeyboardSettings final : public SettingsSession {
    Q_OBJECT
public:
    static KeyboardSettings *instance();
    explicit KeyboardSettings(KeyboardBackend *backend, QObject *parent = nullptr);
    const KeyboardSnapshot &applied() const { return m_applied; }
    const KeyboardSnapshot &staged() const { return m_staged; }
    bool isReady() const { return m_ready; }
    bool canApply() const;
public slots:
    void selectValue(const QString &key, const QVariant &value);
    void selectValues(const QVariantMap &delta);
    void restoreDefaults();
    void refresh();
    void apply() override;
    void revert() override;
private:
    void read();
    void onRead(quint64 token, bool success, const KeyboardSnapshot &snapshot, const QString &error);
    void onWritten(quint64 token, KeyboardOutcome outcome, const KeyboardSnapshot &snapshot, const QString &error);
    KeyboardBackend *m_backend;
    KeyboardSnapshot m_applied, m_staged;
    QVariantMap m_requested;
    QString m_lockedOwner, m_writeOwner, m_writeVersion;
    quint64 m_token = 0;
    bool m_ready = false, m_reading = false, m_writing = false, m_pendingRefresh = false, m_keepError = false;
};
