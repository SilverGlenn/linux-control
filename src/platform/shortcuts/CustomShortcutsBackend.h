#pragma once
#include "settings/CustomShortcutsSettings.h"
#include <QDBusConnection>
#include <QFileSystemWatcher>
#include <QFutureWatcher>
#include <QQueue>
#include <QTimer>
#include <QVariant>
#include <functional>
#include <memory>

struct CustomShortcutsReply {
    bool success = false, uncertain = false;
    QVariant value;
    QString error;
};
// Calls are synchronous on a value-capturing worker, never on the GUI thread.
// Production transport checks exact D-Bus arity/types and bounds every reply.
class CustomShortcutsTransport {
public:
    virtual ~CustomShortcutsTransport() = default;
    virtual CustomShortcutsReply call(const QString &destination, const QString &path, const QString &interface,
                                     const QString &method, const QVariantList &arguments) = 0;
};
std::shared_ptr<CustomShortcutsTransport> createCustomShortcutsDBusTransport(const QDBusConnection &);
struct CustomShortcutsBackendOptions {
    QString config, dataHome;
    QStringList dataDirs;
    bool isolated = false;
    int readBudgetMs = 30000, writeBudgetMs = 60000, persistenceBudgetMs = 4000;
    // Isolated worker test seams; ignored in production.
    std::function<void()> beforeScan;
    std::function<void(int, const QString &)> beforeCommit, afterCommit;
};
struct CustomShortcutsJob {
    QList<quint64> tokens;
    bool write = false;
    CustomShortcutsSnapshot expected;
    CustomShortcutChanges changes;
};
struct CustomShortcutsResult {
    bool success = false;
    ShortcutsOutcome outcome = ShortcutsOutcome::Failed;
    CustomShortcutsSnapshot snapshot;
    QString error;
};
class KdeCustomShortcutsBackend final : public CustomShortcutsBackend {
    Q_OBJECT
public:
    explicit KdeCustomShortcutsBackend(QObject *parent = nullptr);
    KdeCustomShortcutsBackend(std::shared_ptr<CustomShortcutsTransport>, const CustomShortcutsBackendOptions &, QObject *parent = nullptr);
    void readCurrent(quint64 token) override;
    void writeChanges(quint64 token, const CustomShortcutsSnapshot &, const CustomShortcutChanges &) override;
private:
    void startNext();
    void watchFiles();
    std::shared_ptr<CustomShortcutsTransport> m_transport;
    CustomShortcutsBackendOptions m_options;
    QFutureWatcher<CustomShortcutsResult> m_future;
    QQueue<CustomShortcutsJob> m_queue;
    CustomShortcutsJob m_active;
    QFileSystemWatcher m_watcher;
    QTimer m_debounce;
    QString m_lockedOwner;
    bool m_activeJob = false;
};
