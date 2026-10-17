#pragma once
#include <QObject>
#include <QDateTime>
#include <QList>
#include <QStringList>

struct PackageChange {
    enum class Action { Upgrade, Install, Remove, Configure };
    Action action = Action::Upgrade;
    QString package, installedVersion, candidateVersion, source;
};
Q_DECLARE_METATYPE(PackageChange)
struct PackageSimulationResult {
    bool success = false;
    QList<PackageChange> changes;
    int upgraded = 0, installed = 0, removed = 0, notUpgraded = 0;
    QString error, warnings, rawOutput;
    QDateTime completedAt;
};
Q_DECLARE_METATYPE(PackageSimulationResult)
struct PackageSimulationProcessResult {
    QByteArray standardOutput, standardError;
    int exitCode = -1;
    bool normalExit = false, cancelled = false, timedOut = false, outputTruncated = false;
    QString error;
};
Q_DECLARE_METATYPE(PackageSimulationProcessResult)

// Parses only a complete C-locale apt-get simulation with matching operation
// records and numeric summary. A successful exit by itself is insufficient.
PackageSimulationResult parseAptPackageSimulation(const QByteArray &output);
QStringList aptPackageSimulationArguments(const QString &absoluteSafetyConfigPath);
QByteArray aptPackageSimulationSafetyConfiguration();
class PackageSimulationBackend : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    virtual bool isAvailable() const = 0;
    virtual QString unavailableReason() const = 0;
    virtual void start(quint64 requestId) = 0;
    virtual void cancel(quint64 requestId) = 0;
signals:
    void finished(quint64 requestId, const PackageSimulationProcessResult &result);
};
// Tests may inject an absolute executable that emits fixture output. Production
// always uses /usr/bin/apt-get and immutable read-only argv; no shell is involved.
PackageSimulationBackend *createAptPackageSimulationBackend(QObject *parent = nullptr);
PackageSimulationBackend *createAptPackageSimulationBackend(const QString &absoluteExecutable,
                                                            int timeoutMs, QObject *parent = nullptr);
class PackageSimulationService final : public QObject {
    Q_OBJECT
public:
    enum class State { Idle, Running, Cancelling, Succeeded, Failed, Cancelled, TimedOut };
    Q_ENUM(State)
    static PackageSimulationService *instance();
    explicit PackageSimulationService(PackageSimulationBackend *backend, QObject *parent = nullptr);
    State state() const { return m_state; }
    bool isBusy() const { return m_state == State::Running || m_state == State::Cancelling; }
    bool providerAvailable() const { return m_backend->isAvailable(); }
    QString unavailableReason() const { return m_backend->unavailableReason(); }
    QString statusText() const;
    PackageSimulationResult result() const { return m_result; }
    QString providerName() const { return QStringLiteral("APT full-upgrade simulation"); }
public slots:
    void startPreview();
    void cancel();
signals:
    void changed();
private:
    void complete(quint64 requestId, const PackageSimulationProcessResult &result);
    PackageSimulationBackend *m_backend;
    State m_state = State::Idle;
    PackageSimulationResult m_result;
    quint64 m_requestId = 0;
};
