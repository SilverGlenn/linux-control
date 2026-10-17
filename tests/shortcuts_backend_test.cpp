#include <QtTest>
#include <KConfig>
#include <KConfigGroup>
#include <KGlobalAccel>
#include <KGlobalShortcutInfo>
#include <QDBusArgument>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QProcess>
#include <QTemporaryDir>
#include <algorithm>
#include "platform/shortcuts/ShortcutsBackend.h"

namespace {
const QString rootXml=QStringLiteral(R"XML(<node><interface name="org.kde.KGlobalAccel">
<method name="allComponents"><arg direction="out" type="ao"/></method>
<method name="doRegister"><arg direction="in" type="as"/></method>
<method name="shortcutKeys"><arg direction="out" type="a(ai)"/><arg direction="in" type="as"/></method>
<method name="defaultShortcutKeys"><arg direction="out" type="a(ai)"/><arg direction="in" type="as"/></method>
<method name="setForeignShortcutKeys"><arg direction="in" type="as"/><arg direction="in" type="a(ai)"/></method>
<method name="globalShortcutsByKey"><arg direction="out" type="a(ssssssaiai)"/><arg direction="in" type="(ai)"/><arg direction="in" type="(i)"/></method>
</interface></node>)XML");
const QString componentXml=QStringLiteral(R"XML(<node><interface name="org.kde.kglobalaccel.Component">
<method name="getShortcutContexts"><arg direction="out" type="as"/></method>
<method name="allShortcutInfos"><arg direction="out" type="a(ssssssaiai)"/><arg direction="in" type="s"/></method>
</interface></node>)XML");
const QString kwinXml=QStringLiteral(R"XML(<node><interface name="org.kde.KWin"><method name="supportInformation"><arg direction="out" type="s"/></method></interface></node>)XML");
const QString layoutsXml=QStringLiteral(R"XML(<node><interface name="org.kde.KeyboardLayouts"><method name="getLayoutsList"><arg direction="out" type="a(sss)"/></method><signal name="layoutListChanged"/></interface></node>)XML");
const QString layoutFirst=QStringLiteral("Switch keyboard layout to Anglais (Colemak)");
const QString layoutSecond=QStringLiteral("Switch keyboard layout to 日本語");
ShortcutAction action(const QString &component,const QString &context,const QString &name,const QList<QKeySequence> &keys={})
{
    ShortcutAction result;result.id={component,context,name};result.componentFriendly=component+QStringLiteral(" label");result.contextFriendly=context+QStringLiteral(" label");result.friendlyName=name+QStringLiteral(" label");result.current=keys;result.defaults=component.endsWith(QLatin1String(".desktop"))?keys:QList<QKeySequence>{QKeySequence(QStringLiteral("Meta+F12"))};return result;
}
QString encoded(const QList<QKeySequence> &keys)
{QStringList parts;for(const auto &key:keys)if(!key.isEmpty())parts<<key.toString(QKeySequence::PortableText);return parts.isEmpty()?QStringLiteral("none"):parts.join(QLatin1Char('\t'));}
}
class FakeShortcutsTransport final:public ShortcutsTransport {
public:
    QString owner=QStringLiteral(":1.77"),config;
    QList<ShortcutAction> actions={action(QStringLiteral("kwin"),QStringLiteral("default"),QStringLiteral("Window Close"),{QKeySequence(QStringLiteral("Alt+F4"))}),
        action(QStringLiteral("third.party.desktop"),QStringLiteral("default"),QStringLiteral("_launch"),{QKeySequence(QStringLiteral("Meta+U")),QKeySequence(QStringLiteral("Ctrl+K, Ctrl+U"))}),
        action(QStringLiteral("third.party.desktop"),QStringLiteral("editor"),QStringLiteral("_launch"),{QKeySequence(QStringLiteral("Meta+E"))}),
        action(QStringLiteral("KDE Keyboard Layout Switcher"),QStringLiteral("default"),QStringLiteral("Switch to Next Keyboard Layout"),{QKeySequence(QStringLiteral("Meta+Space"))})};
    int writes=0,calls=0,registrations=0,reloads=0;
    QString kwinOwner=QStringLiteral(":1.90"),keyboardOwner=kwinOwner,kwinVersion=QStringLiteral("6.6.6"),reloadDestination;
    QVariantList layouts={QVariantMap{{QStringLiteral("shortName"),QStringLiteral("us")},{QStringLiteral("displayName"),QStringLiteral("US")},{QStringLiteral("longName"),QStringLiteral("Anglais (Colemak)")}},QVariantMap{{QStringLiteral("shortName"),QStringLiteral("jp")},{QStringLiteral("displayName"),QStringLiteral("日本")},{QStringLiteral("longName"),QStringLiteral("日本語")}}};
    bool stallRegister=false,registerFailure=false,reloadFailure=false,reloadUncertain=false;
    QList<ShortcutActionId> fresh;
    bool badInterface=false,stall=false,stallWriter=false,persist=true,reject=false;
    QStringList methods,destinations;
    QList<int> matchTypes;
    QList<QPair<ShortcutActionId,QList<QKeySequence>>> written;
    std::function<void(const QString &)> beforeCall;
    void save() {
        if(!persist || config.isEmpty())return;
        KConfig file(config,KConfig::SimpleConfig);
        for(const auto &a:actions){
            if(fresh.contains(a.id))continue;
            const bool service=a.id.component.endsWith(QLatin1String(".desktop"));
            KConfigGroup group=service?file.group("services").group(a.id.component):file.group(a.id.component);
            if(a.id.context!=QLatin1String("default"))group=group.group(a.id.context);
            if(service){if(ShortcutsTypes::sameKeys(a.current,a.defaults))group.revertToDefault(a.id.action);else group.writeEntry(a.id.action,encoded(a.current));}
            else group.writeEntry(a.id.action,QStringList{encoded(a.current),encoded(a.defaults),a.friendlyName});
        }
        file.sync();
    }
    void call(const QString &destination,const QString &path,const QString &,const QString &method,const QVariantList &args,Completion completion) override {
        ++calls;methods<<method;destinations<<destination;if(beforeCall)beforeCall(method);if(stall)return;
        ShortcutsReply reply;reply.success=true;
        if(method==QLatin1String("GetNameOwner"))reply.value=args.first().toString()==QLatin1String("org.kde.KWin")?kwinOwner:args.first().toString()==QLatin1String("org.kde.keyboard")?keyboardOwner:owner;
        else if(destination!=owner && destination!=kwinOwner){reply.success=false;reply.error=QStringLiteral("The owner vanished.");}
        else if(method==QLatin1String("Introspect"))reply.value=badInterface?QStringLiteral("<node/>"):path==QLatin1String("/kglobalaccel")?rootXml:path==QLatin1String("/KWin")?kwinXml:path==QLatin1String("/Layouts")?layoutsXml:componentXml;
        else if(method==QLatin1String("supportInformation"))reply.value=QStringLiteral("KWin version: ")+kwinVersion+QLatin1Char('\n');
        else if(method==QLatin1String("getLayoutsList"))reply.value=layouts;
        else if(method==QLatin1String("allComponents")){
            QStringList paths;for(const auto &a:actions){const auto p=QStringLiteral("/component/")+a.id.component.toUtf8().toHex();if(!paths.contains(p))paths<<p;}reply.value=paths;
        } else if(method==QLatin1String("getShortcutContexts")){
            QStringList contexts;for(const auto &a:actions)if(path==QStringLiteral("/component/")+a.id.component.toUtf8().toHex()&&!contexts.contains(a.id.context))contexts<<a.id.context;reply.value=contexts;
        } else if(method==QLatin1String("allShortcutInfos")){
            QList<ShortcutAction> found;for(auto a:actions)if(path==QStringLiteral("/component/")+a.id.component.toUtf8().toHex()&&a.id.context==args.first().toString()){
                // Match the lossy native metadata payload; the full v2 getters must replace this.
                for(auto &key:a.current)key=QKeySequence(key[0].toCombined());for(auto &key:a.defaults)key=QKeySequence(key[0].toCombined());found<<a;
            }reply.value=QVariant::fromValue(found);
        } else if(method==QLatin1String("globalShortcutsByKey")){
            const auto key=qvariant_cast<QKeySequence>(args[0]);matchTypes<<int(qvariant_cast<KGlobalAccel::MatchType>(args[1]));QList<ShortcutAction> found;
            for(const auto &a:actions)for(const auto &assigned:a.current)if(ShortcutsTypes::conflicts(key,assigned)){found<<a;break;}reply.value=QVariant::fromValue(found);
        } else {
            const auto id=args.first().toStringList();const auto parts=id.value(0).split(QLatin1Char('|'));
            ShortcutActionId identity{parts.value(0),parts.value(1,QStringLiteral("default")),id.value(1)};
            auto found=std::find_if(actions.begin(),actions.end(),[&](const auto &a){return a.id==identity;});
            if(method==QLatin1String("doRegister")){
                ++registrations;if(stallRegister)return;
                if(registerFailure){reply.success=false;reply.uncertain=true;reply.error=QStringLiteral("Registration failed.");}
                else if(found==actions.end()){
                    auto value=action(identity.component,identity.context,identity.action);value.friendlyName=id.value(3);value.current.clear();value.defaults.clear();
                    for(const auto &existing:actions)if(existing.id.component==identity.component&&existing.id.context==identity.context){value.componentFriendly=existing.componentFriendly;value.contextFriendly=existing.contextFriendly;break;}
                    actions<<value;fresh<<identity;
                }
            } else if(found==actions.end()){reply.success=false;reply.error=QStringLiteral("Unknown action.");}
            else if(method==QLatin1String("shortcutKeys"))reply.value=QVariant::fromValue(found->current);
            else if(method==QLatin1String("defaultShortcutKeys"))reply.value=QVariant::fromValue(found->defaults);
            else if(method==QLatin1String("setForeignShortcutKeys")){
                ++writes;if(stallWriter)return;const auto keys=qvariant_cast<QList<QKeySequence>>(args[1]);written<<qMakePair(identity,keys);fresh.removeAll(identity);found->current=reject?QList<QKeySequence>{}:keys;save();
            } else {reply.success=false;reply.error=QStringLiteral("Unexpected method.");}
        }
        QTimer::singleShot(0,this,[completion=std::move(completion),reply]{completion(reply);});
    }
    void reloadLayouts(const QString &destination,Completion completion) override {
        ++reloads;reloadDestination=destination;ShortcutsReply reply;reply.success=!reloadFailure && destination==kwinOwner;reply.uncertain=!reply.success&&reloadUncertain;reply.error=reply.success?QString{}:QStringLiteral("Fake native reload failed.");
        QTimer::singleShot(0,this,[completion=std::move(completion),reply]{completion(reply);});
    }
};
struct WireShortcutInfo {QString name,friendly,component,componentFriendly,context,contextFriendly;QList<int> keys,defaults;};
Q_DECLARE_METATYPE(WireShortcutInfo)
Q_DECLARE_METATYPE(QList<WireShortcutInfo>)
QDBusArgument &operator<<(QDBusArgument &arg,const WireShortcutInfo &info)
{arg.beginStructure();arg<<info.name<<info.friendly<<info.component<<info.componentFriendly<<info.context<<info.contextFriendly<<info.keys<<info.defaults;arg.endStructure();return arg;}
const QDBusArgument &operator>>(const QDBusArgument &arg,WireShortcutInfo &info)
{arg.beginStructure();arg>>info.name>>info.friendly>>info.component>>info.componentFriendly>>info.context>>info.contextFriendly>>info.keys>>info.defaults;arg.endStructure();return arg;}
class WireShortcutDaemon final:public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface","org.kde.KGlobalAccel")
public:
    QList<QKeySequence> keys={QKeySequence(QStringLiteral("Meta+F9")),QKeySequence(QStringLiteral("Ctrl+K, Ctrl+M"))};
    int writes=0;
