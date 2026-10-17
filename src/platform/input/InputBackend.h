#pragma once
#include "settings/InputSettings.h"
#include <QDBusConnection>
#include <QTimer>
#include <functional>
#include <memory>

struct InputReply {
    bool success = false;
    QVariant value;
    QString errorName;
};
// Injectable asynchronous transport. Tests supply a bounded fake runner or a
// private D-Bus; production never launches a shell or settings module.
class InputTransport : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    using Completion = std::function<void(const InputReply &)>;
    virtual void call(const QString &destination, const QString &path,
                      const QString &interface, const QString &method,
                      const QVariantList &arguments, Completion completion) = 0;
signals:
    void topologyChanged();
public slots:
    void topologyNotice(const QString &) { emit topologyChanged(); }
};
InputTransport *createInputDBusTransport(const QDBusConnection &connection, QObject *parent = nullptr);
class KWinInputBackend final : public InputBackend {
    Q_OBJECT
public:
    explicit KWinInputBackend(QObject *parent = nullptr);
    KWinInputBackend(InputTransport *transport, bool waylandSession, QObject *parent = nullptr,
                     int readBudgetMs = 12000, int writeBudgetMs = 45000);
    explicit KWinInputBackend(const QDBusConnection &connection, bool waylandSession, QObject *parent = nullptr);
    void readCurrent(quint64 token) override;
    void writeChanges(quint64 token, const InputSnapshot &expected, const InputSnapshot &requested) override;
private:
    using ReadCompletion = std::function<void(bool, const InputSnapshot &, const QString &)>;
    void snapshot(ReadCompletion completion);
    void readNextDevice(const std::shared_ptr<struct InputReadContext> &context);
    void finishRead(const std::shared_ptr<struct InputReadContext> &context);
    void applyNext(const std::shared_ptr<struct InputWriteContext> &context);
    void finishWrite(const std::shared_ptr<struct InputWriteContext> &context,
                     InputOutcome outcome, const InputSnapshot &snapshot, const QString &error);
    InputTransport *m_transport;
    QTimer m_poll;
    InputSnapshot m_observed;
    bool m_wayland, m_writing = false;
    int m_reads = 0, m_readBudgetMs = 12000, m_writeBudgetMs = 45000;
    quint64 m_epoch = 0;
    QString m_uncertainOwner;
};
