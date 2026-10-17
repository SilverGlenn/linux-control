#include "NightTimeBackend.h"

#include <KConfig>
#include <KConfigGroup>
#include "operations/OperationManager.h"
#include "operations/SettingsFileReservations.h"
#include <QCoreApplication>
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrentRun>
#include <atomic>
#include <fcntl.h>
#include <cerrno>
#include <kconfig_version.h>
#include <QCryptographicHash>
#include <QDataStream>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusServiceWatcher>
#include <QDBusVariant>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QProcess>
#include <QStandardPaths>
#include <QXmlStreamReader>
#include <cmath>
#include <memory>
#include <sys/stat.h>
#include <unistd.h>

namespace {
const QString Service=QStringLiteral("org.kde.NightTime");
const QString Path=QStringLiteral("/org/kde/NightTime/Manager");
const QString Interface=QStringLiteral("org.kde.NightTime.Manager");
const QString Executable=QStringLiteral("/usr/lib/x86_64-linux-gnu/libexec/knighttimed");
const QString Library=QStringLiteral("/usr/lib/x86_64-linux-gnu/libKNightTime.so.6.6.5");
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
        completion(false,QStringLiteral("Day and Night settings require the audited KConfig 6.24.0 SDK."));return;
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
        QObject::connect(watcher,&QFutureWatcher<QPair<bool,QString>>::finished,context,[watcher,completion]{
            const auto result=watcher->result();watcher->deleteLater();completion(result.first,result.second);
        });
        watcher->setFuture(QtConcurrent::run([valid,versions,pid]() mutable {
        valid=valid&&versions.value("knighttime")==QLatin1String("6.6.5")&&versions.value("libknighttime0")==QLatin1String("6.6.5")
            &&versions.value("plasma-workspace")==QLatin1String("6.6.6")&&versions.value("libkf6configcore6")==QLatin1String("6.24.0")
            &&packageFile(QStringLiteral("knighttime"),Executable)
            &&packageFile(QStringLiteral("libknighttime0:amd64"),Library);
        if(valid&&pid){
            const auto target=QFileInfo(QStringLiteral("/proc/%1/exe").arg(pid)).symLinkTarget();
            QFile maps(QStringLiteral("/proc/%1/maps").arg(pid));bool linked=false;
            if(maps.open(QIODevice::ReadOnly)){
                const auto data=maps.read(4*1024*1024+1);
                if(data.size()<=4*1024*1024)for(const auto &line:data.split('\n'))if(line.endsWith(' '+Library.toUtf8()))linked=true;
            }
            valid=target==Executable&&linked;
        }
        return qMakePair(valid,valid?QString():QStringLiteral("The installed Day and Night packages or existing daemon identity do not match audited KNightTime 6.6.5, Plasma Workspace 6.6.6, and KConfig 6.24.0."));
        }));
    };
    QObject::connect(process,qOverload<int,QProcess::ExitStatus>(&QProcess::finished),context,[done,timer]{timer->stop();done(false);});
    QObject::connect(process,&QProcess::errorOccurred,context,[done,timer](QProcess::ProcessError error){if(error==QProcess::FailedToStart){timer->stop();done(true);}});
    QObject::connect(timer,&QTimer::timeout,context,[done]{done(true);});
    process->start(QStringLiteral("/usr/bin/dpkg-query"),{QStringLiteral("-W"),QStringLiteral("-f=${Package} ${Version}\n"),
        QStringLiteral("knighttime"),QStringLiteral("libknighttime0"),QStringLiteral("plasma-workspace"),QStringLiteral("libkf6configcore6")});timer->start();
}
bool interfaceSupported(const QString &text)
{
    if(text.size()>65536)return false;
    QXmlStreamReader xml(text);bool inInterface=false;int interfaces=0,version=0;QString method;
    struct Method {QStringList inputs,outputs;};QMap<QString,Method> methods;
    while(!xml.atEnd()){
        xml.readNext();
        if(xml.isStartElement()&&xml.name()==QLatin1String("interface")){
            inInterface=xml.attributes().value("name")==Interface;if(inInterface&&++interfaces!=1)return false;
        }else if(xml.isEndElement()&&xml.name()==QLatin1String("interface"))inInterface=false;
        else if(inInterface&&xml.isStartElement()&&xml.name()==QLatin1String("property")&&xml.attributes().value("name")==QLatin1String("Version")){
            if(++version!=1||xml.attributes().value("type")!=QLatin1String("u")||xml.attributes().value("access")!=QLatin1String("read"))return false;
        }else if(inInterface&&xml.isStartElement()&&xml.name()==QLatin1String("method")){
            method=xml.attributes().value("name").toString();if(methods.contains(method))return false;methods.insert(method,{});
        }else if(inInterface&&!method.isEmpty()&&xml.isStartElement()&&xml.name()==QLatin1String("arg")){
            const auto direction=xml.attributes().value("direction");const auto type=xml.attributes().value("type").toString();
            if(direction==QLatin1String("in"))methods[method].inputs<<type;
            else if(direction==QLatin1String("out"))methods[method].outputs<<type;
            else return false;
        }
        else if(xml.isEndElement()&&xml.name()==QLatin1String("method"))method.clear();
    }
    return !xml.hasError()&&interfaces==1&&version==1
        &&methods.contains("Subscribe")&&methods.value("Subscribe").inputs==QStringList{QStringLiteral("a{sv}")}
        &&methods.value("Subscribe").outputs==QStringList{QStringLiteral("a{sv}")}
        &&methods.contains("Unsubscribe")&&methods.value("Unsubscribe").inputs==QStringList{QStringLiteral("u")}
        &&methods.value("Unsubscribe").outputs.isEmpty();
}
class DBusNightTimeService final : public NightTimeService {
    Q_OBJECT
public:
    DBusNightTimeService(const QDBusConnection &bus,NightTimeIdentityVerifier verifier,QObject *parent)
        :NightTimeService(parent),m_bus(bus),m_verifier(std::move(verifier))
    {
        if(!m_verifier)m_verifier=[this](uint pid,std::function<void(bool,QString)> completion){verifyInstalled(this,pid,std::move(completion));};
        auto *watcher=new QDBusServiceWatcher(Service,bus,QDBusServiceWatcher::WatchForOwnerChange,this);
        connect(watcher,&QDBusServiceWatcher::serviceOwnerChanged,this,[this]{++m_epoch;emit externalChange();});
    }
    void query(quint64 token,const QString &expectedOwner) override
    {
        auto operation=std::make_shared<Query>();operation->token=token;operation->epoch=m_epoch;operation->expectedOwner=expectedOwner;
        // The verifier with pid zero checks installation only and never launches
        // the daemon. No method is sent to the activatable well-known name.
        QPointer<DBusNightTimeService> safe(this);
        m_verifier(0,[safe,operation](bool installed,const QString &error){
            if(!safe)return;operation->runtime.installedVerified=installed;operation->runtime.version=installed?QStringLiteral("6.6.5"):QString();
            if(!installed){safe->finish(operation,false,error);return;}
            safe->owner([safe,operation](const QString &owner,bool absent){
                if(!safe)return;
                if(owner.isEmpty()){
                    if(absent&&operation->expectedOwner.isEmpty())safe->finish(operation,true,QStringLiteral("The native daemon is not running. Saved settings can be edited without starting it."));
                    else safe->finish(operation,false,QStringLiteral("The pinned Day and Night owner disappeared or cannot be read."));
                    return;
                }
                operation->runtime.owner=owner;operation->runtime.serviceAvailable=true;
                if(!operation->expectedOwner.isEmpty()&&owner!=operation->expectedOwner){safe->finish(operation,false,QStringLiteral("The Day and Night owner changed."));return;}
                safe->call(owner,Path,QStringLiteral("org.freedesktop.DBus.Introspectable"),QStringLiteral("Introspect"),{},[safe,operation](const QDBusMessage &message){
                    if(!safe)return;const QDBusPendingReply<QString> reply(message);
                    if(reply.isError()||!interfaceSupported(reply.value())){safe->finish(operation,false,QStringLiteral("The native Day and Night interface does not match API 1."));return;}
                    safe->call(operation->runtime.owner,Path,QStringLiteral("org.freedesktop.DBus.Properties"),QStringLiteral("Get"),{Interface,QStringLiteral("Version")},[safe,operation](const QDBusMessage &message){
                        if(!safe)return;const QDBusPendingReply<QDBusVariant> reply(message);const auto value=reply.value().variant();
                        if(reply.isError()||value.metaType().id()!=QMetaType::UInt||value.toUInt()!=1){safe->finish(operation,false,QStringLiteral("The native Day and Night API version is unsupported."));return;}
                        operation->runtime.apiVersion=1;
                        safe->call(QStringLiteral("org.freedesktop.DBus"),QStringLiteral("/org/freedesktop/DBus"),QStringLiteral("org.freedesktop.DBus"),QStringLiteral("GetConnectionUnixProcessID"),{operation->runtime.owner},[safe,operation](const QDBusMessage &message){
                            if(!safe)return;const QDBusPendingReply<uint> reply(message);
                            if(reply.isError()||!reply.value()){safe->finish(operation,false,QStringLiteral("The native Day and Night process cannot be verified."));return;}
                            safe->m_verifier(reply.value(),[safe,operation](bool verified,const QString &error){
                                if(!safe)return;if(!verified){safe->finish(operation,false,error);return;}
                                safe->owner([safe,operation](const QString &owner,bool){
                                    if(!safe)return;if(owner!=operation->runtime.owner){safe->finish(operation,false,QStringLiteral("The native Day and Night owner changed during discovery."));return;}
                                    operation->runtime.providerVerified=true;safe->finish(operation,true,{});
                                });
                            });
                        });
                    });
                });
            });
        });
    }
private:
    struct Query {quint64 token=0,epoch=0;QString expectedOwner;NightTimeRuntime runtime;bool done=false;};
    void finish(const std::shared_ptr<Query> &operation,bool success,QString error)
    {
        if(operation->done)return;operation->done=true;
        if(operation->epoch!=m_epoch){success=false;operation->runtime.providerVerified=false;error=QStringLiteral("The native Day and Night service changed during discovery.");}
        // Only a successful NameHasNoOwner result proves offline editing safe.
        // A transport failure must not be mistaken for daemon absence.
        if(!success&&!operation->runtime.serviceAvailable)operation->runtime.installedVerified=false;
        operation->runtime.epoch=m_epoch;operation->runtime.reason=error;
        emit queryFinished(operation->token,success,operation->runtime,error);
    }
    using Completion=std::function<void(const QDBusMessage &)>;
    void call(const QString &destination,const QString &path,const QString &interface,const QString &method,const QVariantList &arguments,Completion completion)
    {
        auto message=QDBusMessage::createMethodCall(destination,path,interface,method);message.setArguments(arguments);
        auto *pending=new QDBusPendingCallWatcher(m_bus.asyncCall(message,1500),this);
        auto done=std::make_shared<bool>(false);QPointer<QDBusPendingCallWatcher> safe(pending);
        auto finish=[pending,safe,done,completion=std::move(completion)]{if(*done||!safe)return;*done=true;const auto reply=pending->reply();pending->deleteLater();completion(reply);};
        connect(pending,&QDBusPendingCallWatcher::finished,this,finish);if(pending->isFinished())QTimer::singleShot(0,this,finish);
    }
    void owner(std::function<void(QString,bool)> completion)
    {
        call(QStringLiteral("org.freedesktop.DBus"),QStringLiteral("/org/freedesktop/DBus"),QStringLiteral("org.freedesktop.DBus"),QStringLiteral("GetNameOwner"),{Service},[completion=std::move(completion)](const QDBusMessage &message){
            const QDBusPendingReply<QString> reply(message);completion(reply.isError()?QString():reply.value(),reply.isError()&&message.errorName()==QLatin1String("org.freedesktop.DBus.Error.NameHasNoOwner"));
        });
    }
    QDBusConnection m_bus;NightTimeIdentityVerifier m_verifier;quint64 m_epoch=0;
};
QByteArray keySyntax(const QString &path,const QString &groupName,const QString &key)
{
    QFile file(path);if(!file.open(QIODevice::ReadOnly))return {};
    QByteArray evidence;QDataStream stream(&evidence,QIODevice::WriteOnly);bool group=false;
    while(!file.atEnd()){
        const auto line=file.readLine().trimmed(),header='['+groupName.toUtf8()+']';
        if(line.startsWith('[')){
            auto suffix=line.startsWith(header)?line.mid(header.size()):QByteArray("invalid");
            while(suffix.startsWith("[$i]")||suffix.startsWith("[$d]"))suffix.remove(0,4);
            group=suffix.isEmpty();
            if(group&&line!=header)stream<<line;
        }else if(group){
            const auto left=line.left(line.contains('=')?line.indexOf('='):line.size()).trimmed(),name=key.toUtf8();
            if(left==name||left.startsWith(name+'['))stream<<line;
        }
    }
    return evidence;
}
QVariant nativeValue(const KConfigGroup &group,NightTimeKey key,const QVariant &fallback,bool &known)
{
    const auto name=NightTimeTypes::keyName(key);if(!group.hasKey(name)){known=true;return fallback;}
    const auto raw=group.readEntry(name,QString());
    if(key==NightTimeKey::Automatic){known=true;return group.readEntry(name,fallback.toBool());}
    if(key==NightTimeKey::Source){
        if(raw.compare(QLatin1String("Location"),Qt::CaseInsensitive)==0){known=true;return 0;}
        if(raw.compare(QLatin1String("Times"),Qt::CaseInsensitive)==0){known=true;return 1;}
        bool ok=false;raw.toInt(&ok);known=ok;return group.readEntry(name,fallback.toInt());
    }
    if(key==NightTimeKey::SunriseStart||key==NightTimeKey::SunsetStart){const auto value=group.readEntry(name,fallback.toTime());known=value.isValid();return value;}
    bool ok=false;
    if(key==NightTimeKey::TransitionDuration){raw.toUInt(&ok);known=ok;return group.readEntry(name,fallback.toUInt());}
    raw.toDouble(&ok);const auto value=group.readEntry(name,fallback.toDouble());known=ok&&std::isfinite(value);return value;
}
KConfig::OpenFlags flags(bool cascade,bool daemon,bool sources)
{return cascade?(daemon?KConfig::NoGlobals:KConfig::FullConfig):(sources?KConfig::NoGlobals:KConfig::SimpleConfig);}
bool capability(const NightTimeRuntime &runtime)
{return runtime.installedVerified&&(!runtime.serviceAvailable||(runtime.providerVerified&&!runtime.owner.isEmpty()&&runtime.apiVersion==1));}
}

