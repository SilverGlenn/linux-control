#include "ScreenLockBackend.h"
#include <KConfig>
#include <KConfigGroup>
#include "operations/OperationManager.h"
#include "operations/SettingsFileReservations.h"
#include <QCoreApplication>
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrentRun>
#include <QSet>
#include <kconfig_version.h>
#include <QCryptographicHash>
#include <QDataStream>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusServiceWatcher>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QProcess>
#include <QStandardPaths>
#include <QXmlStreamReader>
#include <cmath>
#include <cerrno>
#include <unistd.h>
#include <memory>
#include <atomic>
#include <sys/stat.h>
#include <fcntl.h>
namespace {
const QString Service=QStringLiteral("org.kde.screensaver"),Alias=QStringLiteral("org.freedesktop.ScreenSaver"),KWin=QStringLiteral("org.kde.KWin");
const QString Path=QStringLiteral("/ScreenSaver"),Executable=QStringLiteral("/usr/bin/kwin_wayland"),Library=QStringLiteral("/usr/lib/x86_64-linux-gnu/libKScreenLocker.so.6.6.5");
constexpr qint64 MaximumConfigSize=2*1024*1024;
QByteArray digest(const QByteArray &data){return QCryptographicHash::hash(data,QCryptographicHash::Sha256);}
bool protectedFile(const QString &path)
{
    struct stat metadata{};
    return ::stat(QFile::encodeName(path).constData(),&metadata)==0&&S_ISREG(metadata.st_mode)
        &&metadata.st_uid==0&&!(metadata.st_mode&0022);
}
bool packageFile(const QString &package,const QString &path)
{
    QFile manifest(QStringLiteral("/var/lib/dpkg/info/")+package+QStringLiteral(".list"));
    if(!protectedFile(manifest.fileName())||!manifest.open(QIODevice::ReadOnly)||manifest.size()>MaximumConfigSize)return false;
    if(!manifest.readAll().split('\n').contains(path.toUtf8())||!protectedFile(path))return false;
    QFile sums(QStringLiteral("/var/lib/dpkg/info/")+package+QStringLiteral(".md5sums")),file(path);
    if(!protectedFile(sums.fileName())||!sums.open(QIODevice::ReadOnly)||sums.size()>MaximumConfigSize
        ||!file.open(QIODevice::ReadOnly)||file.size()>32*1024*1024)return false;
    QByteArray expected;
    while(!sums.atEnd()){
        const auto line=sums.readLine().trimmed();if(line.mid(34)==path.mid(1).toUtf8()){
            if(!expected.isEmpty())return false;expected=line.left(32);
        }
    }
    return expected.size()==32&&QCryptographicHash::hash(file.readAll(),QCryptographicHash::Md5).toHex()==expected;
}
void verifyInstalled(QObject *context,uint pid,std::function<void(bool,QString)> completion)
{
    if(QString::fromLatin1(KCONFIG_VERSION_STRING)!=QLatin1String("6.24.0")){
        completion(false,QStringLiteral("screen-lock settings require the audited KConfig 6.24.0 SDK."));return;
    }
    auto *process=new QProcess(context);auto *timer=new QTimer(process);timer->setSingleShot(true);timer->setInterval(2000);
    auto completed=std::make_shared<bool>(false);
    auto done=[context,process,completed,completion=std::move(completion),pid](bool timedOut){
        if(*completed)return;*completed=true;
        if(timedOut)process->kill();
        bool valid=!timedOut&&process->exitStatus()==QProcess::NormalExit&&process->exitCode()==0;
        QMap<QString,QString> versions;
        for(const auto &line:QString::fromUtf8(process->readAllStandardOutput()).split(QLatin1Char('\n'),Qt::SkipEmptyParts)){
            const auto fields=line.split(QLatin1Char(' '),Qt::SkipEmptyParts);if(fields.size()!=2||versions.contains(fields[0])){valid=false;continue;}
            versions.insert(fields[0],fields[1].section(QLatin1Char(':'),-1).section(QLatin1Char('-'),0,0));
        }
        process->deleteLater();
        auto *watcher=new QFutureWatcher<QPair<bool,QString>>(context);
        QObject::connect(watcher,&QFutureWatcher<QPair<bool,QString>>::finished,context,[watcher,completion]{const auto result=watcher->result();watcher->deleteLater();completion(result.first,result.second);});
        watcher->setFuture(QtConcurrent::run([valid,versions,pid]() mutable {
        valid=valid&&versions.value("kde-config-screenlocker")==QLatin1String("6.6.5")&&versions.value("libkscreenlocker6")==QLatin1String("6.6.5")
            &&versions.value("kwin-wayland")==QLatin1String("6.6.6")&&versions.value("libkf6configcore6")==QLatin1String("6.24.0")
            &&packageFile(QStringLiteral("kwin-wayland"),Executable)
            &&packageFile(QStringLiteral("libkscreenlocker6:amd64"),Library);
        if(valid&&pid){
            auto target=QFileInfo(QStringLiteral("/proc/%1/exe").arg(pid)).symLinkTarget();
            if(target.isEmpty()){
                // CAP_SYS_NICE can hide exe/maps from the same user. This is
                // weaker compatibility evidence, not executable or loaded-library
                // attestation: comm/cmdline are self-reported, alongside the
                // pinned D-Bus PID, supportInformation, and installed package hash.
                QFile comm(QStringLiteral("/proc/%1/comm").arg(pid)),command(QStringLiteral("/proc/%1/cmdline").arg(pid));
                valid=comm.open(QIODevice::ReadOnly)&&command.open(QIODevice::ReadOnly)
                    &&comm.read(4096).trimmed()==QByteArray("kwin_wayland")
                    &&command.read(32768).split('\0').value(0)==Executable.toUtf8();
            }else valid=target==Executable;
        }
        return qMakePair(valid,valid?QString():QStringLiteral("The installed screen-lock packages or existing daemon identity do not match audited KScreenLocker 6.6.5, KWin 6.6.6, and KConfig 6.24.0."));
        }));
    };
    QObject::connect(process,qOverload<int,QProcess::ExitStatus>(&QProcess::finished),context,[done,timer]{timer->stop();done(false);});
    QObject::connect(process,&QProcess::errorOccurred,context,[done,timer](QProcess::ProcessError error){if(error==QProcess::FailedToStart){timer->stop();done(true);}});
    QObject::connect(timer,&QTimer::timeout,context,[done]{done(true);});
    process->start(QStringLiteral("/usr/bin/dpkg-query"),{QStringLiteral("-W"),QStringLiteral("-f=${Package} ${Version}\n"),
        QStringLiteral("kde-config-screenlocker"),QStringLiteral("libkscreenlocker6"),QStringLiteral("kwin-wayland"),QStringLiteral("libkf6configcore6")});timer->start();
}
bool interfaceSupported(const QString &text)
{
    if(text.size()>65536)return false;QXmlStreamReader xml(text);bool selected=false;int interfaces=0,configure=0;QString method;
    while(!xml.atEnd()){
        xml.readNext();
        if(xml.isStartElement()&&xml.name()==QLatin1String("interface")){selected=xml.attributes().value("name")==Service;if(selected&&++interfaces!=1)return false;}
        else if(xml.isEndElement()&&xml.name()==QLatin1String("interface"))selected=false;
        else if(selected&&xml.isStartElement()&&xml.name()==QLatin1String("method")){method=xml.attributes().value("name").toString();if(method==QLatin1String("configure")&&++configure!=1)return false;}
        else if(selected&&method==QLatin1String("configure")&&xml.isStartElement()&&xml.name()==QLatin1String("arg"))return false;
        else if(xml.isEndElement()&&xml.name()==QLatin1String("method"))method.clear();
    }
    return !xml.hasError()&&interfaces==1&&configure==1;
}
class DBusScreenLockService final : public ScreenLockService {
    Q_OBJECT
public:
    DBusScreenLockService(const QDBusConnection &bus,ScreenLockIdentityVerifier verifier,QObject *parent)
        :ScreenLockService(parent),m_bus(bus),m_verifier(std::move(verifier))
    {
        if(!m_verifier)m_verifier=[this](uint pid,std::function<void(bool,QString)> done){verifyInstalled(this,pid,std::move(done));};
        auto *watcher=new QDBusServiceWatcher(Service,bus,QDBusServiceWatcher::WatchForOwnerChange,this);watcher->addWatchedService(Alias);watcher->addWatchedService(KWin);
        connect(watcher,&QDBusServiceWatcher::serviceOwnerChanged,this,[this]{++m_epoch;emit externalChange();});
    }
    void query(quint64 token,const QString &expectedOwner) override
    {discover([this,token](bool success,const ScreenLockRuntime &runtime,const QString &error){emit queryFinished(token,success,runtime,error);},expectedOwner);}
    void configure(quint64 token,const QString &owner,quint64 epoch,Guard guard) override
    {
        if(owner.isEmpty()||epoch!=m_epoch){emit configureFinished(token,false,QStringLiteral("The pinned screen-lock service changed before reload."));return;}
        QPointer<DBusScreenLockService> safe(this);
        // Each configure attempt owns one guard result and at most one native call.
        // A delayed or duplicate guard completion cannot revive a retired attempt.
        const auto attempt=++m_configureAttempt;
        discover([safe,token,owner,epoch,attempt,guard=std::move(guard)](bool success,const ScreenLockRuntime &runtime,const QString &error){
            if(!safe||attempt!=safe->m_configureAttempt)return;
            if(!success||!runtime.providerVerified||runtime.owner!=owner||runtime.epoch!=epoch){emit safe->configureFinished(token,false,error.isEmpty()?QStringLiteral("The screen-lock provider changed before reload."):error);return;}
            if(!guard){emit safe->configureFinished(token,false,QStringLiteral("The saved-preferences guard is unavailable."));return;}
            auto completed=std::make_shared<bool>(false);
            guard([safe,token,owner,epoch,attempt,completed](bool valid,const QString &boundaryError){
                if(*completed)return;*completed=true;
                if(!safe||attempt!=safe->m_configureAttempt)return;
                if(!valid){emit safe->configureFinished(token,false,boundaryError.isEmpty()?QStringLiteral("Saved screen-lock preferences changed before reload."):boundaryError);return;}
                if(epoch!=safe->m_epoch){emit safe->configureFinished(token,false,QStringLiteral("The screen-lock owner changed during the file guard."));return;}
                safe->owners([safe,token,owner,epoch,attempt](const QStringList &owners,bool){
                    if(!safe||attempt!=safe->m_configureAttempt)return;
                    if(epoch!=safe->m_epoch||owners!=QStringList{owner,owner,owner}){emit safe->configureFinished(token,false,QStringLiteral("The screen-lock owner changed at the final reload boundary."));return;}
                    safe->call(owner,Path,Service,QStringLiteral("configure"),{},[safe,token,owner,epoch,attempt](const QDBusMessage &reply){
                        if(!safe||attempt!=safe->m_configureAttempt)return;
                        const bool acknowledged=reply.type()==QDBusMessage::ReplyMessage&&reply.arguments().isEmpty();
                        if(!acknowledged){emit safe->configureFinished(token,false,QStringLiteral("The native configure request was not acknowledged. A timeout does not cancel queued execution; Revert to inspect saved preferences."));return;}
                        safe->owners([safe,token,owner,epoch,attempt](const QStringList &owners,bool){
                            if(!safe||attempt!=safe->m_configureAttempt)return;
                            const bool stable=epoch==safe->m_epoch&&owners==QStringList{owner,owner,owner};
                            emit safe->configureFinished(token,stable,stable?QString():QStringLiteral("The screen-lock owner changed after configure returned. No replacement service was reloaded."));
                        });
                    });
                });
            });
        },owner);
    }

private:
    using Completion=std::function<void(const QDBusMessage &)>;
    using Discovery=std::function<void(bool,const ScreenLockRuntime &,const QString &)>;
    void call(const QString &destination,const QString &path,const QString &interface,const QString &method,const QVariantList &args,Completion done)
    {
        auto message=QDBusMessage::createMethodCall(destination,path,interface,method);message.setArguments(args);auto *pending=new QDBusPendingCallWatcher(m_bus.asyncCall(message,1500),this);
        auto once=std::make_shared<bool>(false);QPointer<QDBusPendingCallWatcher> safe(pending);
        auto finish=[pending,safe,once,done=std::move(done)]{if(*once||!safe)return;*once=true;const auto reply=pending->reply();pending->deleteLater();done(reply);};
        connect(pending,&QDBusPendingCallWatcher::finished,this,finish);if(pending->isFinished())QTimer::singleShot(0,this,finish);
    }
    void owner(const QString &name,std::function<void(QString,bool)> done)
    {
        call(QStringLiteral("org.freedesktop.DBus"),QStringLiteral("/org/freedesktop/DBus"),QStringLiteral("org.freedesktop.DBus"),QStringLiteral("GetNameOwner"),{name},[done=std::move(done)](const QDBusMessage &message){
            const QDBusPendingReply<QString> reply(message);done(reply.isError()?QString():reply.value(),reply.isError()&&message.errorName()==QLatin1String("org.freedesktop.DBus.Error.NameHasNoOwner"));
        });
    }
    void owners(std::function<void(QStringList,bool)> done)
    {
        QPointer<DBusScreenLockService> safe(this);
        owner(Service,[safe,done=std::move(done)](const QString &first,bool absentFirst){if(!safe)return;
            safe->owner(Alias,[safe,first,absentFirst,done](const QString &second,bool absentSecond){if(!safe)return;
                if(absentFirst&&absentSecond){done({{},{},{}},true);return;}
                safe->owner(KWin,[first,second,done](const QString &third,bool){done({first,second,third},false);});
            });
        });
    }
    void discover(Discovery done,const QString &expectedOwner)
    {
        struct State {ScreenLockRuntime runtime;quint64 epoch=0;};auto state=std::make_shared<State>();state->epoch=m_epoch;QPointer<DBusScreenLockService> safe(this);
        auto finish=[safe,state,done=std::move(done)](bool success,QString error){
            if(!safe)return;if(state->epoch!=safe->m_epoch){success=false;state->runtime.providerVerified=false;error=QStringLiteral("The screen-lock provider changed during discovery.");}
            if(!success&&!state->runtime.serviceAvailable)state->runtime.installedVerified=false;
            state->runtime.epoch=safe->m_epoch;state->runtime.reason=error;done(success,state->runtime,error);
        };
        m_verifier(0,[safe,state,finish,expectedOwner](bool installed,const QString &error){
            if(!safe)return;state->runtime.installedVerified=installed;state->runtime.version=installed?QStringLiteral("6.6.5"):QString();
            if(!installed){finish(false,error);return;}
            safe->owners([safe,state,finish,expectedOwner](const QStringList &owners,bool absent){
                if(!safe)return;
                if(absent&&expectedOwner.isEmpty()){finish(true,QStringLiteral("Both native screen-lock services are absent. Saved preferences are deferred until the locker starts; no service was activated."));return;}
                state->runtime.serviceAvailable=!owners.value(0).isEmpty()||!owners.value(1).isEmpty();state->runtime.owner=owners.value(0);
                if(owners.value(0).isEmpty()||owners!=QStringList{owners[0],owners[0],owners[0]}||(!expectedOwner.isEmpty()&&owners[0]!=expectedOwner)){
                    finish(false,QStringLiteral("The two screen-lock services and KWin do not have the same pinned owner."));return;
                }
                safe->call(owners[0],QStringLiteral("/KWin"),KWin,QStringLiteral("supportInformation"),{},[safe,state,finish](const QDBusMessage &message){
                    if(!safe)return;const QDBusPendingReply<QString> reply(message);QStringList versions,modes;
                    if(!reply.isError()&&reply.value().size()<=65536)for(const auto &line:reply.value().split(QLatin1Char('\n'))){
                        if(line.startsWith(QLatin1String("KWin version:")))versions<<line.mid(13).trimmed();if(line.startsWith(QLatin1String("Operation Mode:")))modes<<line.mid(15).trimmed();
                    }
                    if(reply.isError()||versions!=QStringList{QStringLiteral("6.6.6")}||modes!=QStringList{QStringLiteral("Wayland")}){finish(false,QStringLiteral("Screen-lock reload requires the audited KWin 6.6.6 Wayland provider."));return;}
                    safe->call(state->runtime.owner,Path,QStringLiteral("org.freedesktop.DBus.Introspectable"),QStringLiteral("Introspect"),{},[safe,state,finish](const QDBusMessage &message){
                        if(!safe)return;const QDBusPendingReply<QString> reply(message);if(reply.isError()||!interfaceSupported(reply.value())){finish(false,QStringLiteral("The native screen-lock configure interface is unsupported."));return;}
                        safe->call(QStringLiteral("org.freedesktop.DBus"),QStringLiteral("/org/freedesktop/DBus"),QStringLiteral("org.freedesktop.DBus"),QStringLiteral("GetConnectionUnixProcessID"),{state->runtime.owner},[safe,state,finish](const QDBusMessage &message){
                            if(!safe)return;const QDBusPendingReply<uint> reply(message);if(reply.isError()||!reply.value()){finish(false,QStringLiteral("The screen-lock process could not be verified."));return;}
                            safe->m_verifier(reply.value(),[safe,state,finish](bool verified,const QString &error){if(!safe)return;if(!verified){finish(false,error);return;}
                                safe->owners([safe,state,finish](const QStringList &owners,bool){if(!safe)return;
                                    const auto owner=state->runtime.owner;if(owners!=QStringList{owner,owner,owner}){finish(false,QStringLiteral("The native screen-lock owners changed during discovery."));return;}
                                    state->runtime.providerVerified=true;finish(true,{});
                                });
                            });
                        });
                    });
                });
            });
        });
    }
    QDBusConnection m_bus;ScreenLockIdentityVerifier m_verifier;quint64 m_epoch=0,m_configureAttempt=0;
};
QByteArray keySyntax(const QString &path,const QString &groupName,const QString &key)
{
    QFile file(path);if(!file.open(QIODevice::ReadOnly))return {};bool selected=false;QByteArray data;QDataStream stream(&data,QIODevice::WriteOnly);
    while(!file.atEnd()){
        const auto line=file.readLine().trimmed(),header='['+groupName.toUtf8()+']';
        if(line.startsWith('[')){
            auto suffix=line.startsWith(header)?line.mid(header.size()):QByteArray("invalid");while(suffix.startsWith("[$i]")||suffix.startsWith("[$d]"))suffix.remove(0,4);
            selected=suffix.isEmpty();if(selected&&line!=header)stream<<line;
        }else if(selected){const auto left=line.left(line.contains('=')?line.indexOf('='):line.size()).trimmed(),name=key.toUtf8();if(left==name||left.startsWith(name+'['))stream<<line;}
    }
    return data;
}
QVariant nativeValue(const KConfigGroup &group,ScreenLockKey key,const QVariant &fallback,bool &known)
{
    const auto name=ScreenLockTypes::keyName(key);if(!group.hasKey(name)){known=true;return fallback;}const auto raw=group.readEntry(name,QString());
    if(key==ScreenLockKey::Timeout){bool ok=false;raw.toDouble(&ok);const auto value=group.readEntry(name,fallback.toDouble());known=ok;return value;}
    if(key==ScreenLockKey::LockGrace){bool ok=false;raw.toInt(&ok);known=ok;return group.readEntry(name,fallback.toInt());}
    if(key==ScreenLockKey::Theme||key==ScreenLockKey::WallpaperPlugin){known=true;return raw;}
    known=true;return group.readEntry(name,fallback.toBool());
}
bool usable(const ScreenLockRuntime &r){return r.installedVerified&&(!r.serviceAvailable||(r.providerVerified&&!r.owner.isEmpty()));}
KConfig::OpenFlags flags(bool cascade,bool sources){return cascade?KConfig::FullConfig:sources?KConfig::NoGlobals:KConfig::SimpleConfig;}
}
ScreenLockService *createScreenLockService(const QDBusConnection &bus,QObject *parent){return new DBusScreenLockService(bus,{},parent);}
ScreenLockService *createScreenLockService(const QDBusConnection &bus,ScreenLockIdentityVerifier verifier,QObject *parent){return new DBusScreenLockService(bus,std::move(verifier),parent);}

