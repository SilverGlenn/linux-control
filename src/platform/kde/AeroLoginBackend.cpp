#include "AeroLoginBackend.h"
#include "login/AeroLoginFiles.h"
#include "login/AeroLoginProtocol.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QXmlStreamReader>
#include <QtConcurrent/QtConcurrentRun>
#include <sys/stat.h>
#include <atomic>
#include <memory>

#ifndef CONTROL_AERO_LOGIN_HELPER_PATH
#define CONTROL_AERO_LOGIN_HELPER_PATH "/usr/libexec/controlpanel-aero-login-helper"
#endif
#ifndef CONTROL_AERO_LOGIN_POLICY_PATH
#define CONTROL_AERO_LOGIN_POLICY_PATH "/usr/share/polkit-1/actions/org.linuxcontrol.aerologin.policy"
#endif
#ifndef CONTROL_AERO_LOGIN_MANIFEST_PATH
#define CONTROL_AERO_LOGIN_MANIFEST_PATH "/usr/share/linux-control/aero-login-helper.json"
#endif
namespace {
const QString Action=QStringLiteral("org.linuxcontrol.aerologin.configure");
const QString Policy=QString::fromUtf8(CONTROL_AERO_LOGIN_POLICY_PATH);
const QString Approval=QString::fromUtf8(CONTROL_AERO_LOGIN_MANIFEST_PATH);
constexpr qint64 MaximumRequestBytes=24*1024*1024,MaximumReplyBytes=65536;
std::atomic_bool ActiveAuthorization{false};
struct AuthorizationLease {~AuthorizationLease(){ActiveAuthorization=false;}};
QByteArray hash(const QByteArray &bytes){return QCryptographicHash::hash(bytes,QCryptographicHash::Sha256);}
bool trustedPath(const QString &path,const QString &anchor,quint32 expectedUid,bool executable,QString &error)
{
    if(!QFileInfo(path).isAbsolute()||QDir::cleanPath(path)!=path||(!anchor.isEmpty()&&path!=anchor&&!path.startsWith(anchor+QLatin1Char('/')))){
        error=QStringLiteral("The helper installation contains an invalid fixed path.");return false;
    }
    if(anchor.isEmpty()){
        struct stat root{};if(::lstat("/",&root)!=0||!S_ISDIR(root.st_mode)||root.st_uid!=expectedUid||(root.st_mode&0022)){
            error=QStringLiteral("The root of the helper installation is untrusted.");return false;
        }
    }
    const auto components=path.split(QLatin1Char('/'),Qt::SkipEmptyParts);QString current;
    for(int i=0;i<components.size();++i){
        current+=QLatin1Char('/')+components[i];if(!anchor.isEmpty()&&current!=anchor&&!current.startsWith(anchor+QLatin1Char('/')))continue;
        struct stat metadata{};if(::lstat(QFile::encodeName(current).constData(),&metadata)!=0||S_ISLNK(metadata.st_mode)||metadata.st_uid!=expectedUid||(metadata.st_mode&0022)){
            error=QStringLiteral("The helper, policy, approval, or an ancestor is missing, untrusted, writable by other users, or a symlink.");return false;
        }
        if(i+1<components.size()? !S_ISDIR(metadata.st_mode):!S_ISREG(metadata.st_mode)){
            error=QStringLiteral("The helper installation contains a nonregular file or nondirectory ancestor.");return false;
        }
        if(i+1==components.size()&&((metadata.st_mode&06000)||(executable&&!(metadata.st_mode&0111)))){
            error=QStringLiteral("The helper must be a protected ordinary executable, without setuid or setgid bits.");return false;
        }
    }
    return true;
}
QByteArray readBounded(const QString &path,qint64 maximum,QString &error)
{
    const QFileInfo info(path);if(!info.isFile()||!info.isReadable()||info.size()>maximum){error=QStringLiteral("The helper installation contains an unreadable or oversized file.");return {};}
    QFile file(path);if(!file.open(QIODevice::ReadOnly)){error=QStringLiteral("The helper installation cannot be read.");return {};}
    const auto data=file.read(maximum+1);if(data.size()>maximum||file.error()!=QFileDevice::NoError){error=QStringLiteral("The helper installation changed during its bounded read.");return {};}
    return data;
}
bool approvedPolicy(const QByteArray &bytes,const QString &helper)
{
    QXmlStreamReader xml(bytes);int actions=0;bool selected=false;QStringList any,inactive,active,paths;
    while(!xml.atEnd()){
        xml.readNext();if(!xml.isStartElement())continue;
        if(xml.name()==QLatin1String("action")){if(++actions!=1||xml.attributes().value("id")!=Action)return false;selected=true;}
        else if(selected&&xml.name()==QLatin1String("allow_any"))any<<xml.readElementText().trimmed();
        else if(selected&&xml.name()==QLatin1String("allow_inactive"))inactive<<xml.readElementText().trimmed();
        else if(selected&&xml.name()==QLatin1String("allow_active"))active<<xml.readElementText().trimmed();
        else if(selected&&xml.name()==QLatin1String("annotate")){
            const auto key=xml.attributes().value("key").toString(),value=xml.readElementText().trimmed();
            if(key==QLatin1String("org.freedesktop.policykit.exec.path"))paths<<value;
            else if(key!=QLatin1String("org.freedesktop.policykit.exec.allow_gui")||value!=QLatin1String("false"))return false;
        }
    }
    return !xml.hasError()&&actions==1&&any==QStringList{QStringLiteral("no")}&&inactive==QStringList{QStringLiteral("no")}
        &&active==QStringList{QStringLiteral("auth_admin")}&&paths==QStringList{helper};
}
class ProcessAeroLoginAuthRunner final:public AeroLoginAuthRunner {
    Q_OBJECT
public:
    using AeroLoginAuthRunner::AeroLoginAuthRunner;
    void start(quint64 token,const QByteArray &request,bool recovery) override
    {
        if(m_busy||ActiveAuthorization){emit finished(token,false,{},QStringLiteral("An authorized login appearance job is already running."));return;}
        const auto plan=aeroLoginAuthPlan(request,recovery);if(!plan.valid){emit finished(token,false,{},plan.error);return;}
        bool idle=false;if(!ActiveAuthorization.compare_exchange_strong(idle,true)){emit finished(token,false,{},QStringLiteral("An authorized login appearance job is already running."));return;}
        auto lease=std::make_shared<AuthorizationLease>();m_busy=true;QPointer<ProcessAeroLoginAuthRunner> safe(this);auto *watcher=new QFutureWatcher<AeroLoginHelperCapability>(this);
        connect(watcher,&QFutureWatcher<AeroLoginHelperCapability>::finished,this,[safe,watcher,token,request,recovery,lease]{
            const auto capability=watcher->result();watcher->deleteLater();if(!safe)return;
            if(!capability.available){safe->m_busy=false;emit safe->finished(token,false,{},capability.reason);return;}
            safe->launch(token,request,recovery,lease);
        });
        watcher->setFuture(QtConcurrent::run([]{return inspectAeroLoginHelper();}));
    }
private:
    void launch(quint64 token,const QByteArray &request,bool recovery,std::shared_ptr<AuthorizationLease> lease)
    {
        // Escrow is intentionally parentless: a QObject parent's destruction
        // would make QProcess kill a privileged transaction. Retire only after
        // terminal completion; application exit abandons tracking, not the job.
        auto *process=new QProcess;auto *deadline=new QTimer(process);deadline->setSingleShot(true);deadline->setInterval(30000);
        struct State {QByteArray output,error;bool done=false,oversized=false;std::shared_ptr<AuthorizationLease> lease;};auto state=std::make_shared<State>();state->lease=std::move(lease);QPointer<ProcessAeroLoginAuthRunner> safe(this);
        auto drain=[process,state]{
            const auto output=process->readAllStandardOutput(),error=process->readAllStandardError();
            if(state->output.size()+output.size()>MaximumReplyBytes)state->oversized=true;
            if(state->output.size()<MaximumReplyBytes)state->output+=output.left(MaximumReplyBytes-state->output.size());
            if(state->error.size()<16384)state->error+=error.left(16384-state->error.size());
        };
        auto complete=[safe,process,deadline,state,token,drain](bool failedToStart){
            if(state->done)return;state->done=true;deadline->stop();drain();const auto code=process->exitCode();
            const bool succeeded=!failedToStart&&process->exitStatus()==QProcess::NormalExit&&code==0&&!state->oversized;
            QString error;if(failedToStart)error=QStringLiteral("Administrative authorization could not be started.");
            else if(code==126)error=QStringLiteral("Administrative authorization was cancelled. Saved state will be checked.");
            else if(code==127)error=QStringLiteral("Administrative authorization was unavailable or denied. Saved state will be checked.");
            else if(state->oversized)error=QStringLiteral("The authorized helper returned an oversized acknowledgement; saved state is uncertain.");
            else if(!succeeded)error=QStringLiteral("The authorized helper terminated without a successful acknowledgement. Saved state will be checked.");
            const auto reply=state->output;state->lease.reset();process->deleteLater();if(safe){safe->m_busy=false;emit safe->finished(token,succeeded,reply,error);}
        };
        connect(process,&QProcess::readyReadStandardOutput,QCoreApplication::instance(),drain);connect(process,&QProcess::readyReadStandardError,QCoreApplication::instance(),drain);
        connect(process,qOverload<int,QProcess::ExitStatus>(&QProcess::finished),QCoreApplication::instance(),[complete]{complete(false);});
        connect(process,&QProcess::errorOccurred,QCoreApplication::instance(),[complete](QProcess::ProcessError error){if(error==QProcess::FailedToStart)complete(true);});
        connect(process,&QProcess::started,QCoreApplication::instance(),[process,request,deadline]{process->write(request);process->closeWriteChannel();deadline->start();});
        connect(deadline,&QTimer::timeout,QCoreApplication::instance(),[safe,token]{if(safe)emit safe->delayed(token,QStringLiteral("Authorization or the privileged transaction is taking longer than expected. It has not been cancelled; waiting for a terminal result protects transaction recovery."));});
        const auto plan=aeroLoginAuthPlan(request,recovery);process->setProcessEnvironment(plan.environment);process->setProgram(plan.program);process->setArguments(plan.arguments);process->start();
    }
    bool m_busy=false;
};
}

