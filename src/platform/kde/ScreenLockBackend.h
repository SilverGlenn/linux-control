#pragma once
#include "settings/ScreenLockSettings.h"
#include <QDBusConnection>
#include <QFileSystemWatcher>
#include <QTimer>
#include <QPointer>
#include <functional>

class ScreenLockService : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    using GuardCompletion=std::function<void(bool,const QString &)>;
    using Guard=std::function<void(GuardCompletion)>;
    virtual void query(quint64 token,const QString &expectedOwner={})=0;
    virtual void configure(quint64 token,const QString &expectedOwner,quint64 expectedEpoch,Guard guard)=0;
signals:
    void queryFinished(quint64 token,bool success,const ScreenLockRuntime &runtime,const QString &error);
    void configureFinished(quint64 token,bool acknowledged,const QString &error);
    void externalChange();
};
using ScreenLockIdentityVerifier=std::function<void(uint pid,std::function<void(bool,QString)> completion)>;
ScreenLockService *createScreenLockService(const QDBusConnection &connection,QObject *parent=nullptr);
ScreenLockService *createScreenLockService(const QDBusConnection &connection,ScreenLockIdentityVerifier verifier,QObject *parent=nullptr);

class ScreenLockWriteJob;

class KdeScreenLockBackend final : public ScreenLockBackend {
    Q_OBJECT
public:
    struct ConfigContext {
        bool fullCascade=false;
        QStringList defaultSources;
        // Private-file fixture seams. Production ignores all hooks and overrides.
        bool fixture=false;
        std::function<void()> beforeFileRead,beforeSync,afterSync,beforeBoundaryRead;
        int softDeadlineMs=30000;
    };
    explicit KdeScreenLockBackend(QObject *parent=nullptr);
    KdeScreenLockBackend(const QString &absoluteFile,ScreenLockService *service,const ConfigContext &context,QObject *parent=nullptr);
    void readCurrent(quint64 token) override;
    void writeChanges(quint64 token,const ScreenLockSnapshot &expected,const ScreenLockSnapshot &requested) override;
private:
    enum class Phase {Idle,ReadQuery,ReadFiles,Write};
    void queryFinished(quint64 token,bool success,const ScreenLockRuntime &runtime,const QString &error);
    QStringList relevantFiles() const;
    void watchFiles();
    QString m_file;ScreenLockService *m_service;ConfigContext m_context;
    Phase m_phase=Phase::Idle;quint64 m_token=0;
    QPointer<ScreenLockWriteJob> m_writeJob;
    QFileSystemWatcher m_watcher;QTimer m_debounce,m_deadline;
};