namespace {
using ConfigContext=KdeScreenLockBackend::ConfigContext;
QStringList relevantFiles(const QString &fileName,const ConfigContext &context)
{
    QStringList paths{fileName};paths<<context.defaultSources;
    if(context.fullCascade){for(const auto &directory:QStandardPaths::standardLocations(QStandardPaths::GenericConfigLocation))paths<<directory+"/kscreenlockerrc"<<directory+"/kdeglobals"<<directory+"/system.kdeglobals";if(!qEnvironmentVariableIsSet("KDE_SKIP_KDERC"))paths<<"/etc/kde5rc";}
    paths.removeDuplicates();return paths;
}
bool readFile(const QString &fileName,const KdeScreenLockBackend::ConfigContext &context,ScreenLockSnapshot &snapshot,const ScreenLockRuntime &runtime,QString &error)
{
    if(!QFileInfo(fileName).isAbsolute()||(context.fullCascade&&QDir::cleanPath(fileName)!=QDir::cleanPath(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)+"/kscreenlockerrc"))){error=QStringLiteral("Native cascading requires the active XDG kscreenlockerrc path.");return false;}
    if(context.fixture&&context.beforeFileRead)context.beforeFileRead();
    QMap<QString,QString> canonicalPaths;QMap<QPair<quint64,quint64>,QString> identities;
    for(const auto &path:relevantFiles(fileName,context)){
        const QFileInfo info(path);if(!info.exists()&&!info.isSymLink())continue;
        if(!info.isFile()||!info.isReadable()||info.size()>MaximumConfigSize){error=QStringLiteral("A screen-lock configuration source is not a readable bounded regular file, or is a broken link.");return false;}
        const auto canonical=info.canonicalFilePath(),normalized=QDir::cleanPath(info.absoluteFilePath());struct stat metadata{};
        if(canonical.isEmpty()||::stat(QFile::encodeName(path).constData(),&metadata)!=0||!S_ISREG(metadata.st_mode)){
            error=QStringLiteral("A screen-lock configuration source changed identity while being inspected.");return false;
        }
        const auto identity=qMakePair(quint64(metadata.st_dev),quint64(metadata.st_ino));
        if((canonicalPaths.contains(canonical)&&canonicalPaths.value(canonical)!=normalized)||(identities.contains(identity)&&identities.value(identity)!=normalized)){
            error=QStringLiteral("Distinct screen-lock configuration or default paths alias the same file. No mutation is permitted.");return false;
        }
        canonicalPaths.insert(canonical,normalized);identities.insert(identity,normalized);
        QFile file(path);if(!file.open(QIODevice::ReadOnly)||file.size()>MaximumConfigSize){error=QStringLiteral("A screen-lock configuration source is unreadable or exceeds the bounded read size.");return false;}
    }
    auto config=std::make_unique<KConfig>(context.fullCascade?QStringLiteral("kscreenlockerrc"):fileName,flags(context.fullCascade,!context.defaultSources.isEmpty()));if(!context.defaultSources.isEmpty())config->addConfigSources(context.defaultSources);config->reparseConfiguration();
    KConfig local(fileName,KConfig::SimpleConfig);const auto globalPath=QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)+"/kdeglobals";
    std::unique_ptr<KConfig> global;if(context.fullCascade)global=std::make_unique<KConfig>(globalPath,KConfig::SimpleConfig);
    snapshot={};snapshot.runtime=runtime;QByteArray revision;QDataStream all(&revision,QIODevice::WriteOnly);
    for(const auto key:ScreenLockTypes::keys()){
        const auto name=ScreenLockTypes::keyName(key),groupName=ScreenLockTypes::groupName(key);auto group=config->group(groupName);const auto localGroup=local.group(groupName);const auto globalGroup=global?global->group(groupName):KConfigGroup{};ScreenLockField f;
        config->setReadDefaults(true);f.inheritedPresent=group.hasKey(name);bool defaultKnown=true;const auto inherited=nativeValue(group,key,ScreenLockTypes::builtinDefault(key),defaultKnown);config->setReadDefaults(false);
        f.effectivePresent=group.hasKey(name);bool known=true;f.value=nativeValue(group,key,inherited,known);f.known=known&&(f.effectivePresent||(defaultKnown&&runtime.installedVerified));f.defaultValue=inherited;f.defaultKnown=defaultKnown&&runtime.installedVerified;
        f.localPresent=localGroup.hasKey(name);f.globalPresent=context.fullCascade&&globalGroup.hasKey(name);f.immutable=group.isImmutable()||group.isEntryImmutable(name);
        f.writable=ScreenLockTypes::owned(key)&&usable(runtime)&&!f.immutable&&config->accessMode()==KConfig::ReadWrite;
        f.reason=!ScreenLockTypes::owned(key)?QStringLiteral("This native companion preference is preserved outside the scalar editor."):!usable(runtime)?runtime.reason.isEmpty()?QStringLiteral("The audited screen-lock installation or provider is unavailable."):runtime.reason:f.immutable?QStringLiteral("This native key is locked by policy."):config->accessMode()!=KConfig::ReadWrite?QStringLiteral("The native screen-lock configuration is not writable."):QString();
        f.defaultNoOp=runtime.installedVerified&&f.known&&f.defaultKnown&&f.value.metaType()==f.defaultValue.metaType()&&f.value==f.defaultValue;f.resettable=f.writable&&f.defaultKnown&&f.known&&!f.defaultNoOp;
        if(f.resettable&&f.globalPresent&&!f.localPresent&&!f.inheritedPresent&&global->accessMode()!=KConfig::ReadWrite)f.resettable=false;
        f.resetReason=!f.writable?f.reason:!f.defaultKnown?QStringLiteral("The inherited native default is unknown or malformed."):f.defaultNoOp?QStringLiteral("KDE leaves this saved value unchanged when restoring defaults."):!f.known?QStringLiteral("Choose an explicit supported value to repair this preference."):!f.resettable?QStringLiteral("The native global configuration is not writable."):QString();
        QByteArray defaults;QDataStream inheritedStream(&defaults,QIODevice::WriteOnly);inheritedStream<<f.inheritedPresent<<f.defaultKnown<<inherited<<f.immutable;auto inheritedPaths=relevantFiles(fileName,context);inheritedPaths.removeAll(fileName);if(context.fullCascade)inheritedPaths.removeAll(globalPath);for(const auto &path:inheritedPaths)inheritedStream<<path<<keySyntax(path,groupName,name);f.inheritedRevision=digest(defaults);
        QByteArray globals;QDataStream globalStream(&globals,QIODevice::WriteOnly);globalStream<<f.globalPresent<<(context.fullCascade?keySyntax(globalPath,groupName,name):QByteArray{});f.globalRevision=digest(globals);
        QByteArray raw;QDataStream stream(&raw,QIODevice::WriteOnly);stream<<f.effectivePresent<<f.value<<f.known<<f.localPresent<<f.immutable<<keySyntax(fileName,groupName,name)<<f.inheritedRevision<<f.globalRevision;f.revision=digest(raw);all<<int(key)<<f.revision;snapshot.fields.insert(key,f);
    }
    snapshot.revision=digest(revision);error.clear();return true;
}
struct FileResult {
    bool success=false,saved=false;
    ScreenLockSnapshot snapshot;
    QString error;
    QStringList leases;
    SettingsFileReservations::Inspection admission;
    QByteArray sources;
};
QByteArray sourceRevision(const QString &fileName,const ConfigContext &context,QString &error)
{
    QByteArray bytes;QDataStream stream(&bytes,QIODevice::WriteOnly);
    for(const auto &path:relevantFiles(fileName,context)){
        stream<<path;struct stat before{};
        if(::stat(QFile::encodeName(path).constData(),&before)!=0){
            if(errno==ENOENT&&!QFileInfo(path).isSymLink()){stream<<false;continue;}
            error=QStringLiteral("A Screen Locking source identity could not be inspected.");return {};
        }
        const int fd=::open(QFile::encodeName(path).constData(),O_RDONLY|O_NONBLOCK|O_CLOEXEC);if(fd<0){error=QStringLiteral("A Screen Locking source could not be read for the final file guard.");return {};}
        QFile source;if(!source.open(fd,QIODevice::ReadOnly,QFileDevice::AutoCloseHandle)){::close(fd);error=QStringLiteral("A Screen Locking source handle could not be inspected.");return {};}struct stat opened{},after{};
        if(::fstat(fd,&opened)!=0||!S_ISREG(opened.st_mode)||opened.st_size>MaximumConfigSize||opened.st_dev!=before.st_dev||opened.st_ino!=before.st_ino){error=QStringLiteral("A Screen Locking source changed identity while being fingerprinted.");return {};}
        const auto data=source.read(MaximumConfigSize+1);
        if(source.error()!=QFileDevice::NoError||data.size()>MaximumConfigSize||::fstat(fd,&after)!=0||after.st_size!=opened.st_size||after.st_mtim.tv_sec!=opened.st_mtim.tv_sec||after.st_mtim.tv_nsec!=opened.st_mtim.tv_nsec){error=QStringLiteral("A Screen Locking source changed during the final file guard.");return {};}
        struct stat link{};if(::lstat(QFile::encodeName(path).constData(),&link)!=0){error=QStringLiteral("A Screen Locking source path changed while being fingerprinted.");return {};}
        stream<<quint64(link.st_dev)<<quint64(link.st_ino)<<quint64(link.st_mode)<<quint64(link.st_uid)<<quint64(link.st_gid)<<QFileInfo(path).symLinkTarget();
        stream<<true<<quint64(opened.st_dev)<<quint64(opened.st_ino)<<quint64(opened.st_mode)<<quint64(opened.st_uid)<<quint64(opened.st_gid)<<qint64(opened.st_mtim.tv_sec)<<qint64(opened.st_mtim.tv_nsec)<<data;
    }
    return digest(bytes);
}
QStringList reservationPaths(const QString &fileName,const ConfigContext &context)
{
    QStringList paths{fileName};if(context.fullCascade)paths<<QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)+"/kdeglobals";
    paths.removeDuplicates();return paths;
}
FileResult readFiles(const QString &fileName,const ConfigContext &context,const ScreenLockRuntime &runtime,bool includeLeases=false)
{
    FileResult result;result.success=readFile(fileName,context,result.snapshot,runtime,result.error);
    if(result.success){result.sources=sourceRevision(fileName,context,result.error);result.success=!result.sources.isEmpty();}
    if(result.success&&includeLeases){result.leases=SettingsFileReservations::currentKeys(reservationPaths(fileName,context),&result.error);result.success=!result.leases.isEmpty();}return result;
}
FileResult saveFiles(const QString &fileName,const ConfigContext &context,const ScreenLockRuntime &runtime,const ScreenLockSnapshot &expected,const ScreenLockSnapshot &requested,const QStringList &leased,const std::shared_ptr<std::atomic_bool> &ownerChanged)
{
    FileResult result;ScreenLockSnapshot fresh;
    if(!readFile(fileName,context,fresh,runtime,result.error)||!ScreenLockTypes::sameConfiguration(expected,fresh)||!ScreenLockTypes::requestValid(fresh,requested,result.error)||SettingsFileReservations::currentKeys(reservationPaths(fileName,context))!=leased){
        if(result.error.isEmpty())result.error=QStringLiteral("Screen-lock configuration, policy, defaults or file identity changed at the native write boundary.");return result;
    }
    if(ownerChanged->load()){result.error=QStringLiteral("The screen-lock provider changed before native file mutation.");return result;}
    {
        // The config and every group are born, used, cleaned and destroyed in
        // this worker. Never transfer a KSharedConfig repository across threads.
        KConfig config(context.fullCascade?QStringLiteral("kscreenlockerrc"):fileName,flags(context.fullCascade,!context.defaultSources.isEmpty()));
        if(!context.defaultSources.isEmpty())config.addConfigSources(context.defaultSources);config.reparseConfiguration();
        for(auto it=requested.intents.cbegin();it!=requested.intents.cend();++it){
            const auto key=it.key();auto group=config.group(ScreenLockTypes::groupName(key));const auto name=ScreenLockTypes::keyName(key);const auto value=requested.fields.value(key).value;
            if(it.value()==ScreenLockIntent::RestoreDefault&&!expected.fields.value(key).inheritedPresent)group.revertToDefault(name);
            else if(key==ScreenLockKey::Timeout)group.writeEntry(name,value.toDouble());else if(key==ScreenLockKey::LockGrace)group.writeEntry(name,value.toInt());else group.writeEntry(name,value.toBool());
        }
        if(context.fixture&&context.beforeSync)context.beforeSync();
        if(ownerChanged->load()){config.markAsClean();result.error=QStringLiteral("The screen-lock provider changed before native synchronization; no configure request was sent.");return result;}
        // Native KConfig waits for its own lock and then merges disk entries.
        // This is native merge behavior, not compare-and-swap under that lock.
        const bool synced=config.sync();result.saved=true;
        if(!synced)config.markAsClean(); // no destructor/reparse retry, even after failure
        if(context.fixture&&context.afterSync)context.afterSync();
        if(!synced){result.error=QStringLiteral("Native screen-lock saving failed. Some requested preferences may be saved; no configure request was sent. Revert to inspect them.");return result;}
    }
    result.success=readFile(fileName,context,result.snapshot,runtime,result.error)
        &&ScreenLockTypes::matchesRequest(expected,requested,result.snapshot,result.error)
        &&ScreenLockTypes::activeBranchValid(result.snapshot.fields,result.error);
    if(!result.success&&result.error.isEmpty())result.error=QStringLiteral("The saved screen-lock preferences could not be verified; no configure request was sent.");
    if(result.success){result.sources=sourceRevision(fileName,context,result.error);result.success=!result.sources.isEmpty();if(result.success){result.leases=SettingsFileReservations::currentKeys(reservationPaths(fileName,context),&result.error);result.success=!result.leases.isEmpty();}}
    return result;
}
}

