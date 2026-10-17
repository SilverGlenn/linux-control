#pragma once
#include "settings/ShortcutsTypes.h"
#include <QFileSystemWatcher>
#include <QFutureWatcher>
#include <QQueue>
#include <QTimer>
#include <functional>

struct StandardShortcutsBackendOptions {
    QString config = QStringLiteral("kdeglobals");
    bool isolated = false;
    // Isolated only: absolute paths, least-specific first. No session bus or
    // user/system configuration is accessed by the isolated constructor.
    QStringList systemConfigs;
    std::function<void()> beforeRead, beforeCommit, afterCommit;
    std::function<bool(const QByteArrayList &, QString &)> notify;
};
struct StandardShortcutsJob {
    QList<quint64> tokens;
    bool write = false;
    ShortcutsSnapshot expected;
    ShortcutChanges requested;
};
struct StandardShortcutsResult {
    bool success = false;
    ShortcutsOutcome outcome = ShortcutsOutcome::Failed;
    ShortcutsSnapshot snapshot;
    QString error;
};
// Edits KDE's standard in-application bindings, not application-owned overrides
// or the global accelerator registry. Success proves durable native bindings;
// application adoption may require a restart, and no action is invoked.
class KdeStandardShortcutsBackend final : public ShortcutsBackend {
    Q_OBJECT
public:
    explicit KdeStandardShortcutsBackend(QObject *parent = nullptr);
    explicit KdeStandardShortcutsBackend(const StandardShortcutsBackendOptions &, QObject *parent = nullptr);
    void readCurrent(quint64 token) override;
    void writeChanges(quint64 token, const ShortcutsSnapshot &expected, const ShortcutChanges &requested) override;
private:
    void startNext();
    void watchFiles();
    StandardShortcutsBackendOptions m_options;
    QList<ShortcutAction> m_catalog;
    QFileSystemWatcher m_watcher;
    QTimer m_debounce;
    QFutureWatcher<StandardShortcutsResult> m_future;
    QQueue<StandardShortcutsJob> m_queue;
    StandardShortcutsJob m_active;
    QByteArray m_revision;
    bool m_jobActive = false;
};
