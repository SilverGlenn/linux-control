#include "NightLightBackend.h"

#include <KConfig>
#include <KConfigGroup>
#include "operations/OperationManager.h"
#include "operations/SettingsFileReservations.h"
#include <QCoreApplication>
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrentRun>
#include <QCryptographicHash>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusServiceWatcher>
#include <QDBusVariant>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QSet>
#include <QXmlStreamReader>
#include <algorithm>
#include <functional>
#include <memory>
#include <atomic>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

namespace {
const QString serviceName=QStringLiteral("org.kde.KWin.NightLight");
const QString objectPath=QStringLiteral("/org/kde/KWin/NightLight");
const QString propertiesInterface=QStringLiteral("org.freedesktop.DBus.Properties");
constexpr qint64 MaximumConfigSize=2*1024*1024;

QVariant unwrapped(QVariant value)
{
    if(value.metaType()==QMetaType::fromType<QDBusVariant>())return value.value<QDBusVariant>().variant();
    return value;
}
bool interfaceSupported(const QString &text,bool &preview)
{
    if(text.size()>65536)return false;
    const QMap<QString,QString> expected{{"available","b"},{"enabled","b"},{"running","b"},{"inhibited","b"},{"daylight","b"},
        {"mode","u"},{"currentTemperature","u"},{"targetTemperature","u"},{"previousTransitionDateTime","t"},
        {"scheduledTransitionDateTime","t"},{"previousTransitionDuration","u"},{"scheduledTransitionDuration","u"}};
    QMap<QString,QString> found;QMap<QString,QStringList> methods;
    QXmlStreamReader xml(text);bool inInterface=false;QString method;
    while(!xml.atEnd()) {
        xml.readNext();
        if(xml.isStartElement()&&xml.name()==QLatin1String("interface"))inInterface=xml.attributes().value("name")==serviceName;
        else if(xml.isEndElement()&&xml.name()==QLatin1String("interface"))inInterface=false;
        else if(inInterface&&xml.isStartElement()&&xml.name()==QLatin1String("property")) {
            const auto name=xml.attributes().value("name").toString();
            if(found.contains(name)||xml.attributes().value("access")!=QLatin1String("read"))return false;
            found.insert(name,xml.attributes().value("type").toString());
        } else if(inInterface&&xml.isStartElement()&&xml.name()==QLatin1String("method")) {
            method=xml.attributes().value("name").toString();if(methods.contains(method))return false;methods.insert(method,{});
        } else if(inInterface&&xml.isStartElement()&&xml.name()==QLatin1String("arg")&&!method.isEmpty()) {
            methods[method]<<xml.attributes().value("direction").toString()+QLatin1Char(':')+xml.attributes().value("type").toString();
        } else if(xml.isEndElement()&&xml.name()==QLatin1String("method"))method.clear();
    }
    preview=!xml.hasError()&&methods.value("preview")==QStringList{QStringLiteral("in:u")}
        &&methods.contains("stopPreview")&&methods.value("stopPreview").isEmpty();
    if(xml.hasError())return false;
    for(auto it=expected.cbegin();it!=expected.cend();++it)if(found.value(it.key())!=it.value())return false;
    return true;
}
bool readProperties(const QVariantMap &values,NightLightRuntime &runtime)
{
    const QStringList booleans{"available","enabled","running","inhibited","daylight"};
    const QStringList integers{"mode","currentTemperature","targetTemperature","previousTransitionDuration","scheduledTransitionDuration"};
    for(const auto &key:booleans)if(!values.contains(key)||unwrapped(values.value(key)).metaType().id()!=QMetaType::Bool)return false;
    for(const auto &key:integers)if(!values.contains(key)||unwrapped(values.value(key)).metaType().id()!=QMetaType::UInt)return false;
    for(const auto &key:{QStringLiteral("previousTransitionDateTime"),QStringLiteral("scheduledTransitionDateTime")})
        if(!values.contains(key)||unwrapped(values.value(key)).metaType().id()!=QMetaType::ULongLong)return false;
    runtime.available=unwrapped(values.value("available")).toBool();runtime.enabled=unwrapped(values.value("enabled")).toBool();
    runtime.running=unwrapped(values.value("running")).toBool();runtime.inhibited=unwrapped(values.value("inhibited")).toBool();runtime.daylight=unwrapped(values.value("daylight")).toBool();
    runtime.mode=unwrapped(values.value("mode")).toUInt();runtime.currentTemperature=unwrapped(values.value("currentTemperature")).toUInt();runtime.targetTemperature=unwrapped(values.value("targetTemperature")).toUInt();
    runtime.previousTransitionDateTime=unwrapped(values.value("previousTransitionDateTime")).toULongLong();runtime.scheduledTransitionDateTime=unwrapped(values.value("scheduledTransitionDateTime")).toULongLong();
    runtime.previousTransitionDuration=unwrapped(values.value("previousTransitionDuration")).toUInt();runtime.scheduledTransitionDuration=unwrapped(values.value("scheduledTransitionDuration")).toUInt();
    return runtime.mode<=1 && runtime.currentTemperature>=1000 && runtime.currentTemperature<=6500
        && runtime.targetTemperature>=1000 && runtime.targetTemperature<=6500;
}

class DBusNightLightService final : public NightLightService {
    Q_OBJECT
public:
    DBusNightLightService(const QDBusConnection &bus,std::function<QByteArray()> schema,QObject *parent):NightLightService(parent),m_bus(bus),m_schema(std::move(schema))
    {
        auto *watcher=new QDBusServiceWatcher(serviceName,bus,QDBusServiceWatcher::WatchForOwnerChange,this);
        watcher->addWatchedService(QStringLiteral("org.kde.KWin"));
        connect(watcher,&QDBusServiceWatcher::serviceOwnerChanged,this,[this]{++m_epoch;m_owner.clear();m_previewAvailable=false;m_previewRpcSent=false;++m_previewAttempt;QPointer<DBusNightLightService> safe(this);emit providerChanged();if(safe)emit safe->externalChange();});
        m_bus.connect(serviceName,objectPath,propertiesInterface,QStringLiteral("PropertiesChanged"),this,
            SLOT(propertiesChanged(QString,QVariantMap,QStringList,QDBusMessage)));
    }
    void query(quint64 token,const QString &expectedOwner) override
    {
        auto operation=std::make_shared<Query>();operation->token=token;operation->epoch=m_epoch;operation->expectedOwner=expectedOwner;
        owner(serviceName,[this,operation](const QString &name){
            if(name.isEmpty()||(!operation->expectedOwner.isEmpty()&&name!=operation->expectedOwner)){finish(operation,false,QStringLiteral("The Night Light owner is unavailable or changed."));return;}
            operation->runtime.owner=name;operation->runtime.serviceAvailable=true;
            owner(QStringLiteral("org.kde.KWin"),[this,operation](const QString &name){
                if(name!=operation->runtime.owner){finish(operation,false,QStringLiteral("Night Light is not owned by the current KWin process."));return;}
                call(name,QStringLiteral("/KWin"),QStringLiteral("org.kde.KWin"),QStringLiteral("supportInformation"),{},[this,operation](const QDBusMessage &message){
                    const QDBusPendingReply<QString> reply(message);
                    if(reply.isError()||reply.value().size()>65536){finish(operation,false,QStringLiteral("The KWin runtime version cannot be verified."));return;}
                    QStringList versions,modes;
                    for(const auto &line:reply.value().split(QLatin1Char('\n'))){
                        if(line.startsWith(QLatin1String("KWin version:")))versions<<line.mid(13).trimmed();
                        if(line.startsWith(QLatin1String("Operation Mode:")))modes<<line.mid(15).trimmed();
                    }
                    if(versions.size()==1)operation->runtime.version=versions.front();
                    if(versions!=QStringList{QStringLiteral("6.6.6")}||modes!=QStringList{QStringLiteral("Wayland")}){finish(operation,false,QStringLiteral("Night Light settings require the audited KWin 6.6.6 Wayland runtime."));return;}
                    schemaRead([this,operation](const QByteArray &bytes){
                        QString schemaError;
                        if(operation->epoch!=m_epoch||!validateNightLightSchema(bytes,schemaError)){finish(operation,false,QStringLiteral("The installed Night Light schema or pinned owner changed during discovery."));return;}
                        call(operation->runtime.owner,objectPath,QStringLiteral("org.freedesktop.DBus.Introspectable"),QStringLiteral("Introspect"),{},[this,operation](const QDBusMessage &message){
                            const QDBusPendingReply<QString> reply(message);bool preview=false;
                            if(reply.isError()||!interfaceSupported(reply.value(),preview)){finish(operation,false,QStringLiteral("The native Night Light interface does not match the audited schema."));return;}
                            operation->runtime.schemaSupported=true;operation->runtime.previewSupported=preview;
                            call(operation->runtime.owner,objectPath,propertiesInterface,QStringLiteral("GetAll"),{serviceName},[this,operation](const QDBusMessage &message){
                                const QDBusPendingReply<QVariantMap> reply(message);
                                if(reply.isError()||!readProperties(reply.value(),operation->runtime)){operation->runtime.schemaSupported=false;finish(operation,false,QStringLiteral("The native Night Light properties could not be read with their expected types."));return;}
                                owner(serviceName,[this,operation](const QString &name){
                                    if(name!=operation->runtime.owner){finish(operation,false,QStringLiteral("Night Light restarted during discovery."));return;}
                                    owner(QStringLiteral("org.kde.KWin"),[this,operation](const QString &name){
                                        if(name!=operation->runtime.owner){finish(operation,false,QStringLiteral("KWin changed during Night Light discovery."));return;}
                                        finish(operation,true,{});
                                    });
                                });
                            });
                        });
                    });
                });
            });
        });
    }
    void preview(quint64 token,int temperature,const QString &expectedOwner,quint64 expectedEpoch) override
    {
        const auto attempt=++m_previewAttempt;
        if(temperature<NightLightTypes::MinimumTemperature||temperature>NightLightTypes::MaximumTemperature||expectedOwner.isEmpty()||m_owner!=expectedOwner||m_epoch!=expectedEpoch||!m_previewAvailable){
            emit previewFinished(token,false,false,QStringLiteral("The preview temperature or pinned Night Light owner is unavailable."));return;
        }
        schemaRead([this,token,temperature,expectedOwner,expectedEpoch,attempt](const QByteArray &bytes){
            QString error;
            if(attempt!=m_previewAttempt)return; // pure local retirement has its own backend completion
            if(m_owner!=expectedOwner||m_epoch!=expectedEpoch||!m_previewAvailable||!validateNightLightSchema(bytes,error)){
                emit previewFinished(token,false,false,QStringLiteral("The preview schema, local request or pinned owner changed before dispatch."));return;
            }
            m_previewRpcSent=true;++m_previewDispatchSerial;
            call(expectedOwner,objectPath,serviceName,QStringLiteral("preview"),{uint(temperature)},[this,token,expectedEpoch,expectedOwner](const QDBusMessage &message){
                const bool success=expectedEpoch==m_epoch&&m_owner==expectedOwner&&message.type()==QDBusMessage::ReplyMessage&&message.arguments().isEmpty();
                emit previewFinished(token,success,false,success?QString():QStringLiteral("The Night Light preview request was not acknowledged. A transport timeout does not cancel queued execution; native expiry is 15 seconds after execution."));
            });
        });
    }

