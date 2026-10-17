#pragma once
#include "settings/FontSettings.h"
#include <QFileSystemWatcher>
#include <QTimer>
#include <QStringList>
#include <functional>
class KConfig;
class QDBusConnection;

// Completion means the refresh request was sent, never that applications adopted
// the fonts. Rendering changes require applications to restart.
class FontReloadService : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    virtual bool isAvailable() const = 0;
    virtual QString unavailableReason() const = 0;
    virtual void requestReload() = 0;
signals:
    void finished(bool success, const QString &error);
    void availabilityChanged();
};
FontReloadService *createKdeFontReloadService(const QDBusConnection &bus, QObject *parent = nullptr);
// Fontconfig reports files AND include directories. Directory membership must
// participate in conflict detection without trying to read a directory as XML.
bool fontConfigurationRevision(const QStringList &paths, QByteArray &revision, QString &error);
class KdeFontBackend final : public FontBackend {
    Q_OBJECT
public:
    explicit KdeFontBackend(QObject *parent = nullptr);
    // Both paths must be absolute. No global settings/fontconfig or session bus
    // are used through this constructor; tests inject a fake reload service.
    KdeFontBackend(const QString &kdeglobalsPath, const QString &fontconfigPath,
                   FontReloadService *reload, QObject *parent = nullptr);
    // Isolated explicit-path fixtures only; production instances ignore it.
    void setSyncForTest(std::function<bool(KConfig &)> sync) { if (m_isolated) m_syncForTest = std::move(sync); }
    void readCurrent() override;
    void writePreferences(const FontState &expected, const FontPreferences &requested) override;
private:
    KdeFontBackend(const QString &, const QString &, FontReloadService *, bool isolated, QObject *);
    bool readState(FontState &state, QString &error) const;
    QStringList relevantFiles() const;
    void watchFiles();
    QString m_configName, m_userPath, m_fontconfigPath;
    FontReloadService *m_reload;
    bool m_isolated, m_writing = false;
    std::function<bool(KConfig &)> m_syncForTest;
    QFileSystemWatcher m_watcher;
    QTimer m_debounce, m_reloadTimeout;
};
