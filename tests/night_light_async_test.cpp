#include <QtTest>
#include "platform/kde/NightLightBackend.h"
#include "operations/OperationManager.h"
#include <QDir>
#include <QFile>
#include <QLockFile>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSaveFile>
#include <QSemaphore>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <atomic>
#include <memory>
#include <unistd.h>

namespace {
struct WorkerGate {
    std::atomic_bool armed{false};QSemaphore entered,released;
    void wait(){if(armed.exchange(false)){entered.release();released.acquire();}}
};
class AsyncNightLightService final : public NightLightService {
public:
    NightLightRuntime state;int queries=0,previews=0,stops=0;bool holdObservation=false;quint64 heldToken=0;QString heldOwner;
    AsyncNightLightService(){state.owner=":private-night.1";state.epoch=1;state.version="6.6.6";state.serviceAvailable=state.schemaSupported=state.known=state.available=state.previewSupported=true;state.mode=1;state.daylight=true;state.currentTemperature=state.targetTemperature=6500;}
    void query(quint64 token,const QString &expectedOwner={}) override {
        ++queries;if(holdObservation&&queries>=3){heldToken=token;heldOwner=expectedOwner;return;}
        const bool valid=state.known&&state.schemaSupported&&(expectedOwner.isEmpty()||expectedOwner==state.owner);emit queryFinished(token,valid,state,valid?QString():QStringLiteral("Fixture provider changed"));
    }
    void release(){holdObservation=false;query(heldToken,heldOwner);}
    void preview(quint64 token,int,const QString &owner,quint64 epoch) override {const bool valid=owner==state.owner&&epoch==state.epoch;if(valid)++previews;emit previewFinished(token,valid,false,valid?QString():QStringLiteral("Preview owner changed"));}
    void stopPreview(quint64 token,const QString &owner,quint64 epoch) override {const bool valid=owner==state.owner&&epoch==state.epoch;if(valid)++stops;emit previewFinished(token,valid,true,valid?QString():QStringLiteral("Stop owner changed"));}
    void replaceProvider(){++state.epoch;state.owner=":private-night.2";emit providerChanged();emit externalChange();}
};
}