AeroLoginHelperCapability inspectAeroLoginHelper(const AeroLoginHelperContext &context)
{
    AeroLoginHelperCapability capability;QString helper=QString::fromUtf8(CONTROL_AERO_LOGIN_HELPER_PATH),policy=Policy,approval=Approval,anchor;
    quint32 uid=0;if(context.fixture){helper=context.helperPath;policy=context.policyPath;approval=context.approvalPath;uid=context.expectedUid;anchor=QFileInfo(helper).absolutePath();}
    else if(!context.helperPath.isEmpty()||!context.policyPath.isEmpty()||!context.approvalPath.isEmpty()||context.expectedUid!=0){capability.reason=QStringLiteral("Production helper paths and ownership cannot be overridden.");return capability;}
    if(!trustedPath(helper,anchor,uid,true,capability.reason)||!trustedPath(policy,anchor,uid,false,capability.reason)||!trustedPath(approval,anchor,uid,false,capability.reason))return capability;
    const auto executable=readBounded(helper,32*1024*1024,capability.reason);if(!capability.reason.isEmpty())return capability;
    const auto policyBytes=readBounded(policy,65536,capability.reason);if(!capability.reason.isEmpty())return capability;
    const auto approvalBytes=readBounded(approval,4096,capability.reason);if(!capability.reason.isEmpty())return capability;
    const auto document=QJsonDocument::fromJson(approvalBytes);const auto object=document.object();
    QStringList keys=object.keys();keys.sort();QStringList expectedKeys{"helperSha256","policySha256","protocol","version"};expectedKeys.sort();
    if(!executable.startsWith(QByteArray("\x7f" "ELF",4))||!approvedPolicy(policyBytes,helper)||!document.isObject()||keys!=expectedKeys
        ||object.value("version")!=QJsonValue(1)||object.value("protocol")!=QJsonValue(1)
        ||object.value("helperSha256").toString().toLatin1()!=hash(executable).toHex()||object.value("policySha256").toString().toLatin1()!=hash(policyBytes).toHex()){
        capability.reason=QStringLiteral("The helper content, policy, protocol, or trusted installation approval does not match.");return capability;
    }
    capability.available=true;capability.revision=hash(executable+policyBytes+approvalBytes);return capability;
}
AeroLoginAuthRunner *createAeroLoginAuthRunner(QObject *parent){return new ProcessAeroLoginAuthRunner(parent);}
AeroLoginAuthPlan aeroLoginAuthPlan(const QByteArray &request,bool recovery)
{
    AeroLoginAuthPlan plan;if(request.isEmpty()||request.size()>MaximumRequestBytes){plan.error=QStringLiteral("The bounded login appearance request is invalid.");return plan;}
    plan.program=QStringLiteral("/usr/bin/pkexec");plan.arguments={QStringLiteral("--disable-internal-agent"),QString::fromUtf8(CONTROL_AERO_LOGIN_HELPER_PATH),recovery?QStringLiteral("--recover"):QStringLiteral("--apply")};
    plan.environment.insert("PATH","/usr/sbin:/usr/bin:/sbin:/bin");plan.environment.insert("LANG","C.UTF-8");plan.environment.insert("LC_ALL","C.UTF-8");plan.input=request;plan.valid=true;return plan;
}

