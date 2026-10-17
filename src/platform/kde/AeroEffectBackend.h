#pragma once

#include "settings/AeroEffectSettings.h"
#include <QFileSystemWatcher>
#include <QJsonObject>
#include <QStringList>
#include <QTimer>

class QDBusConnection;

// Metadata is read with QPluginLoader::metaData(), never instance()/load().
struct AeroEffectPluginIdentity {
    QByteArray sha256;
    QJsonObject metadata;
    QString error;
};
QByteArray supportedAeroEffectSha256();
AeroEffectPluginIdentity inspectAeroEffectPlugin(const QString &absolutePath);
bool validateAeroEffectPlugin(const AeroEffectPluginIdentity &identity, QString &reason);
bool isSupportedAeroKWinReport(const QString &report);

class AeroEffectRuntime : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    virtual void probe() = 0; // Read-only version/interface/loaded checks.
    virtual void reconfigure() = 0; // Only the fixed aeroglassblur effect.
signals:
    void probeFinished(bool compatible, bool loaded, const QString &reason);
    // Success is method reply + loaded check, not a rendered-strength assertion.
    void reconfigureFinished(bool success, const QString &error);
    void externalChange();
};
AeroEffectRuntime *createAeroEffectRuntime(const QDBusConnection &connection);

// Production writes are retired in favor of the shared Aero theme transaction.
// readCurrent() is the public read-only probe. Its state reports installation
// support, KWin compatibility, loaded status, writable policy, and saved value.
class KdeAeroEffectBackend final : public AeroEffectBackend {
    Q_OBJECT
public:
    explicit KdeAeroEffectBackend(QObject *parent = nullptr);
    KdeAeroEffectBackend(const QString &absoluteConfigFile, AeroEffectRuntime *runtime,
                         const AeroEffectPluginIdentity &identity, QObject *parent = nullptr);
    void readCurrent() override;
    void writeStrength(int expected, int requested) override;
private:
    KdeAeroEffectBackend(const QString &, AeroEffectRuntime *, const AeroEffectPluginIdentity &, bool, QObject *);
    bool readState(AeroEffectState &state, QString &error) const;
    void onProbe(bool compatible, bool loaded, const QString &reason);
    void watchFiles();
    QString m_configName, m_userFile, m_pluginPath;
    QStringList m_watchedFiles;
    AeroEffectRuntime *m_runtime;
    AeroEffectPluginIdentity m_identity;
    bool m_isolated;
    bool m_runtimeSupported = false, m_loaded = false;
    QString m_runtimeReason;
    int m_expected = 3, m_requested = 3;
    enum class Phase { Idle, Read, Prewrite, Reconfigure } m_phase = Phase::Idle;
    QFileSystemWatcher m_watcher;
    QTimer m_debounce;
};