class NightLightAsyncTest final : public QObject {
    Q_OBJECT
    QTemporaryDir directory;QString path;quint64 token=0;
    QList<QPointer<KdeNightLightBackend>> backends;QList<QPointer<AsyncNightLightService>> services;QList<std::shared_ptr<WorkerGate>> gates;
    std::shared_ptr<WorkerGate> gate(){auto value=std::make_shared<WorkerGate>();gates<<value;return value;}
    void save(const QString &file,const QByteArray &bytes){QSaveFile out(file);QVERIFY(out.open(QIODevice::WriteOnly));QCOMPARE(out.write(bytes),bytes.size());QVERIFY(out.commit());}
    QByteArray bytes(const QString &file){QFile in(file);if(!in.open(QIODevice::ReadOnly))return {};return in.readAll();}
    KdeNightLightBackend::ConfigContext context(){KdeNightLightBackend::ConfigContext value;value.fixture=true;value.softDeadlineMs=25;return value;}
    KdeNightLightBackend *make(const KdeNightLightBackend::ConfigContext &config,AsyncNightLightService **service=nullptr,const QString &file={}){
        auto *native=new AsyncNightLightService;auto *backend=new KdeNightLightBackend(file.isEmpty()?path:file,native,config);backends<<backend;services<<native;if(service)*service=native;return backend;
    }
    NightLightSnapshot read(KdeNightLightBackend *backend){QSignalSpy done(backend,&NightLightBackend::readFinished);backend->readCurrent(++token);if(done.isEmpty())done.wait(5000);if(done.size()!=1||!done[0][1].toBool())return {};return qvariant_cast<NightLightSnapshot>(done[0][2]);}
    NightLightSnapshot request(const NightLightSnapshot &before){auto next=before;QString error;if(!NightLightTypes::stage(next,before,NightLightKey::NightTemperature,4700,error))return {};return next;}
private slots:
    void initTestCase(){qRegisterMetaType<NightLightSnapshot>();qRegisterMetaType<NightLightRuntime>();QStandardPaths::setTestModeEnabled(false);}
    void init(){QVERIFY(directory.isValid());path=directory.filePath("kwinrc");token=0;save(path,"[NightColor]\nNightTemperature=4500\n[Foreign]\nKeep=original\n");}
    void cleanup(){
        for(const auto &held:gates)held->released.release(16);
        for(auto service:services)if(service&&service->heldToken)service->release();
        QTRY_VERIFY_WITH_TIMEOUT(!OperationManager::instance()->hasActiveJobs(),10000);
        for(auto backend:backends)delete backend;backends.clear();services.clear();gates.clear();
    }
    void heldReadHasHeartbeatSoftPendingAndSurvivesBackendDeletion(){
        const auto held=gate();held->armed=true;auto config=context();config.beforeFileRead=[held]{held->wait();};auto *backend=make(config);
        QSignalSpy done(backend,&NightLightBackend::readFinished),pending(backend,&NightLightBackend::operationPending);int beats=0;QTimer heartbeat;heartbeat.setInterval(1);connect(&heartbeat,&QTimer::timeout,this,[&]{++beats;});heartbeat.start();
        backend->readCurrent(++token);QTRY_VERIFY(held->entered.available()>0);QTRY_VERIFY(beats>=3);QTRY_VERIFY(!pending.isEmpty());QCOMPARE(done.size(),0);QVERIFY(OperationManager::instance()->hasActiveJobs());delete backend;QVERIFY(OperationManager::instance()->hasActiveJobs());held->released.release();QTRY_VERIFY(!OperationManager::instance()->hasActiveJobs());QCOMPARE(done.size(),0);
    }
    void heldNativeWriteHasOneTerminalAndWorkerLocalHooks(){
        const auto held=gate();held->armed=true;auto config=context();auto *gui=QThread::currentThread();struct Facts{std::atomic_bool wrong{false};std::atomic_int before{0},after{0};};const auto facts=std::make_shared<Facts>();
        config.beforeSync=[held,gui,facts]{++facts->before;if(QThread::currentThread()==gui)facts->wrong=true;held->wait();};config.afterSync=[gui,facts]{++facts->after;if(QThread::currentThread()==gui)facts->wrong=true;};auto *backend=make(config);const auto before=read(backend),next=request(before);QCOMPARE(next.intents.size(),1);
        QSignalSpy done(backend,&NightLightBackend::writeFinished),pending(backend,&NightLightBackend::operationPending);const auto original=bytes(path);backend->writeChanges(++token,before,next);QTRY_VERIFY(held->entered.available()>0);QTRY_VERIFY(!pending.isEmpty());QCOMPARE(done.size(),0);QCOMPARE(bytes(path),original);held->released.release();QTRY_COMPARE(done.size(),1);QVERIFY2(done[0][3].toString().isEmpty(),qPrintable(done[0][3].toString()));QVERIFY(done[0][1].toBool()&&done[0][2].toBool());QCOMPARE(facts->before.load(),1);QCOMPARE(facts->after.load(),1);QVERIFY(!facts->wrong);QTest::qWait(30);QCOMPARE(done.size(),1);QVERIFY(bytes(path).contains("Keep=original"));
    }
    void realQLockWaitAllowsGuiUnlockAndDoesNotFalselyTimeout(){
        auto *backend=make(context());const auto before=read(backend),next=request(before);QLockFile lock(path+".lock");QVERIFY(lock.tryLock());int beats=0;bool unlocked=false;QTimer heartbeat;heartbeat.setInterval(1);connect(&heartbeat,&QTimer::timeout,this,[&]{++beats;});heartbeat.start();QTimer::singleShot(120,this,[&]{unlocked=true;lock.unlock();});
        QSignalSpy done(backend,&NightLightBackend::writeFinished),pending(backend,&NightLightBackend::operationPending);backend->writeChanges(++token,before,next);QTRY_COMPARE_WITH_TIMEOUT(done.size(),1,10000);QVERIFY(unlocked&&beats>=3);QVERIFY(!pending.isEmpty());QVERIFY2(done[0][3].toString().isEmpty(),qPrintable(done[0][3].toString()));QVERIFY(done[0][1].toBool());
    }
    void orphanedWriteRetainsServiceAndRejectsSameFileOrAlias_data(){QTest::addColumn<int>("alias");QTest::newRow("same-path")<<0;QTest::newRow("dot-alias")<<1;QTest::newRow("hardlink")<<2;}
    void orphanedWriteRetainsServiceAndRejectsSameFileOrAlias(){
        QFETCH(int,alias);const auto held=gate();held->armed=true;auto config=context();config.beforeSync=[held]{held->wait();};AsyncNightLightService *service=nullptr;auto *first=make(config,&service);QPointer<AsyncNightLightService> retained(service);const auto before=read(first),next=request(before);QSignalSpy firstDone(first,&NightLightBackend::writeFinished);first->writeChanges(++token,before,next);QTRY_VERIFY(held->entered.available()>0);delete first;QVERIFY(retained);QVERIFY(OperationManager::instance()->hasActiveJobs());
        QString competingPath=path;if(alias==1)competingPath=directory.filePath("./kwinrc");if(alias==2){competingPath=directory.filePath("hardlink-kwinrc");QFile::remove(competingPath);QVERIFY(::link(QFile::encodeName(path).constData(),QFile::encodeName(competingPath).constData())==0);}
        auto *second=make(context(),nullptr,competingPath);const auto other=read(second);auto competing=other;QString error;QVERIFY(NightLightTypes::stage(competing,other,NightLightKey::DayTemperature,6000,error));QSignalSpy secondDone(second,&NightLightBackend::writeFinished);second->writeChanges(++token,other,competing);QTRY_COMPARE(secondDone.size(),1);QVERIFY(!secondDone[0][1].toBool());QVERIFY(!secondDone[0][3].toString().isEmpty());held->released.release();QTRY_VERIFY_WITH_TIMEOUT(!OperationManager::instance()->hasActiveJobs(),10000);QCOMPARE(firstDone.size(),0);QVERIFY(bytes(path).contains("NightTemperature=4700"));QVERIFY(!bytes(path).contains("DayTemperature=6000"));QTRY_VERIFY(!retained);
    }
    void replacementInodeAliasRemainsReservedDuringObservation(){
        AsyncNightLightService *service=nullptr;auto *first=make(context(),&service);const auto before=read(first),next=request(before);service->holdObservation=true;QSignalSpy firstDone(first,&NightLightBackend::writeFinished);first->writeChanges(++token,before,next);QTRY_VERIFY(service->heldToken!=0);QVERIFY(bytes(path).contains("NightTemperature=4700"));delete first;QPointer<AsyncNightLightService> retained(service);
        const auto alias=directory.filePath("new-inode-alias");QFile::remove(alias);QVERIFY(::link(QFile::encodeName(path).constData(),QFile::encodeName(alias).constData())==0);auto *second=make(context(),nullptr,alias);const auto observed=read(second);auto competing=observed;QString error;QVERIFY(NightLightTypes::stage(competing,observed,NightLightKey::DayTemperature,6000,error));QSignalSpy rejected(second,&NightLightBackend::writeFinished);second->writeChanges(++token,observed,competing);QTRY_COMPARE(rejected.size(),1);QVERIFY(!rejected[0][1].toBool());service->release();QTRY_VERIFY(!OperationManager::instance()->hasActiveJobs());QCOMPARE(firstDone.size(),0);QTRY_VERIFY(!retained);
    }
    void fullCascadeAlsoReservesUserKdeglobals(){
        const auto home=QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation),local=home+"/kwinrc",global=home+"/kdeglobals";save(local,"[NightColor]\nMode=DarkLight\n");save(global,"[NightColor]\nNightTemperature=3500\n[Foreign]\nKeep=global\n");
        const auto held=gate();held->armed=true;auto config=context();config.fullCascade=true;config.beforeSync=[held]{held->wait();};auto *first=make(config,nullptr,local);auto *second=make(context(),nullptr,global);const auto before=read(first),other=read(second);auto reset=before;QString error;QVERIFY(NightLightTypes::restore(reset,before,NightLightKey::NightTemperature,error));auto competing=other;QVERIFY(NightLightTypes::stage(competing,other,NightLightKey::DayTemperature,6000,error));QSignalSpy done(first,&NightLightBackend::writeFinished),rejected(second,&NightLightBackend::writeFinished);first->writeChanges(++token,before,reset);QTRY_VERIFY(held->entered.available()>0);second->writeChanges(++token,other,competing);QTRY_COMPARE(rejected.size(),1);QVERIFY(!rejected[0][1].toBool());held->released.release();QTRY_COMPARE(done.size(),1);QVERIFY2(done[0][3].toString().isEmpty(),qPrintable(done[0][3].toString()));QVERIFY(bytes(global).contains("Keep=global"));QVERIFY(!bytes(global).contains("DayTemperature=6000"));QFile::remove(local);QFile::remove(global);
    }
    void failedNativeSyncIsTerminalAndNeverRetried(){
        auto config=context();auto sabotaged=std::make_shared<std::atomic_bool>(false);const auto file=path;config.beforeSync=[file,sabotaged]{sabotaged->store(QFile::remove(file)&&QDir().mkdir(file));};auto *backend=make(config);const auto before=read(backend),next=request(before);QSignalSpy done(backend,&NightLightBackend::writeFinished);backend->writeChanges(++token,before,next);QTRY_COMPARE_WITH_TIMEOUT(done.size(),1,10000);QVERIFY(sabotaged->load());QVERIFY(!done[0][3].toString().isEmpty());QVERIFY(!done[0][2].toBool());QTRY_VERIFY(!OperationManager::instance()->hasActiveJobs());QVERIFY(QDir().rmdir(path));const QByteArray restored("[NightColor]\nNightTemperature=4500\n[Foreign]\nKeep=after-failure\n");save(path,restored);QTest::qWait(50);QCOMPARE(done.size(),1);QCOMPARE(bytes(path),restored);
    }
    void confirmedProviderLossBeforeSyncAbortsWhileOrdinaryStatusDoesNot_data(){QTest::addColumn<bool>("providerLoss");QTest::newRow("owner-replaced")<<true;QTest::newRow("ordinary-properties")<<false;}
    void confirmedProviderLossBeforeSyncAbortsWhileOrdinaryStatusDoesNot(){
        QFETCH(bool,providerLoss);const auto held=gate();held->armed=true;auto config=context();config.beforeSync=[held]{held->wait();};AsyncNightLightService *service=nullptr;auto *backend=make(config,&service);const auto before=read(backend),next=request(before);const auto original=bytes(path);QSignalSpy done(backend,&NightLightBackend::writeFinished);backend->writeChanges(++token,before,next);QTRY_VERIFY(held->entered.available()>0);if(providerLoss)service->replaceProvider();else emit service->externalChange();held->released.release();QTRY_COMPARE(done.size(),1);if(providerLoss){QVERIFY(!done[0][1].toBool());QVERIFY(!done[0][3].toString().isEmpty());QCOMPARE(bytes(path),original);}else{QVERIFY(done[0][1].toBool());QVERIFY2(done[0][3].toString().isEmpty(),qPrintable(done[0][3].toString()));}
    }
    void providerLossAfterSyncIsReportedAsUncertainSavedState(){
        const auto held=gate();held->armed=true;auto config=context();config.afterSync=[held]{held->wait();};AsyncNightLightService *service=nullptr;auto *backend=make(config,&service);const auto before=read(backend),next=request(before);QSignalSpy done(backend,&NightLightBackend::writeFinished);backend->writeChanges(++token,before,next);QTRY_VERIFY(held->entered.available()>0);service->replaceProvider();held->released.release();QTRY_COMPARE(done.size(),1);QVERIFY(done[0][1].toBool()&&done[0][2].toBool());QVERIFY(!done[0][3].toString().isEmpty());QVERIFY(bytes(path).contains("NightTemperature=4700"));
    }
    void previewCleanupStillTargetsPinnedServiceWhileJobOwnsIt(){
        const auto held=gate();held->armed=true;auto config=context();config.beforeSync=[held]{held->wait();};AsyncNightLightService *service=nullptr;auto *backend=make(config,&service);const auto before=read(backend),next=request(before);backend->preview(900,4000,before.runtime.owner,before.runtime.epoch);QCOMPARE(service->previews,1);backend->writeChanges(++token,before,next);QTRY_VERIFY(held->entered.available()>0);
        int outwardStops=0;QPointer<KdeNightLightBackend> dying(backend);connect(backend,&NightLightBackend::previewFinished,this,[&outwardStops,dying](quint64,bool,bool stopped,const QString &){if(stopped){++outwardStops;if(dying)delete dying;}});
        delete backend;QCOMPARE(outwardStops,0);QCOMPARE(service->stops,1);QVERIFY(OperationManager::instance()->hasActiveJobs());held->released.release();QTRY_VERIFY(!OperationManager::instance()->hasActiveJobs());
    }
    void fixtureHooksAreIgnoredWithoutExplicitFixtureFlag(){
        const auto calls=std::make_shared<std::atomic_int>(0);KdeNightLightBackend::ConfigContext config;config.beforeFileRead=[calls]{++*calls;};config.beforeSync=[calls]{++*calls;};config.afterSync=[calls]{++*calls;};config.softDeadlineMs=1;auto *backend=make(config);const auto before=read(backend),next=request(before);QSignalSpy done(backend,&NightLightBackend::writeFinished);backend->writeChanges(++token,before,next);QTRY_COMPARE(done.size(),1);QVERIFY2(done[0][3].toString().isEmpty(),qPrintable(done[0][3].toString()));QCOMPARE(calls->load(),0);
    }
};
int main(int argc,char **argv)
{
    QCoreApplication app(argc,argv);
    if(!qEnvironmentVariableIsSet("CONTROL_NIGHT_ASYNC_CHILD")){
        QTemporaryDir sandbox;if(!sandbox.isValid())return 2;auto environment=QProcessEnvironment::systemEnvironment();const auto config=sandbox.filePath("config"),defaults=sandbox.filePath("defaults"),cache=sandbox.filePath("cache");if(!QDir().mkpath(config)||!QDir().mkpath(defaults)||!QDir().mkpath(cache))return 3;
        environment.insert("XDG_CONFIG_HOME",config);environment.insert("XDG_CONFIG_DIRS",defaults);environment.insert("XDG_CACHE_HOME",cache);environment.insert("KDE_SKIP_KDERC","1");environment.insert("CONTROL_NIGHT_ASYNC_CHILD","1");QProcess child;child.setProcessEnvironment(environment);child.setProcessChannelMode(QProcess::ForwardedChannels);QStringList arguments{"--",app.applicationFilePath()};arguments<<app.arguments().mid(1);child.start("/usr/bin/dbus-run-session",arguments);if(!child.waitForStarted(3000)||!child.waitForFinished(120000)){child.kill();child.waitForFinished(1000);return 4;}return child.exitStatus()==QProcess::NormalExit?child.exitCode():5;
    }
    NightLightAsyncTest test;return QTest::qExec(&test,argc,argv);
}
#include "night_light_async_test.moc"
