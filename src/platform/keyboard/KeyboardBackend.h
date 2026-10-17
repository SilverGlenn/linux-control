#pragma once
#include "settings/KeyboardSettings.h"
#include <QDBusConnection>
#include <QFileSystemWatcher>
#include <QTimer>
#include <functional>
#include <memory>

class KConfig;
struct KeyboardReply {
    bool success = false;
    QVariant value;
    QString error;
};
class KeyboardTransport : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    using Completion = std::function<void(const KeyboardReply &)>;
    virtual void call(const QString &destination, const QString &path, const QString &interface,
                      const QString &method, const QVariantList &arguments, Completion completion) = 0;
    virtual bool notify(const QString &owner, const QString &config, const QString &group,
                        const QStringList &keys) = 0;
signals:
    void changed();
};
KeyboardTransport *createKeyboardDBusTransport(const QDBusConnection &connection, QObject *parent = nullptr);
struct KeyboardBackendOptions {
    QString inputConfig = QStringLiteral("kcminputrc");
    QString layoutConfig = QStringLiteral("kxkbrc");
    QString workspaceConfig = QStringLiteral("plasmarc");
    QString registryPath = QStringLiteral("/usr/share/xkeyboard-config-2/rules/evdev.xml");
    bool isolated = false;
    bool wayland = true;
    int readBudgetMs = 10000;
    int writeBudgetMs = 20000;
    // Used only with isolated private-file fixtures.
    std::function<bool(KConfig &)> syncForTest;
};
class KdeKeyboardBackend final : public KeyboardBackend {
    Q_OBJECT
public:
    explicit KdeKeyboardBackend(QObject *parent = nullptr);
    KdeKeyboardBackend(KeyboardTransport *transport, const KeyboardBackendOptions &options,
                       QObject *parent = nullptr);
    void readCurrent(quint64 token) override;
    void writePreferences(quint64 token, const KeyboardSnapshot &expected,
                          const QVariantMap &requested) override;
private:
    using Completion = std::function<void(bool, const KeyboardSnapshot &, const QString &)>;
    void snapshot(Completion completion);
    bool readFiles(KeyboardSnapshot &snapshot, QString &error) const;
    void nextWrite(const std::shared_ptr<struct KeyboardWriteJob> &job);
    void finishWrite(const std::shared_ptr<struct KeyboardWriteJob> &job, KeyboardOutcome outcome,
                     const QString &error);
    void watchFiles();
    KeyboardTransport *m_transport;
    KeyboardBackendOptions m_options;
    QFileSystemWatcher m_watcher;
    QTimer m_debounce;
    std::shared_ptr<struct KeyboardWriteJob> m_job;
    int m_reads = 0;
    QString m_uncertainOwner;
};
