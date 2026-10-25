#pragma once

#include <QObject>
#include <QString>

class ProcessJob;

// Application-owned manual update workflow. Constructing or displaying it never
// starts a process or contacts a release server.
class ControlPanelUpdateService final : public QObject {
    Q_OBJECT
public:
    enum class Phase {
        Idle, Checking, Downloading, NotConfigured, NoReleases, Available,
        UpToDate, Incompatible, Ready, Installed, RolledBack, ManualRepair, Error
    };
    struct State {
        Phase phase = Phase::Idle;
        QString message, buildId, releaseTag, receipt;
        bool installerLaunched = false;
    };
    static ControlPanelUpdateService *instance();
    // Explicit dependency injection for private fixture helpers. The same path,
    // ownership and permissions checks apply as in production; no env override.
    explicit ControlPanelUpdateService(const QString &helperPath, QObject *parent = nullptr);
    const State &state() const { return m_state; }
    bool busy() const;
    static QString currentBuildId();
    void checkForUpdates();
    void prepareUpdate();
    void refreshLocalStatus();
    // MainWindow calls this only for the explicit Close and install action and its full exit
    // guard. The detached trusted helper waits for the application lock to clear.
    bool launchInstaller(QString *error = nullptr);
signals:
    void changed();
private:
    void run(const QString &command);
    void consumeResult(ProcessJob *job, const QString &command);
    void fail(const QString &message);
    bool helperAvailable(QString *error) const;
    QString m_helperPath;
    State m_state;
};
