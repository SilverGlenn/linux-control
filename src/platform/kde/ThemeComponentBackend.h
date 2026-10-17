#pragma once

#include "settings/ThemeComponentSettings.h"
#include <QFileSystemWatcher>
#include <QTimer>

// This acknowledges dispatch only, never live adoption by every application.
class ThemeComponentReloadService : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    virtual bool isAvailable(ThemeComponent component) const = 0;
    virtual QString unavailableReason(ThemeComponent component) const = 0;
    virtual bool requestReload(ThemeComponent component, QString &error) = 0;
signals:
    void availabilityChanged();
};

class KdeThemeComponentBackend final : public ThemeComponentBackend {
    Q_OBJECT
public:
    // Legacy production adapter is read-only. Approved versioned assets and
    // theme writes are owned by the shared AeroThemeBackend transaction.
    explicit KdeThemeComponentBackend(QObject *parent = nullptr);
    // Test boundary: absolute temporary directory, SimpleConfig, fake reloads,
    // injected installed choices. No real bus, icon cache or user files touched.
    KdeThemeComponentBackend(const QString &absoluteConfigDirectory,
                             ThemeComponentReloadService *reload,
                             const ThemeComponentChoices &choices,
                             QObject *parent = nullptr);
    void readCurrent() override;
    void writeComponents(const QStringList &expected, const QStringList &requested) override;
private:
    KdeThemeComponentBackend(const QString &directory, ThemeComponentReloadService *reload,
                             const ThemeComponentChoices &choices, bool isolated, QObject *parent);
    bool readState(ThemeComponentState &state, QString &error) const;
    ThemeComponentChoices installedChoices() const;
    QString configName(int component) const;
    void watchFiles();
    QString m_directory;
    ThemeComponentReloadService *m_reload;
    ThemeComponentChoices m_choices;
    bool m_isolated;
    QStringList m_watchedFiles;
    QFileSystemWatcher m_watcher;
    QTimer m_debounce;
};
