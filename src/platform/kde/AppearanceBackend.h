#pragma once

#include "settings/AppearanceSettings.h"

#include <QFileSystemWatcher>
#include <QTimer>
#include <QStringList>
#include <QPointer>
#include <functional>

class QDBusConnection;

// Injection boundary for desktop reloads. Unit tests use a fake service and an
// absolute temporary KConfig file, never a real session bus or user preference.
class AppearanceReloadService : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    virtual bool isAvailable() const = 0;
    virtual QString unavailableReason() const = 0;
    virtual QString identity() const { return {}; }
    virtual void requestReload(const QString &action) = 0;
signals:
    // Success requires KWin's diagnostic reply to report the requested live option.
    void finished(bool success, const QString &error);
    void availabilityChanged();
};

// Diagnostic format and enum mapping are intentionally pinned to KWin 6.6.6.
// Unsupported versions disable Apply instead of guessing at a private enum.
bool parseKWinAppearanceReport(const QString &report, int &operation);

// An explicit connection permits disconnected/private-bus capability tests.
AppearanceReloadService *createKWinAppearanceReloadService(const QDBusConnection &connection,
                                                          QObject *parent = nullptr);

class KdeAppearanceBackend final : public AppearanceBackend {
    Q_OBJECT
public:
    explicit KdeAppearanceBackend(QObject *parent = nullptr);
    // Explicit temporary-file mode: SimpleConfig excludes globals and cascading.
    KdeAppearanceBackend(const QString &absoluteConfigPath,
                         AppearanceReloadService *reload, QObject *parent = nullptr);
    struct FileContext {
        bool fixture = false;
        int softDeadlineMs = 20000;
        std::function<void()> beforeRead, beforeSync, afterSync;
    };
    KdeAppearanceBackend(const QString &absoluteConfigPath, AppearanceReloadService *reload,
                         const FileContext &context, QObject *parent = nullptr);
    void readCurrent() override;
    void writeAction(const QString &expected, const QString &action) override;
private:
    KdeAppearanceBackend(const QString &configName, AppearanceReloadService *reload,
                         bool isolated, QObject *parent);
    void beginJob(bool write, const QString &expected = {}, const QString &requested = {});
    void watchFiles();
    QString m_configName;
    QString m_userPath;
    QStringList m_watchedFiles;
    AppearanceReloadService *m_reload;
    bool m_isolated;
    FileContext m_context;
    QPointer<QObject> m_job;
    QFileSystemWatcher m_watcher;
    QTimer m_debounce;
};
