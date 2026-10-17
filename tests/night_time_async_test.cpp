#include <QtTest>
#include "platform/kde/NightTimeBackend.h"
#include "operations/OperationManager.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSaveFile>
#include <QSemaphore>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <atomic>
#include <memory>
#include <sys/stat.h>
#include <unistd.h>

namespace {
struct WorkerGate {
    std::atomic<bool> armed{true};QSemaphore entered,released;
    void waitOnce(){if(armed.exchange(false)){entered.release();released.acquire();}}
};
class AsyncNightTimeService final:public NightTimeService {
public:
    NightTimeRuntime state;int queries=0,holdAt=0;quint64 heldToken=0;QString heldOwner;
    AsyncNightTimeService(){state.installedVerified=true;state.version=QStringLiteral("6.6.5");state.epoch=1;}
    void online(){state.serviceAvailable=state.providerVerified=true;state.apiVersion=1;state.owner=QStringLiteral(":private-fixture.1");}
    void query(quint64 token,const QString &owner={})override{
        ++queries;if(queries==holdAt){heldToken=token;heldOwner=owner;return;}deliver(token,owner);
    }
    void deliver(quint64 token,const QString &owner){
        const bool valid=state.installedVerified&&(!state.serviceAvailable||state.providerVerified)&&(owner.isEmpty()||owner==state.owner);
        emit queryFinished(token,valid,state,valid?QString():QStringLiteral("Private fixture identity changed"));
    }
    void release(){const auto token=heldToken;heldToken=0;deliver(token,heldOwner);}
};
}
class NightTimeAsyncTest final:public QObject {
    Q_OBJECT
    QTemporaryDir directory;QString path;quint64 token=0;
    QList<QPointer<KdeNightTimeBackend>> backends;QList<QPointer<AsyncNightTimeService>> services;QList<std::shared_ptr<WorkerGate>> gates;
    std::shared_ptr<WorkerGate> gate(){auto result=std::make_shared<WorkerGate>();gates<<result;return result;}
    void save(const QString &file,const QByteArray &bytes){QSaveFile output(file);QVERIFY(output.open(QIODevice::WriteOnly));QCOMPARE(output.write(bytes),bytes.size());QVERIFY(output.commit());}
    QByteArray contents(const QString &file){QFile input(file);return input.open(QIODevice::ReadOnly)?input.readAll():QByteArray();}
    KdeNightTimeBackend::ConfigContext context(){KdeNightTimeBackend::ConfigContext result;result.fixture=true;result.softDeadlineMs=25;return result;}
    KdeNightTimeBackend *make(const KdeNightTimeBackend::ConfigContext &config,AsyncNightTimeService **service=nullptr,const QString &file={}){
        auto *native=new AsyncNightTimeService;auto *result=new KdeNightTimeBackend(file.isEmpty()?path:file,native,config);backends<<result;services<<native;if(service)*service=native;return result;
    }
    NightTimeSnapshot read(KdeNightTimeBackend *backend){QSignalSpy done(backend,&NightTimeBackend::readFinished);backend->readCurrent(++token);if(done.isEmpty())done.wait(5000);if(done.size()!=1||!done[0][1].toBool())return {};return qvariant_cast<NightTimeSnapshot>(done[0][2]);}
    NightTimeSnapshot request(const NightTimeSnapshot &before,double latitude=12.5){auto result=before;QString error;if(!NightTimeTypes::stage(result,before,NightTimeKey::Latitude,latitude,error))return {};return result;}
private slots:
    void initTestCase(){qRegisterMetaType<NightTimeSnapshot>();qRegisterMetaType<NightTimeRuntime>();QStandardPaths::setTestModeEnabled(false);}
    void init(){QVERIFY(directory.isValid());path=directory.filePath("knighttimerc");token=0;save(path,"[General]\nSource=Times\n[Location]\nAutomatic=true\nLatitude=1\nLongitude=2\n[Foreign]\nKeep=original\n");}
    void cleanup(){
        ::chmod(QFile::encodeName(directory.path()).constData(),0700);
        for(const auto &value:gates)value->released.release(16);
        for(auto service:services)if(service&&service->heldToken)service->release();
        QTRY_VERIFY_WITH_TIMEOUT(!OperationManager::instance()->hasActiveJobs(),10000);
        for(auto backend:backends)delete backend;backends.clear();services.clear();gates.clear();if(QFileInfo(path).isDir())QDir(path).removeRecursively();
    }
    void slowReadRemainsTrackedAndResponsiveUntilWorkerTerminal(){
        const auto held=gate();auto config=context();config.beforeFileRead=[held]{held->waitOnce();};auto *backend=make(config);
        QSignalSpy done(backend,&NightTimeBackend::readFinished),pending(backend,&NightTimeBackend::operationPending);int beats=0;QTimer heartbeat;heartbeat.setInterval(1);connect(&heartbeat,&QTimer::timeout,this,[&]{++beats;});heartbeat.start();
        backend->readCurrent(++token);QTRY_VERIFY(held->entered.available()>0);QTRY_VERIFY(beats>=3);QTRY_VERIFY(!pending.isEmpty());QCOMPARE(done.size(),0);QVERIFY(OperationManager::instance()->hasActiveJobs());
        held->released.release();QTRY_COMPARE(done.size(),1);QVERIFY2(done[0][1].toBool(),qPrintable(done[0][3].toString()));QTRY_VERIFY(!OperationManager::instance()->hasActiveJobs());
    }
    void deletingReadBackendRetainsServiceAndActivity(){
        const auto held=gate();auto config=context();config.beforeFileRead=[held]{held->waitOnce();};AsyncNightTimeService *service=nullptr;auto *backend=make(config,&service);QPointer<AsyncNightTimeService> retained(service);QSignalSpy done(backend,&NightTimeBackend::readFinished);
        backend->readCurrent(++token);QTRY_VERIFY(held->entered.available()>0);delete backend;QVERIFY(retained);QVERIFY(OperationManager::instance()->hasActiveJobs());held->released.release();QTRY_VERIFY(!OperationManager::instance()->hasActiveJobs());QCOMPARE(done.size(),0);QTRY_VERIFY(!retained);
    }
    void heldBeforeSyncHasSoftPendingWithoutFalseTerminal(){
        const auto held=gate();auto config=context();config.beforeSync=[held]{held->waitOnce();};auto *backend=make(config);const auto before=read(backend),next=request(before);QCOMPARE(before.fields.size(),7);QVERIFY(!next.intents.isEmpty());const auto original=contents(path);
        QSignalSpy done(backend,&NightTimeBackend::writeFinished),pending(backend,&NightTimeBackend::operationPending);int beats=0;QTimer heartbeat;heartbeat.setInterval(1);connect(&heartbeat,&QTimer::timeout,this,[&]{++beats;});heartbeat.start();
        backend->writeChanges(++token,before,next);QTRY_VERIFY(held->entered.available()>0);QTRY_VERIFY(beats>=3);QTRY_VERIFY(!pending.isEmpty());QCOMPARE(done.size(),0);QCOMPARE(contents(path),original);QVERIFY(OperationManager::instance()->hasActiveJobs());
        held->released.release();QTRY_COMPARE(done.size(),1);QVERIFY(done[0][1].toBool());QVERIFY(done[0][2].toBool());QVERIFY2(done[0][3].toString().isEmpty(),qPrintable(done[0][3].toString()));QTest::qWait(35);QCOMPARE(done.size(),1);QVERIFY(contents(path).contains("Latitude=12.5"));
    }
    void nativeLockCanBeReleasedByGuiTimer(){
        auto *backend=make(context());const auto before=read(backend),next=request(before);QLockFile lock(path+QStringLiteral(".lock"));QVERIFY(lock.tryLock());int beats=0;bool unlocked=false;
        QTimer heartbeat;heartbeat.setInterval(1);connect(&heartbeat,&QTimer::timeout,this,[&]{++beats;});heartbeat.start();QTimer unlocker;unlocker.setSingleShot(true);connect(&unlocker,&QTimer::timeout,this,[&]{unlocked=true;lock.unlock();});unlocker.start(150);
        QSignalSpy done(backend,&NightTimeBackend::writeFinished),pending(backend,&NightTimeBackend::operationPending);backend->writeChanges(++token,before,next);QTRY_COMPARE_WITH_TIMEOUT(done.size(),1,10000);QVERIFY(unlocked);QVERIFY(beats>=3);QVERIFY(!pending.isEmpty());QVERIFY(done[0][1].toBool());QVERIFY2(done[0][3].toString().isEmpty(),qPrintable(done[0][3].toString()));
    }
    void deletedBackendKeepsCanonicalLeaseUntilTerminal_data(){QTest::addColumn<bool>("alias");QTest::newRow("same-path")<<false;QTest::newRow("dot-alias")<<true;}
    void deletedBackendKeepsCanonicalLeaseUntilTerminal(){
        QFETCH(bool,alias);const auto held=gate();auto config=context();config.beforeSync=[held]{held->waitOnce();};AsyncNightTimeService *service=nullptr;auto *first=make(config,&service);QPointer<AsyncNightTimeService> retained(service);const auto before=read(first),next=request(before);QSignalSpy firstDone(first,&NightTimeBackend::writeFinished);
        first->writeChanges(++token,before,next);QTRY_VERIFY(held->entered.available()>0);delete first;QVERIFY(retained);QVERIFY(OperationManager::instance()->hasActiveJobs());
        auto *second=make(context(),nullptr,alias?directory.filePath("./knighttimerc"):path);QSignalSpy secondDone(second,&NightTimeBackend::writeFinished);second->writeChanges(++token,before,request(before,20.0));QTRY_COMPARE(secondDone.size(),1);QVERIFY(!secondDone[0][1].toBool());QVERIFY(!secondDone[0][3].toString().isEmpty());
        held->released.release();QTRY_VERIFY_WITH_TIMEOUT(!OperationManager::instance()->hasActiveJobs(),10000);QCOMPARE(firstDone.size(),0);QCOMPARE(secondDone.size(),1);QVERIFY(contents(path).contains("Latitude=12.5"));QVERIFY(!contents(path).contains("Latitude=20"));
        const auto fresh=read(second);QSignalSpy retry(second,&NightTimeBackend::writeFinished);second->writeChanges(++token,fresh,request(fresh,20.0));QTRY_COMPARE(retry.size(),1);QVERIFY(retry[0][1].toBool());QVERIFY2(retry[0][3].toString().isEmpty(),qPrintable(retry[0][3].toString()));
    }
    void oldAndReplacementHardlinksStayLeasedThroughFinalQuery_data(){QTest::addColumn<bool>("oldInode");QTest::addColumn<bool>("orphan");QTest::newRow("old-alive")<<true<<false;QTest::newRow("new-alive")<<false<<false;QTest::newRow("new-orphan")<<false<<true;}
    void oldAndReplacementHardlinksStayLeasedThroughFinalQuery(){
        QFETCH(bool,oldInode);QFETCH(bool,orphan);const auto oldAlias=directory.filePath("old-alias"),newAlias=directory.filePath("new-alias");QFile::remove(oldAlias);QFile::remove(newAlias);QVERIFY(::link(QFile::encodeName(path).constData(),QFile::encodeName(oldAlias).constData())==0);
        AsyncNightTimeService *service=nullptr;auto *first=make(context(),&service);const auto before=read(first),next=request(before);service->holdAt=3;QPointer<AsyncNightTimeService> retained(service);QSignalSpy firstDone(first,&NightTimeBackend::writeFinished);first->writeChanges(++token,before,next);QTRY_VERIFY(service->heldToken!=0);QVERIFY(contents(path).contains("Latitude=12.5"));
        QVERIFY(::link(QFile::encodeName(path).constData(),QFile::encodeName(newAlias).constData())==0);if(orphan)delete first;
        auto *second=make(context(),nullptr,oldInode?oldAlias:newAlias);const auto aliasBefore=read(second);QCOMPARE(aliasBefore.fields.size(),7);QSignalSpy secondDone(second,&NightTimeBackend::writeFinished);second->writeChanges(++token,aliasBefore,request(aliasBefore,20.0));QTRY_COMPARE(secondDone.size(),1);QVERIFY(!secondDone[0][1].toBool());QVERIFY(!secondDone[0][3].toString().isEmpty());QVERIFY(OperationManager::instance()->hasActiveJobs());
        retained->release();if(!orphan){QTRY_COMPARE(firstDone.size(),1);QVERIFY2(firstDone[0][3].toString().isEmpty(),qPrintable(firstDone[0][3].toString()));}QTRY_VERIFY(!OperationManager::instance()->hasActiveJobs());QCOMPARE(firstDone.size(),orphan?0:1);QVERIFY(!contents(path).contains("Latitude=20"));QVERIFY(!contents(oldAlias).contains("Latitude=20"));
    }
    void fullCascadeAlsoReservesUserKdeglobals(){
        const auto home=QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation),local=home+QStringLiteral("/knighttimerc"),global=home+QStringLiteral("/kdeglobals");save(local,"[General]\nSource=Times\n");save(global,"[Location]\nLatitude=1\n[Foreign]\nKeep=global\n");
        const auto held=gate();auto config=context();config.fullCascade=true;config.beforeSync=[held]{held->waitOnce();};auto *first=make(config,nullptr,local),*second=make(context(),nullptr,global);const auto before=read(first),globalBefore=read(second);QVERIFY(before.fields.value(NightTimeKey::Latitude).globalPresent);
        QSignalSpy firstDone(first,&NightTimeBackend::writeFinished),secondDone(second,&NightTimeBackend::writeFinished);first->writeChanges(++token,before,request(before));QTRY_VERIFY(held->entered.available()>0);second->writeChanges(++token,globalBefore,request(globalBefore,20.0));QTRY_COMPARE(secondDone.size(),1);QVERIFY(!secondDone[0][1].toBool());held->released.release();QTRY_COMPARE(firstDone.size(),1);QVERIFY2(firstDone[0][3].toString().isEmpty(),qPrintable(firstDone[0][3].toString()));QVERIFY(contents(global).contains("Keep=global"));QVERIFY(!contents(global).contains("Latitude=20"));QFile::remove(local);QFile::remove(global);
    }
    void defaultsPolicyManagedValueAndProviderChangesAbortBeforeSync_data(){QTest::addColumn<int>("change");QTest::newRow("default")<<0;QTest::newRow("policy")<<1;QTest::newRow("owned-value")<<2;QTest::newRow("provider")<<3;}
    void defaultsPolicyManagedValueAndProviderChangesAbortBeforeSync(){
        QFETCH(int,change);const auto defaults=directory.filePath("defaults");save(defaults,"[Location]\nLongitude=2\n");const auto held=gate();auto config=context();config.defaultSources={defaults};config.beforeSync=[held]{held->waitOnce();};AsyncNightTimeService *service=nullptr;auto *backend=make(config,&service);service->online();const auto before=read(backend),next=request(before);const auto original=contents(path);QSignalSpy done(backend,&NightTimeBackend::writeFinished);
        backend->writeChanges(++token,before,next);QTRY_VERIFY(held->entered.available()>0);
        if(change==0)save(defaults,"[Location]\nLongitude=3\n");else if(change==1)save(defaults,"[Location]\nLongitude=2\nLatitude[$i]=1\n");else if(change==2)save(path,original+"\n[Times]\nSunsetStart=19,0,0\n");else{++service->state.epoch;emit service->externalChange();}
        held->released.release();QTRY_COMPARE(done.size(),1);QVERIFY(!done[0][1].toBool());QVERIFY(!done[0][2].toBool());QVERIFY(!done[0][3].toString().isEmpty());QVERIFY(!contents(path).contains("Latitude=12.5"));QTest::qWait(30);QCOMPARE(done.size(),1);
    }
    void providerChangeDuringNativeLockWaitReportsUncertainSavedResult(){
        AsyncNightTimeService *service=nullptr;auto *backend=make(context(),&service);service->online();const auto before=read(backend),next=request(before);QLockFile lock(path+QStringLiteral(".lock"));QVERIFY(lock.tryLock());QSignalSpy done(backend,&NightTimeBackend::writeFinished),pending(backend,&NightTimeBackend::operationPending);
        backend->writeChanges(++token,before,next);QTRY_VERIFY(!pending.isEmpty());QCOMPARE(done.size(),0);++service->state.epoch;emit service->externalChange();lock.unlock();QTRY_COMPARE_WITH_TIMEOUT(done.size(),1,10000);QVERIFY(done[0][1].toBool());QVERIFY(!done[0][3].toString().isEmpty());QVERIFY(contents(path).contains("Latitude=12.5"));QTest::qWait(30);QCOMPARE(done.size(),1);
    }
    void failedBoundaryDoesNotRetryAfterPathRestored(){
        auto config=context();auto sabotaged=std::make_shared<std::atomic<bool>>(false);const auto target=path;config.beforeSync=[target,sabotaged]{sabotaged->store(QFile::remove(target)&&QDir().mkdir(target));};auto *backend=make(config);const auto before=read(backend),next=request(before);QSignalSpy done(backend,&NightTimeBackend::writeFinished);backend->writeChanges(++token,before,next);QTRY_COMPARE(done.size(),1);QVERIFY(sabotaged->load());QVERIFY(!done[0][3].toString().isEmpty());QTRY_VERIFY(!OperationManager::instance()->hasActiveJobs());QVERIFY(QDir().rmdir(path));const QByteArray restored="[Location]\nLatitude=3\n[Foreign]\nKeep=after-failure\n";save(path,restored);QTest::qWait(50);QCOMPARE(contents(path),restored);QCOMPARE(done.size(),1);
    }
    void nativeSyncFailureDoesNotRetryFromConfigDestruction(){
        if(::geteuid()==0)QSKIP("Permission-failure fixture requires an unprivileged user.");
        const auto parent=directory.path();auto changed=std::make_shared<std::atomic<bool>>(false);auto config=context();
        config.beforeSync=[parent,changed]{changed->store(::chmod(QFile::encodeName(parent).constData(),0500)==0);};
        config.afterSync=[parent]{::chmod(QFile::encodeName(parent).constData(),0700);};
        auto *backend=make(config);const auto before=read(backend),next=request(before);QSignalSpy done(backend,&NightTimeBackend::writeFinished);
        backend->writeChanges(++token,before,next);QTRY_COMPARE_WITH_TIMEOUT(done.size(),1,10000);::chmod(QFile::encodeName(parent).constData(),0700);
        QVERIFY(changed->load());QVERIFY(done[0][1].toBool());QVERIFY(!done[0][3].toString().isEmpty());QTRY_VERIFY(!OperationManager::instance()->hasActiveJobs());
        const QByteArray restored="[Location]\nLatitude=3\n[Foreign]\nKeep=after-native-failure\n";save(path,restored);QTest::qWait(80);QCOMPARE(contents(path),restored);QCOMPARE(done.size(),1);
    }
    void fifoBrokenLinkAndDistinctSourceAliasesAreRejectedBeforeOpening_data(){QTest::addColumn<int>("kind");QTest::newRow("fifo")<<0;QTest::newRow("broken-link")<<1;QTest::newRow("source-hardlink")<<2;}
    void fifoBrokenLinkAndDistinctSourceAliasesAreRejectedBeforeOpening(){
        QFETCH(int,kind);auto config=context();const auto alias=directory.filePath("source-alias");QFile::remove(alias);
        if(kind==0){QVERIFY(QFile::remove(path));QVERIFY(::mkfifo(QFile::encodeName(path).constData(),0600)==0);}else if(kind==1){QVERIFY(QFile::remove(path));QVERIFY(QFile::link(directory.filePath("missing"),path));}else{QVERIFY(::link(QFile::encodeName(path).constData(),QFile::encodeName(alias).constData())==0);config.defaultSources={alias};}
        auto *backend=make(config);QSignalSpy done(backend,&NightTimeBackend::readFinished);backend->readCurrent(++token);QTRY_COMPARE(done.size(),1);QVERIFY(!done[0][1].toBool());QVERIFY(!done[0][3].toString().isEmpty());QFile::remove(path);
    }
    void hooksAndDeadlineOverrideAreIgnoredOutsideFixtureMode(){
        auto calls=std::make_shared<std::atomic<int>>(0);KdeNightTimeBackend::ConfigContext config;config.softDeadlineMs=1;config.beforeFileRead=[calls]{++*calls;};config.beforeSync=[calls]{++*calls;};config.afterSync=[calls]{++*calls;};auto *backend=make(config);const auto before=read(backend),next=request(before);QSignalSpy done(backend,&NightTimeBackend::writeFinished);backend->writeChanges(++token,before,next);QTRY_COMPARE(done.size(),1);QVERIFY2(done[0][3].toString().isEmpty(),qPrintable(done[0][3].toString()));QCOMPARE(calls->load(),0);
    }
};
int main(int argc,char **argv){
    QCoreApplication app(argc,argv);if(!qEnvironmentVariableIsSet("CONTROL_NIGHT_TIME_ASYNC_CHILD")){
        QTemporaryDir sandbox;if(!sandbox.isValid())return 2;auto environment=QProcessEnvironment::systemEnvironment();const auto config=sandbox.filePath("config"),defaults=sandbox.filePath("defaults"),cache=sandbox.filePath("cache"),state=sandbox.filePath("state");if(!QDir().mkpath(config)||!QDir().mkpath(defaults)||!QDir().mkpath(cache)||!QDir().mkpath(state))return 3;
        environment.insert("XDG_CONFIG_HOME",config);environment.insert("XDG_CONFIG_DIRS",defaults);environment.insert("XDG_CACHE_HOME",cache);environment.insert("XDG_STATE_HOME",state);environment.insert("KDE_SKIP_KDERC","1");environment.insert("CONTROL_NIGHT_TIME_ASYNC_CHILD","1");QProcess child;child.setProcessEnvironment(environment);child.setProcessChannelMode(QProcess::ForwardedChannels);QStringList arguments{"--",app.applicationFilePath()};arguments<<app.arguments().mid(1);child.start("/usr/bin/dbus-run-session",arguments);if(!child.waitForStarted(3000)||!child.waitForFinished(120000)){child.kill();child.waitForFinished(1000);return 4;}return child.exitStatus()==QProcess::NormalExit?child.exitCode():5;
    }NightTimeAsyncTest test;return QTest::qExec(&test,argc,argv);
}
#include "night_time_async_test.moc"
