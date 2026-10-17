#pragma once

#include "settings/NightTimeSettings.h"
#include <QDBusConnection>
#include <QFileSystemWatcher>
#include <QTimer>
#include <QPointer>
#include <functional>

class NightTimeService : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    virtual void query(quint64 token, const QString &expectedOwner = {}) = 0;
signals:
    void queryFinished(quint64 token, bool success, const NightTimeRuntime &runtime, const QString &error);
    void externalChange();
};

// An injected verifier performs no native activation and is used only by private
// bus fixtures. Production verifies the installed packages and process identity.
using NightTimeIdentityVerifier = std::function<void(uint pid, std::function<void(bool, QString)> completion)>;
NightTimeService *createNightTimeService(const QDBusConnection &connection, QObject *parent = nullptr);
NightTimeService *createNightTimeService(const QDBusConnection &connection,
                                       NightTimeIdentityVerifier verifier, QObject *parent = nullptr);

class NightTimeReadJob;
class NightTimeWriteJob;

class KdeNightTimeBackend final : public NightTimeBackend {
    Q_OBJECT
public:
    struct ConfigContext {
        bool fullCascade = false;
        QStringList defaultSources, daemonDefaultSources;
        QString cachedStateFile;
        // Private-file fixture seams; production ignores hooks and overrides.
        bool fixture=false;
        std::function<void()> beforeFileRead,beforeSync,afterSync;
        int softDeadlineMs=30000;
    };
    explicit KdeNightTimeBackend(QObject *parent = nullptr);
    KdeNightTimeBackend(const QString &absoluteFile, NightTimeService *service,
                        const ConfigContext &context, QObject *parent = nullptr);
    void readCurrent(quint64 token) override;
    void writeChanges(quint64 token, const NightTimeSnapshot &expected,
                      const NightTimeSnapshot &requested) override;
private:
    enum class Phase { Idle, Read, Write };
    QStringList relevantFiles(bool daemon=false) const;
    void watchFiles();
    QString m_file;
    ConfigContext m_context;
    QPointer<NightTimeService> m_service;
    Phase m_phase = Phase::Idle;
    quint64 m_token = 0;
    QPointer<NightTimeReadJob> m_readJob;
    QPointer<NightTimeWriteJob> m_writeJob;
    QFileSystemWatcher m_watcher;
    QTimer m_debounce;
};
