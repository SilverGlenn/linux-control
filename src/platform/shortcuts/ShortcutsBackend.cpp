#include "ShortcutsBackend.h"
#include <KConfig>
#include <KConfigGroup>
#include <KGlobalAccel>
#include <KGlobalShortcutInfo>
#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QDBusArgument>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusServiceWatcher>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QIODevice>
#include <QPointer>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QXmlStreamReader>
#include <algorithm>
#include <deque>

namespace {
const QString service=QStringLiteral("org.kde.kglobalaccel");
const QString rootPath=QStringLiteral("/kglobalaccel");
const QString rootInterface=QStringLiteral("org.kde.KGlobalAccel");
const QString componentInterface=QStringLiteral("org.kde.kglobalaccel.Component");
const QString busInterface=QStringLiteral("org.freedesktop.DBus");
const QString busPath=QStringLiteral("/org/freedesktop/DBus");
const QString introspectionInterface=QStringLiteral("org.freedesktop.DBus.Introspectable");
const QString infosSignature=QStringLiteral("a(ssssssaiai)");
const QString keysSignature=QStringLiteral("a(ai)");
const QString layoutComponent=QStringLiteral("KDE Keyboard Layout Switcher");
const QString layoutPrefix=QStringLiteral("Switch keyboard layout to ");
const QString kwinService=QStringLiteral("org.kde.KWin");
const QString keyboardService=QStringLiteral("org.kde.keyboard");
const QString layoutsInterface=QStringLiteral("org.kde.KeyboardLayouts");
const QString layoutsPath=QStringLiteral("/Layouts");
QByteArray layoutConfigRevision(const ShortcutsBackendOptions &options)
{
    QStringList paths;
    if(QFileInfo(options.layoutConfig).isAbsolute())paths<<options.layoutConfig;
    else for(const auto &root:QStandardPaths::standardLocations(QStandardPaths::GenericConfigLocation))paths<<root+QLatin1Char('/')+options.layoutConfig;
    QByteArray bytes;QDataStream stream(&bytes,QIODevice::WriteOnly);
    for(const auto &path:paths){QFile file(path);stream<<path;if(file.open(QIODevice::ReadOnly))stream<<file.readAll();else stream<<QByteArray();}
    return QCryptographicHash::hash(bytes,QCryptographicHash::Sha256);
}
bool layoutActionId(const ShortcutActionId &id)
{ return id.component==layoutComponent && id.context==QLatin1String("default") && id.action.startsWith(layoutPrefix); }

QString configPath(const ShortcutsBackendOptions &options)
{ return QFileInfo(options.config).isAbsolute()?options.config:QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)+QLatin1Char('/')+options.config; }
KConfig::OpenFlags configFlags(const ShortcutsBackendOptions &options)
{ Q_UNUSED(options); return KConfig::SimpleConfig; }
bool serviceAction(const ShortcutActionId &id) { return id.component.endsWith(QLatin1String(".desktop")); }
QStringList actionPath(const ShortcutActionId &id)
{
    QStringList path;if(serviceAction(id))path<<QStringLiteral("services");path<<id.component;
    if(id.context!=QLatin1String("default"))path<<id.context;return path;
}
KConfigGroup actionGroup(KConfig &config,const ShortcutActionId &id)
{
    const auto path=actionPath(id);KConfigGroup group(&config,path.first());
    for(int i=1;i<path.size();++i)group=group.group(path[i]);return group;
}
QList<QKeySequence> nonempty(const QList<QKeySequence> &keys)
{ QList<QKeySequence> result; for (const auto &key : keys) if (!key.isEmpty()) result<<key; return result; }
QList<QKeySequence> parsedKeys(const QString &text)
{
    QList<QKeySequence> result;
    if (text==QLatin1String("none") || text.isEmpty()) return result;
    for (const auto &part : text.split(QLatin1Char('\t'))) result<<QKeySequence::fromString(part,QKeySequence::PortableText);
    return result;
}
using DiskEntries=QMap<QStringList,QMap<QString,QString>>;
DiskEntries diskEntries(const ShortcutsBackendOptions &options)
{
    KConfig config(options.config,configFlags(options));DiskEntries result;
    std::function<void(const KConfigGroup &,const QStringList &)> visit;
    visit=[&](const KConfigGroup &group,const QStringList &path){
        if(!group.keyList().isEmpty())result.insert(path,group.entryMap());
        for(const auto &child:group.groupList())visit(group.group(child),path+QStringList{child});
    };
    for(const auto &group:config.groupList())visit(config.group(group),{group});
    return result;
}
QByteArray configRevision(const ShortcutsBackendOptions &options)
{
    QStringList paths{configPath(options)};
    paths.removeDuplicates();QByteArray bytes;QDataStream stream(&bytes,QIODevice::WriteOnly);
    for(const auto &path:paths){QFile file(path);stream<<path;if(file.open(QIODevice::ReadOnly))stream<<file.readAll();else stream<<QByteArray();}
    return QCryptographicHash::hash(bytes,QCryptographicHash::Sha256);
}
QByteArray registryRevision(ShortcutsSnapshot snapshot)
{ snapshot.configRevision.clear();return ShortcutsTypes::revision(snapshot); }
bool signatureMatches(const QString &source,const QString &interface,const QMap<QString,QStringList> &required)
{
    QXmlStreamReader xml(source); bool inside=false,method=false; QString name; QStringList args; QSet<QString> found;
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement() && xml.name()==QLatin1String("interface")) inside=xml.attributes().value(QStringLiteral("name"))==interface;
        if (inside && xml.isStartElement() && xml.name()==QLatin1String("method")) {method=true;name=xml.attributes().value(QStringLiteral("name")).toString();args.clear();}
        if (inside && method && xml.isStartElement() && xml.name()==QLatin1String("arg")) {
            const auto direction=xml.attributes().value(QStringLiteral("direction")).toString();
            args<<(direction==QLatin1String("out")?QStringLiteral("out:"):QStringLiteral("in:"))+xml.attributes().value(QStringLiteral("type")).toString();
        }
        if (xml.isEndElement() && xml.name()==QLatin1String("method") && method) {if(required.value(name)==args && required.contains(name))found.insert(name);method=false;}
        if (xml.isEndElement() && xml.name()==QLatin1String("interface")) inside=false;
    }
    return !xml.hasError() && found.size()==required.size();
}
bool layoutReloadSignal(const QString &source)
{
    QXmlStreamReader xml(source);bool inside=false,signal=false,found=false;int arguments=0;
    while(!xml.atEnd()){
        xml.readNext();
        if(xml.isStartElement() && xml.name()==QLatin1String("interface"))inside=xml.attributes().value(QStringLiteral("name"))==layoutsInterface;
        if(inside && xml.isStartElement() && xml.name()==QLatin1String("signal")){signal=xml.attributes().value(QStringLiteral("name"))==QLatin1String("layoutListChanged");arguments=0;}
        if(signal && xml.isStartElement() && xml.name()==QLatin1String("arg"))++arguments;
        if(xml.isEndElement() && xml.name()==QLatin1String("signal")){if(signal && !arguments)found=true;signal=false;}
        if(xml.isEndElement() && xml.name()==QLatin1String("interface"))inside=false;
    }
    return found&&!xml.hasError();
}
const QMap<QString,QStringList> rootMethods={
    {QStringLiteral("allComponents"),{QStringLiteral("out:ao")}},
    {QStringLiteral("shortcutKeys"),{QStringLiteral("out:a(ai)"),QStringLiteral("in:as")}},
    {QStringLiteral("defaultShortcutKeys"),{QStringLiteral("out:a(ai)"),QStringLiteral("in:as")}},
    {QStringLiteral("setForeignShortcutKeys"),{QStringLiteral("in:as"),QStringLiteral("in:a(ai)")}},
    {QStringLiteral("globalShortcutsByKey"),{QStringLiteral("out:a(ssssssaiai)"),QStringLiteral("in:(ai)"),QStringLiteral("in:(i)")}},
};
const QMap<QString,QStringList> componentMethods={
    {QStringLiteral("getShortcutContexts"),{QStringLiteral("out:as")}},
    {QStringLiteral("allShortcutInfos"),{QStringLiteral("out:a(ssssssaiai)"),QStringLiteral("in:s")}},
};
class DBusShortcutsTransport final : public ShortcutsTransport {
    Q_OBJECT
public:
    DBusShortcutsTransport(const QDBusConnection &bus,QObject *parent):ShortcutsTransport(parent),m_bus(bus)
    {
        qDBusRegisterMetaType<QKeySequence>();qDBusRegisterMetaType<QList<QKeySequence>>();
        qDBusRegisterMetaType<KGlobalAccel::MatchType>();
        qDBusRegisterMetaType<QByteArrayList>();qDBusRegisterMetaType<QHash<QString,QByteArrayList>>();
        auto *watcher=new QDBusServiceWatcher(service,bus,QDBusServiceWatcher::WatchForOwnerChange,this);
        watcher->addWatchedService(kwinService);watcher->addWatchedService(keyboardService);
        connect(watcher,&QDBusServiceWatcher::serviceOwnerChanged,this,[this]{emit changed();});
        m_bus.connect(keyboardService,layoutsPath,layoutsInterface,QStringLiteral("layoutListChanged"),this,SLOT(nativeChanged()));
        m_bus.connect(service,rootPath,rootInterface,QStringLiteral("yourShortcutsChanged"),this,SLOT(nativeChanged()));
    }
    void call(const QString &destination,const QString &path,const QString &interface,const QString &method,
              const QVariantList &arguments,Completion completion) override
    {
        auto message=QDBusMessage::createMethodCall(destination,path,interface,method); message.setArguments(arguments);
        auto *watcher=new QDBusPendingCallWatcher(m_bus.asyncCall(message,2000),this);
        auto done=std::make_shared<bool>(false);
        auto deliver=[watcher,done,method,completion=std::move(completion)] {
            if(*done)return;*done=true;
            const auto message=watcher->reply();watcher->deleteLater();ShortcutsReply reply;
            reply.success=message.type()==QDBusMessage::ReplyMessage;
            reply.error=message.errorName()+QStringLiteral(": ")+message.errorMessage();
            reply.uncertain=!reply.success && (method==QLatin1String("setForeignShortcutKeys")||method==QLatin1String("doRegister"));
            if(!reply.success){completion(reply);return;}
            const int expected=(method==QLatin1String("setForeignShortcutKeys")||method==QLatin1String("doRegister"))?0:1;
            if(message.arguments().size()!=expected){reply.success=false;reply.error=QStringLiteral("Unexpected D-Bus reply arity.");reply.uncertain=expected==0;completion(reply);return;}
            if(expected==0){completion(reply);return;}
            const auto value=message.arguments().first();
            const bool structured=method==QLatin1String("allComponents")||method==QLatin1String("allShortcutInfos")||method==QLatin1String("globalShortcutsByKey")||method.endsWith(QLatin1String("ShortcutKeys"))||method==QLatin1String("shortcutKeys")||method==QLatin1String("getLayoutsList");
            if(structured) {
                if(value.metaType()!=QMetaType::fromType<QDBusArgument>()){reply.success=false;}
                else {
                    const auto arg=qvariant_cast<QDBusArgument>(value);
                    const auto signature=arg.currentSignature();
                    if(method==QLatin1String("getLayoutsList") && signature==QLatin1String("a(sss)")){
                        QVariantList layouts;arg.beginArray();
                        while(!arg.atEnd() && layouts.size()<64){QString shortName,displayName,longName;arg.beginStructure();arg>>shortName>>displayName>>longName;arg.endStructure();layouts<<QVariantMap{{QStringLiteral("shortName"),shortName},{QStringLiteral("displayName"),displayName},{QStringLiteral("longName"),longName}};}
                        reply.success=arg.atEnd();arg.endArray();reply.value=layouts;
                    } else if(method==QLatin1String("allComponents") && signature==QLatin1String("ao")) {
                        QStringList paths;arg.beginArray();
                        while(!arg.atEnd() && paths.size()<4096){QDBusObjectPath path;arg>>path;paths<<path.path();}
                        reply.success=arg.atEnd();arg.endArray();reply.value=paths;
                    } else if((method==QLatin1String("allShortcutInfos")||method==QLatin1String("globalShortcutsByKey")) && signature==infosSignature) {
                        QList<ShortcutAction> actions;arg.beginArray();
                        while(!arg.atEnd() && actions.size()<20000){
                            ShortcutAction action;arg.beginStructure();
                            arg>>action.id.action>>action.friendlyName>>action.id.component>>action.componentFriendly>>action.id.context>>action.contextFriendly;
                            for(int which=0;which<2;++which){arg.beginArray();int count=0;while(!arg.atEnd() && count<4096){int key;arg>>key;(which?action.defaults:action.current)<<QKeySequence(key);++count;}if(!arg.atEnd())reply.success=false;arg.endArray();}
                            arg.endStructure();actions<<action;
                        }
                        reply.success=reply.success&&arg.atEnd();arg.endArray();reply.value=QVariant::fromValue(actions);
                    } else if(signature==keysSignature) {
                        QList<QKeySequence> keys;arg.beginArray();
                        while(!arg.atEnd() && keys.size()<4096){int chord[4]={};int count=0;arg.beginStructure();arg.beginArray();while(!arg.atEnd()&&count<4){arg>>chord[count];++count;}if(count!=4 || !arg.atEnd())reply.success=false;arg.endArray();arg.endStructure();keys<<QKeySequence(chord[0],chord[1],chord[2],chord[3]);}
                        reply.success=reply.success&&arg.atEnd();arg.endArray();reply.value=QVariant::fromValue(keys);
                    } else reply.success=false;
                }
            } else if(method==QLatin1String("getShortcutContexts")) {
                reply.success=value.metaType()==QMetaType::fromType<QStringList>();reply.value=value;
            } else {reply.success=value.metaType()==QMetaType::fromType<QString>();reply.value=value;}
            if(!reply.success)reply.error=QStringLiteral("Unexpected or oversized D-Bus reply signature.");
            completion(reply);
        };
        connect(watcher,&QDBusPendingCallWatcher::finished,this,deliver);
        if(watcher->isFinished())QTimer::singleShot(0,this,deliver);
    }
    void reloadLayouts(const QString &owner,Completion completion) override
    {
        if(m_reload){ShortcutsReply reply;reply.error=QStringLiteral("A layout reload is already pending.");completion(reply);return;}
        m_reload=std::move(completion);m_reloadOwner=owner;m_reloadSent=false;
        if(!m_bus.connect(owner,layoutsPath,layoutsInterface,QStringLiteral("layoutListChanged"),this,SLOT(layoutReloaded(QDBusMessage)))){
            finishReload(false,false,QStringLiteral("Cannot subscribe to the pinned layout reload acknowledgement."));return;
        }
        m_reloadTimer=new QTimer(this);m_reloadTimer->setSingleShot(true);
        connect(m_reloadTimer,&QTimer::timeout,this,[this]{finishReload(false,m_reloadSent,QStringLiteral("The pinned KWin layout reload acknowledgement timed out."));});
        auto signal=QDBusMessage::createTargetedSignal(owner,QStringLiteral("/kxkbrc"),QStringLiteral("org.kde.kconfig.notify"),QStringLiteral("ConfigChanged"));
        signal<<QVariant::fromValue(QHash<QString,QByteArrayList>{{QStringLiteral("Layout"),{QByteArrayLiteral("LayoutList")}}});
        m_reloadSent=m_bus.send(signal);
        if(!m_reloadSent){finishReload(false,false,QStringLiteral("The targeted native layout reload notification could not be sent."));return;}
        m_reloadTimer->start(2000);
    }
private slots:
    void nativeChanged(){emit changed();}
    void layoutReloaded(const QDBusMessage &message){if(m_reload && m_reloadSent && message.service()==m_reloadOwner && message.arguments().isEmpty())finishReload(true,false,{});}
private:
    void finishReload(bool success,bool uncertain,const QString &error){
        if(!m_reload)return;auto completion=std::move(m_reload);m_reload={};
        m_bus.disconnect(m_reloadOwner,layoutsPath,layoutsInterface,QStringLiteral("layoutListChanged"),this,SLOT(layoutReloaded(QDBusMessage)));
        if(m_reloadTimer){m_reloadTimer->stop();m_reloadTimer->deleteLater();m_reloadTimer=nullptr;}
        ShortcutsReply reply;reply.success=success;reply.uncertain=uncertain;reply.error=error;completion(reply);
    }
    QDBusConnection m_bus;
    Completion m_reload;
    QString m_reloadOwner;
    QTimer *m_reloadTimer=nullptr;
    bool m_reloadSent=false;
};
struct ReadJob {
    ShortcutsSnapshot snapshot;
    std::function<void(bool,const ShortcutsSnapshot &,const QString &)> completion;
    QTimer *timer=nullptr;
    bool done=false;
    int pending=0;
    QStringList paths;
    QMap<QString,QStringList> topology;
    std::deque<std::function<void()>> tasks;
};
}
struct ShortcutsWriteJob {
    quint64 token=0;
    ShortcutsSnapshot expected,current;
    ShortcutChanges requested;
    QList<QPair<ShortcutActionId,QList<QKeySequence>>> operations;
    QList<QPair<ShortcutActionId,QKeySequence>> checks;
    int position=0,checkPosition=0,matchType=0,written=0,removalCount=0;
    bool phaseChecked=false;
    DiskEntries baselineDisk;
    QByteArray lastDisk;
    bool done=false,outstanding=false,layoutChanged=false,reloaded=false;
    QTimer *timer=nullptr;
    QElapsedTimer persistence;
};
ShortcutsTransport *createShortcutsDBusTransport(const QDBusConnection &connection,QObject *parent)
{ return new DBusShortcutsTransport(connection,parent); }
void ShortcutsTransport::reloadLayouts(const QString &,Completion completion)
{ ShortcutsReply reply;reply.error=QStringLiteral("Layout reload is unavailable in this transport.");completion(reply); }
KdeShortcutsBackend::KdeShortcutsBackend(QObject *parent)
    :KdeShortcutsBackend(createShortcutsDBusTransport(QDBusConnection::sessionBus()),[]{ShortcutsBackendOptions options;options.enableLayoutAdapter=true;return options;}(),parent){}