class ScreenLockWriteJob final : public QObject {
public:
    using Finished=std::function<void(bool,bool,const QString &)>;
    using Pending=std::function<void(const QString &)>;
    ScreenLockWriteJob(QString file,ConfigContext context,ScreenLockService *service,quint64 token,ScreenLockSnapshot expected,ScreenLockSnapshot requested)
        :QObject(QCoreApplication::instance()),m_file(std::move(file)),m_context(std::move(context)),m_service(service),m_token(token),m_expected(std::move(expected)),m_requested(std::move(requested))
    {
        service->setParent(this);m_deadline.setSingleShot(true);m_deadline.setInterval(m_context.fixture?qMax(1,m_context.softDeadlineMs):30000);
        connect(&m_deadline,&QTimer::timeout,this,[this]{if(m_phase!=Phase::Done&&pending)pending(QStringLiteral("Saving Screen Locking is taking longer and continues in the background. Native file work or an already sent configure request has not completed. The operation remains tracked; no retry or cancellation was attempted."));});
        connect(service,&ScreenLockService::externalChange,this,[this]{m_ownerChanged->store(true);});
        connect(service,&ScreenLockService::queryFinished,this,[this](quint64 token,bool success,const ScreenLockRuntime &runtime,const QString &error){if(token==m_token)queried(success,runtime,error);});
        connect(service,&ScreenLockService::configureFinished,this,[this](quint64 token,bool acknowledged,const QString &error){
            if(token!=m_token||m_phase!=Phase::Configure)return;m_acknowledged=acknowledged;m_reloadError=error;verify();
        });
    }
    Finished finished;Pending pending;
    void start()
    {
        OperationManager::instance()->setActivity(this,QStringLiteral("Saving Screen Locking"),true);
        m_deadline.start();m_phase=Phase::PreflightQuery;m_service->query(m_token,m_expected.runtime.owner);
    }
private:
    enum class Phase {PreflightQuery,LeaseRead,Save,Configure,BoundaryRead,VerifyQuery,VerifyFiles,Done};
    template<class Work,class Done> void worker(Work work,Done done)
    {
        auto *watcher=new QFutureWatcher<FileResult>(this);
        connect(watcher,&QFutureWatcher<FileResult>::finished,this,[this,watcher,done=std::move(done)]{auto result=watcher->result();watcher->deleteLater();if(m_phase!=Phase::Done)done(std::move(result));});
        watcher->setFuture(QtConcurrent::run(std::move(work)));
    }
    void queried(bool success,const ScreenLockRuntime &runtime,const QString &error)
    {
        if(m_phase==Phase::PreflightQuery){
            if(!success){complete(false,false,error.isEmpty()?QStringLiteral("The screen-lock provider could not be verified before saving."):error);return;}
            m_phase=Phase::LeaseRead;const auto file=m_file;const auto context=m_context;
            const auto inventory=SettingsFileReservations::snapshot();
            worker([file,context,runtime,inventory]{
                auto result=readFiles(file,context,runtime,true);if(!result.success)return result;
                result.admission=SettingsFileReservations::inspect(reservationPaths(file,context),inventory);
                if(!result.admission.success){result.success=false;result.error=result.admission.error;}return result;
            },[this](FileResult result){
                QString validation;
                if(!result.success||!ScreenLockTypes::sameConfiguration(m_expected,result.snapshot)||!ScreenLockTypes::requestValid(result.snapshot,m_requested,validation)){
                    complete(false,false,!result.error.isEmpty()?result.error:!validation.isEmpty()?validation:QStringLiteral("Screen-lock preferences, policy, defaults or provider changed before saving."));return;
                }
                m_reservation=SettingsFileReservations::acquire(result.admission,validation);
                if(!m_reservation.valid()){complete(false,false,validation);return;}
                m_leases=result.admission.keys;
                m_phase=Phase::Save;const auto file=m_file;const auto context=m_context;const auto expected=m_expected;const auto requested=m_requested;const auto runtime=result.snapshot.runtime;const auto leases=m_leases;const auto ownerChanged=m_ownerChanged;const auto reservation=m_reservation;
                if(!SettingsFileReservations::beginMutation(reservation)){complete(false,false,QStringLiteral("The settings-file reservation is already mutating."));return;}
                worker([file,context,runtime,expected,requested,leases,ownerChanged,reservation]{auto result=saveFiles(file,context,runtime,expected,requested,leases,ownerChanged);SettingsFileReservations::endMutation(reservation);return result;},[this](FileResult saved){
                    if(!saved.leases.isEmpty())SettingsFileReservations::refresh(m_reservation,saved.leases);
                    m_saved=saved.saved;if(!saved.success){complete(m_saved,false,saved.error);return;}
                    m_savedSnapshot=saved.snapshot;m_savedSources=saved.sources;
                    if(!saved.snapshot.runtime.serviceAvailable){verify();return;}
                    m_phase=Phase::Configure;
                    QPointer<ScreenLockWriteJob> safe(this);
                    m_service->configure(m_token,m_savedSnapshot.runtime.owner,m_savedSnapshot.runtime.epoch,[safe](ScreenLockService::GuardCompletion done){
                        if(!safe){done(false,QStringLiteral("The tracked Screen Locking operation is unavailable."));return;}safe->boundary(std::move(done));
                    });
                });
            });return;
        }
        if(m_phase!=Phase::VerifyQuery)return;
        m_phase=Phase::VerifyFiles;const auto file=m_file;const auto context=m_context;
        worker([file,context,runtime]{return readFiles(file,context,runtime);},[this,success,error](FileResult result){
            QString verification;const bool matches=success&&result.success&&ScreenLockTypes::matchesRequest(m_expected,m_requested,result.snapshot,verification);
            complete(m_saved,m_acknowledged,!m_reloadError.isEmpty()?m_reloadError:!success?error.isEmpty()?QStringLiteral("The screen-lock provider could not be verified after saving."):error:!result.success?result.error:matches?QString():verification.isEmpty()?QStringLiteral("Saved screen-lock preferences could not be verified."):verification);
        });
    }
    void boundary(ScreenLockService::GuardCompletion done)
    {
        if(m_phase!=Phase::Configure||m_guardStarted){done(false,QStringLiteral("This configure guard no longer owns the save operation."));return;}
        m_guardStarted=true;m_phase=Phase::BoundaryRead;const auto file=m_file;const auto context=m_context;const auto saved=m_savedSnapshot;const auto sources=m_savedSources;
        worker([file,context,saved,sources]{
            if(context.fixture&&context.beforeBoundaryRead)context.beforeBoundaryRead();auto result=readFiles(file,context,saved.runtime);
            if(result.success&&(result.sources!=sources||!ScreenLockTypes::sameConfiguration(saved,result.snapshot)||!ScreenLockTypes::activeBranchValid(result.snapshot.fields,result.error))){result.success=false;if(result.error.isEmpty())result.error=QStringLiteral("Screen-lock preferences changed before configure; no reload was sent.");}return result;
        },[this,done=std::move(done)](FileResult result){m_phase=Phase::Configure;done(result.success,result.error);});
    }
    void verify(){m_phase=Phase::VerifyQuery;m_service->query(m_token,m_expected.runtime.owner);}
    void complete(bool saved,bool acknowledged,const QString &error)
    {
        if(m_phase==Phase::Done)return;m_phase=Phase::Done;m_deadline.stop();
        SettingsFileReservations::release(m_reservation);
        // Completion hands the service back to a surviving backend before any
        // outward notification. Otherwise it remains owned by this retiring job.
        const auto callback=std::move(finished);if(callback)callback(saved,acknowledged,error);
        OperationManager::instance()->setActivity(this,{},false);deleteLater();
    }
    SettingsFileReservations::Reservation m_reservation;QByteArray m_savedSources;
    std::shared_ptr<std::atomic_bool> m_ownerChanged=std::make_shared<std::atomic_bool>(false);
    QString m_file;ConfigContext m_context;ScreenLockService *m_service;quint64 m_token;
    ScreenLockSnapshot m_expected,m_requested,m_savedSnapshot;QStringList m_leases;
    Phase m_phase=Phase::PreflightQuery;bool m_saved=false,m_acknowledged=false,m_guardStarted=false;QString m_reloadError;QTimer m_deadline;
};