    bool retirePendingPreview() override {++m_previewAttempt;return m_previewRpcSent;}
    void stopPreview(quint64 token,const QString &expectedOwner,quint64 expectedEpoch) override
    {
        ++m_previewAttempt; // a stop also retires any not-yet-dispatched schema worker
        if(expectedOwner.isEmpty()||m_owner!=expectedOwner||m_epoch!=expectedEpoch){emit previewFinished(token,false,true,QStringLiteral("The Night Light owner changed; no replacement process was sent a stop request."));return;}
        const auto epoch=m_epoch,dispatchSerial=m_previewDispatchSerial;
        call(expectedOwner,objectPath,serviceName,QStringLiteral("stopPreview"),{},[this,token,epoch,dispatchSerial,expectedOwner](const QDBusMessage &message){
            const bool success=epoch==m_epoch&&m_owner==expectedOwner&&message.type()==QDBusMessage::ReplyMessage&&message.arguments().isEmpty();
            if(success&&dispatchSerial==m_previewDispatchSerial)m_previewRpcSent=false;
            emit previewFinished(token,success,true,success?QString():QStringLiteral("Stopping the global Night Light preview could not be acknowledged. Queued preview execution may still occur; native expiry is 15 seconds after execution."));
        });
    }
private slots:
    void propertiesChanged(const QString &interface,const QVariantMap &,const QStringList &,const QDBusMessage &message)
    {if(interface==serviceName&&!m_owner.isEmpty()&&message.service()==m_owner)emit externalChange();}
private:
    struct Query {quint64 token=0,epoch=0;QString expectedOwner;NightLightRuntime runtime;bool done=false;};
    void finish(const std::shared_ptr<Query> &operation,bool success,const QString &error)
    {
        if(operation->done)return;operation->done=true;
        success=success&&operation->epoch==m_epoch;
        operation->runtime.known=success;operation->runtime.epoch=m_epoch;
        operation->runtime.reason=success?QString():error.isEmpty()?QStringLiteral("The native Night Light provider changed during discovery."):error;
        if(success)m_owner=operation->runtime.owner;
        m_previewAvailable=success&&operation->runtime.available&&operation->runtime.previewSupported;
        emit queryFinished(operation->token,success,operation->runtime,operation->runtime.reason);
    }
    void schemaRead(std::function<void(const QByteArray &)> done)
    {
        const auto source=m_schema;auto *watcher=new QFutureWatcher<QByteArray>(this);
        connect(watcher,&QFutureWatcher<QByteArray>::finished,this,[watcher,done=std::move(done)]{const auto bytes=watcher->result();watcher->deleteLater();done(bytes);});
        watcher->setFuture(QtConcurrent::run([source]{return source();}));
    }
    using Completion=std::function<void(const QDBusMessage &)>;
    void call(const QString &service,const QString &path,const QString &interface,const QString &method,const QVariantList &arguments,Completion completion)
    {
        auto message=QDBusMessage::createMethodCall(service,path,interface,method);message.setArguments(arguments);
        auto *pending=new QDBusPendingCallWatcher(m_bus.asyncCall(message,1500),this);
        auto done=std::make_shared<bool>(false);QPointer<QDBusPendingCallWatcher> safe(pending);
        auto finish=[pending,safe,done,completion=std::move(completion)]{
            if(*done||!safe)return;*done=true;const auto message=pending->reply();pending->deleteLater();completion(message);
        };
        connect(pending,&QDBusPendingCallWatcher::finished,this,finish);
        if(pending->isFinished())QTimer::singleShot(0,this,finish);
    }
    void owner(const QString &name,std::function<void(const QString &)> completion)
    {
        call(QStringLiteral("org.freedesktop.DBus"),QStringLiteral("/org/freedesktop/DBus"),QStringLiteral("org.freedesktop.DBus"),QStringLiteral("GetNameOwner"),{name},[completion=std::move(completion)](const QDBusMessage &message){
            const QDBusPendingReply<QString> reply(message);completion(reply.isError()?QString():reply.value());
        });
    }
    QDBusConnection m_bus;QString m_owner;quint64 m_epoch=0,m_previewAttempt=0,m_previewDispatchSerial=0;bool m_previewAvailable=false,m_previewRpcSent=false;std::function<QByteArray()> m_schema;
};

QByteArray digest(const QByteArray &value){return QCryptographicHash::hash(value,QCryptographicHash::Sha256);}
QByteArray keySyntax(const QString &path,const QString &key)
{
    const int fd=::open(QFile::encodeName(path).constData(),O_RDONLY|O_NONBLOCK|O_CLOEXEC);if(fd<0)return {};
    QFile file;if(!file.open(fd,QIODevice::ReadOnly,QFileDevice::AutoCloseHandle)){::close(fd);return {};}
    struct stat metadata{};if(::fstat(fd,&metadata)!=0||!S_ISREG(metadata.st_mode)||metadata.st_size>MaximumConfigSize)return {};
    bool group=false;QByteArray evidence;QDataStream stream(&evidence,QIODevice::WriteOnly);
    while(!file.atEnd()){
        const auto line=file.readLine().trimmed();
        if(line.startsWith('[')){
            group=line=="[NightColor]"||line=="[NightColor][$i]"||line=="[NightColor][$d]";
            if(group&&(line.endsWith("[$i]")||line.endsWith("[$d]")))stream<<line;
        }else if(group){
            const auto left=line.left(line.contains('=')?line.indexOf('='):line.size()).trimmed();const auto name=key.toUtf8();
            if(left==name||left.startsWith(name+'['))stream<<line;
        }
    }
    return evidence;
}
QVariant nativeValue(const KConfigGroup &group,NightLightKey key,const QVariant &fallback,bool &known)
{
    const auto name=NightLightTypes::keyName(key);
    if(!group.hasKey(name)){known=true;return fallback;}
    const auto raw=group.readEntry(name,QString{});
    if(key==NightLightKey::Active){
        known=true; // KF truth conversion: false/no/off/0 are false; other text is true.
        return group.readEntry(name,fallback.toBool());
    }
    if(key==NightLightKey::Mode){
        if(raw.compare(QLatin1String("Constant"),Qt::CaseInsensitive)==0){known=true;return 0;}
        if(raw.compare(QLatin1String("DarkLight"),Qt::CaseInsensitive)==0){known=true;return 1;}
    }
    bool ok=false;raw.toInt(&ok);known=ok;return group.readEntry(name,fallback.toInt());
}
QVariant editorValue(NightLightKey key,const QVariant &raw)
{
    return key==NightLightKey::DayTemperature||key==NightLightKey::NightTemperature
        ?QVariant(qBound(NightLightTypes::MinimumTemperature,raw.toInt(),NightLightTypes::MaximumTemperature)):raw;
}
}

