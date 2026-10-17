#include <QtTest>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMetaType>
#include <QProcess>
#include <QDir>
#include <QTemporaryDir>
#include <KConfig>
#include <KConfigGroup>
#include "platform/keyboard/KeyboardBackend.h"

class FakeKeyboardTransport final : public KeyboardTransport {
public:
    QString owner=QStringLiteral(":1.42"),keyboardOwner=owner,version=QStringLiteral("6.6.6");
    bool notificationSuccess=true,stall=false,badInterface=false;
    int calls=0,notifications=0;
    QStringList destinations,configs,groups;
    QList<QStringList> notifiedKeys;
    std::function<void(int)> beforeCall;
    void call(const QString &destination,const QString &,const QString &,const QString &method,
              const QVariantList &args,Completion completion) override {
        ++calls;destinations<<destination;
        if(beforeCall)beforeCall(calls);
        if(stall)return;
        KeyboardReply r;r.success=true;
        if(method==QLatin1String("GetNameOwner"))r.value=args.first().toString()==QLatin1String("org.kde.keyboard")?keyboardOwner:owner;
        else if(method==QLatin1String("supportInformation"))r.value=QStringLiteral("KWin version: ")+version+QLatin1Char('\n');
        else if(method==QLatin1String("Introspect"))r.value=badInterface?QStringLiteral("<node/>"):QStringLiteral("<node><interface name=\"org.kde.KeyboardLayouts\"><method name=\"getLayoutsList\"><arg type=\"a(sss)\" direction=\"out\"/></method></interface></node>");
        else if(method==QLatin1String("getLayoutsList"))r.value=QVariantList{QVariantMap{{"shortName","us"},{"displayName","US"},{"longName","English (US)"}}};
        else r.success=false;
        QTimer::singleShot(0,this,[completion=std::move(completion),r]{completion(r);});
    }
    bool notify(const QString &destination,const QString &config,const QString &group,const QStringList &keys) override {
        destinations<<destination;configs<<config;groups<<group;notifiedKeys<<keys;++notifications;return notificationSuccess;
    }
};
struct WireKeyboardLayout { QString shortName,displayName,longName; };
Q_DECLARE_METATYPE(WireKeyboardLayout)
Q_DECLARE_METATYPE(QList<WireKeyboardLayout>)
QDBusArgument &operator<<(QDBusArgument &arg,const WireKeyboardLayout &layout)
{arg.beginStructure();arg<<layout.shortName<<layout.displayName<<layout.longName;arg.endStructure();return arg;}
const QDBusArgument &operator>>(const QDBusArgument &arg,WireKeyboardLayout &layout)
{arg.beginStructure();arg>>layout.shortName>>layout.displayName>>layout.longName;arg.endStructure();return arg;}
class WireKeyboardKWin final:public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface","org.kde.KWin")
public slots:
    QString supportInformation()const{return QStringLiteral("KWin version: 6.6.6\n");}
};
class WireKeyboardLayouts final:public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface","org.kde.KeyboardLayouts")
public slots:
    QList<WireKeyboardLayout> getLayoutsList()const{return {{QStringLiteral("us"),QStringLiteral("U"),QStringLiteral("English (US)")}};}
};
class WireConfigReceiver final:public QObject {
    Q_OBJECT
public:
    QHash<QString,QByteArrayList> changes;
    int count=0;
public slots:
    void notice(const QHash<QString,QByteArrayList> &value){changes=value;++count;}
};
class KeyboardBackendTest final:public QObject {
    Q_OBJECT
    static KeyboardBackendOptions options(QTemporaryDir &dir) {
        KeyboardBackendOptions result;result.inputConfig=dir.filePath(QStringLiteral("kcminputrc"));
        result.layoutConfig=dir.filePath(QStringLiteral("kxkbrc"));result.workspaceConfig=dir.filePath(QStringLiteral("plasmarc"));result.isolated=true;
        result.registryPath=dir.filePath(QStringLiteral("evdev.xml"));
        QFile registry(result.registryPath);registry.open(QIODevice::WriteOnly);
        registry.write("<xkbConfigRegistry><modelList><model><configItem><name>pc104</name><description>Generic 104</description></configItem></model><model><configItem><name>pc105</name><description>Generic 105</description></configItem></model></modelList><layoutList><layout><configItem><name>us</name><description>English US</description></configItem><variantList><variant><configItem><name>dvorak</name><description>Dvorak</description></configItem></variant></variantList></layout></layoutList><optionList><group allowMultipleSelection=\"true\"><configItem><name>compose</name><description>Compose key</description></configItem><option><configItem><name>compose:ralt</name><description>Right Alt</description></configItem></option></group></optionList></xkbConfigRegistry>");
        registry.close();return result;
    }
    static KeyboardSnapshot read(KdeKeyboardBackend &backend) {
        QSignalSpy spy(&backend,&KeyboardBackend::readFinished);backend.readCurrent(1);
        if(spy.isEmpty())spy.wait(3000);
        if(spy.isEmpty())return {};
        return qvariant_cast<KeyboardSnapshot>(spy.first().at(2));
    }
    static QList<QVariant> write(KdeKeyboardBackend &backend,const KeyboardSnapshot &expected,const QVariantMap &values) {
        QSignalSpy spy(&backend,&KeyboardBackend::writeFinished);backend.writePreferences(2,expected,values);
        if(spy.isEmpty())spy.wait(6000);
        return spy.isEmpty()?QList<QVariant>{}:spy.first();
    }
private slots:
    void initTestCase(){qRegisterMetaType<KeyboardSnapshot>();qRegisterMetaType<KeyboardOutcome>();}
    void readAndSaveBothFilesPreservesUnknown() {
        QTemporaryDir dir;auto o=options(dir);
        {KConfig c(o.inputConfig,KConfig::SimpleConfig);KConfigGroup g(&c,"Other");g.writeEntry("Keep","value");QVERIFY(c.sync());}
        {KConfig c(o.layoutConfig,KConfig::SimpleConfig);KConfigGroup g(&c,"Layout");g.writeEntry("VendorFuture","preserve");g.writeEntry("Options",QStringList{"vendor:unknown"});QVERIFY(c.sync());}
        auto *transport=new FakeKeyboardTransport;KdeKeyboardBackend backend(transport,o);
        auto state=read(backend);QVERIFY(state.available);QVERIFY2(state.writable,qPrintable(state.reason));QVERIFY(state.runtimeLayoutsVerified);
        {KConfig workspace(o.workspaceConfig,KConfig::SimpleConfig);KConfigGroup(&workspace,"OSD").writeEntry("VendorUnknown",42);QVERIFY(workspace.sync());}
        state=read(backend);
        auto requested=state.values;requested["kbdLayoutChangedEnabled"]=false;requested["RepeatDelay"]=700;requested["RepeatRate"]=31.5;requested["Model"]="pc105";
        auto result=write(backend,state,requested);QVERIFY(!result.isEmpty());QCOMPARE(qvariant_cast<KeyboardOutcome>(result.at(1)),KeyboardOutcome::Success);
        auto saved=qvariant_cast<KeyboardSnapshot>(result.at(2));QCOMPARE(saved.values.value("RepeatDelay").toInt(),700);QCOMPARE(saved.values.value("RepeatRate").toDouble(),31.5);
        QCOMPARE(saved.values.value("Options").toStringList(),QStringList{"vendor:unknown"});
        QVERIFY(saved.runtimeStatus.contains("not expose"));QCOMPARE(transport->notifications,2);
        QVERIFY(transport->configs.contains("kcminputrc"));QVERIFY(transport->configs.contains("kxkbrc"));
        KConfig input(o.inputConfig,KConfig::SimpleConfig),layout(o.layoutConfig,KConfig::SimpleConfig),workspace(o.workspaceConfig,KConfig::SimpleConfig);
        QCOMPARE(KConfigGroup(&workspace,"OSD").readEntry("VendorUnknown",0),42);
        QCOMPARE(KConfigGroup(&workspace,"OSD").readEntry("kbdLayoutChangedEnabled",true),false);
        QCOMPARE(KConfigGroup(&input,"Other").readEntry("Keep",QString()),QStringLiteral("value"));
        QCOMPARE(KConfigGroup(&layout,"Layout").readEntry("VendorFuture",QString()),QStringLiteral("preserve"));
        for(const auto &destination:transport->destinations)QVERIFY(destination=="org.freedesktop.DBus"||destination==transport->owner);
    }
    void failedSyncCannotRetryAfterFailureObserverRestoresDestination() {
        QTemporaryDir dir; auto o=options(dir);const auto parent=dir.filePath("input"),parked=dir.filePath("input-parked");
        QVERIFY(QDir().mkpath(parent));o.inputConfig=parent+"/kcminputrc";
        const QByteArray original="[Keyboard]\nRepeatDelay=600\n[Other]\nKeep=original\n";
        {QFile file(o.inputConfig);QVERIFY(file.open(QIODevice::WriteOnly));QCOMPARE(file.write(original),qint64(original.size()));}
        bool dirty=false,blocked=false,syncFailed=false,restored=false;int notifications=-1;
        o.syncForTest=[&](KConfig &config){
            dirty=config.isDirty();if(!QDir().rename(parent,parked))return false;
            QFile blocker(parent);blocked=blocker.open(QIODevice::WriteOnly);blocker.close();
            const bool result=config.sync();syncFailed=!result;return result;
        };
        {
            auto *transport=new FakeKeyboardTransport;KdeKeyboardBackend backend(transport,o);const auto baseline=read(backend);QVERIFY(baseline.writable);
            connect(&backend,&KeyboardBackend::writeFinished,this,[&](quint64,KeyboardOutcome outcome,const KeyboardSnapshot &,const QString &){
                if(outcome!=KeyboardOutcome::Success&&blocked)restored=QFile::remove(parent)&&QDir().rename(parked,parent);
            });
            auto requested=baseline.values;requested["RepeatDelay"]=900;const auto result=write(backend,baseline,requested);
            QVERIFY(!result.isEmpty());QCOMPARE(qvariant_cast<KeyboardOutcome>(result.at(1)),KeyboardOutcome::Uncertain);
            QVERIFY(dirty&&blocked&&syncFailed&&restored);notifications=transport->notifications;
        }
        QCoreApplication::processEvents();QFile saved(o.inputConfig);QVERIFY(saved.open(QIODevice::ReadOnly));QCOMPARE(saved.readAll(),original);QCOMPARE(notifications,0);
    }
    void ownerAndVersionMustMatch() {
        QTemporaryDir dir;auto o=options(dir);auto *transport=new FakeKeyboardTransport;KdeKeyboardBackend backend(transport,o);
        transport->keyboardOwner=":1.77";auto state=read(backend);QVERIFY(state.available);QVERIFY(!state.writable);
        transport->keyboardOwner=transport->owner;transport->version="6.7.0";state=read(backend);QVERIFY(!state.writable);
        transport->version="6.6.6";transport->badInterface=true;state=read(backend);QVERIFY(!state.writable);
        QVERIFY(!QFileInfo::exists(o.inputConfig));QCOMPARE(transport->notifications,0);
    }
    void staleBaselineRejectedWithoutWrite() {
        QTemporaryDir dir;auto o=options(dir);auto *transport=new FakeKeyboardTransport;KdeKeyboardBackend backend(transport,o);
        auto state=read(backend);QVERIFY(state.writable);
        {KConfig c(o.inputConfig,KConfig::SimpleConfig);KConfigGroup(&c,"Other").writeEntry("Changed",true);QVERIFY(c.sync());}
        auto values=state.values;values["RepeatDelay"]=800;
        auto result=write(backend,state,values);QVERIFY(!result.isEmpty());QCOMPARE(qvariant_cast<KeyboardOutcome>(result.at(1)),KeyboardOutcome::Failed);QCOMPARE(transport->notifications,0);
        QCOMPARE(read(backend).values.value("RepeatDelay").toInt(),600);
    }
    void immutableKeysRespected() {
        QTemporaryDir dir;auto o=options(dir);QFile config(o.inputConfig);QVERIFY(config.open(QIODevice::WriteOnly));config.write("[Keyboard]\nRepeatDelay[$i]=750\n");config.close();
        auto *transport=new FakeKeyboardTransport;KdeKeyboardBackend backend(transport,o);auto state=read(backend);
        QVERIFY(state.immutableKeys.contains("RepeatDelay"));auto values=state.values;values["RepeatDelay"]=900;
        auto result=write(backend,state,values);QVERIFY(!result.isEmpty());QCOMPARE(qvariant_cast<KeyboardOutcome>(result.at(1)),KeyboardOutcome::Failed);QCOMPARE(transport->notifications,0);
    }
    void ownerChangesBetweenWritesReportsPartial() {
        QTemporaryDir dir;auto o=options(dir);auto *transport=new FakeKeyboardTransport;KdeKeyboardBackend backend(transport,o);auto state=read(backend);QVERIFY(state.writable);
        const int initial=transport->calls;
        transport->beforeCall=[transport,initial](int count){if(count==initial+8){transport->owner=":1.99";transport->keyboardOwner=transport->owner;}};
        auto values=state.values;values["Model"]="pc105";values["RepeatDelay"]=800;
        auto result=write(backend,state,values);QVERIFY(!result.isEmpty());QCOMPARE(qvariant_cast<KeyboardOutcome>(result.at(1)),KeyboardOutcome::Partial);
        auto saved=qvariant_cast<KeyboardSnapshot>(result.at(2));QCOMPARE(saved.values.value("Model").toString(),QStringLiteral("pc105"));QCOMPARE(saved.values.value("RepeatDelay").toInt(),600);
    }
    void thirdFileConflictReportsEarlierSavedFiles() {
        QTemporaryDir dir;auto o=options(dir);auto *transport=new FakeKeyboardTransport;KdeKeyboardBackend backend(transport,o);auto state=read(backend);QVERIFY(state.writable);
        const int initial=transport->calls;
        transport->beforeCall=[transport,initial](int count){if(count==initial+15){transport->owner=":1.99";transport->keyboardOwner=transport->owner;}};
        auto values=state.values;values["Model"]="pc105";values["RepeatDelay"]=800;values["kbdLayoutChangedEnabled"]=false;
        auto result=write(backend,state,values);QVERIFY(!result.isEmpty());QCOMPARE(qvariant_cast<KeyboardOutcome>(result.at(1)),KeyboardOutcome::Partial);
        auto saved=qvariant_cast<KeyboardSnapshot>(result.at(2));QCOMPARE(saved.values.value("Model").toString(),QStringLiteral("pc105"));QCOMPARE(saved.values.value("RepeatDelay").toInt(),800);QCOMPARE(saved.values.value("kbdLayoutChangedEnabled").toBool(),true);
        QVERIFY(!QFileInfo::exists(o.workspaceConfig));QCOMPARE(transport->notifications,0);
    }
    void reloadFailureIsUncertainButReadbackTrue() {
        QTemporaryDir dir;auto o=options(dir);auto *transport=new FakeKeyboardTransport;KdeKeyboardBackend backend(transport,o);auto state=read(backend);QVERIFY(state.writable);
        transport->notificationSuccess=false;auto values=state.values;values["NumLock"]=0;
        auto result=write(backend,state,values);QVERIFY(!result.isEmpty());QCOMPARE(qvariant_cast<KeyboardOutcome>(result.at(1)),KeyboardOutcome::Uncertain);
        QCOMPARE(qvariant_cast<KeyboardSnapshot>(result.at(2)).values.value("NumLock").toInt(),0);
    }
    void readAndApplyBudgetsBoundSilentTransport() {
        QTemporaryDir dir;auto o=options(dir);o.readBudgetMs=30;o.writeBudgetMs=50;
        auto *transport=new FakeKeyboardTransport;KdeKeyboardBackend backend(transport,o);auto state=read(backend);QVERIFY(state.writable);
        transport->stall=true;auto values=state.values;values["RepeatDelay"]=900;
        auto result=write(backend,state,values);QVERIFY(!result.isEmpty());QCOMPARE(qvariant_cast<KeyboardOutcome>(result.at(1)),KeyboardOutcome::Failed);
        state=read(backend);QVERIFY(state.available);QVERIFY(!state.writable);QCOMPARE(transport->notifications,0);
    }
    void privateBusWireAndNativeNotification() {
        QProcess process;process.start(QStringLiteral("dbus-daemon"),{QStringLiteral("--session"),QStringLiteral("--nofork"),QStringLiteral("--print-address=1")});
        QVERIFY(process.waitForStarted());QVERIFY(process.waitForReadyRead());const QString address=QString::fromUtf8(process.readLine()).trimmed();
        const QString serverName=QStringLiteral("keyboard-test-server"),clientName=QStringLiteral("keyboard-test-client");
        auto server=QDBusConnection::connectToBus(address,serverName);auto client=QDBusConnection::connectToBus(address,clientName);
        QVERIFY(server.isConnected());QVERIFY(client.isConnected());qDBusRegisterMetaType<WireKeyboardLayout>();qDBusRegisterMetaType<QList<WireKeyboardLayout>>();qDBusRegisterMetaType<QByteArrayList>();qDBusRegisterMetaType<QHash<QString,QByteArrayList>>();
        WireKeyboardKWin kwin;WireKeyboardLayouts layouts;WireConfigReceiver receiver;
        QVERIFY(server.registerService("org.kde.KWin"));QVERIFY(server.registerService("org.kde.keyboard"));
        QVERIFY(server.registerObject("/KWin",&kwin,QDBusConnection::ExportAllSlots));QVERIFY(server.registerObject("/Layouts",&layouts,QDBusConnection::ExportAllSlots));
        QVERIFY(server.connect(QString(),"/kcminputrc","org.kde.kconfig.notify","ConfigChanged",&receiver,SLOT(notice(QHash<QString,QByteArrayList>))));
        QTemporaryDir dir;auto o=options(dir);auto *transport=createKeyboardDBusTransport(client);KdeKeyboardBackend backend(transport,o);auto state=read(backend);QVERIFY2(state.writable,qPrintable(state.reason));
        QCOMPARE(state.runtimeLayouts.size(),1);QCOMPARE(state.runtimeLayouts.first().shortName,QStringLiteral("us"));
        auto values=state.values;values["RepeatDelay"]=850;auto result=write(backend,state,values);QVERIFY(!result.isEmpty());QCOMPARE(qvariant_cast<KeyboardOutcome>(result.at(1)),KeyboardOutcome::Success);
        QTRY_COMPARE(receiver.count,1);QCOMPARE(receiver.changes.value("Keyboard"),QByteArrayList{"RepeatDelay"});
        QDBusConnection::disconnectFromBus(clientName);QDBusConnection::disconnectFromBus(serverName);process.terminate();QVERIFY(process.waitForFinished());
    }
};
QTEST_GUILESS_MAIN(KeyboardBackendTest)
#include "keyboard_backend_test.moc"