KdeScreenLockBackend::KdeScreenLockBackend(QObject *parent)
    :KdeScreenLockBackend(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)+QStringLiteral("/kscreenlockerrc"),createScreenLockService(QDBusConnection::sessionBus()),ConfigContext{true,{}},parent){}
KdeScreenLockBackend::KdeScreenLockBackend(const QString &file,ScreenLockService *service,const ConfigContext &context,QObject *parent)
    :ScreenLockBackend(parent),m_file(file),m_service(service),m_context(context)
{
    Q_ASSERT(service);service->setParent(this);m_deadline.setSingleShot(true);m_deadline.setInterval(m_context.fixture?qMax(1,m_context.softDeadlineMs):30000);
    connect(&m_deadline,&QTimer::timeout,this,[this]{
        if(m_phase==Phase::ReadQuery){const auto token=m_token;m_phase=Phase::Idle;QPointer<KdeScreenLockBackend> safe(this);OperationManager::instance()->setActivity(this,{},false);if(safe)emit readFinished(token,false,{},QStringLiteral("Screen-lock discovery timed out."));}
        else if(m_phase==Phase::ReadFiles)emit operationPending(m_token,QStringLiteral("Reading Screen-lock files is taking longer and continues in the background. The read remains tracked until its worker completes."));
    });
    connect(service,&ScreenLockService::queryFinished,this,&KdeScreenLockBackend::queryFinished);
    m_debounce.setSingleShot(true);m_debounce.setInterval(100);connect(&m_debounce,&QTimer::timeout,this,&ScreenLockBackend::externalChange);
    auto changed=[this]{watchFiles();m_debounce.start();};connect(service,&ScreenLockService::externalChange,this,changed);connect(&m_watcher,&QFileSystemWatcher::fileChanged,this,changed);connect(&m_watcher,&QFileSystemWatcher::directoryChanged,this,changed);watchFiles();
}
QStringList KdeScreenLockBackend::relevantFiles() const{return ::relevantFiles(m_file,m_context);}
void KdeScreenLockBackend::watchFiles()
{
    for(const auto &path:relevantFiles()){
        auto directory=QFileInfo(path).absolutePath();while(!QDir(directory).exists()&&directory!=QLatin1String("/"))directory=QFileInfo(directory).absolutePath();
        if(QDir(directory).exists()&&!m_watcher.directories().contains(directory))m_watcher.addPath(directory);if(QFileInfo::exists(path)&&!m_watcher.files().contains(path))m_watcher.addPath(path);
    }
}
void KdeScreenLockBackend::readCurrent(quint64 token)
{
    if(m_phase!=Phase::Idle){emit readFinished(token,false,{},QStringLiteral("Another screen-lock operation is in progress."));return;}
    m_phase=Phase::ReadQuery;m_token=token;m_deadline.start();QPointer<KdeScreenLockBackend> safe(this);OperationManager::instance()->setActivity(this,QStringLiteral("Reading Screen Locking"),true);if(safe)m_service->query(token);
}
void KdeScreenLockBackend::queryFinished(quint64 token,bool,const ScreenLockRuntime &runtime,const QString &)
{
    if(token!=m_token||m_phase!=Phase::ReadQuery)return;m_phase=Phase::ReadFiles;
    const auto file=m_file;const auto context=m_context;QPointer<KdeScreenLockBackend> safe(this);
    // Read workers carry values only and remain application-owned even if the
    // requesting backend disappears. Their activity is unrelated to package mutation.
    auto *readJob=new QObject(QCoreApplication::instance());auto *watcher=new QFutureWatcher<FileResult>(readJob);
    connect(watcher,&QFutureWatcher<FileResult>::finished,readJob,[safe,watcher,readJob,token]{
        auto result=watcher->result();watcher->deleteLater();
        const bool deliver=safe&&safe->m_phase==Phase::ReadFiles&&safe->m_token==token;
        if(deliver){safe->m_deadline.stop();safe->m_phase=Phase::Idle;safe->watchFiles();}
        OperationManager::instance()->setActivity(readJob,{},false);
        if(deliver&&safe)emit safe->readFinished(token,result.success,result.snapshot,result.error);
        readJob->deleteLater();
    });
    OperationManager::instance()->setActivity(readJob,QStringLiteral("Reading Screen Locking"),true);
    if(safe)OperationManager::instance()->setActivity(safe,{},false);
    watcher->setFuture(QtConcurrent::run([file,context,runtime]{return readFiles(file,context,runtime);}));
}
void KdeScreenLockBackend::writeChanges(quint64 token,const ScreenLockSnapshot &expected,const ScreenLockSnapshot &requested)
{
    if(m_phase!=Phase::Idle){emit writeFinished(token,false,false,QStringLiteral("Another screen-lock operation is in progress."));return;}QString error;
    if(requested.intents.isEmpty()||!ScreenLockTypes::requestValid(expected,requested,error)){emit writeFinished(token,false,false,error.isEmpty()?QStringLiteral("No explicit screen-lock change was requested."):error);return;}
    m_token=token;m_phase=Phase::Write;
    auto *job=new ScreenLockWriteJob(m_file,m_context,m_service,token,expected,requested);m_writeJob=job;QPointer<KdeScreenLockBackend> safe(this);
    job->pending=[safe,token](const QString &message){if(safe&&safe->m_phase==Phase::Write&&safe->m_token==token)emit safe->operationPending(token,message);};
    job->finished=[safe,token](bool saved,bool acknowledged,const QString &message){
        if(!safe)return;safe->m_service->setParent(safe);safe->m_writeJob=nullptr;safe->m_phase=Phase::Idle;safe->watchFiles();emit safe->writeFinished(token,saved,acknowledged,message);
    };
    job->start();
}
#include "ScreenLockBackend.moc"
