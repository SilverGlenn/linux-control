#pragma once
#include "settings/AeroLoginSettings.h"
#include <QFileSystemWatcher>
#include <QProcessEnvironment>
#include <QTimer>
#include <functional>

struct AeroLoginHelperCapability {
    bool available=false;
    QString reason;
    QByteArray revision;
};
// Only isolated verification fixtures use custom paths/uid. Production always
// takes the compiled helper and the fixed installed policy/approval paths.
struct AeroLoginHelperContext {
    QString helperPath,policyPath,approvalPath;
    quint32 expectedUid=0;
    bool fixture=false;
};
AeroLoginHelperCapability inspectAeroLoginHelper(const AeroLoginHelperContext &context={});
struct AeroLoginAuthPlan {
    bool valid=false;
    QString program,error;
    QStringList arguments;
    QProcessEnvironment environment;
    QByteArray input;
};
// Pure construction for review/fixtures; never starts authorization or a helper.
AeroLoginAuthPlan aeroLoginAuthPlan(const QByteArray &request,bool recovery);

class AeroLoginAuthRunner:public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    virtual void start(quint64 token,const QByteArray &request,bool recovery)=0;
signals:
    // Emitted only after a known terminal process outcome, never at a soft timeout.
    void finished(quint64 token,bool processSucceeded,const QByteArray &reply,const QString &error);
    void delayed(quint64 token,const QString &message);
};
AeroLoginAuthRunner *createAeroLoginAuthRunner(QObject *parent=nullptr);

class KdeAeroLoginBackend final:public AeroLoginBackend {
    Q_OBJECT
public:
    using Reader=std::function<AeroLoginSnapshot(QString &)>;
    using HelperCheck=std::function<AeroLoginHelperCapability()>;
    explicit KdeAeroLoginBackend(QObject *parent=nullptr);
    KdeAeroLoginBackend(AeroLoginAuthRunner *runner,Reader reader,HelperCheck helperCheck,
                        const QStringList &watchedPaths,QObject *parent=nullptr);
    void readCurrent(quint64 token) override;
    void writeChanges(quint64 token,const AeroLoginSnapshot &expected,const AeroLoginSnapshot &requested) override;
    void recover(quint64 token,const AeroLoginSnapshot &expected) override;
signals:
    void authDelayed(quint64 token,const QString &message);
private:
    enum class Phase{Idle,Read,Preflight,RecoveryPreflight,Authorization,Readback};
    void beginRead();
    void onRead(bool success,const AeroLoginSnapshot &snapshot,const QString &error);
    void onAuthorized(quint64 token,bool processSucceeded,const QByteArray &reply,const QString &error);
    void finishWrite(bool saved,const QString &error);
    void watchFiles();
    AeroLoginAuthRunner *m_runner;
    Reader m_reader;HelperCheck m_helperCheck;QStringList m_paths;
    Phase m_phase=Phase::Idle;quint64 m_token=0;
    AeroLoginSnapshot m_expected,m_requested;
    bool m_recovery=false,m_saved=false;QString m_writeError;QByteArray m_postRevision;
    QFileSystemWatcher m_watcher;QTimer m_debounce;
};
