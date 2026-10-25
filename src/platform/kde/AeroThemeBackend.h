#pragma once

#include "settings/AeroThemeSettings.h"
#include <QFileSystemWatcher>
#include <QTimer>
#include <memory>
#include <functional>
class KConfig;
class QDBusConnection;
class AeroThemeFileController;
struct AeroThemeBackendOptions {
    // Isolated fixtures only. Production always uses native synchronization.
    std::function<void()> beforeRead, beforeSync, afterSync;
    std::function<bool(KConfig &)> syncForTest;
    int softDeadlineMs = 20000;
    int runtimeDeadlineMs = 12000;
};

class AeroThemeAssetProbe {
public:
    virtual ~AeroThemeAssetProbe() = default;
    virtual bool validate(QStringList &issues) const = 0;
};
class AeroThemeRuntime : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    virtual void probe() = 0;
    virtual void reload(const QSet<QString> &changedKeys) = 0;
    // Recheck immediately before each unsent native request after asynchronous discovery.
    virtual void reloadGuarded(const QSet<QString> &changedKeys, std::function<bool()> guard) {
        if (guard()) reload(changedKeys);
        else emit reloadFinished(false, tr("Aero reload was retired before dispatch. Saved preferences remain."));
    }
signals:
    void probeFinished(bool supported, const QString &reason);
    // Acknowledgement/notification dispatch only; new app sessions may be needed.
    void reloadFinished(bool success, const QString &error);
    void externalChange();
};

// Explicit private connection supports wire/owner fixtures without a session bus.
AeroThemeRuntime *createKdeAeroThemeRuntime(const QDBusConnection &bus);

class KdeAeroThemeBackend final : public AeroThemeBackend {
    Q_OBJECT
public:
    explicit KdeAeroThemeBackend(QObject *parent = nullptr);
    // Isolated boundary: SimpleConfig in an absolute temporary directory, fake
    // assets/runtime, no real bus, palette notifications, or system mutation.
    KdeAeroThemeBackend(const QString &absoluteConfigDirectory, AeroThemeRuntime *runtime,
                        std::unique_ptr<AeroThemeAssetProbe> assets, QObject *parent = nullptr);
    KdeAeroThemeBackend(const QString &absoluteConfigDirectory, AeroThemeRuntime *runtime,
                        std::unique_ptr<AeroThemeAssetProbe> assets, const AeroThemeBackendOptions &options, QObject *parent = nullptr);
    ~KdeAeroThemeBackend() override;
    void cancel() override;
    void readCurrent() override;
    void writeSettings(const QVariantMap &expected, const QVariantMap &requested) override;
private:
    KdeAeroThemeBackend(const QString &, AeroThemeRuntime *, std::unique_ptr<AeroThemeAssetProbe>, bool, const AeroThemeBackendOptions &, QObject *);
    AeroThemeFileController *m_controller;
};