bool validateNightLightSchema(const QByteArray &bytes,QString &error)
{
    error=QStringLiteral("The installed Night Light schema does not match the audited four-key KWin 6.6.6 contract.");
    if(bytes.isEmpty()||bytes.size()>16384)return false;
    struct Entry{QString type,value;QStringList choices;};QMap<QString,Entry> entries;
    QXmlStreamReader xml(bytes);QString key;bool group=false,file=false;int fileCount=0,groupCount=0;QSet<QString> defaults;
    while(!xml.atEnd()){
        xml.readNext();
        if(xml.isStartElement()&&xml.name()==QLatin1String("kcfgfile")){if(++fileCount!=1)return false;file=xml.attributes().value("arg")==QLatin1String("true");}
        else if(xml.isStartElement()&&xml.name()==QLatin1String("group")){if(++groupCount!=1||group||xml.attributes().value("name")!=QLatin1String("NightColor"))return false;group=true;}
        else if(xml.isStartElement()&&xml.name()==QLatin1String("entry")){
            if(!group)return false;key=xml.attributes().value("name").toString();if(entries.contains(key))return false;entries.insert(key,{xml.attributes().value("type").toString(),{}, {}});
        }else if(xml.isStartElement()&&xml.name()==QLatin1String("default")){
            if(key.isEmpty()||defaults.contains(key))return false;defaults.insert(key);entries[key].value=xml.readElementText().trimmed();
        }else if(xml.isStartElement()&&xml.name()==QLatin1String("choice")){
            if(key.isEmpty())return false;entries[key].choices<<xml.attributes().value("name").toString();
        }else if(xml.isStartElement()&&(xml.name()==QLatin1String("min")||xml.name()==QLatin1String("max")))return false;
        else if(xml.isEndElement()&&xml.name()==QLatin1String("entry"))key.clear();
        else if(xml.isEndElement()&&xml.name()==QLatin1String("group"))group=false;
    }
    if(xml.hasError()||!file||entries.size()!=4)return false;
    for(const auto k:NightLightTypes::keys()){
        const auto name=NightLightTypes::keyName(k);if(!entries.contains(name))return false;const auto entry=entries[name];
        const auto type=k==NightLightKey::Active?QStringLiteral("Bool"):k==NightLightKey::Mode?QStringLiteral("Enum"):QStringLiteral("Int");
        const auto value=k==NightLightKey::Active?QStringLiteral("false"):k==NightLightKey::Mode?QStringLiteral("NightLightMode::DarkLight"):NightLightTypes::builtinDefault(k).toString();
        if(entry.type!=type||entry.value!=value)return false;
        if(k==NightLightKey::Mode&&entry.choices!=QStringList{QStringLiteral("Constant"),QStringLiteral("DarkLight")})return false;
        if(k!=NightLightKey::Mode&&!entry.choices.isEmpty())return false;
    }
    error.clear();return true;
}
NightLightService *createNightLightService(const QDBusConnection &connection,QObject *parent)
{
    return new DBusNightLightService(connection,[]{
        const QByteArray path("/usr/share/config.kcfg/nightlightsettings.kcfg");
        const int fd=::open(path.constData(),O_RDONLY|O_NONBLOCK|O_CLOEXEC);if(fd<0)return QByteArray();
        QFile file;if(!file.open(fd,QIODevice::ReadOnly,QFileDevice::AutoCloseHandle)){::close(fd);return QByteArray();}
        struct stat metadata{};if(::fstat(fd,&metadata)!=0||!S_ISREG(metadata.st_mode)||metadata.st_size>16384)return QByteArray();
        const auto bytes=file.read(16385);return bytes.size()<=16384&&file.error()==QFileDevice::NoError?bytes:QByteArray();
    },parent);
}
NightLightService *createNightLightService(const QDBusConnection &connection,const QByteArray &schema,QObject *parent)
{return new DBusNightLightService(connection,[schema]{return schema;},parent);}