NightTimeService *createNightTimeService(const QDBusConnection &connection,QObject *parent)
{return new DBusNightTimeService(connection,{},parent);}
NightTimeService *createNightTimeService(const QDBusConnection &connection,NightTimeIdentityVerifier verifier,QObject *parent)
{return new DBusNightTimeService(connection,std::move(verifier),parent);}

// Source checks precede KConfig/QFile opening, including absent/broken links and
// distinct paths that alias one inode. No source can turn a read into a FIFO wait.
static bool safeSources(const QStringList &paths,QString &error)
{
    QMap<QString,QString> identities;
    for(const auto &path:paths){
        struct stat link{},actual{};const auto bytes=QFile::encodeName(path);
        if(::lstat(bytes.constData(),&link)!=0){if(errno==ENOENT)continue;error=QStringLiteral("A Day and Night source cannot be inspected.");return false;}
        if(::stat(bytes.constData(),&actual)!=0||!S_ISREG(actual.st_mode)||actual.st_size>MaximumConfigSize){error=QStringLiteral("A Day and Night source is not a bounded regular file (or is a broken link).");return false;}
        const auto identity=QString::number(qulonglong(actual.st_dev))+QLatin1Char(':')+QString::number(qulonglong(actual.st_ino)),clean=QDir::cleanPath(path);
        if(identities.contains(identity)&&identities.value(identity)!=clean){error=QStringLiteral("Distinct Day and Night configuration sources alias the same file.");return false;}identities.insert(identity,clean);
        const int fd=::open(bytes.constData(),O_RDONLY|O_NONBLOCK|O_CLOEXEC);struct stat opened{};
        const bool safe=fd>=0&&::fstat(fd,&opened)==0&&S_ISREG(opened.st_mode)&&opened.st_size<=MaximumConfigSize&&opened.st_dev==actual.st_dev&&opened.st_ino==actual.st_ino;
        if(fd>=0)::close(fd);if(!safe){error=QStringLiteral("A Day and Night configuration source is unreadable or changed during inspection.");return false;}
    }return true;
}
static QStringList sourceFiles(const QString &file,const KdeNightTimeBackend::ConfigContext &context,bool daemon=false)
{
    QStringList paths{file};paths<<(daemon?context.daemonDefaultSources:context.defaultSources);
    if(context.fullCascade){
        for(const auto &directory:QStandardPaths::standardLocations(QStandardPaths::GenericConfigLocation)){
            paths<<directory+QStringLiteral("/knighttimerc");
            if(!daemon)paths<<directory+QStringLiteral("/kdeglobals")<<directory+QStringLiteral("/system.kdeglobals");
        }
        if(!daemon&&!qEnvironmentVariableIsSet("KDE_SKIP_KDERC"))paths<<QStringLiteral("/etc/kde5rc");
    }
    paths.removeDuplicates();return paths;
}
static bool readFile(const QString &fileName,const KdeNightTimeBackend::ConfigContext &context,NightTimeSnapshot &snapshot,const NightTimeRuntime &runtime,QString &error)
{
    if(!QFileInfo(fileName).isAbsolute()||(context.fullCascade&&QDir::cleanPath(fileName)!=QDir::cleanPath(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)+QStringLiteral("/knighttimerc")))){
        error=QStringLiteral("Native cascading requires the active XDG knighttimerc path.");return false;
    }
    auto paths=sourceFiles(fileName,context)+sourceFiles(fileName,context,true);paths.removeDuplicates();
    if(context.fixture&&context.beforeFileRead)context.beforeFileRead();
    if(!safeSources(paths,error))return false;
    snapshot={};snapshot.runtime=runtime;QByteArray revision;QDataStream all(&revision,QIODevice::WriteOnly);
    const auto globalPath=QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)+QStringLiteral("/kdeglobals");
    KConfig local(fileName,KConfig::SimpleConfig),global(context.fullCascade?globalPath:fileName,KConfig::SimpleConfig);
    for(const bool daemon:{false,true}){
        const auto sources=daemon?context.daemonDefaultSources:context.defaultSources;
        auto config=std::make_unique<KConfig>(context.fullCascade?QStringLiteral("knighttimerc"):fileName,flags(context.fullCascade,daemon,!sources.isEmpty()));
        if(!sources.isEmpty())config->addConfigSources(sources);config->reparseConfiguration();
        auto &fields=daemon?snapshot.daemonFields:snapshot.fields;
        for(const auto key:NightTimeTypes::keys()){
            const auto name=NightTimeTypes::keyName(key),groupName=NightTimeTypes::groupName(key);auto group=config->group(groupName);
            const auto localGroup=local.group(groupName),globalGroup=global.group(groupName);NightTimeField field;
            config->setReadDefaults(true);field.inheritedPresent=group.hasKey(name);bool inheritedKnown=true;
            const auto inherited=nativeValue(group,key,NightTimeTypes::builtinDefault(key),inheritedKnown);config->setReadDefaults(false);
            field.effectivePresent=group.hasKey(name);bool known=true;field.value=nativeValue(group,key,inherited,known);
            field.known=known&&(field.effectivePresent||(inheritedKnown&&runtime.installedVerified));
            field.defaultValue=inherited;field.defaultKnown=inheritedKnown&&runtime.installedVerified;
            field.localPresent=localGroup.hasKey(name);field.globalPresent=!daemon&&context.fullCascade&&globalGroup.hasKey(name);
            field.immutable=group.isImmutable()||group.isEntryImmutable(name);
            field.writable=capability(runtime)&&!field.immutable&&config->accessMode()==KConfig::ReadWrite;
            field.reason=!capability(runtime)?runtime.reason.isEmpty()?QStringLiteral("The audited native Day and Night installation or running provider is unavailable."):runtime.reason
                :field.immutable?QStringLiteral("This native key is locked by policy."):config->accessMode()!=KConfig::ReadWrite?QStringLiteral("The native configuration is not writable."):QString();
            field.defaultNoOp=runtime.installedVerified&&(!field.effectivePresent||(field.known&&field.defaultKnown&&field.value==field.defaultValue));
            field.resettable=field.writable&&field.defaultKnown&&field.known&&!field.defaultNoOp;
            if(field.resettable&&field.globalPresent&&!field.localPresent&&!field.inheritedPresent&&global.accessMode()!=KConfig::ReadWrite)field.resettable=false;
            field.resetReason=!field.writable?field.reason:!field.defaultKnown?QStringLiteral("The inherited native default is malformed or unknown.")
                :field.defaultNoOp?QStringLiteral("KDE leaves this saved value unchanged when restoring defaults."):!field.known?QStringLiteral("Choose an explicit supported value to repair the malformed saved value.")
                :!field.resettable?QStringLiteral("The native global configuration is not writable."):QString();
            QByteArray inheritedData;QDataStream inheritedStream(&inheritedData,QIODevice::WriteOnly);
            inheritedStream<<field.inheritedPresent<<field.defaultKnown<<inherited<<field.immutable;
            auto inheritedPaths=sourceFiles(fileName,context,daemon);inheritedPaths.removeAll(fileName);if(!daemon&&context.fullCascade)inheritedPaths.removeAll(globalPath);
            for(const auto &path:inheritedPaths)inheritedStream<<path<<keySyntax(path,groupName,name);
            field.inheritedRevision=digest(inheritedData);QByteArray globalData;QDataStream globals(&globalData,QIODevice::WriteOnly);
            globals<<field.globalPresent<<(field.globalPresent?keySyntax(globalPath,groupName,name):QByteArray{});field.globalRevision=digest(globalData);
            QByteArray raw;QDataStream stream(&raw,QIODevice::WriteOnly);stream<<field.effectivePresent<<field.value<<field.known<<field.localPresent<<field.immutable
                <<keySyntax(fileName,groupName,name)<<field.inheritedRevision<<field.globalRevision;field.revision=digest(raw);
            fields.insert(key,field);all<<daemon<<int(key)<<field.revision;
        }
    }
    snapshot.revision=digest(revision);snapshot.cachedLocation.reason=QStringLiteral("No cached automatic location is available. No location request was made.");
    if(!context.cachedStateFile.isEmpty()){
        QFile state(context.cachedStateFile);
        QString cacheError;
        if(safeSources({context.cachedStateFile},cacheError)&&state.open(QIODevice::ReadOnly)&&state.size()<=MaximumConfigSize){
            KConfig cache(context.cachedStateFile,KConfig::SimpleConfig);const auto group=cache.group("AutomaticLocation");
            const auto latitude=group.readEntry("Latitude",qQNaN()),longitude=group.readEntry("Longitude",qQNaN());
            if(group.readEntry("Available",false)&&std::isfinite(latitude)&&std::isfinite(longitude)&&std::abs(latitude)<=90&&std::abs(longitude)<=180){
                snapshot.cachedLocation.available=true;snapshot.cachedLocation.latitude=latitude;snapshot.cachedLocation.longitude=longitude;
                snapshot.cachedLocation.reason=QStringLiteral("Saved automatic location; the native state has no timestamp, so its age is unknown. No fresh location request was made.");
            }
        }
    }
    error.clear();return true;
}