KdeAeroLoginBackend::KdeAeroLoginBackend(QObject *parent)
    :KdeAeroLoginBackend(createAeroLoginAuthRunner(),[](QString &error){return AeroLoginFiles::read({},&error);},
        []{return inspectAeroLoginHelper();},AeroLoginFiles::watchedPaths()+QStringList{QString::fromUtf8(CONTROL_AERO_LOGIN_HELPER_PATH),Policy,Approval},parent){}
KdeAeroLoginBackend::KdeAeroLoginBackend(AeroLoginAuthRunner *runner,Reader reader,HelperCheck helperCheck,const QStringList &paths,QObject *parent)
    :AeroLoginBackend(parent),m_runner(runner),m_reader(std::move(reader)),m_helperCheck(std::move(helperCheck)),m_paths(paths)
{
    Q_ASSERT(runner);runner->setParent(this);m_paths.removeDuplicates();m_debounce.setSingleShot(true);m_debounce.setInterval(150);
    connect(&m_debounce,&QTimer::timeout,this,&AeroLoginBackend::externalChange);
    auto changed=[this]{watchFiles();m_debounce.start();};connect(&m_watcher,&QFileSystemWatcher::fileChanged,this,changed);connect(&m_watcher,&QFileSystemWatcher::directoryChanged,this,changed);
    connect(runner,&AeroLoginAuthRunner::finished,this,&KdeAeroLoginBackend::onAuthorized);
    connect(runner,&AeroLoginAuthRunner::delayed,this,[this](quint64 token,const QString &message){
        if(m_phase!=Phase::Authorization||token!=m_token)return;
        QPointer<KdeAeroLoginBackend> safe(this);emit operationPending(token,message);if(safe)emit authDelayed(token,message);
    });
    watchFiles();
}
void KdeAeroLoginBackend::watchFiles()
{
    for(const auto &path:m_paths){
        if(!QFileInfo(path).isAbsolute())continue;
        auto directory=QFileInfo(path).absolutePath();while(!QDir(directory).exists()&&directory!=QLatin1String("/"))directory=QFileInfo(directory).absolutePath();
        if(QDir(directory).exists()&&!m_watcher.directories().contains(directory))m_watcher.addPath(directory);
        if(QFileInfo::exists(path)&&!m_watcher.files().contains(path))m_watcher.addPath(path);
    }
}
void KdeAeroLoginBackend::beginRead()
{
    struct Result {bool success=false;AeroLoginSnapshot snapshot;QString error;};
    const auto token=m_token;const auto phase=m_phase;const auto reader=m_reader;const auto helper=m_helperCheck;
    auto *watcher=new QFutureWatcher<Result>(this);
    connect(watcher,&QFutureWatcher<Result>::finished,this,[this,watcher,token,phase]{
        const auto result=watcher->result();watcher->deleteLater();if(token!=m_token||phase!=m_phase)return;
        onRead(result.success,result.snapshot,result.error);
    });
    watcher->setFuture(QtConcurrent::run([reader,helper]{
        Result result;
        if(!reader||!helper){result.error=QStringLiteral("The login appearance file adapter is unavailable.");return result;}
        result.snapshot=reader(result.error);result.success=result.error.isEmpty()&&result.snapshot.fields.size()==AeroLoginTypes::keys().size();
        const auto capability=helper();result.snapshot.helperAvailable=capability.available;result.snapshot.helperReason=capability.reason;result.snapshot.helperRevision=capability.revision;
        if(!result.success&&result.error.isEmpty())result.error=QStringLiteral("The login appearance snapshot is incomplete.");return result;
    }));
}
void KdeAeroLoginBackend::readCurrent(quint64 token)
{
    if(m_phase!=Phase::Idle){emit readFinished(token,false,{},QStringLiteral("Another login appearance operation is in progress."));return;}
    m_token=token;m_phase=Phase::Read;beginRead();
}
void KdeAeroLoginBackend::writeChanges(quint64 token,const AeroLoginSnapshot &expected,const AeroLoginSnapshot &requested)
{
    if(m_phase!=Phase::Idle){emit writeFinished(token,false,QStringLiteral("Another login appearance operation is in progress."));return;}
    QString error;
    if(!expected.helperAvailable||!AeroLoginTypes::dirty(requested)||!AeroLoginTypes::requestValid(expected,requested,error)){
        emit writeFinished(token,false,error.isEmpty()?expected.helperReason.isEmpty()?QStringLiteral("The trusted login appearance helper is unavailable or no change was requested."):expected.helperReason:error);return;
    }
    m_token=token;m_expected=expected;m_requested=requested;m_recovery=false;m_postRevision.clear();m_phase=Phase::Preflight;beginRead();
}
void KdeAeroLoginBackend::recover(quint64 token,const AeroLoginSnapshot &expected)
{
    if(m_phase!=Phase::Idle){emit writeFinished(token,false,QStringLiteral("Another login appearance operation is in progress."));return;}
    if(!expected.helperAvailable||!expected.recoveryPending||!expected.canRecover){emit writeFinished(token,false,QStringLiteral("No trusted, recoverable login appearance transaction is available."));return;}
    m_token=token;m_expected=expected;m_requested={};m_recovery=true;m_postRevision.clear();m_phase=Phase::RecoveryPreflight;beginRead();
}
void KdeAeroLoginBackend::onRead(bool success,const AeroLoginSnapshot &snapshot,const QString &error)
{
    if(m_phase==Phase::Read){m_phase=Phase::Idle;emit readFinished(m_token,success,snapshot,error);return;}
    if(m_phase==Phase::Preflight||m_phase==Phase::RecoveryPreflight){
        QString validation;
        if(!success||!snapshot.helperAvailable||!AeroLoginTypes::sameConfiguration(m_expected,snapshot)
            ||(m_recovery?(!snapshot.recoveryPending||!snapshot.canRecover):!AeroLoginTypes::requestValid(snapshot,m_requested,validation))){
            finishWrite(false,!error.isEmpty()?error:!validation.isEmpty()?validation:!snapshot.helperAvailable?snapshot.helperReason:QStringLiteral("Login appearance sources, permissions, helper capability, or recovery state changed before authorization. Revert before retrying."));return;
        }
        const auto bytes=m_recovery?AeroLoginProtocol::encodeRecovery(snapshot,validation):AeroLoginProtocol::encode(snapshot,m_requested,validation);
        if(bytes.isEmpty()||!validation.isEmpty()){finishWrite(false,validation.isEmpty()?QStringLiteral("The login appearance request could not be encoded."):validation);return;}
        m_phase=Phase::Authorization;m_runner->start(m_token,bytes,m_recovery);return;
    }
    if(m_phase!=Phase::Readback)return;
    QString verification;bool matches=false;
    if(success){
        matches=m_recovery?!snapshot.recoveryPending&&snapshot.sourceValid&&snapshot.trustedInstallation:AeroLoginTypes::matchesRequest(m_expected,m_requested,snapshot,verification);
        if(m_postRevision.size()!=32||snapshot.revision!=m_postRevision){matches=false;verification=QStringLiteral("The fresh login appearance source revision differs from the helper's commit receipt. Outside changes or a lost result require review.");}
    }
    finishWrite(m_saved,!m_writeError.isEmpty()?m_writeError:!success?error:!matches?verification.isEmpty()?QStringLiteral("Fresh saved login appearance files did not match the authorized result."):verification:QString());
}
void KdeAeroLoginBackend::onAuthorized(quint64 token,bool processSucceeded,const QByteArray &reply,const QString &error)
{
    if(token!=m_token||m_phase!=Phase::Authorization)return;
    bool saved=false,recoveryPending=false;QString helperError;
    const bool decoded=AeroLoginProtocol::decodeReply(reply,saved,helperError,recoveryPending,&m_postRevision);m_saved=decoded&&saved;
    m_writeError=!error.isEmpty()?error:!decoded?helperError.isEmpty()?QStringLiteral("The authorized helper acknowledgement was malformed or lost; saved state is uncertain."):helperError:!processSucceeded?QStringLiteral("The authorized helper did not terminate successfully; saved state is uncertain."):!helperError.isEmpty()?helperError:recoveryPending?QStringLiteral("The helper reported a pending recoverable transaction."):!saved?QStringLiteral("The helper did not confirm saving login appearance preferences."):m_postRevision.size()!=32?QStringLiteral("The helper did not provide a valid source revision receipt."):QString();
    m_phase=Phase::Readback;beginRead();
}
void KdeAeroLoginBackend::finishWrite(bool saved,const QString &error)
{const auto token=m_token;m_phase=Phase::Idle;emit writeFinished(token,saved,error,m_postRevision);}

#include "AeroLoginBackend.moc"