public slots:
    QList<QDBusObjectPath> allComponents()const{return {QDBusObjectPath(QStringLiteral("/component/wire"))};}
    QList<QKeySequence> shortcutKeys(const QStringList &)const{return keys;}
    QList<QKeySequence> defaultShortcutKeys(const QStringList &)const{return {QKeySequence(QStringLiteral("Ctrl+K, Ctrl+D"))};}
    void setForeignShortcutKeys(const QStringList &,const QList<QKeySequence> &value){keys=value;++writes;}
    QList<WireShortcutInfo> globalShortcutsByKey(const QKeySequence &,KGlobalAccel::MatchType)const{return {};}
};
class WireShortcutComponent final:public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface","org.kde.kglobalaccel.Component")
public slots:
    QStringList getShortcutContexts()const{return {QStringLiteral("default")};}
    QList<WireShortcutInfo> allShortcutInfos(const QString &)const{return {{QStringLiteral("wireAction"),QStringLiteral("Wire action"),QStringLiteral("wire"),QStringLiteral("Wire component"),QStringLiteral("default"),QStringLiteral("Default"),{int(Qt::META|Qt::Key_F9)},{int(Qt::CTRL|Qt::Key_K)}}};}
};
struct WireRuntimeLayout {QString shortName,displayName,longName;};
Q_DECLARE_METATYPE(WireRuntimeLayout)
Q_DECLARE_METATYPE(QList<WireRuntimeLayout>)
QDBusArgument &operator<<(QDBusArgument &arg,const WireRuntimeLayout &layout)
{arg.beginStructure();arg<<layout.shortName<<layout.displayName<<layout.longName;arg.endStructure();return arg;}
const QDBusArgument &operator>>(const QDBusArgument &arg,WireRuntimeLayout &layout)
{arg.beginStructure();arg>>layout.shortName>>layout.displayName>>layout.longName;arg.endStructure();return arg;}
class WireRuntimeLayouts final:public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface","org.kde.KeyboardLayouts")
public slots:
    QList<WireRuntimeLayout> getLayoutsList()const{return {{QStringLiteral("us"),QStringLiteral("US"),QStringLiteral("Anglais (Colemak)")},{QStringLiteral("jp"),QStringLiteral("日本"),QStringLiteral("日本語")}};}