namespace {
struct FileResult { bool ok=false;NightTimeSnapshot snapshot;QString error;SettingsFileReservations::Inspection inspection; };
struct SaveResult { bool saved=false,notified=false;QString error;QStringList keys; };
static QStringList targets(const QString &file,const KdeNightTimeBackend::ConfigContext &context)
{
    QStringList paths{file};if(context.fullCascade)paths<<QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)+QStringLiteral("/kdeglobals");return paths;
}
static SaveResult saveFile(const QString &file,const KdeNightTimeBackend::ConfigContext &context,
                           const NightTimeSnapshot &expected,const NightTimeSnapshot &requested,
                           const SettingsFileReservations::Reservation &reservation,const QStringList &originalKeys,
                           const std::shared_ptr<std::atomic<bool>> &providerChanged)
{
    struct MutationEnd { SettingsFileReservations::Reservation value;~MutationEnd(){SettingsFileReservations::endMutation(value);} } end{reservation};
    SaveResult result;
    auto operation=[&]{
        NightTimeSnapshot fresh;QString error;
        if(!readFile(file,context,fresh,expected.runtime,error)||!NightTimeTypes::sameConfiguration(expected,fresh)
            ||!NightTimeTypes::requestValid(fresh,requested,error)||SettingsFileReservations::currentKeys(reservation.paths,&error)!=originalKeys){
            result.error=error.isEmpty()?QStringLiteral("Day and Night preferences, defaults, policy, or file identity changed before saving."):error;return;
        }
        if(providerChanged->load()){result.error=QStringLiteral("The Day and Night provider changed before saving. Reload before retrying.");return;}
        KConfig config(context.fullCascade?QStringLiteral("knighttimerc"):file,flags(context.fullCascade,false,!context.defaultSources.isEmpty()));
        if(!context.defaultSources.isEmpty())config.addConfigSources(context.defaultSources);config.reparseConfiguration();
        for(auto it=requested.intents.cbegin();it!=requested.intents.cend();++it){
            const auto key=it.key();auto group=config.group(NightTimeTypes::groupName(key));const auto name=NightTimeTypes::keyName(key);const auto field=requested.fields.value(key);
            if(it.value()==NightTimeIntent::RestoreDefault&&!expected.fields.value(key).inheritedPresent)group.revertToDefault(name,KConfig::Notify);
            else if(key==NightTimeKey::Automatic)group.writeEntry(name,field.value.toBool(),KConfig::Notify);
            else if(key==NightTimeKey::Source)group.writeEntry(name,field.value.toInt()==0?QStringLiteral("Location"):QStringLiteral("Times"),KConfig::Notify);
            else if(key==NightTimeKey::SunriseStart||key==NightTimeKey::SunsetStart)group.writeEntry(name,field.value.toTime(),KConfig::Notify);
            else if(key==NightTimeKey::TransitionDuration)group.writeEntry(name,field.value.toUInt(),KConfig::Notify);
            else group.writeEntry(name,field.value.toDouble(),KConfig::Notify);
        }
        if(context.fixture&&context.beforeSync)context.beforeSync();
        // A confirmed change before entering native sync is still abortable.
        if(providerChanged->load()){config.markAsClean();result.error=QStringLiteral("The Day and Night provider changed before native saving began.");return;}
        NightTimeSnapshot boundary;
        if(!readFile(file,context,boundary,expected.runtime,error)||!NightTimeTypes::sameConfiguration(expected,boundary)
            ||!NightTimeTypes::requestValid(boundary,requested,error)||SettingsFileReservations::currentKeys(reservation.paths,&error)!=originalKeys){
            config.markAsClean();result.error=error.isEmpty()?QStringLiteral("Day and Night defaults, policy, preferences, or file identity changed at the save boundary."):error;return;
        }
        if(providerChanged->load()){config.markAsClean();result.error=QStringLiteral("The Day and Night provider changed before native saving began.");return;}
        result.saved=true;
        const bool synced=config.sync();
        // A failed native sync can have partially saved and broadcast Notify.
        // markAsClean prevents destruction from implicitly retrying that save.
        if(!synced){config.markAsClean();result.error=QStringLiteral("The native Day and Night save failed. Some keys or a native notification may already have been sent. Reload before retrying.");}
        else result.notified=true;
        if(context.fixture&&context.afterSync)context.afterSync();
    };
    operation(); // All KConfig/group destruction completes inside this worker.
    QString identityError;result.keys=SettingsFileReservations::currentKeys(reservation.paths,&identityError);
    if(result.error.isEmpty()&&result.keys.isEmpty())result.error=identityError;
    return result;
}
}

