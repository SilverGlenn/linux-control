#pragma once

#include "settings/PowerPolicySettings.h"
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusVariant>
#include <QVariantMap>
#include <QFileSystemWatcher>
#include <QTimer>
#include <QDBusContext>
#include <QPointer>
#include <functional>

struct PowerPolicyCapabilities {
    bool serviceAvailable = false;
    bool schemaSupported = false;
    bool displaySupported = false;
    bool sleepSupported = false;
    bool suspendActionSupported = false;
    bool canSuspendKnown = false, canSuspend = false;
    bool hibernateKnown = false, hibernateSupported = false;
    bool hybridSuspendSupported = false, suspendThenHibernateSupported = false;
    bool dimSupported = false, brightnessSupported = false, keyboardBrightnessSupported = false;
    bool buttonSupported = false, lidKnown = false, hasLid = false;
    bool batteryKnown = false;
    bool hasBattery = false;
    bool mobileKnown = false;
    bool mobile = false;
    bool defaultMobileKnown = false, defaultMobile = false;
    bool virtualMachineKnown = false;
    bool virtualMachine = false;
    QString reason;
    QString owner, revision;
};
Q_DECLARE_METATYPE(PowerPolicyCapabilities)

// Tests inject this D-Bus boundary. No test needs the real session daemon.
class PowerPolicyDaemon : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    virtual void queryCapabilities() = 0;
    virtual void reload() = 0;
    virtual void cancel() = 0;
    using GuardCompletion=std::function<void(bool,const QString &)>;
    using Guard=std::function<void(GuardCompletion)>;
    virtual void reloadGuarded(const PowerPolicyCapabilities &expected, Guard guard);
    // Local getter generation retirement only; never cancels a native setter.
    virtual void retireCapabilitiesQuery() {}
signals:
    void capabilitiesFinished(const PowerPolicyCapabilities &capabilities);
    void reloadFinished(bool acknowledged, bool cancelled, const QString &error);
    void changed();
    void providerChanged();
};

class PowerPolicyDBus final : public PowerPolicyDaemon, protected QDBusContext {
    Q_OBJECT
public:
    explicit PowerPolicyDBus(QObject *parent = nullptr);
    PowerPolicyDBus(const QDBusConnection &session, const QDBusConnection &system,
                    bool schemaSupported, QObject *parent = nullptr);
    void queryCapabilities() override;
    void reload() override;
    void cancel() override;
    void reloadGuarded(const PowerPolicyCapabilities &expected, Guard guard) override;
    void retireCapabilitiesQuery() override;
private slots:
    void configurationReloaded();
    void capabilitiesChanged();
    void portalSettingChanged(const QString &group, const QString &key, const QDBusVariant &value);
    void hardwarePropertiesChanged(const QString &interface, const QVariantMap &changed,
                                   const QStringList &invalidated);
    void powerSourceChanged(const QString &profile);
private:
    using Reply = std::function<void(const QDBusMessage &)>;
    void call(const QDBusConnection &bus, const QString &service, const QString &path,
              const QString &interface, const QString &method, const QVariantList &args,
              Reply completion);
    void finishReload(bool acknowledged, bool cancelled, const QString &error);
    void queryNativeCapabilities(quint64 generation, quint64 epoch, bool schemaSupported);
    void invalidateProvider(const QString &reason);
    void sendReload(const QString &owner, quint64 epoch);
    QDBusConnection m_session, m_system;
    bool m_schemaSupported = false, m_schemaAuto = false, m_guardedReload = false;
    bool m_reloading = false;
    bool m_reloadSignal = false;
    bool m_reloadReply = false;
    quint64 m_generation = 0;
    quint64 m_queryGeneration = 0, m_ownerEpoch = 0;
    QString m_owner, m_reloadOwner, m_capabilityRevision;
    quint64 m_guardGeneration = 0;
    QTimer m_reloadTimer;
};

class PowerPolicyFileJob;

class KdePowerPolicyBackend final : public PowerPolicyBackend {
    Q_OBJECT
public:
    explicit KdePowerPolicyBackend(QObject *parent = nullptr);
    KdePowerPolicyBackend(const QString &configFile, PowerPolicyDaemon *daemon,
                          QObject *parent = nullptr);
    // Full cascading uses active XDG roots; fixtures must isolate them before
    // constructing QCoreApplication. Simple injected paths never load globals.
    struct ConfigContext {
        bool fullCascade = false;
        QStringList defaultSources;
        bool fixture = false;
        std::function<void()> beforeFileRead,beforeSync,afterSync;
        int softDeadlineMs = 20000;
    };
    KdePowerPolicyBackend(const QString &configFile, PowerPolicyDaemon *daemon,
                          const ConfigContext &context, QObject *parent = nullptr);
    void readCurrent() override;
    void writePolicy(const PowerPolicySnapshot &expected,
                     const PowerPolicySnapshot &requested) override;
    void cancel() override;
private:
    void watchConfig();
    void beginJob(bool write,const PowerPolicySnapshot &expected={},const PowerPolicySnapshot &requested={});
    QString m_configFile;
    PowerPolicyDaemon *m_daemon;
    ConfigContext m_context;
    QPointer<PowerPolicyFileJob> m_job;
    QFileSystemWatcher m_watcher;
    QTimer m_debounce;
};
