#pragma once
#include "settings/ShortcutsTypes.h"
#include <QDBusConnection>
#include <QFileSystemWatcher>
#include <QTimer>
#include <QVariant>
#include <functional>
#include <memory>

struct ShortcutsReply {
    bool success = false;
    QVariant value;
    QString error;
    bool uncertain = false;
};
class ShortcutsTransport : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    using Completion = std::function<void(const ShortcutsReply &)>;
    virtual void call(const QString &destination, const QString &path, const QString &interface,
                      const QString &method, const QVariantList &arguments, Completion completion) = 0;
    virtual void reloadLayouts(const QString &owner, Completion completion);
signals:
    void changed();
};
ShortcutsTransport *createShortcutsDBusTransport(const QDBusConnection &connection, QObject *parent = nullptr);
struct ShortcutsBackendOptions {
    QString config = QStringLiteral("kglobalshortcutsrc");
    bool isolated = false;
    int readBudgetMs = 30000;
    int writeBudgetMs = 60000;
    int persistenceBudgetMs = 4000;
    bool enableLayoutAdapter = false;
    QString layoutConfig = QStringLiteral("kxkbrc");
};
class KdeShortcutsBackend final : public ShortcutsBackend {
    Q_OBJECT
public:
    explicit KdeShortcutsBackend(QObject *parent = nullptr);
    KdeShortcutsBackend(ShortcutsTransport *transport, const ShortcutsBackendOptions &options, QObject *parent = nullptr);
    void readCurrent(quint64 token) override;
    void writeChanges(quint64 token, const ShortcutsSnapshot &expected, const ShortcutChanges &requested) override;
private:
    using Completion = std::function<void(bool,const ShortcutsSnapshot &,const QString &)>;
    void snapshot(Completion completion);
    void readLayoutAdapter(const QString &rootXml, std::function<void(const ShortcutsLayoutAdapter &)> completion);
    void guardLayoutState(const std::shared_ptr<struct ShortcutsWriteJob> &job, std::function<void(bool,const QString &)> completion);
    void reloadLayoutCallbacks(const std::shared_ptr<struct ShortcutsWriteJob> &job);
    void nextWrite(const std::shared_ptr<struct ShortcutsWriteJob> &job);
    void preflight(const std::shared_ptr<struct ShortcutsWriteJob> &job);
    bool guardDisk(const std::shared_ptr<struct ShortcutsWriteJob> &job, QString &error);
    void verifyPersistence(const std::shared_ptr<struct ShortcutsWriteJob> &job);
    void finishWrite(const std::shared_ptr<struct ShortcutsWriteJob> &job, ShortcutsOutcome outcome, const QString &error);
    void watchFiles();
    ShortcutsTransport *m_transport;
    ShortcutsBackendOptions m_options;
    QFileSystemWatcher m_watcher;
    QTimer m_debounce;
    std::shared_ptr<struct ShortcutsWriteJob> m_job;
    QString m_uncertainOwner;
    int m_reads = 0;
};