// Application-owned jobs retain their service, activity, and reservation through
// the real worker terminal. Soft deadlines never finish or retry a native save.
class NightTimeReadJob : public QObject {
public:
    using Done=std::function<void(bool,const NightTimeSnapshot &,bool,bool,const QString &)>;
    QString file;KdeNightTimeBackend::ConfigContext context;QPointer<NightTimeService> service;
    quint64 token;bool writing=false,done=false,saved=false,notified=false;
    NightTimeSnapshot expected,requested;Done completion;std::function<void(const QString &)> pending;
    QTimer deadline;SettingsFileReservations::Reservation reservation;
    std::shared_ptr<std::atomic<bool>> providerChanged=std::make_shared<std::atomic<bool>>(false);
    NightTimeReadJob(const QString &path,const KdeNightTimeBackend::ConfigContext &settings,NightTimeService *native,quint64 id)
        :QObject(QCoreApplication::instance()),file(path),context(settings),service(native),token(id)
    {
        native->setParent(this);deadline.setSingleShot(true);deadline.setInterval(context.fixture?qMax(1,context.softDeadlineMs):30000);
        connect(&deadline,&QTimer::timeout,this,[this]{if(!done&&pending)pending(writing
            ?QStringLiteral("Saving Day and Night preferences is taking longer. The operation continues; wait before retrying or closing.")
            :QStringLiteral("Reading Day and Night preferences is taking longer. The operation continues; please wait."));});
        connect(native,&NightTimeService::externalChange,this,[changed=providerChanged]{changed->store(true);});
    }
    void start(){
        OperationManager::instance()->setActivity(this,writing?QStringLiteral("Saving Day and Night preferences"):QStringLiteral("Reading Day and Night preferences"),true);
        deadline.start();query(false);
    }
    void finish(bool ok,const NightTimeSnapshot &snapshot,const QString &error){
        if(done)return;done=true;deadline.stop();SettingsFileReservations::release(reservation);
        OperationManager::instance()->setActivity(this,{},false);
        if(completion)completion(ok,snapshot,saved,notified,error);deleteLater();
    }
    void query(bool verifying){
        if(!service){finish(false,{},QStringLiteral("The Day and Night service became unavailable."));return;}
        auto connection=std::make_shared<QMetaObject::Connection>();auto once=std::make_shared<bool>(false);
        *connection=connect(service,&NightTimeService::queryFinished,this,[this,connection,once,verifying](quint64 id,bool success,const NightTimeRuntime &runtime,const QString &error){
            if(id!=token||*once||done)return;*once=true;disconnect(*connection);
            if(writing&&!success){finish(false,{},error.isEmpty()?QStringLiteral("Day and Night provider verification failed."):error);return;}
            const auto registry=writing&&!verifying?SettingsFileReservations::snapshot():SettingsFileReservations::Snapshot{};
            const auto path=file;const auto settings=context;
            auto *watcher=new QFutureWatcher<FileResult>(this);
            connect(watcher,&QFutureWatcher<FileResult>::finished,this,[this,watcher,verifying]{
                const auto result=watcher->result();watcher->deleteLater();if(done)return;
                if(!writing){finish(result.ok,result.snapshot,result.error);return;}
                if(!result.ok){finish(false,{},result.error);return;}
                QString error;
                if(verifying){
                    if(providerChanged->load()||!NightTimeTypes::matchesRequest(expected,requested,result.snapshot,error)){
                        finish(false,result.snapshot,error.isEmpty()?QStringLiteral("The provider changed during saving; the native save and notification may already have occurred. Reload to inspect the result."):error);return;
                    }finish(true,result.snapshot,{});return;
                }
                if(providerChanged->load()||!NightTimeTypes::sameConfiguration(expected,result.snapshot)||!NightTimeTypes::requestValid(result.snapshot,requested,error)){
                    finish(false,{},error.isEmpty()?QStringLiteral("Day and Night preferences, policy, defaults, or provider changed before saving. Reload before retrying."):error);return;
                }
                if(!result.inspection.success){finish(false,{},result.inspection.error);return;}
                reservation=SettingsFileReservations::acquire(result.inspection,error);
                if(!reservation.valid()){finish(false,{},error);return;}
                if(!SettingsFileReservations::beginMutation(reservation)){finish(false,{},QStringLiteral("The native file reservation is unavailable."));return;}
                const auto path=file;const auto settings=context;const auto before=expected,after=requested;const auto lease=reservation;const auto keys=result.inspection.keys;const auto changed=providerChanged;
                auto *save=new QFutureWatcher<SaveResult>(this);
                connect(save,&QFutureWatcher<SaveResult>::finished,this,[this,save]{
                    const auto result=save->result();save->deleteLater();saved=result.saved;notified=result.notified;
                    const bool refreshed=SettingsFileReservations::refresh(reservation,result.keys);
                    if(!result.error.isEmpty()||!refreshed){finish(false,{},!result.error.isEmpty()?result.error:QStringLiteral("The saved file identity could not be retained for verification."));return;}
                    query(true);
                });
                save->setFuture(QtConcurrent::run([path,settings,before,after,lease,keys,changed]{return saveFile(path,settings,before,after,lease,keys,changed);}));
            });
            watcher->setFuture(QtConcurrent::run([path,settings,runtime,registry,writing=writing,verifying]{
                FileResult result;result.ok=readFile(path,settings,result.snapshot,runtime,result.error);
                if(result.ok&&writing&&!verifying)result.inspection=SettingsFileReservations::inspect(targets(path,settings),registry);
                return result;
            }));
        });
        service->query(token,writing?expected.runtime.owner:QString());
    }
};
class NightTimeWriteJob final:public NightTimeReadJob {public:using NightTimeReadJob::NightTimeReadJob;};

