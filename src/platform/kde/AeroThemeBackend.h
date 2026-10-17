#pragma once

#include "settings/AeroThemeSettings.h"
#include <QFileSystemWatcher>
#include <QTimer>
#include <memory>

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
signals:
    void probeFinished(bool supported, const QString &reason);
    // Acknowledgement/notification dispatch only; new app sessions may be needed.
    void reloadFinished(bool success, const QString &error);
    void externalChange();
};

class KdeAeroThemeBackend final : public AeroThemeBackend {
    Q_OBJECT
public:
    explicit KdeAeroThemeBackend(QObject *parent = nullptr);
    // Isolated boundary: SimpleConfig in an absolute temporary directory, fake
    // assets/runtime, no real bus, palette notifications, or system mutation.
    KdeAeroThemeBackend(const QString &absoluteConfigDirectory, AeroThemeRuntime *runtime,
                        std::unique_ptr<AeroThemeAssetProbe> assets, QObject *parent = nullptr);
    void readCurrent() override;
    void writeSettings(const QVariantMap &expected, const QVariantMap &requested) override;
private:
    KdeAeroThemeBackend(const QString &, AeroThemeRuntime *, std::unique_ptr<AeroThemeAssetProbe>, bool, QObject *);
    void startProbe();
    void finishProbe();
    bool readState(AeroThemeState &state, QString &error) const;
    bool save(QString &error);
    void watchFiles();
    QString configName(const QString &file) const;
    QString m_directory;
    AeroThemeRuntime *m_runtime;
    std::unique_ptr<AeroThemeAssetProbe> m_assets;
    bool m_isolated, m_assetsSupported = false, m_runtimeSupported = false;
    bool m_assetsDone = false, m_runtimeDone = false, m_timedOut = false;
    quint64 m_generation = 0;
    QStringList m_assetIssues;
    QString m_runtimeReason;
    QVariantMap m_expected, m_requested;
    enum class Phase { Idle, Read, Preflight, Reload } m_phase = Phase::Idle;
    QFileSystemWatcher m_watcher;
    QTimer m_debounce, m_timeout;
    QStringList m_watchedFiles;
};