namespace {
using ConfigContext=KdeNightLightBackend::ConfigContext;
QStringList relevantFiles(const QString &fileName,const ConfigContext &context)
{
    QStringList paths{fileName};paths<<context.defaultSources;
    if(context.fullCascade){for(const auto &directory:QStandardPaths::standardLocations(QStandardPaths::GenericConfigLocation))paths<<directory+"/kwinrc"<<directory+"/kdeglobals"<<directory+"/system.kdeglobals";if(!qEnvironmentVariableIsSet("KDE_SKIP_KDERC"))paths<<"/etc/kde5rc";}
    paths.removeDuplicates();return paths;
}
bool readFile(const QString &fileName,const ConfigContext &context,NightLightSnapshot &snapshot,const NightLightRuntime &runtime,QString &error)
{
    if(!QFileInfo(fileName).isAbsolute()||(context.fullCascade&&QDir::cleanPath(fileName)!=QDir::cleanPath(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)+QStringLiteral("/kwinrc")))){
        error=QStringLiteral("Native cascading requires the active XDG kwinrc path.");return false;
    }
    if(context.fixture&&context.beforeFileRead)context.beforeFileRead();
    QMap<QString,QString> canonicalPaths;QMap<QPair<quint64,quint64>,QString> identities;
    for(const auto &path:relevantFiles(fileName,context)){
        const QFileInfo info(path);if(!info.exists()&&!info.isSymLink())continue;
        if(!info.isFile()||!info.isReadable()||info.size()>MaximumConfigSize){error=QStringLiteral("A Night Light source is not a readable bounded regular file, or is a broken link.");return false;}
        const auto canonical=info.canonicalFilePath(),normalized=QDir::cleanPath(info.absoluteFilePath());struct stat metadata{};
        if(canonical.isEmpty()||::stat(QFile::encodeName(path).constData(),&metadata)!=0||!S_ISREG(metadata.st_mode)){error=QStringLiteral("A Night Light source changed identity during inspection.");return false;}
        const auto identity=qMakePair(quint64(metadata.st_dev),quint64(metadata.st_ino));
        if((canonicalPaths.contains(canonical)&&canonicalPaths.value(canonical)!=normalized)||(identities.contains(identity)&&identities.value(identity)!=normalized)){error=QStringLiteral("Distinct Night Light configuration or defaults paths alias the same file. No mutation is permitted.");return false;}
        canonicalPaths.insert(canonical,normalized);identities.insert(identity,normalized);
        const int fd=::open(QFile::encodeName(path).constData(),O_RDONLY|O_NONBLOCK|O_CLOEXEC);if(fd<0){error=QStringLiteral("A native Night Light source is unreadable.");return false;}
        QFile file;if(!file.open(fd,QIODevice::ReadOnly,QFileDevice::AutoCloseHandle)){::close(fd);error=QStringLiteral("A native Night Light source could not be opened.");return false;}
        struct stat opened{};if(::fstat(fd,&opened)!=0||!S_ISREG(opened.st_mode)||opened.st_dev!=metadata.st_dev||opened.st_ino!=metadata.st_ino||opened.st_size>MaximumConfigSize){error=QStringLiteral("A native Night Light source changed identity before reading.");return false;}
    }
    auto config=std::make_unique<KConfig>(context.fullCascade?QStringLiteral("kwinrc"):fileName,context.fullCascade?KConfig::FullConfig:context.defaultSources.isEmpty()?KConfig::SimpleConfig:KConfig::NoGlobals);
    if(!context.defaultSources.isEmpty())config->addConfigSources(context.defaultSources);config->reparseConfiguration();
    KConfig local(fileName,KConfig::SimpleConfig);
    const auto globalPath=QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)+QStringLiteral("/kdeglobals");
    std::unique_ptr<KConfig> global;if(context.fullCascade)global=std::make_unique<KConfig>(globalPath,KConfig::SimpleConfig);
    auto group=config->group("NightColor");const auto localGroup=local.group("NightColor"),globalGroup=global?global->group("NightColor"):KConfigGroup{};
    snapshot={};snapshot.runtime=runtime;QByteArray revision;QDataStream all(&revision,QIODevice::WriteOnly);
    for(const auto key:NightLightTypes::keys()){
        const auto name=NightLightTypes::keyName(key);NightLightField field;
        config->setReadDefaults(true);field.inheritedPresent=group.hasKey(name);
        bool inheritedKnown=true;const auto inheritedValue=nativeValue(group,key,NightLightTypes::builtinDefault(key),inheritedKnown);
        config->setReadDefaults(false);
        const bool effective=group.hasKey(name);bool loadedKnown=true;
        field.value=nativeValue(group,key,inheritedValue,loadedKnown);
        field.known=loadedKnown&&(effective||(inheritedKnown&&runtime.schemaSupported));
        field.defaultValue=editorValue(key,inheritedValue);field.defaultKnown=inheritedKnown&&runtime.schemaSupported;
        field.effectivePresent=effective;field.localPresent=localGroup.hasKey(name);field.globalPresent=context.fullCascade&&globalGroup.hasKey(name);
        field.immutable=group.isImmutable()||group.isEntryImmutable(name);
        field.writable=runtime.serviceAvailable&&runtime.schemaSupported&&runtime.known&&runtime.available
            &&!field.immutable&&config->accessMode()==KConfig::ReadWrite;
        field.reason=!runtime.serviceAvailable||!runtime.schemaSupported||!runtime.known||!runtime.available
            ?runtime.reason.isEmpty()?QStringLiteral("The audited native Night Light service is unavailable."):runtime.reason
            :field.immutable?QStringLiteral("This native key is locked by policy.")
            :config->accessMode()!=KConfig::ReadWrite?QStringLiteral("The native Night Light configuration is not writable."):QString();
        field.defaultNoOp=runtime.schemaSupported&&(!effective||(field.known&&field.defaultKnown&&editorValue(key,field.value)==field.defaultValue));
        field.resettable=field.writable&&field.defaultKnown&&field.known&&!field.defaultNoOp;
        if(field.resettable&&field.globalPresent&&!field.localPresent&&!field.inheritedPresent&&global->accessMode()!=KConfig::ReadWrite)field.resettable=false;
        field.resetReason=!field.writable?field.reason:!field.defaultKnown?QStringLiteral("The inherited native editor default is malformed or unknown.")
            :field.defaultNoOp?QStringLiteral("KDE leaves this saved value unchanged when restoring defaults.")
            :!field.known?QStringLiteral("The saved value is malformed; choose an explicit supported value to repair it.")
            :!field.resettable?QStringLiteral("The native global configuration is not writable."):QString();
        QByteArray inherited;QDataStream inheritedStream(&inherited,QIODevice::WriteOnly);
        inheritedStream<<field.inheritedPresent<<field.defaultKnown<<inheritedValue<<field.immutable;
        auto inheritedPaths=relevantFiles(fileName,context);inheritedPaths.removeAll(fileName);if(context.fullCascade)inheritedPaths.removeAll(globalPath);
        for(const auto &path:inheritedPaths)inheritedStream<<path<<keySyntax(path,name);
        field.inheritedRevision=digest(inherited);
        QByteArray globalData;QDataStream globalStream(&globalData,QIODevice::WriteOnly);
        globalStream<<field.globalPresent<<(context.fullCascade?keySyntax(globalPath,name):QByteArray{});
        field.globalRevision=digest(globalData);
        QByteArray data;QDataStream stream(&data,QIODevice::WriteOnly);
        stream<<effective<<field.value<<field.known<<field.localPresent<<field.immutable<<keySyntax(fileName,name)<<field.inheritedRevision<<field.globalRevision;
        field.revision=digest(data);all<<int(key)<<field.revision;snapshot.fields.insert(key,field);
    }
    snapshot.revision=digest(revision);error.clear();return true;
}
namespace Reservations=SettingsFileReservations;
QStringList mutationPaths(const QString &fileName,const ConfigContext &context)
{
    QStringList paths{fileName};if(context.fullCascade)paths<<QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)+QStringLiteral("/kdeglobals");return paths;
}
struct FileResult {
    bool success=false,saved=false,notified=false;
    NightLightSnapshot snapshot;
    QString error;
    Reservations::Inspection inspection;
    QStringList currentKeys;
};
FileResult readFiles(const QString &fileName,const ConfigContext &context,const NightLightRuntime &runtime)
{FileResult result;result.success=readFile(fileName,context,result.snapshot,runtime,result.error);return result;}
FileResult saveFiles(const QString &fileName,const ConfigContext &context,const NightLightRuntime &runtime,const NightLightSnapshot &expected,const NightLightSnapshot &requested,const QStringList &expectedKeys,const Reservations::Reservation &reservation,const std::shared_ptr<std::atomic_bool> &providerChanged)
{
    // endMutation is atomic-only. The app-owned GUI continuation retains the
    // reservation until all native work and readback have actually terminated.
    struct MutationEnd {Reservations::Reservation value;~MutationEnd(){Reservations::endMutation(value);}} end{reservation};
    FileResult result;NightLightSnapshot fresh;
    if(!readFile(fileName,context,fresh,runtime,result.error)||!NightLightTypes::sameConfiguration(expected,fresh)||!NightLightTypes::requestValid(fresh,requested,result.error)||Reservations::currentKeys(reservation.paths,&result.error)!=expectedKeys){
        if(result.error.isEmpty())result.error=QStringLiteral("Night Light preferences, defaults, policy or file identity changed at the native save boundary.");return result;
    }
    if(providerChanged->load()){result.error=QStringLiteral("The Night Light provider changed before file mutation.");return result;}
    {
        // Own every native object on this worker. A shared pointer to a GUI
        // KSharedConfig instance would not provide cross-thread synchronization.
        KConfig config(context.fullCascade?QStringLiteral("kwinrc"):fileName,context.fullCascade?KConfig::FullConfig:context.defaultSources.isEmpty()?KConfig::SimpleConfig:KConfig::NoGlobals);
        if(!context.defaultSources.isEmpty())config.addConfigSources(context.defaultSources);config.reparseConfiguration();auto group=config.group("NightColor");
        for(auto it=requested.intents.cbegin();it!=requested.intents.cend();++it){
            const auto key=it.key();const auto name=NightLightTypes::keyName(key);const auto field=requested.fields.value(key);
            if(it.value()==NightLightIntent::RestoreDefault&&!expected.fields.value(key).inheritedPresent)group.revertToDefault(name,KConfig::Notify);
            else if(key==NightLightKey::Active)group.writeEntry(name,field.value.toBool(),KConfig::Notify);
            else if(key==NightLightKey::Mode&&field.value.toInt()>=0&&field.value.toInt()<=1)group.writeEntry(name,field.value.toInt()==0?QStringLiteral("Constant"):QStringLiteral("DarkLight"),KConfig::Notify);
            else group.writeEntry(name,field.value.toInt(),KConfig::Notify);
        }
        if(context.fixture&&context.beforeSync)context.beforeSync();
        if(providerChanged->load()){config.markAsClean();result.error=QStringLiteral("The Night Light provider changed before native synchronization. No file synchronization was attempted.");return result;}
        // KConfig performs a native post-lock merge, not compare-and-swap.
        // Notify is a broadcast: sync can emit it even after a later write
        // failure, and owner loss during the native wait cannot cancel it.
        const bool synced=config.sync();result.saved=true;
        if(!synced)config.markAsClean(); // suppress every destructor/reparse retry
        if(context.fixture&&context.afterSync)context.afterSync();
        result.currentKeys=Reservations::currentKeys(reservation.paths);
        if(!synced){result.error=QStringLiteral("The native Night Light save failed. Some preferences and a broadcast notification may already have been delivered. Revert to inspect saved settings.");return result;}
        result.notified=true;
    }
    if(providerChanged->load()){result.error=QStringLiteral("Night Light was saved, but its provider changed during synchronization. Native broadcast delivery and runtime adoption are uncertain.");return result;}
    result.success=readFile(fileName,context,result.snapshot,runtime,result.error)&&NightLightTypes::matchesRequest(expected,requested,result.snapshot,result.error);
    if(!result.success&&result.error.isEmpty())result.error=QStringLiteral("Saved Night Light preferences could not be verified.");
    return result;
}
}