KdeNightTimeBackend::KdeNightTimeBackend(QObject *parent)
    :KdeNightTimeBackend(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)+QStringLiteral("/knighttimerc"),
        createNightTimeService(QDBusConnection::sessionBus()),ConfigContext{true,{},{},{}},parent){}
KdeNightTimeBackend::KdeNightTimeBackend(const QString &file,NightTimeService *service,const ConfigContext &context,QObject *parent)
    :NightTimeBackend(parent),m_file(file),m_context(context),m_service(service)
{
    if(m_context.fullCascade&&m_context.cachedStateFile.isEmpty())m_context.cachedStateFile=QStandardPaths::writableLocation(QStandardPaths::GenericStateLocation)+QStringLiteral("/knighttimestaterc");
    Q_ASSERT(service);service->setParent(this);
    m_debounce.setSingleShot(true);m_debounce.setInterval(100);connect(&m_debounce,&QTimer::timeout,this,&NightTimeBackend::externalChange);
    auto changed=[this]{watchFiles();m_debounce.start();};connect(service,&NightTimeService::externalChange,this,changed);
    connect(&m_watcher,&QFileSystemWatcher::fileChanged,this,changed);connect(&m_watcher,&QFileSystemWatcher::directoryChanged,this,changed);watchFiles();
}
QStringList KdeNightTimeBackend::relevantFiles(bool daemon)const{return sourceFiles(m_file,m_context,daemon);}
void KdeNightTimeBackend::watchFiles()
{
    auto paths=relevantFiles()+relevantFiles(true);if(!m_context.cachedStateFile.isEmpty())paths<<m_context.cachedStateFile;paths.removeDuplicates();
    for(const auto &path:paths){
        auto directory=QFileInfo(path).absolutePath();while(!QDir(directory).exists()&&directory!=QLatin1String("/"))directory=QFileInfo(directory).absolutePath();
        if(QDir(directory).exists()&&!m_watcher.directories().contains(directory))m_watcher.addPath(directory);
        if(QFileInfo::exists(path)&&!m_watcher.files().contains(path))m_watcher.addPath(path);
    }
}
void KdeNightTimeBackend::readCurrent(quint64 token)
{
    if(m_phase!=Phase::Idle||!m_service){emit readFinished(token,false,{},QStringLiteral("Another Day and Night operation is in progress."));return;}
    m_phase=Phase::Read;m_token=token;auto *job=new NightTimeReadJob(m_file,m_context,m_service,token);m_readJob=job;QPointer<KdeNightTimeBackend> safe(this);
    job->pending=[safe,token](const QString &text){if(safe)emit safe->operationPending(token,text);};
    job->completion=[safe,job,token](bool ok,const NightTimeSnapshot &snapshot,bool,bool,const QString &error){
        if(!safe)return;if(job->service)job->service->setParent(safe);safe->m_phase=Phase::Idle;safe->m_readJob=nullptr;safe->watchFiles();emit safe->readFinished(token,ok,snapshot,error);
    };job->start();
}
void KdeNightTimeBackend::writeChanges(quint64 token,const NightTimeSnapshot &expected,const NightTimeSnapshot &requested)
{
    if(m_phase!=Phase::Idle||!m_service){emit writeFinished(token,false,false,QStringLiteral("Another Day and Night operation is in progress."));return;}
    QString error;if(requested.intents.isEmpty()||!NightTimeTypes::requestValid(expected,requested,error)){
        emit writeFinished(token,false,false,error.isEmpty()?QStringLiteral("No explicit Day and Night change was requested."):error);return;
    }
    m_phase=Phase::Write;m_token=token;auto *job=new NightTimeWriteJob(m_file,m_context,m_service,token);m_writeJob=job;job->writing=true;job->expected=expected;job->requested=requested;QPointer<KdeNightTimeBackend> safe(this);
    job->pending=[safe,token](const QString &text){if(safe)emit safe->operationPending(token,text);};
    job->completion=[safe,job,token](bool,const NightTimeSnapshot &,bool saved,bool notified,const QString &error){
        if(!safe)return;if(job->service)job->service->setParent(safe);safe->m_phase=Phase::Idle;safe->m_writeJob=nullptr;safe->watchFiles();emit safe->writeFinished(token,saved,notified,error);
    };job->start();
}
#include "NightTimeBackend.moc"