KdeShortcutsBackend::KdeShortcutsBackend(ShortcutsTransport *transport,const ShortcutsBackendOptions &options,QObject *parent)
    :ShortcutsBackend(parent),m_transport(transport),m_options(options)
{
    if(!transport->parent())transport->setParent(this);
    qRegisterMetaType<ShortcutsSnapshot>();qRegisterMetaType<ShortcutsOutcome>();
    m_debounce.setSingleShot(true);m_debounce.setInterval(150);
    connect(&m_debounce,&QTimer::timeout,this,[this]{watchFiles();emit externalChange();});
    connect(&m_watcher,&QFileSystemWatcher::fileChanged,this,[this]{m_debounce.start();});
    connect(&m_watcher,&QFileSystemWatcher::directoryChanged,this,[this]{m_debounce.start();});
    connect(transport,&ShortcutsTransport::changed,this,[this]{m_debounce.start();});
    watchFiles();
}
void KdeShortcutsBackend::watchFiles()
{
    QStringList paths{configPath(m_options)};
    if(m_options.enableLayoutAdapter){if(QFileInfo(m_options.layoutConfig).isAbsolute())paths<<m_options.layoutConfig;else for(const auto &root:QStandardPaths::standardLocations(QStandardPaths::GenericConfigLocation))paths<<root+QLatin1Char('/')+m_options.layoutConfig;}
    for(const auto &path:paths){
        if(QFileInfo::exists(path)&&!m_watcher.files().contains(path))m_watcher.addPath(path);
        const auto parent=QFileInfo(path).absolutePath();
        if(QFileInfo::exists(parent)&&!m_watcher.directories().contains(parent))m_watcher.addPath(parent);
    }
}
void KdeShortcutsBackend::readLayoutAdapter(const QString &rootXml,std::function<void(const ShortcutsLayoutAdapter &)> completion)
{
    struct State {ShortcutsLayoutAdapter value;QTimer *timer=nullptr;bool done=false;};
    auto state=std::make_shared<State>();state->timer=new QTimer(this);state->timer->setSingleShot(true);
    auto finish=[state,completion=std::move(completion)](const QString &error){
        if(state->done)return;state->done=true;state->timer->stop();state->timer->deleteLater();
        state->value.available=error.isEmpty();state->value.reason=error;completion(state->value);
    };
    connect(state->timer,&QTimer::timeout,this,[finish]{finish(QObject::tr("The KWin layout capability read timed out."));});state->timer->start(qMin(m_options.readBudgetMs,10000));
    if(!m_options.enableLayoutAdapter){finish(tr("The optional KWin layout shortcut adapter is disabled."));return;}
    if(m_options.isolated && !QFileInfo(m_options.layoutConfig).isAbsolute()){finish(tr("Isolated layout capability requires an absolute kxkbrc path."));return;}
    const int end=rootXml.lastIndexOf(QStringLiteral("</node>"));const auto xml=end<0?rootXml:rootXml.left(end+7);
    if(!signatureMatches(xml,rootInterface,{{QStringLiteral("doRegister"),{QStringLiteral("in:as")}}})){finish(tr("The shortcut daemon does not expose the audited configuration-action registration protocol."));return;}
    state->value.configRevision=layoutConfigRevision(m_options);
    m_transport->call(busInterface,busPath,busInterface,QStringLiteral("GetNameOwner"),{kwinService},[this,state,finish](const auto &reply){
        if(state->done)return;
        if(!reply.success || reply.value.metaType()!=QMetaType::fromType<QString>() || !reply.value.toString().startsWith(QLatin1Char(':'))){finish(tr("The KWin compositor owner is unavailable."));return;}
        state->value.owner=reply.value.toString();
        m_transport->call(busInterface,busPath,busInterface,QStringLiteral("GetNameOwner"),{keyboardService},[this,state,finish](const auto &reply){
            if(state->done)return;
            if(!reply.success || reply.value.toString()!=state->value.owner){finish(tr("The keyboard-layout service is not owned by the verified KWin compositor."));return;}
            m_transport->call(state->value.owner,QStringLiteral("/KWin"),introspectionInterface,QStringLiteral("Introspect"),{},[this,state,finish](const auto &reply){
                if(state->done)return;
                if(!reply.success || !signatureMatches(reply.value.toString(),kwinService,{{QStringLiteral("supportInformation"),{QStringLiteral("out:s")}}})){finish(tr("KWin does not expose the audited version interface."));return;}
                m_transport->call(state->value.owner,QStringLiteral("/KWin"),kwinService,QStringLiteral("supportInformation"),{},[this,state,finish](const auto &reply){
                    if(state->done)return;
                    const auto match=QRegularExpression(QStringLiteral("^KWin version: ([^\\s]+)\\s*$"),QRegularExpression::MultilineOption).match(reply.value.toString());
                    if(!reply.success || !match.hasMatch()){finish(tr("The KWin version could not be verified."));return;}
                    state->value.version=match.captured(1);
                    if(state->value.version!=QLatin1String("6.6.6")){finish(tr("Per-layout shortcut creation and activation is audited for KWin 6.6.6; this compositor version is read-only for that adapter."));return;}
                    m_transport->call(state->value.owner,layoutsPath,introspectionInterface,QStringLiteral("Introspect"),{},[this,state,finish](const auto &reply){
                        if(state->done)return;
                        if(!reply.success || !signatureMatches(reply.value.toString(),layoutsInterface,{{QStringLiteral("getLayoutsList"),{QStringLiteral("out:a(sss)")}}}) || !layoutReloadSignal(reply.value.toString())){finish(tr("KWin does not expose the audited typed layout list and reload acknowledgement."));return;}
                        m_transport->call(state->value.owner,layoutsPath,layoutsInterface,QStringLiteral("getLayoutsList"),{},[this,state,finish](const auto &reply){
                            if(state->done)return;
                            if(!reply.success || reply.value.metaType()!=QMetaType::fromType<QVariantList>() || reply.value.toList().isEmpty() || reply.value.toList().size()>64){finish(tr("The runtime keyboard-layout list is unavailable or invalid."));return;}
                            QByteArray bytes;QDataStream stream(&bytes,QIODevice::WriteOnly);stream<<state->value.owner<<state->value.version<<state->value.configRevision;
                            for(const auto &row:reply.value.toList()){
                                if(row.metaType()!=QMetaType::fromType<QVariantMap>()){finish(tr("Invalid typed keyboard-layout metadata."));return;}
                                const auto map=row.toMap();const auto shortName=map.value(QStringLiteral("shortName")),displayName=map.value(QStringLiteral("displayName")),longName=map.value(QStringLiteral("longName"));
                                if(shortName.metaType()!=QMetaType::fromType<QString>() || displayName.metaType()!=QMetaType::fromType<QString>() || longName.metaType()!=QMetaType::fromType<QString>() || shortName.toString().isEmpty() || longName.toString().isEmpty() || longName.toString().size()>4096){finish(tr("A runtime layout has no verified translated name."));return;}
                                const auto name=layoutPrefix+longName.toString();
                                if(state->value.actionNames.contains(name)){finish(tr("Runtime layouts have duplicate translated shortcut identities. Resolve the duplicate layouts before editing their shortcuts."));return;}
                                state->value.actionNames<<name;stream<<shortName.toString()<<displayName.toString()<<longName.toString();
                            }
                            state->value.revision=QCryptographicHash::hash(bytes,QCryptographicHash::Sha256);
                            m_transport->call(busInterface,busPath,busInterface,QStringLiteral("GetNameOwner"),{kwinService},[this,state,finish](const auto &reply){
                                if(state->done)return;
                                if(!reply.success || reply.value.toString()!=state->value.owner || layoutConfigRevision(m_options)!=state->value.configRevision){finish(tr("The compositor or keyboard configuration changed while reading layout capability."));return;}
                                m_transport->call(busInterface,busPath,busInterface,QStringLiteral("GetNameOwner"),{keyboardService},[state,finish](const auto &reply){if(state->done)return;if(!reply.success || reply.value.toString()!=state->value.owner)finish(QObject::tr("The keyboard-layout owner changed while reading."));else finish({});});
                            });
                        });
                    });
                });
            });
        });
    });
}
void KdeShortcutsBackend::guardLayoutState(const std::shared_ptr<ShortcutsWriteJob> &job,std::function<void(bool,const QString &)> completion)
{
    if(!job->layoutChanged){completion(true,{});return;}
    readLayoutAdapter(job->expected.interfaceSignature,[job,completion=std::move(completion)](const auto &state){
        if(job->done)return;
        const bool valid=state.available && state==job->expected.layouts;
        completion(valid,valid?QString{}:QObject::tr("The verified KWin owner, version, runtime layouts, or keyboard configuration changed. Refresh before applying layout shortcuts."));
    });
}
void KdeShortcutsBackend::snapshot(Completion completion)
{
    auto job=std::make_shared<ReadJob>();job->completion=std::move(completion);
    if(m_options.isolated && !QFileInfo(m_options.config).isAbsolute()) {job->completion(false,{},tr("Isolated shortcut configuration requires an absolute path."));return;}
    ++m_reads;job->timer=new QTimer(this);job->timer->setSingleShot(true);
    auto finish=[this,job](bool success,const QString &error) {
        if(job->done)return;job->done=true;job->tasks.clear();job->timer->stop();job->timer->deleteLater();--m_reads;
        if(success) {
            KConfig config(m_options.config,configFlags(m_options));
            const bool writable=config.isConfigWritable(false);
            for(auto &action:job->snapshot.actions) {
                const auto group=actionGroup(config,action.id);
                action.available=writable && !group.isImmutable() && !group.isEntryImmutable(action.id.action.toUtf8().constData());
            }
            const ShortcutAction *component=nullptr;
            for(const auto &a:job->snapshot.actions)if(a.id.component==layoutComponent && a.id.context==QLatin1String("default")){component=&a;break;}
            QString componentFriendly,contextFriendly;
            if(component){componentFriendly=component->componentFriendly;contextFriendly=component->contextFriendly;}
            else if(job->snapshot.layouts.available){job->snapshot.layouts.available=false;job->snapshot.layouts.reason=tr("The native keyboard-layout component has not registered its default context yet.");}
            for(auto &a:job->snapshot.actions)if(layoutActionId(a.id)){a.layoutAction=true;a.available=a.available && job->snapshot.layouts.available && job->snapshot.layouts.actionNames.contains(a.id.action);}
            if(job->snapshot.layouts.available)for(const auto &name:job->snapshot.layouts.actionNames){
                const ShortcutActionId id{layoutComponent,QStringLiteral("default"),name};
                if(!job->snapshot.find(id)){
                    ShortcutAction a;a.id=id;a.componentFriendly=componentFriendly;a.contextFriendly=contextFriendly;a.registered=false;a.layoutAction=true;
                    const auto group=actionGroup(config,id);a.available=writable && !group.isImmutable() && !group.isEntryImmutable(id.action.toUtf8().constData());
                    job->snapshot.actions<<a;
                }
            }
            std::sort(job->snapshot.actions.begin(),job->snapshot.actions.end(),[](const auto &a,const auto &b){return a.id<b.id;});
            for(auto it=job->topology.cbegin();it!=job->topology.cend();++it)job->snapshot.interfaceSignature+=QLatin1Char('\n')+it.key()+QLatin1Char(':')+it.value().join(QLatin1Char('|'));
            job->snapshot.configRevision=configRevision(m_options);
            job->snapshot.available=true;
            job->snapshot.writable=writable && job->snapshot.owner!=m_uncertainOwner;
            if(!writable)job->snapshot.reason=tr("The native shortcut configuration is read-only.");
            else if(!m_uncertainOwner.isEmpty() && job->snapshot.owner==m_uncertainOwner)
                job->snapshot.reason=tr("A shortcut write has an uncertain outcome for this daemon owner. Start a new desktop session before applying again.");
            job->snapshot.revision=ShortcutsTypes::revision(job->snapshot);
        } else job->snapshot.reason=error;
        job->completion(success,job->snapshot,error);
    };
    connect(job->timer,&QTimer::timeout,this,[finish]{finish(false,QObject::tr("Reading the native shortcut registry timed out."));});
    job->timer->start(m_options.readBudgetMs);
    using Handler=std::function<void(const ShortcutsReply &)>;
    using Enqueue=std::function<void(const QString &,const QString &,const QString &,const QString &,const QVariantList &,Handler)>;
    auto enqueue=std::make_shared<Enqueue>();auto pump=std::make_shared<std::function<void()>>();
    *pump=[this,job,finish] {
        if(job->done)return;
        while(job->pending<8 && !job->tasks.empty()) {auto task=std::move(job->tasks.front());job->tasks.pop_front();++job->pending;task();}
        if(job->pending || !job->tasks.empty())return;
        // End-of-enumeration owner check prevents a restarted service from being adopted mid-read.
        ++job->pending;
        m_transport->call(busInterface,busPath,busInterface,QStringLiteral("GetNameOwner"),{service},[this,job,finish](const auto &reply){
            if(job->done)return;
            if(!reply.success || reply.value.metaType()!=QMetaType::fromType<QString>() || reply.value.toString()!=job->snapshot.owner)
                finish(false,QObject::tr("The shortcut daemon changed while reading. Refresh and retry."));
            else {
                readLayoutAdapter(job->snapshot.interfaceSignature,[this,job,finish](const auto &layouts){
                    if(job->done)return;
                    m_transport->call(busInterface,busPath,busInterface,QStringLiteral("GetNameOwner"),{service},[job,finish,layouts](const auto &owner){if(job->done)return;if(!owner.success || owner.value.toString()!=job->snapshot.owner)finish(false,QObject::tr("The shortcut daemon changed during layout capability discovery."));else {job->snapshot.layouts=layouts;finish(true,{});}});
                });
            }
        });
    };
    *enqueue=[this,job,finish,pump](const QString &destination,const QString &path,const QString &interface,const QString &method,const QVariantList &arguments,Handler handler){
        if(job->done)return;
        job->tasks.emplace_back([this,job,finish,pump,destination,path,interface,method,arguments,handler=std::move(handler)]{
            if(job->done)return;
            m_transport->call(destination,path,interface,method,arguments,[job,finish,pump,method,handler](const auto &reply){
                if(job->done)return;
                if(!reply.success){finish(false,QObject::tr("Native shortcut call %1 failed: %2").arg(method,reply.error));return;}
                handler(reply);--job->pending;(*pump)();
            });
        });
    };
    (*enqueue)(busInterface,busPath,busInterface,QStringLiteral("GetNameOwner"),{service},[job,finish,enqueue](const auto &reply){
        if(reply.value.metaType()!=QMetaType::fromType<QString>() || !reply.value.toString().startsWith(QLatin1Char(':'))){finish(false,QObject::tr("Invalid native shortcut daemon owner."));return;}
        job->snapshot.owner=reply.value.toString();
        (*enqueue)(job->snapshot.owner,rootPath,introspectionInterface,QStringLiteral("Introspect"),{},[job,finish,enqueue](const auto &reply){
            if(!signatureMatches(reply.value.toString(),rootInterface,rootMethods)){finish(false,QObject::tr("The shortcut daemon exposes an unsupported interface."));return;}
            job->snapshot.interfaceSignature=reply.value.toString();
            (*enqueue)(job->snapshot.owner,rootPath,rootInterface,QStringLiteral("allComponents"),{},[job,finish,enqueue](const auto &reply){
                if(reply.value.metaType()!=QMetaType::fromType<QStringList>()){finish(false,QObject::tr("Invalid native shortcut component list."));return;}
                job->paths=reply.value.toStringList();
                QSet<QString> unique;
                for(const auto &path:job->paths) {
                    if(path==QLatin1String("/") || !path.startsWith(QLatin1String("/component/")) || unique.contains(path)){finish(false,QObject::tr("Invalid or duplicate shortcut component path."));return;}
                    unique.insert(path);
                    (*enqueue)(job->snapshot.owner,path,introspectionInterface,QStringLiteral("Introspect"),{},[job,finish,enqueue,path](const auto &reply){
                        if(!signatureMatches(reply.value.toString(),componentInterface,componentMethods)){finish(false,QObject::tr("A shortcut component exposes an unsupported interface."));return;}
                        (*enqueue)(job->snapshot.owner,path,componentInterface,QStringLiteral("getShortcutContexts"),{},[job,finish,enqueue,path](const auto &reply){
                            if(reply.value.metaType()!=QMetaType::fromType<QStringList>() || reply.value.toStringList().size()>4096){finish(false,QObject::tr("Invalid shortcut context list."));return;}
                            auto contexts=reply.value.toStringList();contexts.sort();job->topology.insert(path,contexts);QSet<QString> seen;
                            for(const auto &context:contexts){
                                if(context.isEmpty() || context.contains(QLatin1Char('|')) || seen.contains(context)){finish(false,QObject::tr("Invalid or duplicate shortcut context."));return;}seen.insert(context);
                                (*enqueue)(job->snapshot.owner,path,componentInterface,QStringLiteral("allShortcutInfos"),{context},[job,finish,enqueue,context](const auto &reply){
                                    if(reply.value.metaType()!=QMetaType::fromType<QList<ShortcutAction>>()){finish(false,QObject::tr("Invalid shortcut action list."));return;}
                                    const auto actions=qvariant_cast<QList<ShortcutAction>>(reply.value);
                                    for(auto action:actions){
                                        if(job->snapshot.actions.size()>=20000 || action.id.component.isEmpty() || action.id.component.contains(QLatin1Char('|')) || action.id.context!=context || action.id.action.isEmpty() || job->snapshot.find(action.id)) {finish(false,QObject::tr("Invalid, duplicate, or oversized shortcut action registry."));return;}
                                        const int index=job->snapshot.actions.size();job->snapshot.actions<<action;
                                        // allShortcutInfos has a legacy ai payload which loses all but the first chord.
                                        // The v2 getters are mandatory for both current and default alternatives.
                                        for(const bool defaults:{false,true}) (*enqueue)(job->snapshot.owner,rootPath,rootInterface,defaults?QStringLiteral("defaultShortcutKeys"):QStringLiteral("shortcutKeys"),{action.id.nativeId(action.componentFriendly,action.friendlyName)},[job,finish,index,defaults](const auto &reply){
                                            if(reply.value.metaType()!=QMetaType::fromType<QList<QKeySequence>>()){finish(false,QObject::tr("Invalid typed shortcut sequence list."));return;}
                                            (defaults?job->snapshot.actions[index].defaults:job->snapshot.actions[index].current)=nonempty(qvariant_cast<QList<QKeySequence>>(reply.value));
                                        });
                                    }
                                });
                            }
                        });
                    });
                }
            });
        });
    });
    (*pump)();
}
void KdeShortcutsBackend::readCurrent(quint64 token)
{
    if(m_job){emit readFinished(token,false,{},tr("A native shortcut write is already running."));return;}
    snapshot([this,token](bool success,const auto &state,const auto &error){emit readFinished(token,success,state,error);});
}
void KdeShortcutsBackend::writeChanges(quint64 token,const ShortcutsSnapshot &expected,const ShortcutChanges &requested)
{
    if(m_job || m_reads){emit writeFinished(token,ShortcutsOutcome::Failed,expected,tr("Another shortcut operation is running."));return;}
    QString error;if(!ShortcutsTypes::validate(expected,requested,error)){emit writeFinished(token,ShortcutsOutcome::Failed,expected,error);return;}
    auto job=std::make_shared<ShortcutsWriteJob>();job->token=token;job->expected=expected;job->current=expected;job->requested=requested;m_job=job;
    job->timer=new QTimer(this);job->timer->setSingleShot(true);
    connect(job->timer,&QTimer::timeout,this,[this,job]{
        finishWrite(job,job->outstanding?ShortcutsOutcome::Uncertain:job->written?ShortcutsOutcome::Partial:ShortcutsOutcome::Failed,
                    tr("The native shortcut update timed out. Refresh to inspect the result."));
    });job->timer->start(m_options.writeBudgetMs);
    snapshot([this,job](bool success,const auto &state,const auto &error){
        if(job->done)return;job->current=state;
        if(!success || !state.writable || state.revision!=job->expected.revision || state.owner!=job->expected.owner){finishWrite(job,ShortcutsOutcome::Failed,error.isEmpty()?tr("Shortcuts changed outside this editor. Refresh before applying."):error);return;}
        QString invalid;if(!ShortcutsTypes::validate(state,job->requested,invalid)){finishWrite(job,ShortcutsOutcome::Failed,invalid);return;}
        for(auto it=job->requested.cbegin();it!=job->requested.cend();++it){const auto *a=state.find(it.key());if(a->layoutAction && !ShortcutsTypes::sameKeys(a->current,it.value()))job->layoutChanged=true;}
        job->baselineDisk=diskEntries(m_options);job->lastDisk=configRevision(m_options);
        if(job->lastDisk!=state.configRevision){finishWrite(job,ShortcutsOutcome::Failed,tr("Native shortcut configuration changed during preflight."));return;}
        // A pre-existing disk/cache mismatch could otherwise be overwritten by the daemon's cached KConfig.
        KConfig saved(m_options.config,configFlags(m_options));
        for(const auto &action:state.actions){
            const auto group=actionGroup(saved,action.id);
            const auto entry=group.readEntry(action.id.action,QStringList{});
            const bool mismatch=serviceAction(action.id)
                ? !ShortcutsTypes::sameKeys(group.hasKey(action.id.action)?parsedKeys(group.readEntry(action.id.action,QString{})):action.defaults,action.current)
                : entry.size()==3 && (!ShortcutsTypes::sameKeys(parsedKeys(entry[0]),action.current)||!ShortcutsTypes::sameKeys(parsedKeys(entry[1]),action.defaults));
            if(mismatch){
                finishWrite(job,ShortcutsOutcome::Failed,tr("Saved shortcuts differ from the daemon registry. Wait for native persistence or start a new desktop session before applying."));return;
            }
        }
        // Follow Plasma KCM: release removed alternatives first, then install additions.
        for(auto it=job->requested.cbegin();it!=job->requested.cend();++it) {
            const auto *action=state.find(it.key());if(ShortcutsTypes::sameKeys(action->current,it.value()))continue;
            QList<QKeySequence> retained;for(const auto &key:action->current)if(it.value().contains(key))retained<<key;
            if(!ShortcutsTypes::sameKeys(retained,action->current))job->operations<<qMakePair(it.key(),retained);
            for(const auto &key:it.value())if(!action->current.contains(key))job->checks<<qMakePair(it.key(),key);
        }
        job->removalCount=job->operations.size();
        for(auto it=job->requested.cbegin();it!=job->requested.cend();++it) {
            const auto *action=state.find(it.key());if(ShortcutsTypes::sameKeys(action->current,it.value()))continue;
            QList<QKeySequence> retained;for(const auto &key:action->current)if(it.value().contains(key))retained<<key;
            if(!ShortcutsTypes::sameKeys(retained,it.value()))job->operations<<qMakePair(it.key(),it.value());
        }
        preflight(job);
    });
}
void KdeShortcutsBackend::preflight(const std::shared_ptr<ShortcutsWriteJob> &job)
{
    if(job->done)return;
    if(job->checkPosition>=job->checks.size()){
        guardLayoutState(job,[this,job](bool valid,const auto &error){if(!valid){finishWrite(job,ShortcutsOutcome::Failed,error);return;}nextWrite(job);});return;
    }
    const auto check=job->checks[job->checkPosition];
    m_transport->call(job->current.owner,rootPath,rootInterface,QStringLiteral("globalShortcutsByKey"),
        {QVariant::fromValue(check.second),QVariant::fromValue(static_cast<KGlobalAccel::MatchType>(job->matchType))},[this,job,check](const auto &reply){
            if(job->done)return;
            if(!reply.success || reply.value.metaType()!=QMetaType::fromType<QList<ShortcutAction>>()) {finishWrite(job,ShortcutsOutcome::Failed,tr("Native shortcut conflict detection failed: %1").arg(reply.error));return;}
            const auto collisions=qvariant_cast<QList<ShortcutAction>>(reply.value);
            for(const auto &collision:collisions){
                if(collision.id==check.first || (collision.id.component==check.first.component && collision.id.context!=check.first.context))continue;
                const auto *known=job->current.find(collision.id);
                if(!known){finishWrite(job,ShortcutsOutcome::Failed,tr("An undiscovered shortcut now owns this key. Refresh before applying."));return;}
                const auto final=job->requested.contains(collision.id)?job->requested.value(collision.id):known->current;
                for(const auto &key:final)if(ShortcutsTypes::conflicts(check.second,key)){
                    finishWrite(job,ShortcutsOutcome::Failed,tr("%1 conflicts with %2. Reassign it explicitly before applying.").arg(check.second.toString(QKeySequence::NativeText),known->friendlyName));return;
                }
            }
            if(++job->matchType==3){job->matchType=0;++job->checkPosition;}
            preflight(job);
        });
}
bool KdeShortcutsBackend::guardDisk(const std::shared_ptr<ShortcutsWriteJob> &job,QString &error)
{
    KConfig config(m_options.config,configFlags(m_options));
    if(!config.isConfigWritable(false)){error=tr("Native shortcut configuration became read-only.");return false;}
    for(auto it=job->requested.cbegin();it!=job->requested.cend();++it){
        const auto *action=job->expected.find(it.key());if(action && ShortcutsTypes::sameKeys(action->current,it.value()))continue;
        const auto group=actionGroup(config,it.key());
        if(group.isImmutable() || group.isEntryImmutable(it.key().action.toUtf8().constData())){error=tr("A shortcut action became read-only.");return false;}
    }
    const auto digest=configRevision(m_options);if(digest==job->lastDisk)return true;
    const auto actual=diskEntries(m_options);
    auto baseline=job->baselineDisk;auto remaining=actual;
    for(auto it=job->requested.cbegin();it!=job->requested.cend();++it){
        const auto path=actionPath(it.key());
        const auto old=job->baselineDisk.value(path).value(it.key().action);
        const auto now=actual.value(path).value(it.key().action);
        if(now!=old || job->baselineDisk.value(path).contains(it.key().action)!=actual.value(path).contains(it.key().action)){
            const auto group=actionGroup(config,it.key());bool allowed=false;
            const auto *action=job->expected.find(it.key());QList<QKeySequence> current;bool valid=false;
            if(serviceAction(it.key())){current=group.hasKey(it.key().action)?parsedKeys(group.readEntry(it.key().action,QString{})):action->defaults;valid=true;}
            else {const auto entry=group.readEntry(it.key().action,QStringList{});if(entry.size()==3 && ShortcutsTypes::sameKeys(parsedKeys(entry[1]),action->defaults) && entry[2]==action->friendlyName){current=parsedKeys(entry[0]);valid=true;}}
            if(valid){
                if(ShortcutsTypes::sameKeys(current,action->current))allowed=true;
                for(int i=0;i<job->position;++i)if(job->operations[i].first==it.key() && ShortcutsTypes::sameKeys(current,job->operations[i].second))allowed=true;
            }
            if(!allowed){error=tr("Native shortcut configuration changed outside this editor. Further writes were stopped.");return false;}
        }
        baseline[path].remove(it.key().action);remaining[path].remove(it.key().action);
    }
    // Service serialization removes default overrides and normalizes equivalent empty values.
    // Accept only semantically unchanged unrequested service bindings, preserving registrations.
    for(const auto &a:job->expected.actions)if(serviceAction(a.id) && !job->requested.contains(a.id)){
        const auto path=actionPath(a.id);const auto group=actionGroup(config,a.id);
        const auto current=group.hasKey(a.id.action)?parsedKeys(group.readEntry(a.id.action,QString{})):a.defaults;
        const auto original=job->baselineDisk.value(path).contains(a.id.action)?parsedKeys(job->baselineDisk.value(path).value(a.id.action)):a.defaults;
        if(ShortcutsTypes::sameKeys(current,a.current) && ShortcutsTypes::sameKeys(original,a.current)){
            baseline[path].remove(a.id.action);remaining[path].remove(a.id.action);
        }
    }
    // The daemon materializes these native labels while serializing existing registered components.
    for(const auto &a:job->expected.actions){
        if(serviceAction(a.id))continue;
        const auto path=actionPath(a.id);
        const auto now=remaining.value(path).value(QStringLiteral("_k_friendly_name"));
        if(now==(a.id.context==QLatin1String("default")?a.componentFriendly:a.contextFriendly)){
            baseline[path].remove(QStringLiteral("_k_friendly_name"));remaining[path].remove(QStringLiteral("_k_friendly_name"));
        }
    }
    for(auto it=baseline.begin();it!=baseline.end();)if(it.value().isEmpty())it=baseline.erase(it);else ++it;
    for(auto it=remaining.begin();it!=remaining.end();)if(it.value().isEmpty())it=remaining.erase(it);else ++it;
    if(baseline!=remaining){error=tr("Unrelated native shortcut configuration changed. Further writes were stopped to preserve it.");return false;}
    job->lastDisk=digest;return true;
}
void KdeShortcutsBackend::nextWrite(const std::shared_ptr<ShortcutsWriteJob> &job)
{
    if(job->done)return;
    if(job->position>=job->operations.size()){job->persistence.start();verifyPersistence(job);return;}
    if(job->position==job->removalCount && !job->phaseChecked){
        job->phaseChecked=true;
        snapshot([this,job](bool success,const auto &fresh,const auto &error){
            if(job->done)return;
            if(!success || fresh.owner!=job->current.owner || registryRevision(fresh)!=registryRevision(job->current)){
                if(success)job->current=fresh;
                finishWrite(job,job->written?ShortcutsOutcome::Partial:ShortcutsOutcome::Failed,error.isEmpty()?tr("The shortcut registry changed between update phases."):error);return;
            }
            nextWrite(job);
        });return;
    }
    QString diskError;if(!guardDisk(job,diskError)){finishWrite(job,job->written?ShortcutsOutcome::Partial:ShortcutsOutcome::Failed,diskError);return;}
    const auto operation=job->operations[job->position];
    const auto *pending=job->current.find(operation.first);
    if(pending && !pending->registered){
        if(!pending->layoutAction || !layoutActionId(pending->id) || !job->expected.layouts.available || !job->expected.layouts.actionNames.contains(pending->id.action) || operation.second.isEmpty()){
            finishWrite(job,job->written?ShortcutsOutcome::Partial:ShortcutsOutcome::Failed,tr("An unregistered shortcut is not an approved nonempty runtime layout binding."));return;
        }
        guardLayoutState(job,[this,job,operation](bool valid,const auto &error){
            if(!valid){finishWrite(job,job->written?ShortcutsOutcome::Partial:ShortcutsOutcome::Failed,error);return;}
            QString diskError;if(!guardDisk(job,diskError)){finishWrite(job,job->written?ShortcutsOutcome::Partial:ShortcutsOutcome::Failed,diskError);return;}
            job->outstanding=true;
            // Empty friendly fields preserve metadata if a registration races us; no SetPresent,
            // QAction, invocation subscription, default setter, or cleanup operation is used.
            m_transport->call(job->current.owner,rootPath,rootInterface,QStringLiteral("doRegister"),{operation.first.nativeId()},[this,job,operation](const auto &reply){
                if(job->done)return;
                if(!reply.success){finishWrite(job,ShortcutsOutcome::Uncertain,tr("The native layout action registration has an uncertain outcome: %1").arg(reply.error));return;}
                job->outstanding=false;++job->written;
                for(auto &a:job->current.actions)if(a.id==operation.first)a.registered=true;
                job->current.revision=ShortcutsTypes::revision(job->current);
                snapshot([this,job](bool success,const auto &fresh,const auto &error){
                    if(job->done)return;
                    if(!success || fresh.owner!=job->current.owner || registryRevision(fresh)!=registryRevision(job->current)){
                        if(success)job->current=fresh;
                        finishWrite(job,ShortcutsOutcome::Partial,error.isEmpty()?tr("The registered layout action metadata could not be verified. No binding was assigned."):error);return;
                    }
                    job->current=fresh;nextWrite(job);
                });
            });
        });return;
    }
    m_transport->call(busInterface,busPath,busInterface,QStringLiteral("GetNameOwner"),{service},[this,job,operation](const auto &owner){
        if(job->done)return;
        if(!owner.success || owner.value.toString()!=job->current.owner){finishWrite(job,job->written?ShortcutsOutcome::Partial:ShortcutsOutcome::Failed,tr("The shortcut daemon owner changed before writing."));return;}
        m_transport->call(job->current.owner,rootPath,rootInterface,QStringLiteral("shortcutKeys"),{operation.first.nativeId()},[this,job,operation](const auto &before){
            if(job->done)return;
            const auto *action=job->current.find(operation.first);
            if(!before.success || before.value.metaType()!=QMetaType::fromType<QList<QKeySequence>>() || !action || !ShortcutsTypes::sameKeys(nonempty(qvariant_cast<QList<QKeySequence>>(before.value)),action->current)){
                finishWrite(job,job->written?ShortcutsOutcome::Partial:ShortcutsOutcome::Failed,tr("The target shortcut changed before writing. Refresh to inspect it."));return;
            }
            QString diskError;if(!guardDisk(job,diskError)){finishWrite(job,job->written?ShortcutsOutcome::Partial:ShortcutsOutcome::Failed,diskError);return;}
            const auto nativeId=action->id.nativeId();
            guardLayoutState(job,[this,job,operation,nativeId](bool valid,const auto &error){
                if(!valid){finishWrite(job,job->written?ShortcutsOutcome::Partial:ShortcutsOutcome::Failed,error);return;}
                QString diskError;if(!guardDisk(job,diskError)){finishWrite(job,job->written?ShortcutsOutcome::Partial:ShortcutsOutcome::Failed,diskError);return;}
                job->outstanding=true;
                m_transport->call(job->current.owner,rootPath,rootInterface,QStringLiteral("setForeignShortcutKeys"),
                {nativeId,QVariant::fromValue(operation.second)},[this,job,operation](const auto &reply){
                    if(job->done)return;
                    if(!reply.success){finishWrite(job,ShortcutsOutcome::Uncertain,tr("The native shortcut write has an uncertain outcome: %1").arg(reply.error));return;}
                    job->outstanding=false;++job->written;++job->position;
                    m_transport->call(job->current.owner,rootPath,rootInterface,QStringLiteral("shortcutKeys"),{operation.first.nativeId()},[this,job,operation](const auto &readback){
                        if(job->done)return;
                        if(!readback.success || readback.value.metaType()!=QMetaType::fromType<QList<QKeySequence>>()){
                            finishWrite(job,ShortcutsOutcome::Partial,tr("The daemon acknowledged the shortcut write, but its readback failed. Refresh to inspect it."));return;
                        }
                        const auto actual=nonempty(qvariant_cast<QList<QKeySequence>>(readback.value));
                        for(auto &a:job->current.actions)if(a.id==operation.first)a.current=actual;
                        job->current.revision=ShortcutsTypes::revision(job->current);
                        if(!ShortcutsTypes::sameKeys(actual,operation.second)){
                            finishWrite(job,ShortcutsOutcome::Partial,tr("The daemon did not accept all requested alternatives. No shortcut was stolen. Refresh to inspect the result."));return;
                        }
                        nextWrite(job);
                    });
                });
            });
        });
    });
}
void KdeShortcutsBackend::reloadLayoutCallbacks(const std::shared_ptr<ShortcutsWriteJob> &job)
{
    guardLayoutState(job,[this,job](bool valid,const auto &error){
        if(!valid){finishWrite(job,ShortcutsOutcome::Partial,error);return;}
        QString diskError;if(!guardDisk(job,diskError)){finishWrite(job,ShortcutsOutcome::Partial,diskError);return;}
        job->outstanding=true;
        m_transport->reloadLayouts(job->expected.layouts.owner,[this,job](const auto &reply){
            if(job->done)return;
            if(!reply.success){
                job->outstanding=reply.uncertain;
                finishWrite(job,reply.uncertain?ShortcutsOutcome::Uncertain:ShortcutsOutcome::Partial,tr("Shortcut bindings were saved, but the verified KWin layout reload failed: %1").arg(reply.error));return;
            }
            job->outstanding=false;job->reloaded=true;
            guardLayoutState(job,[this,job](bool valid,const auto &error){if(!valid){finishWrite(job,ShortcutsOutcome::Partial,error);return;}verifyPersistence(job);});
        });
    });
}
void KdeShortcutsBackend::verifyPersistence(const std::shared_ptr<ShortcutsWriteJob> &job)
{
    if(job->done)return;
    snapshot([this,job](bool success,const auto &fresh,const auto &error){
        if(job->done)return;
        if(!success || fresh.owner!=job->current.owner || registryRevision(fresh)!=registryRevision(job->current)){
            if(success)job->current=fresh;
            finishWrite(job,job->written?ShortcutsOutcome::Partial:ShortcutsOutcome::Failed,error.isEmpty()?tr("Shortcuts changed while verifying the native update."):error);return;
        }
        QString diskError;if(!guardDisk(job,diskError)){finishWrite(job,job->written?ShortcutsOutcome::Partial:ShortcutsOutcome::Failed,diskError);return;}
        job->current=fresh;bool persisted=true;KConfig config(m_options.config,configFlags(m_options));
        for(auto it=job->requested.cbegin();it!=job->requested.cend();++it){
            const auto *action=fresh.find(it.key());
            if(!action || !ShortcutsTypes::sameKeys(action->current,it.value())){persisted=false;break;}
            if(ShortcutsTypes::sameKeys(job->expected.find(it.key())->current,it.value()))continue;
            const auto group=actionGroup(config,it.key());
            if(serviceAction(it.key())){
                const auto keys=group.hasKey(it.key().action)?parsedKeys(group.readEntry(it.key().action,QString{})):action->defaults;
                if(!ShortcutsTypes::sameKeys(keys,it.value())){persisted=false;break;}
            } else {
                const auto entry=group.readEntry(it.key().action,QStringList{});
                if(entry.size()!=3 || !ShortcutsTypes::sameKeys(parsedKeys(entry[0]),it.value()) || !ShortcutsTypes::sameKeys(parsedKeys(entry[1]),action->defaults)) {persisted=false;break;}
            }
        }
        if(persisted){if(job->layoutChanged && !job->reloaded)reloadLayoutCallbacks(job);else finishWrite(job,ShortcutsOutcome::Success,{});return;}
        if(job->persistence.elapsed()>=m_options.persistenceBudgetMs){finishWrite(job,ShortcutsOutcome::Partial,tr("The daemon adopted the shortcuts, but durable native configuration readback could not be verified. Refresh to inspect the result."));return;}
        QTimer::singleShot(150,this,[this,job]{verifyPersistence(job);});
    });
}
void KdeShortcutsBackend::finishWrite(const std::shared_ptr<ShortcutsWriteJob> &job,ShortcutsOutcome outcome,const QString &error)
{
    if(job->done)return;job->done=true;job->timer->stop();job->timer->deleteLater();
    if(outcome==ShortcutsOutcome::Uncertain){m_uncertainOwner=job->expected.owner;job->current.writable=false;job->current.reason=error;}
    if(m_job==job)m_job.reset();
    emit writeFinished(job->token,outcome,job->current,error);
}
#include "ShortcutsBackend.moc"