class NightLightWriteJob final : public QObject {
public:
    using Finished=std::function<void(bool,bool,const QString &)>;
    using Pending=std::function<void(const QString &)>;
    NightLightWriteJob(QString file,ConfigContext context,NightLightService *service,quint64 token,NightLightSnapshot expected,NightLightSnapshot requested)
        :QObject(QCoreApplication::instance()),m_file(std::move(file)),m_context(std::move(context)),m_service(service),m_token(token),m_expected(std::move(expected)),m_requested(std::move(requested))
    {
        service->setParent(this);m_deadline.setSingleShot(true);m_deadline.setInterval(m_context.fixture?qMax(1,m_context.softDeadlineMs):20000);
        connect(&m_deadline,&QTimer::timeout,this,[this]{if(m_phase!=Phase::Done&&pending)pending(QStringLiteral("Saving Night Light is taking longer and continues in the background. Wait for completion before retrying or closing."));});
        connect(service,&NightLightService::providerChanged,this,[this]{m_providerChanged->store(true);});
        connect(service,&NightLightService::queryFinished,this,[this](quint64 token,bool success,const NightLightRuntime &runtime,const QString &error){if(token==m_token)queried(success,runtime,error);});
    }
    Finished finished;Pending pending;
    void start()
    {
        OperationManager::instance()->setActivity(this,QStringLiteral("Saving Night Light"),true);m_deadline.start();m_service->query(m_token,m_expected.runtime.owner);
    }
private:
    enum class Phase {PreflightQuery,Admission,Save,ObserveQuery,ObserveFiles,ObserveDelay,Done};
    template<class Work,class Done> void worker(Work work,Done done)
    {
        auto *watcher=new QFutureWatcher<FileResult>(this);
        connect(watcher,&QFutureWatcher<FileResult>::finished,this,[this,watcher,done=std::move(done)]{auto result=watcher->result();watcher->deleteLater();if(m_phase!=Phase::Done)done(std::move(result));});
        watcher->setFuture(QtConcurrent::run(std::move(work)));
    }
    void queried(bool success,const NightLightRuntime &runtime,const QString &error)
    {
        if(m_phase==Phase::PreflightQuery){
            if(!success){complete(false,false,error.isEmpty()?QStringLiteral("The Night Light provider could not be verified before saving."):error);return;}
            m_phase=Phase::Admission;const auto file=m_file;const auto context=m_context;const auto reservations=Reservations::snapshot();
            worker([file,context,runtime,reservations]{auto result=readFiles(file,context,runtime);if(result.success){result.inspection=Reservations::inspect(mutationPaths(file,context),reservations);if(!result.inspection.success){result.success=false;result.error=result.inspection.error;}}return result;},[this](FileResult result){
                QString error;
                if(!result.success||!NightLightTypes::sameConfiguration(m_expected,result.snapshot)||!NightLightTypes::requestValid(result.snapshot,m_requested,error)){
                    complete(false,false,!result.error.isEmpty()?result.error:!error.isEmpty()?error:QStringLiteral("Night Light preferences, defaults, policy or provider changed before saving."));return;
                }
                m_reservation=Reservations::acquire(result.inspection,error);
                if(!m_reservation.valid()){complete(false,false,error);return;}
                if(!Reservations::beginMutation(m_reservation)){complete(false,false,QStringLiteral("The Night Light file reservation could not enter its native transaction."));return;}
                m_phase=Phase::Save;const auto file=m_file;const auto context=m_context;const auto runtime=result.snapshot.runtime;const auto expected=m_expected;const auto requested=m_requested;const auto keys=result.inspection.keys;const auto reservation=m_reservation;const auto changed=m_providerChanged;
                worker([file,context,runtime,expected,requested,keys,reservation,changed]{return saveFiles(file,context,runtime,expected,requested,keys,reservation,changed);},[this](FileResult result){
                    m_saved=result.saved;m_notified=result.notified;
                    const bool refreshed=result.currentKeys.isEmpty()?!result.saved:Reservations::refresh(m_reservation,result.currentKeys);
                    if(!result.success||!refreshed){complete(m_saved,m_notified,!result.error.isEmpty()?result.error:QStringLiteral("The Night Light file reservation could not verify post-save identities. Revert to inspect saved settings."));return;}
                    observe();
                });
            });return;
        }
        if(m_phase!=Phase::ObserveQuery)return;m_phase=Phase::ObserveFiles;const auto file=m_file;const auto context=m_context;
        worker([file,context,runtime]{return readFiles(file,context,runtime);},[this,success,error](FileResult result){
            QString verification;
            if(!result.success||!NightLightTypes::matchesRequest(m_expected,m_requested,result.snapshot,verification)){complete(m_saved,m_notified,!result.error.isEmpty()?result.error:verification);return;}
            if(m_providerChanged->load()){complete(m_saved,m_notified,QStringLiteral("The Night Light provider changed during Apply; broadcast delivery and runtime adoption remain uncertain."));return;}
            if(!success||!NightLightTypes::runtimeMatches(result.snapshot)){
                if(m_attempt<4){++m_attempt;m_phase=Phase::ObserveDelay;QTimer::singleShot(200,this,[this]{if(m_phase==Phase::ObserveDelay)observe();});return;}
                complete(m_saved,m_notified,QStringLiteral("Night Light settings were saved with native broadcast notification, but KWin's exposed state has not matched within the observation window. %1").arg(error));return;
            }
            complete(m_saved,m_notified,{});
        });
    }
    void observe(){m_phase=Phase::ObserveQuery;m_service->query(m_token,m_expected.runtime.owner);}
    void complete(bool saved,bool notified,const QString &error)
    {
        if(m_phase==Phase::Done)return;m_phase=Phase::Done;m_deadline.stop();Reservations::release(m_reservation);
        const auto done=std::move(finished);if(done)done(saved,notified,error);
        OperationManager::instance()->setActivity(this,{},false);deleteLater();
    }
    QString m_file;ConfigContext m_context;NightLightService *m_service;quint64 m_token;
    NightLightSnapshot m_expected,m_requested;Reservations::Reservation m_reservation;
    std::shared_ptr<std::atomic_bool> m_providerChanged=std::make_shared<std::atomic_bool>(false);
    Phase m_phase=Phase::PreflightQuery;bool m_saved=false,m_notified=false;int m_attempt=0;QTimer m_deadline;
};