signals:
    void layoutListChanged();
};
class WireNativeReloadReceiver final:public QObject {
    Q_OBJECT
public:
    WireRuntimeLayouts *layouts=nullptr;
    QHash<QString,QByteArrayList> changes;
    QString sender;
    int notices=0;
    bool acknowledge=true;
public slots:
    void notice(const QHash<QString,QByteArrayList> &value,const QDBusMessage &message){
        ++notices;changes=value;sender=message.service();
        if(acknowledge&&layouts)QTimer::singleShot(0,layouts,[object=layouts]{emit object->layoutListChanged();});
    }
};
class ShortcutsBackendTest final:public QObject {
    Q_OBJECT
    static ShortcutsBackendOptions options(QTemporaryDir &dir){ShortcutsBackendOptions o;o.config=dir.filePath(QStringLiteral("kglobalshortcutsrc"));o.isolated=true;o.persistenceBudgetMs=250;return o;}
    static ShortcutsBackendOptions layoutOptions(QTemporaryDir &dir){auto o=options(dir);o.enableLayoutAdapter=true;o.layoutConfig=dir.filePath(QStringLiteral("kxkbrc"));QFile file(o.layoutConfig);file.open(QIODevice::WriteOnly);file.write("[Layout]\nLayoutList=us,jp\nVariantList=colemak,\n");file.close();return o;}
    static ShortcutActionId layoutId(const QString &name=layoutFirst){return {QStringLiteral("KDE Keyboard Layout Switcher"),QStringLiteral("default"),name};}
    static ShortcutsSnapshot read(KdeShortcutsBackend &backend){QSignalSpy spy(&backend,&ShortcutsBackend::readFinished);backend.readCurrent(1);if(spy.isEmpty())spy.wait(6000);return spy.isEmpty()?ShortcutsSnapshot{}:qvariant_cast<ShortcutsSnapshot>(spy.first().at(2));}
    static QList<QVariant> write(KdeShortcutsBackend &backend,const ShortcutsSnapshot &state,const ShortcutChanges &changes){QSignalSpy spy(&backend,&ShortcutsBackend::writeFinished);backend.writeChanges(2,state,changes);if(spy.isEmpty())spy.wait(6000);return spy.isEmpty()?QList<QVariant>{}:spy.first();}
private slots:
    void initTestCase(){qRegisterMetaType<ShortcutsSnapshot>();qRegisterMetaType<ShortcutsOutcome>();}
    void discoversEveryComponentContextAndFullSequences(){
        QTemporaryDir dir;auto o=options(dir);auto *fake=new FakeShortcutsTransport;KdeShortcutsBackend backend(fake,o);const auto state=read(backend);
        QVERIFY2(state.writable,qPrintable(state.reason));QCOMPARE(state.actions.size(),4);QVERIFY(state.find({QStringLiteral("third.party.desktop"),QStringLiteral("editor"),QStringLiteral("_launch")}));
        const auto *third=state.find({QStringLiteral("third.party.desktop"),QStringLiteral("default"),QStringLiteral("_launch")});QVERIFY(third);QCOMPARE(third->current.size(),2);QCOMPARE(third->current[1].count(),2);QCOMPARE(fake->writes,0);
        for(int i=0;i<fake->methods.size();++i)if(fake->methods[i]!=QLatin1String("GetNameOwner"))QCOMPARE(fake->destinations[i],fake->owner);
        QVERIFY(!fake->methods.contains(QStringLiteral("doRegister")));QVERIFY(!fake->methods.contains(QStringLiteral("invokeShortcut")));QVERIFY(!fake->methods.contains(QStringLiteral("cleanUp")));
    }
    void nativeWritePreservesUnchangedActionsAndDefaults(){
        QTemporaryDir dir;auto o=options(dir);auto *fake=new FakeShortcutsTransport;fake->config=o.config;fake->save();
        {KConfig c(o.config,KConfig::SimpleConfig);KConfigGroup(&c,"Vendor").writeEntry("opaque","preserve");QVERIFY(c.sync());}
        KdeShortcutsBackend backend(fake,o);const auto state=read(backend);const auto target=state.actions.first().id;ShortcutChanges changes{{target,{QKeySequence(QStringLiteral("Meta+Y")),QKeySequence(QStringLiteral("Ctrl+K, Ctrl+Y"))}}};
        const auto result=write(backend,state,changes);QVERIFY(!result.isEmpty());QCOMPARE(qvariant_cast<ShortcutsOutcome>(result[1]),ShortcutsOutcome::Success);QVERIFY(fake->matchTypes.contains(0));QVERIFY(fake->matchTypes.contains(1));QVERIFY(fake->matchTypes.contains(2));
        const auto actual=qvariant_cast<ShortcutsSnapshot>(result[2]);for(const auto &a:state.actions){QCOMPARE(actual.find(a.id)->defaults,a.defaults);if(!(a.id==target))QCOMPARE(actual.find(a.id)->current,a.current);}
        KConfig c(o.config,KConfig::SimpleConfig);QCOMPARE(KConfigGroup(&c,"Vendor").readEntry("opaque"),QStringLiteral("preserve"));
    }
    void rejectsConflictBeforeAnyWrite(){
        QTemporaryDir dir;auto *fake=new FakeShortcutsTransport;KdeShortcutsBackend backend(fake,options(dir));const auto state=read(backend);
        const auto result=write(backend,state,{{fake->actions[0].id,{QKeySequence(QStringLiteral("Ctrl+U"))}}});QVERIFY(!result.isEmpty());QCOMPARE(qvariant_cast<ShortcutsOutcome>(result[1]),ShortcutsOutcome::Failed);QCOMPARE(fake->writes,0);
    }
    void explicitReassignmentReleasesBeforeAssigning(){
        QTemporaryDir dir;auto o=options(dir);auto *fake=new FakeShortcutsTransport;fake->config=o.config;fake->save();KdeShortcutsBackend backend(fake,o);const auto state=read(backend);
        const auto source=fake->actions[1].id,target=fake->actions[0].id;const auto retained=fake->actions[1].current[1];
        const auto result=write(backend,state,{{source,{retained}},{target,{QKeySequence(QStringLiteral("Meta+U"))}}});QCOMPARE(qvariant_cast<ShortcutsOutcome>(result[1]),ShortcutsOutcome::Success);QVERIFY(fake->written.size()>=2);
        int release=-1,assign=-1;for(int i=0;i<fake->written.size();++i){if(fake->written[i].first==source)release=i;if(fake->written[i].first==target && fake->written[i].second.contains(QKeySequence(QStringLiteral("Meta+U"))))assign=i;}QVERIFY(release>=0);QVERIFY(assign>release);
    }
    void externalConfigEditBeforeApplyIsPreserved(){
        QTemporaryDir dir;auto o=options(dir);auto *fake=new FakeShortcutsTransport;fake->config=o.config;fake->save();KdeShortcutsBackend backend(fake,o);const auto state=read(backend);
        {KConfig file(o.config,KConfig::SimpleConfig);file.group("services").group("third.party.desktop").writeEntry("_launch",QStringLiteral("Meta+Q"));QVERIFY(file.sync());}
        const auto result=write(backend,state,{{fake->actions[0].id,{QKeySequence(QStringLiteral("Meta+Y"))}}});QCOMPARE(qvariant_cast<ShortcutsOutcome>(result[1]),ShortcutsOutcome::Failed);QCOMPARE(fake->writes,0);
        KConfig saved(o.config,KConfig::SimpleConfig);QCOMPARE(saved.group("services").group("third.party.desktop").readEntry("_launch"),QStringLiteral("Meta+Q"));
    }
    void delayedNativePersistenceDoesNotSelfConflict(){
        QTemporaryDir dir;auto o=options(dir);o.persistenceBudgetMs=1000;auto *fake=new FakeShortcutsTransport;fake->config=o.config;fake->save();fake->persist=false;KdeShortcutsBackend backend(fake,o);const auto state=read(backend);
        QTimer::singleShot(200,fake,[fake]{fake->persist=true;fake->save();});
        const auto result=write(backend,state,{{fake->actions[0].id,{QKeySequence(QStringLiteral("Meta+Y"))}},{fake->actions[1].id,{QKeySequence(QStringLiteral("Meta+J"))}}});QCOMPARE(qvariant_cast<ShortcutsOutcome>(result[1]),ShortcutsOutcome::Success);
    }
    void diskDaemonMismatchFailsBeforeWriting(){
        QTemporaryDir dir;auto o=options(dir);auto *fake=new FakeShortcutsTransport;fake->config=o.config;fake->save();
        {KConfig file(o.config,KConfig::SimpleConfig);KConfigGroup(&file,"kwin").writeEntry("Window Close",QStringList{"Meta+Q","Meta+F12","Close"});QVERIFY(file.sync());}
        KdeShortcutsBackend backend(fake,o);const auto state=read(backend);const auto result=write(backend,state,{{fake->actions[1].id,{QKeySequence(QStringLiteral("Meta+Y"))}}});QCOMPARE(qvariant_cast<ShortcutsOutcome>(result[1]),ShortcutsOutcome::Failed);QCOMPARE(fake->writes,0);
    }
    void desktopServiceOverridesDefaultsAndDisabledPersistence(){
        QTemporaryDir dir;auto o=options(dir);auto *fake=new FakeShortcutsTransport;fake->config=o.config;
        const auto id=fake->actions[1].id;fake->actions[1].current={QKeySequence(QStringLiteral("Meta+Q"))};fake->save();KdeShortcutsBackend backend(fake,o);
        auto state=read(backend);auto result=write(backend,state,{{id,state.find(id)->defaults}});QVERIFY(!result.isEmpty());QCOMPARE(qvariant_cast<ShortcutsOutcome>(result[1]),ShortcutsOutcome::Success);
        {KConfig file(o.config,KConfig::SimpleConfig);QVERIFY(!file.group("services").group(id.component).hasKey(id.action));QVERIFY(!file.hasGroup(id.component));}
        state=read(backend);result=write(backend,state,{{id,{}}});QCOMPARE(qvariant_cast<ShortcutsOutcome>(result[1]),ShortcutsOutcome::Success);
        {KConfig file(o.config,KConfig::SimpleConfig);QCOMPARE(file.group("services").group(id.component).readEntry(id.action),QStringLiteral("none"));}
        state=read(backend);const QList<QKeySequence> alternatives{QKeySequence(QStringLiteral("Meta+J")),QKeySequence(QStringLiteral("Ctrl+J, Ctrl+M"))};result=write(backend,state,{{id,alternatives}});QCOMPARE(qvariant_cast<ShortcutsOutcome>(result[1]),ShortcutsOutcome::Success);
        {KConfig file(o.config,KConfig::SimpleConfig);QCOMPARE(file.group("services").group(id.component).readEntry(id.action),encoded(alternatives));}
    }
    void immutableServiceOverrideIsReadOnly(){
        QTemporaryDir dir;auto o=options(dir);QFile file(o.config);QVERIFY(file.open(QIODevice::WriteOnly));file.write("[services][third.party.desktop]\n_launch[$i]=Meta+U\n");file.close();auto *fake=new FakeShortcutsTransport;KdeShortcutsBackend backend(fake,o);const auto state=read(backend);QVERIFY(!state.find(fake->actions[1].id)->available);
        const auto result=write(backend,state,{{fake->actions[1].id,{QKeySequence(QStringLiteral("Meta+Y"))}}});QCOMPARE(qvariant_cast<ShortcutsOutcome>(result[1]),ShortcutsOutcome::Failed);QCOMPARE(fake->writes,0);
    }
    void staleOwnerAndRevisionRefuseWrite(){
        QTemporaryDir dir;auto *fake=new FakeShortcutsTransport;KdeShortcutsBackend backend(fake,options(dir));const auto state=read(backend);fake->actions[1].current={QKeySequence(QStringLiteral("Meta+Q"))};
        auto result=write(backend,state,{{fake->actions[0].id,{QKeySequence(QStringLiteral("Meta+Y"))}}});QCOMPARE(qvariant_cast<ShortcutsOutcome>(result[1]),ShortcutsOutcome::Failed);QCOMPARE(fake->writes,0);
        const auto fresh=read(backend);fake->owner=QStringLiteral(":1.88");result=write(backend,fresh,{{fake->actions[0].id,{QKeySequence(QStringLiteral("Meta+Y"))}}});QCOMPARE(qvariant_cast<ShortcutsOutcome>(result[1]),ShortcutsOutcome::Failed);QCOMPARE(fake->writes,0);
    }
    void wrongInterfaceAndBoundedReadFailClosed(){
        QTemporaryDir dir;auto o=options(dir);o.readBudgetMs=30;auto *fake=new FakeShortcutsTransport;fake->badInterface=true;KdeShortcutsBackend backend(fake,o);QVERIFY(!read(backend).available);fake->badInterface=false;fake->stall=true;QElapsedTimer timer;timer.start();QVERIFY(!read(backend).available);QVERIFY(timer.elapsed()<1000);QCOMPARE(fake->writes,0);
    }
    void uncertainWriterLocksSameOwner(){
        QTemporaryDir dir;auto o=options(dir);o.writeBudgetMs=150;auto *fake=new FakeShortcutsTransport;KdeShortcutsBackend backend(fake,o);auto state=read(backend);fake->stallWriter=true;
        auto result=write(backend,state,{{fake->actions[0].id,{QKeySequence(QStringLiteral("Meta+Y"))}}});QCOMPARE(qvariant_cast<ShortcutsOutcome>(result[1]),ShortcutsOutcome::Uncertain);QVERIFY(!read(backend).writable);
        fake->stallWriter=false;fake->owner=QStringLiteral(":1.90");QVERIFY(read(backend).writable);
    }
    void persistenceFailureReportsPartial(){
        QTemporaryDir dir;auto o=options(dir);auto *fake=new FakeShortcutsTransport;fake->persist=false;KdeShortcutsBackend backend(fake,o);const auto state=read(backend);
        const auto result=write(backend,state,{{fake->actions[0].id,{QKeySequence(QStringLiteral("Meta+Y"))}}});QCOMPARE(qvariant_cast<ShortcutsOutcome>(result[1]),ShortcutsOutcome::Partial);QVERIFY(result[3].toString().contains(QStringLiteral("durable")));
    }
    void rejectedNativeKeysReportPartial(){
        QTemporaryDir dir;auto *fake=new FakeShortcutsTransport;fake->reject=true;KdeShortcutsBackend backend(fake,options(dir));const auto state=read(backend);
        const auto result=write(backend,state,{{fake->actions[0].id,{QKeySequence(QStringLiteral("Meta+Y"))}}});QCOMPARE(qvariant_cast<ShortcutsOutcome>(result[1]),ShortcutsOutcome::Partial);
    }
    void immutableActionIsReadOnly(){
        QTemporaryDir dir;auto o=options(dir);QFile file(o.config);QVERIFY(file.open(QIODevice::WriteOnly));file.write("[kwin]\nWindow Close[$i]=Alt+F4,Alt+F4,Close\n");file.close();
        auto *fake=new FakeShortcutsTransport;KdeShortcutsBackend backend(fake,o);const auto state=read(backend);const auto *locked=state.find(fake->actions[0].id);QVERIFY(locked);QVERIFY(!locked->available);
        const auto result=write(backend,state,{{locked->id,{QKeySequence(QStringLiteral("Meta+Y"))}}});QCOMPARE(qvariant_cast<ShortcutsOutcome>(result[1]),ShortcutsOutcome::Failed);QCOMPARE(fake->writes,0);
    }
    void broadBatchUsesLinearNativeCalls(){
        QTemporaryDir dir;auto o=options(dir);auto *fake=new FakeShortcutsTransport;fake->config=o.config;fake->actions.clear();ShortcutChanges changes;
        constexpr int count=40;
        for(int i=0;i<count;++i){
            auto a=action(QStringLiteral("vendor.component"),QStringLiteral("default"),QStringLiteral("action%1").arg(i),{QKeySequence(int(Qt::CTRL|Qt::ALT|Qt::Key_F1))});
            changes.insert(a.id,{QKeySequence(int(Qt::META)|(int(Qt::Key_F1)+i%20),int(Qt::CTRL|Qt::SHIFT)|(int(Qt::Key_A)+i/20))});fake->actions<<a;
        }
        fake->save();KdeShortcutsBackend backend(fake,o);const auto state=read(backend);QCOMPARE(state.actions.size(),count);const auto result=write(backend,state,changes);QVERIFY(!result.isEmpty());QCOMPARE(qvariant_cast<ShortcutsOutcome>(result[1]),ShortcutsOutcome::Success);QVERIFY2(fake->calls<30*count+100,qPrintable(QString::number(fake->calls)));
    }
    void unrelatedConfigEditDuringBatchStopsFurtherWrites(){
        QTemporaryDir dir;auto o=options(dir);auto *fake=new FakeShortcutsTransport;fake->config=o.config;fake->save();KdeShortcutsBackend backend(fake,o);const auto state=read(backend);bool edited=false;
        fake->beforeCall=[&](const QString &method){if(!edited && fake->writes==1 && method==QLatin1String("shortcutKeys")){edited=true;KConfig file(o.config,KConfig::SimpleConfig);KConfigGroup(&file,"Vendor").writeEntry("Keep","external edit");QVERIFY(file.sync());}};
        const auto result=write(backend,state,{{fake->actions[0].id,{QKeySequence(QStringLiteral("Meta+Y"))}}});QCOMPARE(qvariant_cast<ShortcutsOutcome>(result[1]),ShortcutsOutcome::Partial);QCOMPARE(fake->writes,1);KConfig file(o.config,KConfig::SimpleConfig);QCOMPARE(KConfigGroup(&file,"Vendor").readEntry("Keep"),QStringLiteral("external edit"));
    }
    void externalChangeSignalsAreDebounced(){
        QTemporaryDir dir;auto *fake=new FakeShortcutsTransport;KdeShortcutsBackend backend(fake,options(dir));QSignalSpy spy(&backend,&ShortcutsBackend::externalChange);emit fake->changed();emit fake->changed();QTRY_COMPARE(spy.size(),1);
    }
    void sourceMatchingSemantics(){
        QVERIFY(ShortcutsTypes::conflicts(QKeySequence("Alt+B, Alt+F, Alt+G"),QKeySequence("Alt+F, Alt+G")));
        QVERIFY(ShortcutsTypes::conflicts(QKeySequence("Alt+B, Alt+F, Alt+G"),QKeySequence("Alt+F")));
        QVERIFY(!ShortcutsTypes::conflicts(QKeySequence("Alt+B, Alt+F, Alt+G"),QKeySequence("Alt+B, Alt+G")));
        QVERIFY(ShortcutsTypes::conflicts(QKeySequence("Shift+Tab"),QKeySequence("Shift+Backtab")));
        QVERIFY(ShortcutsTypes::conflicts(QKeySequence(int(Qt::Key_Meta)),QKeySequence(int(Qt::MetaModifier))));
    }
    void translatedRuntimeLayoutsAreSynthesizedWithoutRegistration(){
        QTemporaryDir dir;auto o=layoutOptions(dir);auto *fake=new FakeShortcutsTransport;KdeShortcutsBackend backend(fake,o);const auto state=read(backend);QVERIFY2(state.layouts.available,qPrintable(state.layouts.reason));QCOMPARE(state.layouts.owner,fake->kwinOwner);QCOMPARE(state.layouts.actionNames,QStringList({layoutFirst,layoutSecond}));
        const auto *candidate=state.find(layoutId());QVERIFY(candidate);QVERIFY(candidate->available);QVERIFY(candidate->layoutAction);QVERIFY(!candidate->registered);QVERIFY(candidate->current.isEmpty());QVERIFY(candidate->defaults.isEmpty());QVERIFY(candidate->friendlyName.isEmpty());QCOMPARE(fake->registrations,0);QCOMPARE(fake->writes,0);QCOMPARE(fake->reloads,0);QVERIFY(!fake->methods.contains(QStringLiteral("doRegister")));
    }
    void createsLayoutBindingPersistsAndReloadsVerifiedKWin(){
        QTemporaryDir dir;auto o=layoutOptions(dir);auto *fake=new FakeShortcutsTransport;fake->config=o.config;fake->save();KdeShortcutsBackend backend(fake,o);const auto state=read(backend);const QList<QKeySequence> keys{QKeySequence(QStringLiteral("Meta+J")),QKeySequence(QStringLiteral("Ctrl+J, Ctrl+M"))};
        const auto result=write(backend,state,{{layoutId(),keys}});QVERIFY(!result.isEmpty());QCOMPARE(qvariant_cast<ShortcutsOutcome>(result[1]),ShortcutsOutcome::Success);QCOMPARE(fake->registrations,1);QCOMPARE(fake->writes,1);QCOMPARE(fake->reloads,1);QCOMPARE(fake->reloadDestination,fake->kwinOwner);
        const auto actual=qvariant_cast<ShortcutsSnapshot>(result[2]);QVERIFY(actual.find(layoutId())->registered);QVERIFY(actual.find(layoutId())->layoutAction);QCOMPARE(actual.find(layoutId())->current,keys);QVERIFY(actual.layouts==state.layouts);
        for(const auto &a:state.actions)if(a.registered){QCOMPARE(actual.find(a.id)->current,a.current);QCOMPARE(actual.find(a.id)->defaults,a.defaults);QCOMPARE(actual.find(a.id)->friendlyName,a.friendlyName);}
        KConfig config(o.config,KConfig::SimpleConfig);const auto entry=config.group(layoutId().component).readEntry(layoutFirst,QStringList{});QCOMPARE(entry.size(),3);QCOMPARE(entry[0],encoded(keys));QCOMPARE(entry[1],QStringLiteral("none"));QVERIFY(entry[2].isEmpty());
        QVERIFY(!fake->methods.contains(QStringLiteral("setShortcutKeys")));QVERIFY(!fake->methods.contains(QStringLiteral("setInactive")));QVERIFY(!fake->methods.contains(QStringLiteral("invokeShortcut")));QVERIFY(!fake->methods.contains(QStringLiteral("cleanUp")));
    }
    void registeredDisabledLayoutReloadsAndPreservesNativeMetadata(){
        QTemporaryDir dir;auto o=layoutOptions(dir);auto *fake=new FakeShortcutsTransport;fake->config=o.config;auto a=action(layoutId().component,layoutId().context,layoutFirst);a.current.clear();a.defaults.clear();a.friendlyName=QStringLiteral("Existing native label");fake->actions<<a;fake->save();KdeShortcutsBackend backend(fake,o);auto state=read(backend);
        auto result=write(backend,state,{{layoutId(),{QKeySequence(QStringLiteral("Meta+Y"))}}});QCOMPARE(qvariant_cast<ShortcutsOutcome>(result[1]),ShortcutsOutcome::Success);QCOMPARE(fake->registrations,0);QCOMPARE(fake->reloads,1);QCOMPARE(qvariant_cast<ShortcutsSnapshot>(result[2]).find(layoutId())->friendlyName,a.friendlyName);
        state=read(backend);result=write(backend,state,{{layoutId(),{}}});QCOMPARE(qvariant_cast<ShortcutsOutcome>(result[1]),ShortcutsOutcome::Success);QCOMPARE(fake->reloads,2);QCOMPARE(fake->registrations,0);QVERIFY(qvariant_cast<ShortcutsSnapshot>(result[2]).find(layoutId())->registered);QVERIFY(qvariant_cast<ShortcutsSnapshot>(result[2]).find(layoutId())->current.isEmpty());
    }
    void unknownLayoutsAndChangedRuntimeAreRejected(){
        QTemporaryDir dir;auto o=layoutOptions(dir);auto *fake=new FakeShortcutsTransport;KdeShortcutsBackend backend(fake,o);const auto state=read(backend);
        auto result=write(backend,state,{{layoutId(QStringLiteral("Switch keyboard layout to guessed English")),{QKeySequence(QStringLiteral("Meta+Y"))}}});QCOMPARE(qvariant_cast<ShortcutsOutcome>(result[1]),ShortcutsOutcome::Failed);QCOMPARE(fake->registrations,0);
        auto map=fake->layouts[0].toMap();map[QStringLiteral("longName")]=QStringLiteral("Other translated runtime layout");fake->layouts[0]=map;
        result=write(backend,state,{{layoutId(),{QKeySequence(QStringLiteral("Meta+Y"))}}});QCOMPARE(qvariant_cast<ShortcutsOutcome>(result[1]),ShortcutsOutcome::Failed);QCOMPARE(fake->registrations,0);QCOMPARE(fake->writes,0);QCOMPARE(fake->reloads,0);
    }
    void unavailableLayoutAdapterLeavesOrdinaryGlobalsWritable(){
        QTemporaryDir dir;auto o=layoutOptions(dir);auto *fake=new FakeShortcutsTransport;fake->kwinVersion=QStringLiteral("6.7.0");fake->config=o.config;fake->save();KdeShortcutsBackend backend(fake,o);const auto state=read(backend);QVERIFY(state.writable);QVERIFY(!state.layouts.available);QVERIFY(!state.find(layoutId()));
        const auto result=write(backend,state,{{fake->actions[0].id,{QKeySequence(QStringLiteral("Meta+Y"))}}});QCOMPARE(qvariant_cast<ShortcutsOutcome>(result[1]),ShortcutsOutcome::Success);QCOMPARE(fake->registrations,0);QCOMPARE(fake->reloads,0);
    }
    void changedLayoutConfigOrKWinOwnerStopsRegistration(){
        QTemporaryDir dir;auto o=layoutOptions(dir);auto *fake=new FakeShortcutsTransport;KdeShortcutsBackend backend(fake,o);auto state=read(backend);
        {QFile file(o.layoutConfig);QVERIFY(file.open(QIODevice::Append));file.write("DisplayNames=changed\n");}
        auto result=write(backend,state,{{layoutId(),{QKeySequence(QStringLiteral("Meta+Y"))}}});QCOMPARE(qvariant_cast<ShortcutsOutcome>(result[1]),ShortcutsOutcome::Failed);QCOMPARE(fake->registrations,0);
        state=read(backend);fake->kwinOwner=QStringLiteral(":1.100");fake->keyboardOwner=fake->kwinOwner;
        result=write(backend,state,{{layoutId(),{QKeySequence(QStringLiteral("Meta+Y"))}}});QCOMPARE(qvariant_cast<ShortcutsOutcome>(result[1]),ShortcutsOutcome::Failed);QCOMPARE(fake->registrations,0);QCOMPARE(fake->writes,0);
    }
    void registrationTimeoutLocksOwnerWithoutAssigningKeys(){
        QTemporaryDir dir;auto o=layoutOptions(dir);o.writeBudgetMs=180;auto *fake=new FakeShortcutsTransport;fake->stallRegister=true;KdeShortcutsBackend backend(fake,o);const auto state=read(backend);
        const auto result=write(backend,state,{{layoutId(),{QKeySequence(QStringLiteral("Meta+Y"))}}});QCOMPARE(qvariant_cast<ShortcutsOutcome>(result[1]),ShortcutsOutcome::Uncertain);QCOMPARE(fake->registrations,1);QCOMPARE(fake->writes,0);QCOMPARE(fake->reloads,0);QVERIFY(!read(backend).writable);
    }
    void reloadFailureReportsSavedBindingsAndUncertainty(){
        QTemporaryDir dir;auto o=layoutOptions(dir);auto *fake=new FakeShortcutsTransport;fake->config=o.config;fake->save();fake->reloadFailure=true;KdeShortcutsBackend backend(fake,o);auto state=read(backend);
        auto result=write(backend,state,{{layoutId(),{QKeySequence(QStringLiteral("Meta+Y"))}}});QCOMPARE(qvariant_cast<ShortcutsOutcome>(result[1]),ShortcutsOutcome::Partial);QCOMPARE(fake->reloads,1);QCOMPARE(qvariant_cast<ShortcutsSnapshot>(result[2]).find(layoutId())->current,QList<QKeySequence>{QKeySequence(QStringLiteral("Meta+Y"))});
        state=read(backend);fake->reloadUncertain=true;result=write(backend,state,{{layoutId(),{QKeySequence(QStringLiteral("Meta+J"))}}});QCOMPARE(qvariant_cast<ShortcutsOutcome>(result[1]),ShortcutsOutcome::Uncertain);QVERIFY(!read(backend).writable);
    }
    void KWinRestartDuringRegistrationStopsBeforeKeyAssignment(){
        QTemporaryDir dir;auto o=layoutOptions(dir);auto *fake=new FakeShortcutsTransport;KdeShortcutsBackend backend(fake,o);const auto state=read(backend);
        fake->beforeCall=[&](const auto &method){if(method==QLatin1String("doRegister")){fake->kwinOwner=QStringLiteral(":1.101");fake->keyboardOwner=fake->kwinOwner;}};
        const auto result=write(backend,state,{{layoutId(),{QKeySequence(QStringLiteral("Meta+Y"))}}});QCOMPARE(qvariant_cast<ShortcutsOutcome>(result[1]),ShortcutsOutcome::Partial);QCOMPARE(fake->registrations,1);QCOMPARE(fake->writes,0);QCOMPARE(fake->reloads,0);
    }
    void failedRegistrationReplyNeverAssignsOrReloads(){
        QTemporaryDir dir;auto o=layoutOptions(dir);auto *fake=new FakeShortcutsTransport;fake->registerFailure=true;KdeShortcutsBackend backend(fake,o);const auto state=read(backend);
        const auto result=write(backend,state,{{layoutId(),{QKeySequence(QStringLiteral("Meta+Y"))}}});QCOMPARE(qvariant_cast<ShortcutsOutcome>(result[1]),ShortcutsOutcome::Uncertain);QCOMPARE(fake->registrations,1);QCOMPARE(fake->writes,0);QCOMPARE(fake->reloads,0);
    }
    void layoutReassignmentSharesTheOrdinaryGlobalDraft(){
        QTemporaryDir dir;auto o=layoutOptions(dir);auto *fake=new FakeShortcutsTransport;fake->config=o.config;fake->save();KdeShortcutsBackend backend(fake,o);const auto state=read(backend);const auto source=fake->actions[1].id;const auto retained=fake->actions[1].current[1];
        auto result=write(backend,state,{{layoutId(),{QKeySequence(QStringLiteral("Meta+U"))}}});QCOMPARE(qvariant_cast<ShortcutsOutcome>(result[1]),ShortcutsOutcome::Failed);QCOMPARE(fake->writes,0);QCOMPARE(fake->registrations,0);
        result=write(backend,state,{{source,{retained}},{layoutId(),{QKeySequence(QStringLiteral("Meta+U"))}}});QCOMPARE(qvariant_cast<ShortcutsOutcome>(result[1]),ShortcutsOutcome::Success);QVERIFY(!fake->written.isEmpty());QVERIFY(fake->written.first().first==source);QCOMPARE(fake->reloads,1);QCOMPARE(qvariant_cast<ShortcutsSnapshot>(result[2]).find(source)->current,QList<QKeySequence>{retained});
    }
    void duplicateTranslatedLayoutIdentitiesFailClosed(){
        QTemporaryDir dir;auto o=layoutOptions(dir);auto *fake=new FakeShortcutsTransport;fake->layouts[1]=fake->layouts[0];KdeShortcutsBackend backend(fake,o);const auto state=read(backend);QVERIFY(state.available);QVERIFY(state.writable);QVERIFY(!state.layouts.available);QVERIFY(state.layouts.reason.contains(QStringLiteral("duplicate")));QVERIFY(!state.find(layoutId()));QCOMPARE(fake->registrations,0);
    }
    void privateBusTargetsNativeReloadAndRejectsWrongSenderAck(){
        QProcess daemon;daemon.start(QStringLiteral("dbus-daemon"),{QStringLiteral("--session"),QStringLiteral("--nofork"),QStringLiteral("--print-address=1")});QVERIFY(daemon.waitForStarted());QVERIFY(daemon.waitForReadyRead());const auto address=QString::fromUtf8(daemon.readLine()).trimmed();
        auto server=QDBusConnection::connectToBus(address,QStringLiteral("layout-reload-server"));auto client=QDBusConnection::connectToBus(address,QStringLiteral("layout-reload-client"));auto outsider=QDBusConnection::connectToBus(address,QStringLiteral("layout-reload-outsider"));QVERIFY(server.isConnected());QVERIFY(client.isConnected());QVERIFY(outsider.isConnected());
        qDBusRegisterMetaType<QByteArrayList>();qDBusRegisterMetaType<QHash<QString,QByteArrayList>>();qDBusRegisterMetaType<WireRuntimeLayout>();qDBusRegisterMetaType<QList<WireRuntimeLayout>>();
        WireRuntimeLayouts layouts,spoof;WireNativeReloadReceiver receiver,other;receiver.layouts=&layouts;QVERIFY(server.registerObject(QStringLiteral("/Layouts"),&layouts,QDBusConnection::ExportAllSlots|QDBusConnection::ExportAllSignals));QVERIFY(outsider.registerObject(QStringLiteral("/Layouts"),&spoof,QDBusConnection::ExportAllSlots|QDBusConnection::ExportAllSignals));
        QVERIFY(server.connect(QString(),QStringLiteral("/kxkbrc"),QStringLiteral("org.kde.kconfig.notify"),QStringLiteral("ConfigChanged"),&receiver,SLOT(notice(QHash<QString,QByteArrayList>,QDBusMessage))));
        QVERIFY(outsider.connect(QString(),QStringLiteral("/kxkbrc"),QStringLiteral("org.kde.kconfig.notify"),QStringLiteral("ConfigChanged"),&other,SLOT(notice(QHash<QString,QByteArrayList>,QDBusMessage))));
        auto *transport=createShortcutsDBusTransport(client);ShortcutsReply reply;bool done=false;
        transport->call(server.baseService(),QStringLiteral("/Layouts"),QStringLiteral("org.kde.KeyboardLayouts"),QStringLiteral("getLayoutsList"),{},[&](const auto &response){reply=response;done=true;});QTRY_VERIFY(done);QVERIFY(reply.success);QCOMPARE(reply.value.toList().size(),2);QCOMPARE(reply.value.toList()[0].toMap().value(QStringLiteral("longName")).toString(),QStringLiteral("Anglais (Colemak)"));
        done=false;transport->reloadLayouts(server.baseService(),[&](const auto &response){reply=response;done=true;});QTRY_VERIFY(done);QVERIFY2(reply.success,qPrintable(reply.error));QCOMPARE(receiver.notices,1);QCOMPARE(other.notices,0);QCOMPARE(receiver.sender,client.baseService());QCOMPARE(receiver.changes.value(QStringLiteral("Layout")),QByteArrayList{QByteArrayLiteral("LayoutList")});
        receiver.acknowledge=false;done=false;transport->reloadLayouts(server.baseService(),[&](const auto &response){reply=response;done=true;});QTRY_COMPARE(receiver.notices,2);emit spoof.layoutListChanged();QTest::qWait(40);QVERIFY(!done);QTRY_VERIFY_WITH_TIMEOUT(done,3000);QVERIFY(!reply.success);QVERIFY(reply.uncertain);QCOMPARE(other.notices,0);
        delete transport;server.unregisterObject(QStringLiteral("/Layouts"));outsider.unregisterObject(QStringLiteral("/Layouts"));QDBusConnection::disconnectFromBus(QStringLiteral("layout-reload-client"));QDBusConnection::disconnectFromBus(QStringLiteral("layout-reload-server"));QDBusConnection::disconnectFromBus(QStringLiteral("layout-reload-outsider"));daemon.terminate();daemon.waitForFinished();
    }
    void typedPrivateBusUsesV2SequenceWireFormat(){
        QProcess daemon;daemon.start(QStringLiteral("dbus-daemon"),{QStringLiteral("--session"),QStringLiteral("--nofork"),QStringLiteral("--print-address=1")});QVERIFY(daemon.waitForStarted());QVERIFY(daemon.waitForReadyRead());const auto address=QString::fromUtf8(daemon.readLine()).trimmed();
        auto server=QDBusConnection::connectToBus(address,QStringLiteral("shortcuts-test-server"));auto client=QDBusConnection::connectToBus(address,QStringLiteral("shortcuts-test-client"));QVERIFY(server.isConnected());QVERIFY(client.isConnected());
        qDBusRegisterMetaType<QKeySequence>();qDBusRegisterMetaType<QList<QKeySequence>>();qDBusRegisterMetaType<KGlobalAccel::MatchType>();qDBusRegisterMetaType<QList<QDBusObjectPath>>();qDBusRegisterMetaType<WireShortcutInfo>();qDBusRegisterMetaType<QList<WireShortcutInfo>>();
        WireShortcutDaemon object;WireShortcutComponent component;QVERIFY(server.registerService(QStringLiteral("org.kde.kglobalaccel")));QVERIFY(server.registerObject(QStringLiteral("/kglobalaccel"),&object,QDBusConnection::ExportAllSlots));QVERIFY(server.registerObject(QStringLiteral("/component/wire"),&component,QDBusConnection::ExportAllSlots));
        QTemporaryDir dir;KdeShortcutsBackend backend(createShortcutsDBusTransport(client),options(dir));const auto state=read(backend);QVERIFY2(state.available,qPrintable(state.reason));QCOMPARE(state.actions.size(),1);QCOMPARE(state.actions[0].current[1].count(),2);QCOMPARE(state.actions[0].defaults[0].count(),2);QCOMPARE(object.writes,0);
        auto *transport=createShortcutsDBusTransport(client);bool done=false;ShortcutsReply response;const QList<QKeySequence> keys{QKeySequence(QStringLiteral("Ctrl+J, Ctrl+M"))};
        transport->call(server.baseService(),QStringLiteral("/kglobalaccel"),QStringLiteral("org.kde.KGlobalAccel"),QStringLiteral("setForeignShortcutKeys"),{state.actions[0].id.nativeId(),QVariant::fromValue(keys)},[&](const auto &reply){response=reply;done=true;});QTRY_VERIFY(done);QVERIFY(response.success);QCOMPARE(object.keys,keys);QCOMPARE(object.writes,1);delete transport;
        server.unregisterService(QStringLiteral("org.kde.kglobalaccel"));server.unregisterObject(QStringLiteral("/kglobalaccel"));server.unregisterObject(QStringLiteral("/component/wire"));QDBusConnection::disconnectFromBus(QStringLiteral("shortcuts-test-client"));QDBusConnection::disconnectFromBus(QStringLiteral("shortcuts-test-server"));daemon.terminate();daemon.waitForFinished();
    }
};
QTEST_MAIN(ShortcutsBackendTest)
#include "shortcuts_backend_test.moc"
