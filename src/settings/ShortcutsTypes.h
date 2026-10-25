#pragma once
#include <QObject>
#include <QByteArray>
#include <QKeySequence>
#include <QList>
#include <QMap>
#include <QStringList>

struct ShortcutActionId {
    QString component, context, action;
    bool operator==(const ShortcutActionId &other) const { return component == other.component && context == other.context && action == other.action; }
    bool operator<(const ShortcutActionId &other) const;
    QStringList nativeId(const QString &componentFriendly = {}, const QString &actionFriendly = {}) const;
};
struct ShortcutAction {
    ShortcutActionId id;
    QString componentFriendly, contextFriendly, friendlyName;
    QList<QKeySequence> current, defaults;
    bool available = true;
    bool registered = true;
    bool layoutAction = false;
};
using ShortcutChanges = QMap<ShortcutActionId, QList<QKeySequence>>;
struct ShortcutsLayoutAdapter {
    QString owner, version, reason;
    QByteArray revision, configRevision;
    QStringList actionNames;
    bool available = false;
    bool operator==(const ShortcutsLayoutAdapter &other) const {
        return owner == other.owner && version == other.version && reason == other.reason
            && revision == other.revision && configRevision == other.configRevision
            && actionNames == other.actionNames && available == other.available;
    }
};
struct ShortcutsSnapshot {
    QString owner, interfaceSignature, reason;
    QByteArray revision, configRevision;
    QList<ShortcutAction> actions;
    bool available = false, writable = false;
    ShortcutsLayoutAdapter layouts;
    const ShortcutAction *find(const ShortcutActionId &id) const;
};
enum class ShortcutsOutcome { Success, Failed, Partial, Uncertain };
namespace ShortcutsTypes {
bool sameKeys(const QList<QKeySequence> &a, const QList<QKeySequence> &b);
bool validate(const ShortcutsSnapshot &snapshot, const ShortcutChanges &requested, QString &error);
bool conflicts(const QKeySequence &a, const QKeySequence &b);
QByteArray revision(const ShortcutsSnapshot &snapshot);
}
Q_DECLARE_METATYPE(ShortcutAction)
Q_DECLARE_METATYPE(ShortcutActionId)
Q_DECLARE_METATYPE(ShortcutChanges)
Q_DECLARE_METATYPE(ShortcutsSnapshot)
Q_DECLARE_METATYPE(ShortcutsOutcome)
class ShortcutsBackend : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    virtual void readCurrent(quint64 token) = 0;
    virtual void writeChanges(quint64 token, const ShortcutsSnapshot &expected, const ShortcutChanges &requested) = 0;
    virtual void cancel() {} // Running native transactions retain ownership.
signals:
    void readFinished(quint64 token, bool success, const ShortcutsSnapshot &snapshot, const QString &error);
    void writeFinished(quint64 token, ShortcutsOutcome outcome, const ShortcutsSnapshot &snapshot, const QString &error);
    void externalChange();
    void operationPending(const QString &message);
};