KdeNightLightBackend::KdeNightLightBackend(QObject *parent)
    :KdeNightLightBackend(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)+QStringLiteral("/kwinrc"),createNightLightService(QDBusConnection::sessionBus()),ConfigContext{true,{}},parent){}
KdeNightLightBackend::KdeNightLightBackend(const QString &file,NightLightService *service,const ConfigContext &context,QObject *parent)
    :NightLightBackend(parent),m_file(file),m_context(context),m_service(service)
{
    Q_ASSERT(service);service->setParent(this);m_deadline.setSingleShot(true);m_deadline.setInterval(m_context.fixture?qMax(1,m_context.softDeadlineMs):20000);
    connect(&m_deadline,&QTimer::timeout,this,[this]{
        if(m_phase==Phase::ReadQuery){const auto token=m_token;m_phase=Phase::Idle;QPointer<KdeNightLightBackend> safe(this);OperationManager::instance()->setActivity(this,{},false);if(safe)emit readFinished(token,false,{},QStringLiteral("Night Light discovery timed out."));}
        else if(m_phase==Phase::ReadFiles)emit operationPending(m_token,QStringLiteral("Reading Night Light is taking longer and continues in the background. Wait for completion before retrying or closing."));
    });
    m_previewExpiry.setSingleShot(true);m_previewExpiry.setInterval(m_context.fixture?qMax(1,m_context.previewTrackingMs):15000);
    connect(&m_previewExpiry,&QTimer::timeout,this,[this]{retireLocalPreview(QStringLiteral("The local preview tracking window ended. Unsent local work was retired; an already sent native request may still execute later. Remote expiry or cleanup has not been proved."),true,true);});
    connect(service,&NightLightService::queryFinished,this,&KdeNightLightBackend::queryFinished);
    connect(service,&NightLightService::previewFinished,this,[this](quint64 token,bool success,bool stopped,const QString &error){
        if(token!=m_previewToken||m_previewReplyRetired||!m_previewAwaitingReply)return;
        m_previewAwaitingReply=false;
        if(stopped&&success){m_previewMayBeActive=false;m_previewLeaseUncertain=false;m_previewOwner.clear();m_previewExpiry.stop();}else if(!stopped)m_previewExpiry.start();
        emit previewFinished(token,success,stopped,error);
    });
    connect(service,&NightLightService::providerChanged,this,[this]{retireLocalPreview(QStringLiteral("The Night Light provider changed. Unsent local preview work was retired; any already sent request has an uncertain result. No replacement provider was stopped."),true,true);});
    m_debounce.setSingleShot(true);m_debounce.setInterval(100);connect(&m_debounce,&QTimer::timeout,this,&NightLightBackend::externalChange);
    auto changed=[this]{watchFiles();m_debounce.start();};connect(service,&NightLightService::externalChange,this,changed);connect(&m_watcher,&QFileSystemWatcher::fileChanged,this,changed);connect(&m_watcher,&QFileSystemWatcher::directoryChanged,this,changed);watchFiles();
}
KdeNightLightBackend::~KdeNightLightBackend()
{
    // A synchronous rejection/fixture reply must not emit outward from a
    // partially destroyed backend. Keep the service's write-job connections.
    QObject::disconnect(m_service,nullptr,this,nullptr);
    const bool mayHaveDispatched=m_service->retirePendingPreview();m_previewReplyRetired=true;m_previewAwaitingReply=false;
    // This is best effort against our pinned provider, including while that
    // service is temporarily parented to an application-owned write job.
    // Tracking-window expiry never proves remote expiry or cancels queued preview.
    if(mayHaveDispatched&&m_previewMayBeActive&&!m_previewOwner.isEmpty())m_service->stopPreview(0,m_previewOwner,m_previewEpoch);
}
void KdeNightLightBackend::retireLocalPreview(const QString &reason,bool clearLease,bool notifyAwaiting)
{
    const bool mayHaveDispatched=m_service->retirePendingPreview();
    const auto token=m_previewToken;const bool stopped=m_previewAwaitingStop;
    const bool notify=notifyAwaiting&&m_previewAwaitingReply&&!m_previewReplyRetired;
    m_previewReplyRetired=true;m_previewAwaitingReply=false;
    if(clearLease||!mayHaveDispatched){m_previewLeaseUncertain=m_previewLeaseUncertain||m_previewMayBeActive||notify;m_previewMayBeActive=false;m_previewOwner.clear();m_previewExpiry.stop();}
    if(notify)emit previewFinished(token,false,stopped,reason);
}
QStringList KdeNightLightBackend::relevantFiles() const{return ::relevantFiles(m_file,m_context);}
void KdeNightLightBackend::watchFiles()
{
    for(const auto &path:relevantFiles()){
        auto directory=QFileInfo(path).absolutePath();while(!QDir(directory).exists()&&directory!=QLatin1String("/"))directory=QFileInfo(directory).absolutePath();
        if(QDir(directory).exists()&&!m_watcher.directories().contains(directory))m_watcher.addPath(directory);if(QFileInfo::exists(path)&&!m_watcher.files().contains(path))m_watcher.addPath(path);
    }
}
void KdeNightLightBackend::readCurrent(quint64 token)
{
    if(m_phase!=Phase::Idle){emit readFinished(token,false,{},QStringLiteral("Another Night Light operation is in progress."));return;}
    m_phase=Phase::ReadQuery;m_token=token;m_deadline.start();QPointer<KdeNightLightBackend> safe(this);OperationManager::instance()->setActivity(this,QStringLiteral("Reading Night Light"),true);if(safe)m_service->query(token);
}
void KdeNightLightBackend::queryFinished(quint64 token,bool,const NightLightRuntime &runtime,const QString &)
{
    if(token!=m_token||m_phase!=Phase::ReadQuery)return;
    if(m_previewMayBeActive&&(runtime.epoch!=m_previewEpoch||(!runtime.owner.isEmpty()&&runtime.owner!=m_previewOwner))){QPointer<KdeNightLightBackend> safe(this);retireLocalPreview(QStringLiteral("The Night Light provider changed during discovery. Unsent local preview work was retired; remote cleanup has not been proved."),true,true);if(!safe)return;}
    m_phase=Phase::ReadFiles;const auto file=m_file;const auto context=m_context;QPointer<KdeNightLightBackend> safe(this);
    auto *readJob=new QObject(QCoreApplication::instance());auto *watcher=new QFutureWatcher<FileResult>(readJob);
    connect(watcher,&QFutureWatcher<FileResult>::finished,readJob,[safe,watcher,readJob,token]{auto result=watcher->result();watcher->deleteLater();const bool deliver=safe&&safe->m_phase==Phase::ReadFiles&&safe->m_token==token;if(deliver){safe->m_deadline.stop();safe->m_phase=Phase::Idle;safe->watchFiles();}OperationManager::instance()->setActivity(readJob,{},false);if(deliver&&safe)emit safe->readFinished(token,result.success,result.snapshot,result.error);readJob->deleteLater();});
    OperationManager::instance()->setActivity(readJob,QStringLiteral("Reading Night Light"),true);if(safe)OperationManager::instance()->setActivity(safe,{},false);watcher->setFuture(QtConcurrent::run([file,context,runtime]{return readFiles(file,context,runtime);}));
}
void KdeNightLightBackend::writeChanges(quint64 token,const NightLightSnapshot &expected,const NightLightSnapshot &requested)
{
    if(m_phase!=Phase::Idle){emit writeFinished(token,false,false,QStringLiteral("Another Night Light operation is in progress."));return;}QString error;
    if(requested.intents.isEmpty()||!NightLightTypes::requestValid(expected,requested,error)){emit writeFinished(token,false,false,error.isEmpty()?QStringLiteral("No explicit Night Light change was requested."):error);return;}
    ++m_previewGeneration; // accepted Apply retires callers older than its ownership transfer
    m_token=token;m_phase=Phase::Write;auto *job=new NightLightWriteJob(m_file,m_context,m_service,token,expected,requested);m_writeJob=job;QPointer<KdeNightLightBackend> safe(this);
    job->pending=[safe,token](const QString &message){if(safe&&safe->m_phase==Phase::Write&&safe->m_token==token)emit safe->operationPending(token,message);};
    job->finished=[safe,token](bool saved,bool notified,const QString &error){if(!safe)return;safe->m_service->setParent(safe);safe->m_writeJob=nullptr;safe->m_phase=Phase::Idle;safe->watchFiles();emit safe->writeFinished(token,saved,notified,error);};
    // Establish app ownership/activity before an awaiting raw preview observer
    // can delete this backend in response to its local-retirement completion.
    OperationManager::instance()->setActivity(job,QStringLiteral("Saving Night Light"),true);
    if(safe)safe->retireLocalPreview(QStringLiteral("Saved-settings Apply retired unsent local preview work. Any already sent preview request may still execute; native cleanup has not been proved."),false,true);
    job->start();
}
void KdeNightLightBackend::preview(quint64 token,int temperature,const QString &expectedOwner,quint64 expectedEpoch)
{
    const auto generation=++m_previewGeneration;
    if(m_phase!=Phase::Idle){emit previewFinished(token,false,false,QStringLiteral("A saved-settings operation is in progress."));return;}
    if(temperature<NightLightTypes::MinimumTemperature||temperature>NightLightTypes::MaximumTemperature||expectedOwner.isEmpty()){emit previewFinished(token,false,false,QStringLiteral("The preview temperature or native owner is invalid."));return;}
    QPointer<KdeNightLightBackend> safe(this);
    if(m_previewAwaitingReply){const auto previous=m_previewToken;retireLocalPreview(QStringLiteral("A newer preview request retired the previous unsent local work. An already sent request may still execute."),false,true);if(!safe)return;if(m_phase!=Phase::Idle||m_previewGeneration!=generation||m_previewToken!=previous||m_previewAwaitingReply){emit previewFinished(token,false,false,QStringLiteral("A newer reentrant preview operation replaced this request before dispatch."));return;}}
    m_previewToken=token;m_previewEpoch=expectedEpoch;m_previewOwner=expectedOwner;m_previewMayBeActive=true;m_previewReplyRetired=false;m_previewAwaitingReply=true;m_previewAwaitingStop=false;m_previewExpiry.start();m_service->preview(token,temperature,expectedOwner,expectedEpoch);
}
void KdeNightLightBackend::stopPreview(quint64 token,const QString &expectedOwner,quint64 expectedEpoch)
{
    const auto phase=m_phase;const auto generation=++m_previewGeneration;
    if(!m_previewMayBeActive){emit previewFinished(token,!m_previewLeaseUncertain,true,m_previewLeaseUncertain?QStringLiteral("The previous local preview lease was retired. Unsent work cannot dispatch, but remote expiry or cleanup has not been proved; no replacement provider was stopped."):QString());return;}
    if(expectedOwner!=m_previewOwner||expectedEpoch!=m_previewEpoch){emit previewFinished(token,false,true,QStringLiteral("This stop request does not match the owned preview lease."));return;}
    const auto previous=m_previewToken;QPointer<KdeNightLightBackend> safe(this);retireLocalPreview(QStringLiteral("Explicit Stop retired unsent local preview work. An already sent preview request may still execute; the Stop result is checked separately."),false,true);if(!safe)return;
    if(m_phase!=phase||m_previewGeneration!=generation||m_previewToken!=previous||m_previewAwaitingReply){emit previewFinished(token,false,true,QStringLiteral("A newer reentrant preview operation replaced this Stop request before dispatch."));return;}
    if(!m_previewMayBeActive){emit previewFinished(token,false,true,QStringLiteral("Unsent local preview work was retired. No global Stop was sent, and remote cleanup has not been proved."));return;}
    m_previewToken=token;m_previewReplyRetired=false;m_previewAwaitingReply=true;m_previewAwaitingStop=true;m_service->stopPreview(token,expectedOwner,expectedEpoch);
}
#include "NightLightBackend.moc"
