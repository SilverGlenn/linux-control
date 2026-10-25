#include "operations/OperationManager.h"
#include <QLockFile>
#include <QPointer>
#include <QSemaphore>
#include <QSaveFile>
#include <QThread>
#include <atomic>
#include <unistd.h>
#include <sys/stat.h>
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

struct AsyncGate {
    std::atomic_bool armed{true},entered{false};
    QSemaphore released;
    void once(){if(armed.exchange(false)){entered=true;released.acquire();}}
};

class KeyboardBackendTest final:public QObject {
    Q_OBJECT
    QList<std::shared_ptr<AsyncGate>> gates;
    std::shared_ptr<AsyncGate> gate(){auto result=std::make_shared<AsyncGate>();gates<<result;return result;}
    static QByteArray bytes(const QString &path){QFile file(path);return file.open(QIODevice::ReadOnly)?file.readAll():QByteArray();}
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
    void cleanup(){for(const auto &held:gates)held->released.release(32);QTRY_VERIFY_WITH_TIMEOUT(!OperationManager::instance()->hasActiveJobs(),10000);gates.clear();}
    void initTestCase(){qRegisterMetaType<KeyboardSnapshot>();qRegisterMetaType<KeyboardOutcome>();}
    void nonregularConfigAndRegistrySourcesFailClosed_data(){
        QTest::addColumn<int>("source");QTest::newRow("config-fifo")<<0;QTest::newRow("config-broken-link")<<1;
        QTest::newRow("registry-fifo")<<2;QTest::newRow("extras-broken-link")<<3;
    }
    void nonregularConfigAndRegistrySourcesFailClosed()
    {
        QFETCH(int,source);QTemporaryDir directory;auto config=options(directory);
        const auto path=source==2?config.registryPath:source==3?directory.filePath("evdev.extras.xml"):config.inputConfig;
        if(QFileInfo::exists(path))QVERIFY(QFile::remove(path));
        if(source==1||source==3)QVERIFY(::symlink("missing-keyboard-source",QFile::encodeName(path).constData())==0);
        else QVERIFY(::mkfifo(QFile::encodeName(path).constData(),0600)==0);
        auto *transport=new FakeKeyboardTransport;KdeKeyboardBackend backend(transport,config);QSignalSpy done(&backend,&KeyboardBackend::readFinished);backend.readCurrent(1);
        QTRY_COMPARE_WITH_TIMEOUT(done.size(),1,2000);QVERIFY(!done[0][1].toBool());QVERIFY(!done[0][3].toString().isEmpty());QCOMPARE(transport->notifications,0);
    }
    void sameByteSourceReplacementAfterSyncPreventsNotification()
    {
        QTemporaryDir directory;auto config=options(directory);const auto held=gate();config.afterSync=[held]{held->once();};auto *transport=new FakeKeyboardTransport;
        KdeKeyboardBackend backend(transport,config);const auto before=read(backend);auto requested=before.values;requested["RepeatDelay"]=800;QSignalSpy done(&backend,&KeyboardBackend::writeFinished);backend.writePreferences(2,before,requested);QTRY_VERIFY(held->entered.load());
        const auto path=config.registryPath;const auto same=bytes(path);QSaveFile file(path);QVERIFY(file.open(QIODevice::WriteOnly));QCOMPARE(file.write(same),same.size());QVERIFY(file.commit());
        held->released.release();QTRY_COMPARE(done.size(),1);QVERIFY(qvariant_cast<KeyboardOutcome>(done[0][1])!=KeyboardOutcome::Success);QCOMPARE(transport->notifications,0);QCOMPARE(bytes(path),same);
    }
    void nativeLockWaitHasHeartbeatSoftDeadlineAndDelayedSuccess()
    {
        QTemporaryDir directory;auto config=options(directory);config.writeBudgetMs=20;auto *transport=new FakeKeyboardTransport;KdeKeyboardBackend backend(transport,config);const auto before=read(backend);QVERIFY(before.writable);
        QLockFile lock(config.inputConfig+".lock");QVERIFY(lock.tryLock());int beats=0;QTimer heartbeat;heartbeat.setInterval(1);connect(&heartbeat,&QTimer::timeout,this,[&]{++beats;});heartbeat.start();
        QSignalSpy done(&backend,&KeyboardBackend::writeFinished),pending(&backend,&KeyboardBackend::operationPending);auto next=before.values;next["RepeatDelay"]=900;
        QTimer unlock;unlock.setSingleShot(true);connect(&unlock,&QTimer::timeout,this,[&]{lock.unlock();});unlock.start(180);
        backend.writePreferences(2,before,next);QTRY_VERIFY(!pending.isEmpty());QVERIFY(beats>=3);QCOMPARE(done.size(),0);QTRY_COMPARE_WITH_TIMEOUT(done.size(),1,10000);
        QCOMPARE(qvariant_cast<KeyboardOutcome>(done[0][1]),KeyboardOutcome::Success);QCOMPARE(transport->notifications,1);QCOMPARE(read(backend).values.value("RepeatDelay").toInt(),900);QTest::qWait(30);QCOMPARE(done.size(),1);
    }
    void orphanedWriteRetainsTransportActivityAndLease()
    {
        QTemporaryDir directory;auto config=options(directory);config.writeBudgetMs=20;const auto held=gate();config.beforeSync=[held]{held->once();};
        auto *transport=new FakeKeyboardTransport;QPointer<FakeKeyboardTransport> retained(transport);auto *first=new KdeKeyboardBackend(transport,config);const auto before=read(*first);QVERIFY(before.writable);
        auto next=before.values;next["RepeatDelay"]=900;QSignalSpy gone(first,&KeyboardBackend::writeFinished);first->writePreferences(2,before,next);QTRY_VERIFY(held->entered.load());delete first;
        QVERIFY(retained);QVERIFY(OperationManager::instance()->hasActiveJobs());auto secondOptions=config;secondOptions.beforeSync={};secondOptions.inputConfig=directory.filePath("./kcminputrc");
        KdeKeyboardBackend second(new FakeKeyboardTransport,secondOptions);auto competing=before.values;competing["RepeatDelay"]=800;const auto refused=write(second,before,competing);QVERIFY(!refused.isEmpty());QCOMPARE(qvariant_cast<KeyboardOutcome>(refused[1]),KeyboardOutcome::Failed);
        held->released.release();QTRY_VERIFY_WITH_TIMEOUT(!OperationManager::instance()->hasActiveJobs(),10000);QTRY_VERIFY(!retained);QCOMPARE(gone.size(),0);
        const auto actual=read(second);QCOMPARE(actual.values.value("RepeatDelay").toInt(),900);const auto retry=write(second,actual,competing);QVERIFY(!retry.isEmpty());QCOMPARE(qvariant_cast<KeyboardOutcome>(retry[1]),KeyboardOutcome::Success);
    }
    void cancelOrProviderReplacementBeforeNativeSyncDoesNotWrite_data(){QTest::addColumn<bool>("replace");QTest::newRow("cancel")<<false;QTest::newRow("provider")<<true;}
    void cancelOrProviderReplacementBeforeNativeSyncDoesNotWrite()
    {
        QFETCH(bool,replace);QTemporaryDir directory;auto config=options(directory);const auto held=gate();config.beforeSync=[held]{held->once();};auto *transport=new FakeKeyboardTransport;
        KdeKeyboardBackend backend(transport,config);const auto before=read(backend);QVERIFY(before.writable);auto next=before.values;next["RepeatDelay"]=900;
        QSignalSpy done(&backend,&KeyboardBackend::writeFinished);backend.writePreferences(2,before,next);QTRY_VERIFY(held->entered.load());
        if(replace){transport->owner=transport->keyboardOwner=":1.replacement";emit transport->changed();}else backend.cancel();QCOMPARE(done.size(),0);QVERIFY(OperationManager::instance()->hasActiveJobs());
        held->released.release();QTRY_COMPARE(done.size(),1);QCOMPARE(qvariant_cast<KeyboardOutcome>(done[0][1]),KeyboardOutcome::Failed);QCOMPARE(transport->notifications,0);QVERIFY(!QFileInfo::exists(config.inputConfig));
    }
    void cancelOrReplacementDuringNativeWaitCannotReleaseReservationEarly_data(){cancelOrProviderReplacementBeforeNativeSyncDoesNotWrite_data();}
    void cancelOrReplacementDuringNativeWaitCannotReleaseReservationEarly()
    {
        QFETCH(bool,replace);QTemporaryDir directory;auto config=options(directory);config.writeBudgetMs=20;auto entered=std::make_shared<std::atomic_bool>(false);config.syncForTest=[entered](KConfig &config){*entered=true;return config.sync();};
        auto *transport=new FakeKeyboardTransport;KdeKeyboardBackend backend(transport,config);const auto before=read(backend);QVERIFY(before.writable);auto next=before.values;next["RepeatDelay"]=900;
        QLockFile lock(config.inputConfig+".lock");QVERIFY(lock.tryLock());QSignalSpy done(&backend,&KeyboardBackend::writeFinished),pending(&backend,&KeyboardBackend::operationPending);backend.writePreferences(2,before,next);
        QTRY_VERIFY(entered->load());QTRY_VERIFY(!pending.isEmpty());if(replace){transport->owner=transport->keyboardOwner=":1.replacement";emit transport->changed();}else backend.cancel();
        auto other=config;other.syncForTest={};KdeKeyboardBackend second(new FakeKeyboardTransport,other);auto competing=before.values;competing["RepeatDelay"]=750;const auto refused=write(second,before,competing);QVERIFY(!refused.isEmpty());QCOMPARE(qvariant_cast<KeyboardOutcome>(refused[1]),KeyboardOutcome::Failed);QCOMPARE(done.size(),0);
        lock.unlock();QTRY_COMPARE_WITH_TIMEOUT(done.size(),1,10000);QVERIFY(qvariant_cast<KeyboardOutcome>(done[0][1])!=KeyboardOutcome::Success);QCOMPARE(transport->notifications,0);QCOMPARE(read(second).values.value("RepeatDelay").toInt(),900);
    }
    void failedTerminalReadbackCannotAdvertiseKnownWritableState()
    {
        QTemporaryDir directory;auto config=options(directory);auto failRead=std::make_shared<std::atomic_bool>(false),poisoned=std::make_shared<std::atomic_bool>(false);
        const auto target=config.inputConfig;config.beforeRead=[failRead,poisoned,target]{
            if(failRead->load()&&!poisoned->exchange(true)){QFile::remove(target);QDir().mkpath(target);}
        };
        auto *transport=new FakeKeyboardTransport;KdeKeyboardBackend backend(transport,config);const auto before=read(backend);QVERIFY(before.writable);
        const auto initial=transport->calls;
        // Seven audited transport calls per snapshot: pre-write, post-write,
        // then post-notification observation. Poison only the following terminal read.
        transport->beforeCall=[failRead,initial](int call){if(call==initial+21)*failRead=true;};
        auto next=before.values;next["RepeatDelay"]=900;const auto result=write(backend,before,next);QVERIFY(!result.isEmpty());QVERIFY(poisoned->load());
        QCOMPARE(qvariant_cast<KeyboardOutcome>(result[1]),KeyboardOutcome::Uncertain);const auto state=qvariant_cast<KeyboardSnapshot>(result[2]);
        QVERIFY(!state.available);QVERIFY(!state.writable);QVERIFY(result[3].toString().contains("Terminal keyboard readback failed"));QCOMPARE(transport->notifications,1);
    }
    void foreignEditAfterSyncIsPreservedAndPreventsNotification()
    {
        QTemporaryDir directory;auto config=options(directory);const auto held=gate();config.afterSync=[held]{held->once();};auto *transport=new FakeKeyboardTransport;KdeKeyboardBackend backend(transport,config);
        const auto before=read(backend);auto next=before.values;next["RepeatDelay"]=900;QSignalSpy done(&backend,&KeyboardBackend::writeFinished);backend.writePreferences(2,before,next);
        QTRY_VERIFY(held->entered.load());{QFile file(config.inputConfig);QVERIFY(file.open(QIODevice::Append));file.write("\n[Unrelated]\nKeep=foreign\n");}
        held->released.release();QTRY_COMPARE(done.size(),1);QVERIFY(qvariant_cast<KeyboardOutcome>(done[0][1])!=KeyboardOutcome::Success);QCOMPARE(transport->notifications,0);QVERIFY(bytes(config.inputConfig).contains("Keep=foreign"));
    }
    void intermediateNativeInodeAliasRemainsReserved()
    {
        QTemporaryDir directory,other;auto config=options(directory),otherOptions=options(other);
        {QFile file(config.inputConfig);QVERIFY(file.open(QIODevice::WriteOnly));file.write("[Keyboard]\nRepeatDelay=600\nRepeatRate=25\n");}
        const auto held=gate();auto count=std::make_shared<std::atomic_int>(0);auto linked=std::make_shared<std::atomic_bool>(false);const auto alias=directory.filePath("middle-inode"),target=config.inputConfig;
        config.afterSync=[held,count,linked,alias,target]{const auto step=++*count;if(step==1)*linked=::link(QFile::encodeName(target).constData(),QFile::encodeName(alias).constData())==0;if(step==2)held->once();};
        KdeKeyboardBackend first(new FakeKeyboardTransport,config);const auto before=read(first);QVERIFY(before.writable);auto next=before.values;next["RepeatDelay"]=800;next["RepeatRate"]=30.0;
        QSignalSpy done(&first,&KeyboardBackend::writeFinished);first.writePreferences(2,before,next);QTRY_VERIFY(held->entered.load());QVERIFY(linked->load());QVERIFY(::link(QFile::encodeName(alias).constData(),QFile::encodeName(otherOptions.inputConfig).constData())==0);
        KdeKeyboardBackend second(new FakeKeyboardTransport,otherOptions);const auto secondBefore=read(second);QVERIFY(secondBefore.writable);auto competing=secondBefore.values;competing["RepeatDelay"]=950;
        const auto refused=write(second,secondBefore,competing);QVERIFY(!refused.isEmpty());QCOMPARE(qvariant_cast<KeyboardOutcome>(refused[1]),KeyboardOutcome::Failed);
        held->released.release();QTRY_COMPARE(done.size(),1);QCOMPARE(qvariant_cast<KeyboardOutcome>(done[0][1]),KeyboardOutcome::Success);const auto retry=write(second,secondBefore,competing);QVERIFY(!retry.isEmpty());QCOMPARE(qvariant_cast<KeyboardOutcome>(retry[1]),KeyboardOutcome::Success);
    }
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
