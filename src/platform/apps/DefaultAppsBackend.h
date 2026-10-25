#pragma once
#include "settings/DefaultAppsSettings.h"
#include <QFileSystemWatcher>
#include <QTimer>
#include <QFutureWatcher>
#include <QQueue>
#include <functional>

// Tests supply absolute temporary paths and a private desktop-entry directory.
// Production inventory parses installed entries without querying/rebuilding
// KSycoca, running an application, or invoking a settings module.
struct DefaultAppsPaths {
    QString configHome;
    QStringList configDirs, dataDirs;
    QStringList desktops{QStringLiteral("kde")};
    bool isolated = false;
    // Private-runtime test seam. Ignored in production. Runs on the worker;
    // tests use it to hold a scan without blocking the application event loop.
    std::function<void()> beforeScan;
    std::function<void(int, const QString &)> beforeCommit;
    int softDeadlineMs = 10000; // Shorter deadlines are isolated-fixture only.
};
struct DefaultAppsJob {
    QList<quint64> tokens;
    bool write = false, poll = false;
    DefaultAppsSnapshot expected;
    DefaultAppsPreferences requested;
};
struct DefaultAppsJobResult {
    bool success = false;
    DefaultAppsOutcome outcome = DefaultAppsOutcome::Failed;
    DefaultAppsSnapshot snapshot;
    QString error;
};
class KdeDefaultAppsBackend final : public DefaultAppsBackend {
    Q_OBJECT
public:
    explicit KdeDefaultAppsBackend(QObject *parent = nullptr);
    explicit KdeDefaultAppsBackend(const DefaultAppsPaths &, QObject *parent = nullptr);
    void readCurrent(quint64 token) override;
    void writeChanges(quint64 token, const DefaultAppsSnapshot &expected,
                      const DefaultAppsPreferences &requested) override;
    void cancel() override;
private:
    void startNext();
    void enqueueRead(quint64 token, bool poll = false);
    void watch();
    DefaultAppsPaths m_paths;
    QFileSystemWatcher m_watcher;
    QTimer m_debounce, m_poll;
    QByteArray m_revision;
    QQueue<DefaultAppsJob> m_queue;
    DefaultAppsJob m_active;
    bool m_jobActive = false;
};
