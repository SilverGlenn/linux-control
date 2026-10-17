#pragma once
#include "SettingsSession.h"
#include "ShortcutsTypes.h"
#include <QMap>

struct CustomShortcutApplication {
    QString id, name, icon, path, exec;
    QByteArray identity;
    QList<ShortcutAction> defaults;
    bool command = false;
};
struct CustomShortcutCommand { QString component, name, exec; };
struct CustomShortcutEntry {
    QString component, name, exec, path, reason;
    QByteArray identity;
    bool command = false, registered = false, editable = false, removable = false;
    bool pendingAddition = false, pendingRemoval = false;
};
struct CustomShortcutsSnapshot {
    QString owner, interfaceSignature, reason;
    QByteArray revision, configRevision, inventoryRevision;
    QList<CustomShortcutApplication> applications;
    QList<CustomShortcutEntry> entries;
    QList<ShortcutAction> actions;
    QMap<QString, QStringList> contexts;
    bool available = false, writable = false;
    const CustomShortcutEntry *find(const QString &component) const;
    const CustomShortcutApplication *application(const QString &id) const;
};
struct CustomShortcutChanges {
    QStringList addApplications, removeComponents;
    QList<CustomShortcutCommand> addCommands;
    QMap<QString, CustomShortcutCommand> editCommands;
    bool isEmpty() const;
};
namespace CustomShortcutsTypes {
QByteArray revision(const CustomShortcutsSnapshot &snapshot);
bool validComponent(const QString &component);
bool normalizeCommand(const QString &name, const QString &exec, QString &normalized, QString &error);
CustomShortcutChanges changes(const CustomShortcutsSnapshot &before, const CustomShortcutsSnapshot &after);
bool validate(const CustomShortcutsSnapshot &snapshot, const CustomShortcutChanges &changes, QString &error);
}
Q_DECLARE_METATYPE(CustomShortcutsSnapshot)
Q_DECLARE_METATYPE(CustomShortcutChanges)
class CustomShortcutsBackend : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    virtual void readCurrent(quint64 token) = 0;
    virtual void writeChanges(quint64 token, const CustomShortcutsSnapshot &expected, const CustomShortcutChanges &changes) = 0;
signals:
    void readFinished(quint64 token, bool success, const CustomShortcutsSnapshot &snapshot, const QString &error);
    void writeFinished(quint64 token, ShortcutsOutcome outcome, const CustomShortcutsSnapshot &snapshot, const QString &error);
    void externalChange();
};
// App-owned staged definitions. Bindings are edited in the Global session after
// registration; no command/application is launched by this session.
class CustomShortcutsSettings final : public SettingsSession {
    Q_OBJECT
public:
    static CustomShortcutsSettings *instance();
    explicit CustomShortcutsSettings(CustomShortcutsBackend *, QObject *parent = nullptr);
    const CustomShortcutsSnapshot &applied() const { return m_applied; }
    const CustomShortcutsSnapshot &staged() const { return m_staged; }
    bool isReady() const { return m_ready; }
    bool canApply() const;
    bool stageAddApplication(const QString &id);
    bool stageAddCommand(const QString &name, const QString &exec, const QString &component = {});
    bool stageCommands(const QList<CustomShortcutEntry> &definitions, bool registerExisting = false);
    bool stageEditCommand(const QString &component, const QString &name, const QString &exec);
    bool stageRemove(const QString &component);
    bool stageRestore(const QString &component);
public slots:
    void refresh();
    void apply() override;
    void revert() override;
private:
    enum class Phase { Idle, Reading, Writing };
    void read();
    bool stage(const CustomShortcutsSnapshot &candidate);
    void onRead(quint64, bool, const CustomShortcutsSnapshot &, const QString &);
    void onWritten(quint64, ShortcutsOutcome, const CustomShortcutsSnapshot &, const QString &);
    CustomShortcutsBackend *m_backend;
    CustomShortcutsSnapshot m_applied, m_staged, m_expected;
    CustomShortcutChanges m_requested;
    QString m_lockedOwner;
    quint64 m_token = 0;
    Phase m_phase = Phase::Idle;
    bool m_ready = false, m_pendingRefresh = false, m_reverting = false, m_keepError = false;
};
