#include <QtTest>
#include "platform/kde/PowerPolicyBackend.h"
#include "settings/PowerPolicyFields.h"
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
class AsyncPowerDaemon final:public PowerPolicyDaemon {
public:
    PowerPolicyCapabilities caps;int queries=0,reloads=0,cancellations=0;
    bool holdCapabilities=false,automaticReload=true,reloading=false;
    std::shared_ptr<std::atomic<int>> reloadCount=std::make_shared<std::atomic<int>>(0);
    AsyncPowerDaemon(){
        caps.serviceAvailable=caps.schemaSupported=caps.displaySupported=caps.sleepSupported=true;
        caps.suspendActionSupported=caps.canSuspendKnown=caps.canSuspend=true;
        caps.hibernateKnown=caps.hibernateSupported=caps.hybridSuspendSupported=caps.suspendThenHibernateSupported=true;
        caps.dimSupported=caps.brightnessSupported=caps.keyboardBrightnessSupported=caps.buttonSupported=true;
        caps.lidKnown=caps.hasLid=caps.batteryKnown=caps.hasBattery=true;
        caps.mobileKnown=caps.defaultMobileKnown=caps.virtualMachineKnown=true;
        caps.owner=QStringLiteral(":private-power.1");caps.revision=QStringLiteral("private-capabilities-v1");
    }
    void queryCapabilities()override{++queries;if(!holdCapabilities)emit capabilitiesFinished(caps);}
    void reload()override{
        ++reloads;++*reloadCount;reloading=true;
        if(automaticReload)QTimer::singleShot(0,this,[this]{if(reloading)acknowledge();});
    }
    void acknowledge(){if(!reloading)return;reloading=false;emit changed();emit reloadFinished(true,false,{});}
    void cancel()override{++cancellations;if(reloading){reloading=false;emit reloadFinished(false,true,QStringLiteral("Private reload cancelled"));}}
    void releaseCapabilities(){holdCapabilities=false;emit capabilitiesFinished(caps);}
};
}
class PowerPolicyAsyncTest final:public QObject {
    Q_OBJECT
    QTemporaryDir directory;QString path;
    QList<QPointer<KdePowerPolicyBackend>> backends;QList<QPointer<AsyncPowerDaemon>> daemons;
    QList<std::shared_ptr<WorkerGate>> gates;
    std::shared_ptr<WorkerGate> gate(){auto value=std::make_shared<WorkerGate>();gates<<value;return value;}
    void save(const QString &file,const QByteArray &bytes){QSaveFile output(file);QVERIFY(output.open(QIODevice::WriteOnly));QCOMPARE(output.write(bytes),bytes.size());QVERIFY(output.commit());}
    QByteArray contents(const QString &file){QFile input(file);return input.open(QIODevice::ReadOnly)?input.readAll():QByteArray();}
    KdePowerPolicyBackend::ConfigContext context(){KdePowerPolicyBackend::ConfigContext result;result.fixture=true;result.softDeadlineMs=25;return result;}
    KdePowerPolicyBackend *make(const KdePowerPolicyBackend::ConfigContext &config,AsyncPowerDaemon **daemon=nullptr,const QString &file={}){
        auto *native=new AsyncPowerDaemon;auto *backend=new KdePowerPolicyBackend(file.isEmpty()?path:file,native,config);backends<<backend;daemons<<native;if(daemon)*daemon=native;return backend;
    }
    PowerPolicySnapshot read(KdePowerPolicyBackend *backend){
        QSignalSpy done(backend,&PowerPolicyBackend::readFinished);backend->readCurrent();if(done.isEmpty())done.wait(5000);
        if(done.size()!=1||!done[0][0].toBool())return {};return qvariant_cast<PowerPolicySnapshot>(done[0][1]);
    }
    PowerPolicySnapshot request(const PowerPolicySnapshot &before,int seconds=1200){
        auto result=before;QString error;if(result.profiles.size()!=3||!PowerPolicyFields::stage(result.profiles[0],before.profiles[0],PowerPolicyKey::DisplaySeconds,seconds,error))return {};return result;
    }
private slots:
    void initTestCase(){qRegisterMetaType<PowerPolicySnapshot>();qRegisterMetaType<PowerPolicyCapabilities>();QStandardPaths::setTestModeEnabled(false);}
    void init(){
        QVERIFY(directory.isValid());path=directory.filePath("powerdevilrc");
        save(path,"[AC][Display]\nTurnOffDisplayWhenIdle=true\nTurnOffDisplayIdleTimeoutSec=600\nDisplayBrightness=77\n[AC][SuspendAndShutdown]\nAutoSuspendAction=2\nAutoSuspendIdleTimeoutSec=900\n[BatteryManagement]\nBatteryLowLevel=20\nBatteryCriticalLevel=10\nBatteryCriticalAction=2\nBatteryChargeStartThreshold=40\nBatteryChargeStopThreshold=80\n[Other]\nKeep=original\n");
    }
    void cleanup(){
        ::chmod(QFile::encodeName(directory.path()).constData(),0700);
        for(const auto &value:gates)value->released.release(32);
        for(auto native:daemons)if(native){native->automaticReload=true;if(native->holdCapabilities)native->releaseCapabilities();if(native->reloading)native->acknowledge();}
        QTRY_VERIFY_WITH_TIMEOUT(!OperationManager::instance()->hasActiveJobs(),10000);
        for(auto backend:backends)delete backend;backends.clear();daemons.clear();gates.clear();if(QFileInfo(path).isDir())QDir(path).removeRecursively();
    }
    void slowReadHasHeartbeatSoftPendingAndOneActualTerminal(){
        const auto held=gate();auto config=context();config.beforeFileRead=[held]{held->waitOnce();};auto *backend=make(config);
        QSignalSpy done(backend,&PowerPolicyBackend::readFinished),pending(backend,&PowerPolicyBackend::operationPending);int beats=0;QTimer heartbeat;heartbeat.setInterval(1);connect(&heartbeat,&QTimer::timeout,this,[&]{++beats;});heartbeat.start();
        backend->readCurrent();QTRY_VERIFY(held->entered.available()>0);QTRY_VERIFY(beats>=3);QTRY_VERIFY(!pending.isEmpty());QCOMPARE(done.size(),0);QVERIFY(OperationManager::instance()->hasActiveJobs());
        held->released.release();QTRY_COMPARE(done.size(),1);QVERIFY2(done[0][0].toBool(),qPrintable(done[0][2].toString()));QTRY_VERIFY(!OperationManager::instance()->hasActiveJobs());QTest::qWait(30);QCOMPARE(done.size(),1);
    }
    void deletedReadBackendRetainsDaemonAndActivityUntilWorkerEnds(){
        const auto held=gate();auto config=context();config.beforeFileRead=[held]{held->waitOnce();};AsyncPowerDaemon *native=nullptr;auto *backend=make(config,&native);QPointer<AsyncPowerDaemon> retained(native);QSignalSpy done(backend,&PowerPolicyBackend::readFinished);
        backend->readCurrent();QTRY_VERIFY(held->entered.available()>0);delete backend;QVERIFY(retained);QVERIFY(OperationManager::instance()->hasActiveJobs());held->released.release();QTRY_VERIFY(!OperationManager::instance()->hasActiveJobs());QCOMPARE(done.size(),0);QTRY_VERIFY(!retained);
    }
    void heldSaveDoesNotCompleteAtSoftDeadlineOrFlattenPolicies(){
        const auto held=gate();auto config=context();config.beforeSync=[held]{held->waitOnce();};AsyncPowerDaemon *native=nullptr;auto *backend=make(config,&native);const auto before=read(backend),next=request(before);QCOMPARE(before.profiles.size(),3);QCOMPARE(PowerPolicyFields::profileKeys().size(),17);QCOMPARE(PowerPolicyFields::globalKeys().size(),3);const auto original=contents(path);
        QSignalSpy done(backend,&PowerPolicyBackend::writeFinished),pending(backend,&PowerPolicyBackend::operationPending);int beats=0;QTimer heartbeat;heartbeat.setInterval(1);connect(&heartbeat,&QTimer::timeout,this,[&]{++beats;});heartbeat.start();backend->writePolicy(before,next);
        QTRY_VERIFY(held->entered.available()>0);QTRY_VERIFY(beats>=3);QTRY_VERIFY(!pending.isEmpty());QCOMPARE(done.size(),0);QCOMPARE(native->reloads,0);QCOMPARE(contents(path),original);QVERIFY(OperationManager::instance()->hasActiveJobs());
        held->released.release();QTRY_COMPARE(done.size(),1);QVERIFY(done[0][0].toBool());QVERIFY(done[0][1].toBool());QVERIFY(!done[0][2].toBool());QVERIFY2(done[0][3].toString().isEmpty(),qPrintable(done[0][3].toString()));
        const auto actual=read(backend);QString error;QVERIFY2(PowerPolicyFields::matchesRequest(before,next,actual,error),qPrintable(error));QCOMPARE(actual.profiles[0].suspendAction,2u);QCOMPARE(actual.batteryLowLevel,20);QCOMPARE(actual.batteryCriticalLevel,10);QCOMPARE(actual.batteryCriticalAction,2u);
        QVERIFY(contents(path).contains("BatteryChargeStartThreshold=40"));QVERIFY(contents(path).contains("BatteryChargeStopThreshold=80"));QVERIFY(contents(path).contains("Keep=original"));QCOMPARE(native->reloads,1);QTest::qWait(30);QCOMPARE(done.size(),1);
    }
    void realNativeLockWaitAllowsGuiTimerReleaseAndEventualSuccess(){
        auto *backend=make(context());const auto before=read(backend),next=request(before);QLockFile lock(path+QStringLiteral(".lock"));QVERIFY(lock.tryLock());int beats=0;bool unlocked=false;
        QTimer heartbeat;heartbeat.setInterval(1);connect(&heartbeat,&QTimer::timeout,this,[&]{++beats;});heartbeat.start();QTimer unlocker;unlocker.setSingleShot(true);connect(&unlocker,&QTimer::timeout,this,[&]{unlocked=true;lock.unlock();});unlocker.start(150);
        QSignalSpy done(backend,&PowerPolicyBackend::writeFinished),pending(backend,&PowerPolicyBackend::operationPending);backend->writePolicy(before,next);QTRY_COMPARE_WITH_TIMEOUT(done.size(),1,10000);QVERIFY(unlocked);QVERIFY(beats>=3);QVERIFY(!pending.isEmpty());QVERIFY(done[0][0].toBool());QVERIFY(done[0][1].toBool());QVERIFY2(done[0][3].toString().isEmpty(),qPrintable(done[0][3].toString()));
    }
    void cancellationBeforeWorkerCannotWriteAfterLateCapabilityReply(){
        AsyncPowerDaemon *native=nullptr;auto *backend=make(context(),&native);const auto before=read(backend),next=request(before);const auto original=contents(path);native->holdCapabilities=true;
        QSignalSpy done(backend,&PowerPolicyBackend::writeFinished);backend->writePolicy(before,next);QCOMPARE(done.size(),0);backend->cancel();QTRY_COMPARE(done.size(),1);QVERIFY(!done[0][0].toBool());QVERIFY(!done[0][1].toBool());QVERIFY(done[0][2].toBool());
        native->releaseCapabilities();QTest::qWait(50);QCOMPARE(done.size(),1);QCOMPARE(native->reloads,0);QCOMPARE(contents(path),original);QTRY_VERIFY(!OperationManager::instance()->hasActiveJobs());
    }
    void cancellationBeforeSyncWaitsForWorkerAndNeverSaves(){
        const auto held=gate();auto config=context();config.beforeSync=[held]{held->waitOnce();};AsyncPowerDaemon *native=nullptr;auto *backend=make(config,&native);const auto before=read(backend),next=request(before);const auto original=contents(path);QSignalSpy done(backend,&PowerPolicyBackend::writeFinished),pending(backend,&PowerPolicyBackend::operationPending);
        backend->writePolicy(before,next);QTRY_VERIFY(held->entered.available()>0);backend->cancel();QTRY_VERIFY(!pending.isEmpty());QCOMPARE(done.size(),0);QVERIFY(OperationManager::instance()->hasActiveJobs());QCOMPARE(contents(path),original);
        held->released.release();QTRY_COMPARE(done.size(),1);QVERIFY(!done[0][0].toBool());QVERIFY(done[0][2].toBool());QCOMPARE(native->reloads,0);QCOMPARE(contents(path),original);QTest::qWait(30);QCOMPARE(done.size(),1);
    }
    void cancellationDuringNativeSyncKeepsActivityAndReservationUntilRealTerminal(){
        AsyncPowerDaemon *native=nullptr;auto *first=make(context(),&native);const auto before=read(first),next=request(before);const auto original=contents(path);QLockFile lock(path+QStringLiteral(".lock"));QVERIFY(lock.tryLock());QSignalSpy done(first,&PowerPolicyBackend::writeFinished),pending(first,&PowerPolicyBackend::operationPending);
        first->writePolicy(before,next);QTRY_VERIFY(!pending.isEmpty());first->cancel();QTest::qWait(30);QCOMPARE(done.size(),0);QVERIFY(OperationManager::instance()->hasActiveJobs());QCOMPARE(native->reloads,0);QCOMPARE(contents(path),original);
        auto *second=make(context());QSignalSpy refused(second,&PowerPolicyBackend::writeFinished);second->writePolicy(before,request(before,1800));QTRY_COMPARE(refused.size(),1);QVERIFY(!refused[0][0].toBool());QVERIFY(!refused[0][3].toString().isEmpty());
        lock.unlock();QTRY_COMPARE_WITH_TIMEOUT(done.size(),1,10000);QVERIFY(done[0][0].toBool());QVERIFY(!done[0][1].toBool());QVERIFY(done[0][2].toBool());QVERIFY(!done[0][3].toString().isEmpty());QCOMPARE(native->reloads,0);QTRY_VERIFY(!OperationManager::instance()->hasActiveJobs());QVERIFY(contents(path).contains("TurnOffDisplayIdleTimeoutSec=1200"));
        const auto fresh=read(second);QSignalSpy retry(second,&PowerPolicyBackend::writeFinished);second->writePolicy(fresh,request(fresh,1800));QTRY_COMPARE(retry.size(),1);QVERIFY2(retry[0][1].toBool(),qPrintable(retry[0][3].toString()));
    }
    void orphanWriteRetainsDaemonActivityAndCanonicalLease_data(){QTest::addColumn<bool>("alias");QTest::newRow("same-path")<<false;QTest::newRow("dot-path")<<true;}
    void orphanWriteRetainsDaemonActivityAndCanonicalLease(){
        QFETCH(bool,alias);const auto held=gate();auto config=context();config.beforeSync=[held]{held->waitOnce();};AsyncPowerDaemon *native=nullptr;auto *first=make(config,&native);QPointer<AsyncPowerDaemon> retained(native);const auto reloads=native->reloadCount;const auto before=read(first),next=request(before);QSignalSpy gone(first,&PowerPolicyBackend::writeFinished);
        first->writePolicy(before,next);QTRY_VERIFY(held->entered.available()>0);delete first;QVERIFY(retained);QVERIFY(OperationManager::instance()->hasActiveJobs());
        auto *second=make(context(),nullptr,alias?directory.filePath("./powerdevilrc"):path);QSignalSpy refused(second,&PowerPolicyBackend::writeFinished);second->writePolicy(before,request(before,1800));QTRY_COMPARE(refused.size(),1);QVERIFY(!refused[0][0].toBool());QVERIFY(!refused[0][3].toString().isEmpty());
        held->released.release();QTRY_VERIFY_WITH_TIMEOUT(!OperationManager::instance()->hasActiveJobs(),10000);QCOMPARE(gone.size(),0);QCOMPARE(reloads->load(),1);QVERIFY(contents(path).contains("TurnOffDisplayIdleTimeoutSec=1200"));QVERIFY(!contents(path).contains("TurnOffDisplayIdleTimeoutSec=1800"));QTRY_VERIFY(!retained);
        const auto fresh=read(second);QSignalSpy retry(second,&PowerPolicyBackend::writeFinished);second->writePolicy(fresh,request(fresh,1800));QTRY_COMPARE(retry.size(),1);QVERIFY2(retry[0][1].toBool(),qPrintable(retry[0][3].toString()));
    }
    void oldAndReplacementHardlinksStayReservedUntilReloadTerminal_data(){QTest::addColumn<bool>("oldInode");QTest::addColumn<bool>("orphan");QTest::newRow("old-alive")<<true<<false;QTest::newRow("new-alive")<<false<<false;QTest::newRow("new-orphan")<<false<<true;}
    void oldAndReplacementHardlinksStayReservedUntilReloadTerminal(){
        QFETCH(bool,oldInode);QFETCH(bool,orphan);const auto oldAlias=directory.filePath("old-inode"),newAlias=directory.filePath("new-inode");QFile::remove(oldAlias);QFile::remove(newAlias);QVERIFY(::link(QFile::encodeName(path).constData(),QFile::encodeName(oldAlias).constData())==0);
        AsyncPowerDaemon *native=nullptr;auto *first=make(context(),&native);native->automaticReload=false;QPointer<AsyncPowerDaemon> retained(native);const auto before=read(first);QSignalSpy done(first,&PowerPolicyBackend::writeFinished);first->writePolicy(before,request(before));QTRY_COMPARE(native->reloads,1);QCOMPARE(done.size(),0);QVERIFY(::link(QFile::encodeName(path).constData(),QFile::encodeName(newAlias).constData())==0);if(orphan)delete first;
        auto *second=make(context(),nullptr,oldInode?oldAlias:newAlias);const auto aliasBefore=read(second);QCOMPARE(aliasBefore.profiles.size(),3);QSignalSpy refused(second,&PowerPolicyBackend::writeFinished);second->writePolicy(aliasBefore,request(aliasBefore,1800));QTRY_COMPARE(refused.size(),1);QVERIFY(!refused[0][0].toBool());QVERIFY(!refused[0][3].toString().isEmpty());QVERIFY(OperationManager::instance()->hasActiveJobs());
        retained->acknowledge();if(!orphan){QTRY_COMPARE(done.size(),1);QVERIFY2(done[0][1].toBool(),qPrintable(done[0][3].toString()));}QTRY_VERIFY(!OperationManager::instance()->hasActiveJobs());QCOMPARE(done.size(),orphan?0:1);QVERIFY(!contents(path).contains("TurnOffDisplayIdleTimeoutSec=1800"));QVERIFY(!contents(oldAlias).contains("TurnOffDisplayIdleTimeoutSec=1800"));
        const auto fresh=read(second);QSignalSpy retry(second,&PowerPolicyBackend::writeFinished);second->writePolicy(fresh,request(fresh,1800));QTRY_COMPARE(retry.size(),1);QVERIFY2(retry[0][1].toBool(),qPrintable(retry[0][3].toString()));
    }
    void fullCascadeReservesUserKdeglobalsUntilNativeSaveTerminal(){
        const auto home=QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation),local=home+QStringLiteral("/powerdevilrc"),global=home+QStringLiteral("/kdeglobals");save(local,"[AC][Display]\nTurnOffDisplayIdleTimeoutSec=600\n");save(global,"[AC][SuspendAndShutdown]\nPowerButtonAction=8\n[Other]\nKeep=global\n");
        const auto held=gate();auto config=context();config.fullCascade=true;config.beforeSync=[held]{held->waitOnce();};auto *first=make(config,nullptr,local),*second=make(context(),nullptr,global);const auto before=read(first),globalBefore=read(second);QVERIFY(before.profiles[0].keys.value(PowerPolicyKey::PowerButtonAction).globalPresent);auto reset=before;QString error;QVERIFY(PowerPolicyFields::restore(reset.profiles[0],before.profiles[0],PowerPolicyKey::PowerButtonAction,error));
        QSignalSpy done(first,&PowerPolicyBackend::writeFinished),refused(second,&PowerPolicyBackend::writeFinished);first->writePolicy(before,reset);QTRY_VERIFY(held->entered.available()>0);second->writePolicy(globalBefore,request(globalBefore,1800));QTRY_COMPARE(refused.size(),1);QVERIFY(!refused[0][0].toBool());held->released.release();QTRY_COMPARE(done.size(),1);QVERIFY2(done[0][1].toBool(),qPrintable(done[0][3].toString()));QVERIFY(contents(global).contains("Keep=global"));QVERIFY(!contents(global).contains("TurnOffDisplayIdleTimeoutSec=1800"));QFile::remove(local);QFile::remove(global);
    }
    void changedSourcesOrProviderBeforeReloadNeverTriggerReload_data(){
        QTest::addColumn<int>("change");QTest::newRow("foreign-local-key")<<0;QTest::newRow("foreign-default-source")<<1;QTest::newRow("owned-default")<<2;QTest::newRow("owner")<<3;QTest::newRow("same-owner-capability-revision")<<4;QTest::newRow("quiet-capability-drift")<<5;
    }
    void changedSourcesOrProviderBeforeReloadNeverTriggerReload(){
        QFETCH(int,change);const auto defaults=directory.filePath("native-defaults");save(defaults,"[Battery][Display]\nTurnOffDisplayIdleTimeoutSec=300\n[Other]\nDefaultUnowned=old\n");const auto held=gate();auto config=context();config.defaultSources={defaults};config.afterSync=[held]{held->waitOnce();};AsyncPowerDaemon *native=nullptr;auto *backend=make(config,&native);const auto before=read(backend),next=request(before);QSignalSpy done(backend,&PowerPolicyBackend::writeFinished);
        backend->writePolicy(before,next);QTRY_VERIFY(held->entered.available()>0);QVERIFY(contents(path).contains("TurnOffDisplayIdleTimeoutSec=1200"));QCOMPARE(native->reloads,0);
        if(change==0)save(path,contents(path)+"\n[Outside]\nForeign=preserve-me\n");else if(change==1)save(defaults,"[Battery][Display]\nTurnOffDisplayIdleTimeoutSec=300\n[Other]\nDefaultUnowned=changed\n");else if(change==2)save(defaults,"[Battery][Display]\nTurnOffDisplayIdleTimeoutSec=360\n");else{if(change==3)native->caps.owner=QStringLiteral(":private-power.2");else native->caps.revision=QStringLiteral("private-capabilities-v2");if(change!=5)emit native->providerChanged();}
        held->released.release();QTRY_COMPARE(done.size(),1);QVERIFY(done[0][0].toBool());QVERIFY(!done[0][1].toBool());QVERIFY(!done[0][3].toString().isEmpty());QCOMPARE(native->reloads,0);if(change==0)QVERIFY(contents(path).contains("preserve-me"));if(change==1)QVERIFY(contents(defaults).contains("DefaultUnowned=changed"));QTest::qWait(30);QCOMPARE(done.size(),1);
    }
    void policyAndProviderChangesWhileBeforeSyncAbortCleanly_data(){QTest::addColumn<bool>("provider");QTest::newRow("policy")<<false;QTest::newRow("provider")<<true;}
    void policyAndProviderChangesWhileBeforeSyncAbortCleanly(){
        QFETCH(bool,provider);const auto defaults=directory.filePath("boundary-defaults");save(defaults,"[AC][Display]\nTurnOffDisplayIdleTimeoutSec=600\n");const auto held=gate();auto config=context();config.defaultSources={defaults};config.beforeSync=[held]{held->waitOnce();};AsyncPowerDaemon *native=nullptr;auto *backend=make(config,&native);const auto before=read(backend),next=request(before);const auto original=contents(path);QSignalSpy done(backend,&PowerPolicyBackend::writeFinished);
        backend->writePolicy(before,next);QTRY_VERIFY(held->entered.available()>0);if(provider){native->caps.revision=QStringLiteral("replaced-before-sync");emit native->providerChanged();}else save(defaults,"[AC][Display]\nTurnOffDisplayIdleTimeoutSec[$i]=600\n");held->released.release();QTRY_COMPARE(done.size(),1);QVERIFY(!done[0][0].toBool());QVERIFY(!done[0][1].toBool());QVERIFY(!done[0][3].toString().isEmpty());QCOMPARE(native->reloads,0);QCOMPARE(contents(path),original);
    }
    void nativeFailureDoesNotRetryFromDestruction(){
        if(::geteuid()==0)QSKIP("Permission-failure fixture requires an unprivileged user.");const auto parent=directory.path();auto changed=std::make_shared<std::atomic<bool>>(false);auto config=context();config.beforeSync=[parent,changed]{changed->store(::chmod(QFile::encodeName(parent).constData(),0500)==0);};config.afterSync=[parent]{::chmod(QFile::encodeName(parent).constData(),0700);};
        AsyncPowerDaemon *native=nullptr;auto *backend=make(config,&native);const auto before=read(backend),next=request(before);QSignalSpy done(backend,&PowerPolicyBackend::writeFinished);backend->writePolicy(before,next);QTRY_COMPARE_WITH_TIMEOUT(done.size(),1,10000);::chmod(QFile::encodeName(parent).constData(),0700);QVERIFY(changed->load());QVERIFY(done[0][0].toBool());QVERIFY(!done[0][1].toBool());QVERIFY(!done[0][3].toString().isEmpty());QCOMPARE(native->reloads,0);QTRY_VERIFY(!OperationManager::instance()->hasActiveJobs());
        const QByteArray restored="[AC][Display]\nTurnOffDisplayIdleTimeoutSec=1800\n[Other]\nKeep=after-failure\n";save(path,restored);QTest::qWait(80);QCOMPARE(contents(path),restored);QCOMPARE(done.size(),1);
    }
    void fifoBrokenLinkAndDistinctSourceAliasFailWithoutBlocking_data(){QTest::addColumn<int>("kind");QTest::newRow("fifo")<<0;QTest::newRow("broken-link")<<1;QTest::newRow("source-hardlink")<<2;}
    void fifoBrokenLinkAndDistinctSourceAliasFailWithoutBlocking(){
        QFETCH(int,kind);auto config=context();const auto alias=directory.filePath("source-alias");QFile::remove(alias);if(kind==0){QVERIFY(QFile::remove(path));QVERIFY(::mkfifo(QFile::encodeName(path).constData(),0600)==0);}else if(kind==1){QVERIFY(QFile::remove(path));QVERIFY(QFile::link(directory.filePath("missing"),path));}else{QVERIFY(::link(QFile::encodeName(path).constData(),QFile::encodeName(alias).constData())==0);config.defaultSources={alias};}
        auto *backend=make(config);QSignalSpy done(backend,&PowerPolicyBackend::readFinished);backend->readCurrent();QTRY_COMPARE(done.size(),1);QVERIFY(!done[0][0].toBool());QVERIFY(!done[0][2].toString().isEmpty());QFile::remove(path);
    }
    void fixtureHooksAreIgnoredInProductionContext(){
        auto calls=std::make_shared<std::atomic<int>>(0);KdePowerPolicyBackend::ConfigContext config;config.softDeadlineMs=1;config.beforeFileRead=[calls]{++*calls;};config.beforeSync=[calls]{++*calls;};config.afterSync=[calls]{++*calls;};auto *backend=make(config);const auto before=read(backend);QSignalSpy done(backend,&PowerPolicyBackend::writeFinished);backend->writePolicy(before,request(before));QTRY_COMPARE(done.size(),1);QVERIFY2(done[0][1].toBool(),qPrintable(done[0][3].toString()));QCOMPARE(calls->load(),0);
    }
};
int main(int argc,char **argv){
    QCoreApplication app(argc,argv);if(!qEnvironmentVariableIsSet("CONTROL_POWER_ASYNC_CHILD")){
        QTemporaryDir sandbox;if(!sandbox.isValid())return 2;auto environment=QProcessEnvironment::systemEnvironment();const auto config=sandbox.filePath("config"),defaults=sandbox.filePath("defaults"),cache=sandbox.filePath("cache");if(!QDir().mkpath(config)||!QDir().mkpath(defaults)||!QDir().mkpath(cache))return 3;
        environment.insert("XDG_CONFIG_HOME",config);environment.insert("XDG_CONFIG_DIRS",defaults);environment.insert("XDG_CACHE_HOME",cache);environment.insert("KDE_SKIP_KDERC","1");environment.insert("CONTROL_POWER_ASYNC_CHILD","1");QProcess child;child.setProcessEnvironment(environment);child.setProcessChannelMode(QProcess::ForwardedChannels);QStringList arguments{"--",app.applicationFilePath()};arguments<<app.arguments().mid(1);child.start("/usr/bin/dbus-run-session",arguments);if(!child.waitForStarted(3000)||!child.waitForFinished(120000)){child.kill();child.waitForFinished(1000);return 4;}return child.exitStatus()==QProcess::NormalExit?child.exitCode():5;
    }PowerPolicyAsyncTest test;return QTest::qExec(&test,argc,argv);
}
#include "power_policy_async_test.moc"
