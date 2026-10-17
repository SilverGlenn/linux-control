#pragma once

#include "settings/NetworkSettings.h"
#include <QDBusConnection>
#include <QDBusMessage>
#include <QElapsedTimer>
#include <functional>

// Production always uses NetworkManager's system-bus protocol. An explicitly
// injected connection/service is exclusively for isolated D-Bus fixtures.
class NetworkManagerBackend final : public NetworkBackend {
    Q_OBJECT
public:
    struct Options {
        QString service = QStringLiteral("org.freedesktop.NetworkManager");
        int readTimeoutMs = 3000;
        int snapshotTimeoutMs = 12000;
        int writeTimeoutMs = 20000;
        int pollIntervalMs = 250;
        uint rollbackSeconds = 60;
    };
    explicit NetworkManagerBackend(QObject *parent = nullptr);
    NetworkManagerBackend(const QDBusConnection &bus, const Options &options, QObject *parent = nullptr);
    void readCurrent() override;
    void scan(const QString &devicePath) override;
    void beginChange(const NetworkSnapshot &baseline, const NetworkRequest &request) override;
    void keepChange() override;
    void revertChange() override;

private slots:
    void ownerChanged(const QString &name, const QString &oldOwner, const QString &newOwner);
    void daemonChanged();
    void propertiesChanged(const QString &, const QVariantMap &, const QStringList &);

private:
    using SnapshotCallback = std::function<void(bool, const NetworkSnapshot &, const QString &)>;
    using ReplyCallback = std::function<void(const QDBusMessage &)>;
    void call(const QString &destination, const QString &path, const QString &interface,
              const QString &method, const QVariantList &args, bool write, const ReplyCallback &done);
    void getAll(const QString &owner, const QString &path, const QString &interface,
                const std::function<void(bool, const QVariantMap &, const QString &)> &done);
    void readSnapshot(const SnapshotCallback &done);
    void createCheckpoint();
    void mutate();
    void pollOutcome(bool restoring = false);
    void persist(bool inactive);
    void destroyCheckpoint(const NetworkSnapshot &verified);
    void rollback(const QString &reason);
    void abandonOwnedCheckpoint(const NetworkSnapshot &current, const QString &reason);
    void finish(bool kept, bool restored, const NetworkSnapshot &snapshot, const QString &error);
    void uncertain(const QString &error);
    bool sameOwnership(const NetworkSnapshot &snapshot) const;
    bool expectedProfiles(const NetworkSnapshot &snapshot) const;
    bool restoredState(const NetworkSnapshot &snapshot) const;
    bool checkpointLive(const NetworkSnapshot &snapshot) const;
    QString operationError(const QDBusMessage &reply) const;

    QDBusConnection m_bus;
    Options m_options;
    NetworkSnapshot m_last, m_baseline, m_preview;
    NetworkRequest m_request;
    NetworkConnectionData m_expectedSettings;
    QString m_owner, m_checkpoint, m_newProfile, m_activeTarget, m_newUuid, m_failure;
    quint64 m_generation = 0;
    quint64 m_profileVersion = 0;
    bool m_busy = false, m_confirming = false, m_rollingBack = false;
    bool m_uncertain = false, m_scanBusy = false, m_readBusy = false;
    bool m_mutationPending = false;
    bool m_supportsActivate2 = false;
    bool m_checkpointKnownAbsent = true;
    QElapsedTimer m_elapsed;
    QTimer m_deadline;
};
