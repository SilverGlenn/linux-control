#pragma once

#include "OperationManager.h"
#include <QDateTime>
#include <QHash>
#include <QSet>

struct UpdatePackage { QString name, oldVer, newVer; bool aur = false; };

class UpdateService final : public QObject {
    Q_OBJECT
public:
    enum class Phase { Idle, Checking, Available, UpToDate, Incomplete, Error, Downloading, Installing, Done };
    struct State {
        Phase phase = Phase::Idle;
        QList<UpdatePackage> packages;
        QStringList selectedNames, selectedAur;
        QSet<QString> checkedNames;
        QHash<QString, QString> packageSizes;
        QDateTime lastCheck, lastInstall;
        bool everInstalled = false, lastInstallOk = false;
        bool pacmanAvailable = false, yayAvailable = false;
        bool repositoryCheckSucceeded = false, aurCheckSucceeded = false;
        QString error, aurWarning, output, currentPackage;
        int downloadCount = 0, installCount = 0;
    };
    static UpdateService *instance();
    const State &state() const { return m_state; }
    bool busy() const;
    void refreshAvailability();
    void checkForUpdates();
    // Returns an explanation without starting work when selection is unsafe,
    // empty, or a conflicting package transaction is already running.
    QString installUpdates();
    void setSelection(const QSet<QString> &names);
    void setCheckedNames(const QSet<QString> &names);
signals:
    void changed();
private:
    explicit UpdateService(QObject *parent);
    void saveState();
    void parseUpdateList(const QByteArray &data, bool aur);
    void runYayCheck();
    void finishCheck();
    void fetchPackageSizes();
    void runInstall(bool aur);
    void consumeInstallOutput(const QByteArray &data);
    void finishInstall(bool ok, const QString &error = {});
    State m_state;
    QSet<QString> m_seenInstalled, m_seenDownloaded;
    quint64 m_checkGeneration = 0;
    QString m_providerKey;
    QString m_stepOutput;
};
