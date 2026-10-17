#pragma once

#include "settings/WallpaperSettings.h"
#include <QFileSystemWatcher>
#include <QTimer>

class QDBusConnection;
class WallpaperScriptTransport : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    virtual void evaluate(const QString &script) = 0;
signals:
    void finished(bool success, const QString &output, const QString &error);
    void serviceChanged();
};
WallpaperScriptTransport *createWallpaperScriptTransport(const QDBusConnection &connection);

// The same Plasma scripting interface used by plasma-apply-wallpaperimage.
// Data is JSON encoded; neither paths nor identifiers are interpolated as code.
QString wallpaperReadScript();
QString wallpaperWriteScript(const WallpaperDesktop &expected, const QString &image, int fillMode);
bool parseWallpaperState(const QString &json, WallpaperState &state, QString &error);
bool validateWallpaperPicture(const QString &url, QString &error);

class KdeWallpaperBackend final : public WallpaperBackend {
    Q_OBJECT
public:
    explicit KdeWallpaperBackend(QObject *parent = nullptr);
    // An absolute temporary KConfig file excludes the real desktop's config.
    KdeWallpaperBackend(WallpaperScriptTransport *transport, const QString &absoluteConfigFile,
                        QObject *parent = nullptr);
    void readCurrent() override;
    void writePicture(const WallpaperDesktop &expected, const QString &image, int fillMode) override;
private:
    KdeWallpaperBackend(WallpaperScriptTransport *, const QString &, bool, QObject *);
    void applyCapabilities(WallpaperState &state) const;
    void watchFiles();
    WallpaperScriptTransport *m_transport;
    QString m_configName, m_userFile;
    bool m_isolated;
    enum class Phase { Idle, Read, Write } m_phase = Phase::Idle;
    QFileSystemWatcher m_watcher;
    QTimer m_debounce, m_poll;
};
