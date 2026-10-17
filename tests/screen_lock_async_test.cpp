#include <QtTest>
#include "platform/kde/ScreenLockBackend.h"
#include "operations/OperationManager.h"
#include <QCryptographicHash>
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
    std::atomic<bool> armed{true}; QSemaphore entered,released;
    void waitOnce() { if (armed.exchange(false)) { entered.release(); released.acquire(); } }
};
class AsyncLockerService final:public ScreenLockService {
public:
    ScreenLockRuntime state; int queries=0,configureAttempts=0,configures=0;
    std::shared_ptr<std::atomic<int>> accepted=std::make_shared<std::atomic<int>>(0);
    AsyncLockerService() { state.installedVerified=true; state.version=QStringLiteral("6.6.5"); state.epoch=1; }
    void online() { state.serviceAvailable=state.providerVerified=true; state.owner=QStringLiteral(":private-fixture.1"); }
    void query(quint64 token,const QString &owner={}) override {
        ++queries; const bool good=state.installedVerified && (!state.serviceAvailable || state.providerVerified) && (owner.isEmpty() || owner==state.owner);
        emit queryFinished(token,good,state,good?QString():QStringLiteral("Private fixture identity changed"));
    }
    void configure(quint64 token,const QString &owner,quint64 epoch,Guard guard) override {
        ++configureAttempts; QPointer<AsyncLockerService> safe(this); auto once=std::make_shared<bool>(false);
        guard([safe,once,token,owner,epoch](bool allowed,const QString &error) {
            if (*once || !safe) return; *once=true;
            const bool good=allowed && safe->state.providerVerified && owner==safe->state.owner && epoch==safe->state.epoch;
            if (good) { ++safe->configures; ++*safe->accepted; }
            emit safe->configureFinished(token,good,good?QString():error.isEmpty()?QStringLiteral("Provider or boundary changed"):error);
        });
    }
};
}

class ScreenLockAsyncTest final:public QObject {
    Q_OBJECT
    QTemporaryDir directory; QString path; quint64 token=0;
    QList<QPointer<KdeScreenLockBackend>> backends;
    QList<std::shared_ptr<WorkerGate>> gates;
    std::shared_ptr<WorkerGate> gate() { auto value=std::make_shared<WorkerGate>(); gates<<value; return value; }
    void save(const QString &file,const QByteArray &bytes) {
        QSaveFile output(file); QVERIFY(output.open(QIODevice::WriteOnly)); QCOMPARE(output.write(bytes),bytes.size()); QVERIFY(output.commit());
    }
    QByteArray contents(const QString &file) { QFile input(file); if (!input.open(QIODevice::ReadOnly)) return {}; return input.readAll(); }
    KdeScreenLockBackend *make(const KdeScreenLockBackend::ConfigContext &context,AsyncLockerService **service=nullptr,const QString &file={}) {
        auto *native=new AsyncLockerService; auto *backend=new KdeScreenLockBackend(file.isEmpty()?path:file,native,context); backends<<backend; if (service) *service=native; return backend;
    }
    ScreenLockSnapshot read(KdeScreenLockBackend *backend) {
        QSignalSpy done(backend,&ScreenLockBackend::readFinished); backend->readCurrent(++token); if (done.isEmpty()) done.wait(5000);
        if (done.size()!=1 || !done[0][1].toBool()) return {}; return qvariant_cast<ScreenLockSnapshot>(done[0][2]);
    }
    ScreenLockSnapshot request(const ScreenLockSnapshot &before) {
        auto next=before; QString error; if (!ScreenLockTypes::stage(next,before,ScreenLockKey::LockOnResume,false,error)) return {}; return next;
    }
    KdeScreenLockBackend::ConfigContext context() { KdeScreenLockBackend::ConfigContext value; value.fixture=true; value.softDeadlineMs=25; return value; }
private slots:
    void initTestCase() { qRegisterMetaType<ScreenLockSnapshot>(); qRegisterMetaType<ScreenLockRuntime>(); QStandardPaths::setTestModeEnabled(false); }
    void init() { QVERIFY(directory.isValid()); path=directory.filePath("kscreenlockerrc"); token=0; save(path,"[Daemon]\nLock=true\nLockOnResume=true\n[Foreign]\nKeep=original\n"); }
    void cleanup() {
        for (const auto &value:gates) value->released.release(16);
        QTRY_VERIFY_WITH_TIMEOUT(!OperationManager::instance()->hasActiveJobs(),10000);
        for (auto backend:backends) delete backend; backends.clear(); gates.clear(); if (QFileInfo(path).isDir()) QDir(path).removeRecursively();
    }
    void slowFileReadHasHeartbeatAndSoftPendingWithoutFalseCompletion() {
        const auto held=gate(); auto config=context(); config.beforeFileRead=[held]{held->waitOnce();}; auto *backend=make(config);
        QSignalSpy done(backend,&ScreenLockBackend::readFinished),pending(backend,&ScreenLockBackend::operationPending); int beats=0; QTimer heartbeat; heartbeat.setInterval(1); connect(&heartbeat,&QTimer::timeout,this,[&]{++beats;}); heartbeat.start();
        backend->readCurrent(++token); QTRY_VERIFY(held->entered.available()>0); QTRY_VERIFY(beats>=3); QTRY_VERIFY(!pending.isEmpty()); QCOMPARE(done.size(),0); QVERIFY(OperationManager::instance()->hasActiveJobs());
        held->released.release(); QTRY_COMPARE(done.size(),1); QVERIFY2(done[0][1].toBool(),qPrintable(done[0][3].toString())); QTRY_VERIFY(!OperationManager::instance()->hasActiveJobs());
    }
    void heldBeforeSyncIsNonblockingAndHasExactlyOneTerminalResult() {
        const auto held=gate(); auto config=context(); config.beforeSync=[held]{held->waitOnce();}; auto *backend=make(config); const auto before=read(backend); QCOMPARE(before.fields.size(),9); const auto next=request(before); QVERIFY(!next.intents.isEmpty());
        QSignalSpy done(backend,&ScreenLockBackend::writeFinished),pending(backend,&ScreenLockBackend::operationPending); int beats=0; QTimer heartbeat; heartbeat.setInterval(1); connect(&heartbeat,&QTimer::timeout,this,[&]{++beats;}); heartbeat.start(); const auto original=contents(path);
        backend->writeChanges(++token,before,next); QTRY_VERIFY(held->entered.available()>0); QTRY_VERIFY(beats>=3); QTRY_VERIFY(!pending.isEmpty()); QCOMPARE(done.size(),0); QCOMPARE(contents(path),original); QVERIFY(OperationManager::instance()->hasActiveJobs());
        held->released.release(); QTRY_COMPARE(done.size(),1); QVERIFY2(done[0][1].toBool(),qPrintable(done[0][3].toString())); QVERIFY(!done[0][2].toBool()); QVERIFY(done[0][3].toString().isEmpty()); QTest::qWait(30); QCOMPARE(done.size(),1); QVERIFY(contents(path).contains("LockOnResume=false"));
    }
    void nativeLockWaitAllowsGuiTimerToReleaseTheRealQLock() {
        auto config=context(); auto *backend=make(config); const auto before=read(backend); const auto next=request(before); QLockFile lock(path+QStringLiteral(".lock")); QVERIFY(lock.tryLock());
        int beats=0; bool unlocked=false; QTimer heartbeat; heartbeat.setInterval(1); connect(&heartbeat,&QTimer::timeout,this,[&]{++beats;}); heartbeat.start(); QTimer unlocker; unlocker.setSingleShot(true); connect(&unlocker,&QTimer::timeout,this,[&]{ unlocked=true; lock.unlock(); }); unlocker.start(120); QSignalSpy done(backend,&ScreenLockBackend::writeFinished),pending(backend,&ScreenLockBackend::operationPending);
        backend->writeChanges(++token,before,next); QTRY_COMPARE_WITH_TIMEOUT(done.size(),1,10000); QVERIFY(unlocked); QVERIFY(beats>=3); QVERIFY(!pending.isEmpty()); QVERIFY2(done[0][1].toBool(),qPrintable(done[0][3].toString())); QVERIFY(done[0][3].toString().isEmpty()); QVERIFY(contents(path).contains("LockOnResume=false"));
    }
    void deletedBackendKeepsJobActivityServiceAndCanonicalWriteLease_data() { QTest::addColumn<bool>("alias"); QTest::newRow("same-path")<<false; QTest::newRow("dot-alias")<<true; }
    void deletedBackendKeepsJobActivityServiceAndCanonicalWriteLease() {
        QFETCH(bool,alias); const auto held=gate(); auto config=context(); config.beforeSync=[held]{held->waitOnce();}; AsyncLockerService *service=nullptr; auto *first=make(config,&service); QPointer<AsyncLockerService> retained(service); const auto before=read(first),next=request(before); QSignalSpy firstDone(first,&ScreenLockBackend::writeFinished);
        first->writeChanges(++token,before,next); QTRY_VERIFY(held->entered.available()>0); delete first; QVERIFY(retained); QVERIFY(OperationManager::instance()->hasActiveJobs());
        auto *second=make(context(),nullptr,alias?directory.filePath("./kscreenlockerrc"):path); auto competing=before; QString error; QVERIFY(ScreenLockTypes::selectTimeout(competing,before,10.0,error)); QSignalSpy secondDone(second,&ScreenLockBackend::writeFinished); second->writeChanges(++token,before,competing); QTRY_COMPARE(secondDone.size(),1); QVERIFY(!secondDone[0][1].toBool()); QVERIFY(!secondDone[0][3].toString().isEmpty()); QVERIFY(OperationManager::instance()->hasActiveJobs());
        held->released.release(); QTRY_VERIFY_WITH_TIMEOUT(!OperationManager::instance()->hasActiveJobs(),10000); QCOMPARE(firstDone.size(),0); QCOMPARE(secondDone.size(),1); QVERIFY(contents(path).contains("LockOnResume=false")); QVERIFY(!contents(path).contains("Timeout=10"));
        auto *third=make(context()); const auto fresh=read(third); auto retry=fresh; QVERIFY(ScreenLockTypes::selectTimeout(retry,fresh,10.0,error)); QSignalSpy thirdDone(third,&ScreenLockBackend::writeFinished); third->writeChanges(++token,fresh,retry); QTRY_COMPARE(thirdDone.size(),1); QVERIFY2(thirdDone[0][1].toBool(),qPrintable(thirdDone[0][3].toString()));
    }
    void postSyncNewAndRetainedOldInodeAliasesStayLeased_data() {
        QTest::addColumn<bool>("oldInode"); QTest::addColumn<bool>("orphaned");
        QTest::newRow("new-inode-alive")<<false<<false; QTest::newRow("new-inode-orphaned")<<false<<true;
        QTest::newRow("old-inode-alive")<<true<<false; QTest::newRow("old-inode-orphaned")<<true<<true;
    }
    void postSyncNewAndRetainedOldInodeAliasesStayLeased() {
        QFETCH(bool,oldInode); QFETCH(bool,orphaned); const QString oldAlias=directory.filePath("old-inode-alias"),newAlias=directory.filePath("new-inode-alias"); QFile::remove(oldAlias); QFile::remove(newAlias);
        QVERIFY(::link(QFile::encodeName(path).constData(),QFile::encodeName(oldAlias).constData())==0);
        const auto held=gate(); auto config=context(); config.beforeBoundaryRead=[held]{held->waitOnce();}; AsyncLockerService *service=nullptr; auto *first=make(config,&service); service->online(); const auto accepted=service->accepted; const auto before=read(first),next=request(before); QSignalSpy firstDone(first,&ScreenLockBackend::writeFinished);
        first->writeChanges(++token,before,next); QTRY_VERIFY(held->entered.available()>0); QVERIFY(contents(path).contains("LockOnResume=false"));
        struct stat current{},old{}; QVERIFY(::stat(QFile::encodeName(path).constData(),&current)==0); QVERIFY(::stat(QFile::encodeName(oldAlias).constData(),&old)==0); QVERIFY(current.st_ino!=old.st_ino || current.st_dev!=old.st_dev);
        QVERIFY(::link(QFile::encodeName(path).constData(),QFile::encodeName(newAlias).constData())==0); if (orphaned) delete first;
        auto *second=make(context(),nullptr,oldInode?oldAlias:newAlias); const auto aliasBefore=read(second); QCOMPARE(aliasBefore.fields.size(),9); auto competing=aliasBefore; QString error; QVERIFY(ScreenLockTypes::selectTimeout(competing,aliasBefore,10.0,error)); QSignalSpy secondDone(second,&ScreenLockBackend::writeFinished); second->writeChanges(++token,aliasBefore,competing);
        QTRY_COMPARE(secondDone.size(),1); QVERIFY(!secondDone[0][1].toBool()); QVERIFY(!secondDone[0][3].toString().isEmpty()); QCOMPARE(accepted->load(),0); QVERIFY(OperationManager::instance()->hasActiveJobs());
        held->released.release(); if (!orphaned) { QTRY_COMPARE(firstDone.size(),1); QVERIFY2(firstDone[0][1].toBool(),qPrintable(firstDone[0][3].toString())); if (!firstDone[0][2].toBool()) QVERIFY(!firstDone[0][3].toString().isEmpty()); }
        QTRY_VERIFY_WITH_TIMEOUT(!OperationManager::instance()->hasActiveJobs(),10000); QVERIFY(accepted->load()<=1); if (!orphaned) QCOMPARE(accepted->load(),firstDone[0][2].toBool()?1:0); QCOMPARE(firstDone.size(),orphaned?0:1); QCOMPARE(secondDone.size(),1); QVERIFY(contents(path).contains("LockOnResume=false")); QVERIFY(contents(oldAlias).contains("LockOnResume=true")); QVERIFY(!contents(path).contains("Timeout=10")); QVERIFY(!contents(oldAlias).contains("Timeout=10"));
    }
    void fullCascadeWriteAlsoLeasesTheUserGlobalFile() {
        const QString home=QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation),local=home+QStringLiteral("/kscreenlockerrc"),global=home+QStringLiteral("/kdeglobals"); save(local,"[Daemon]\nLock=true\n"); save(global,"[Daemon]\nLockOnResume=false\n[Foreign]\nKeep=global\n");
        const auto held=gate(); auto config=context(); config.fullCascade=true; config.beforeSync=[held]{held->waitOnce();}; auto *first=make(config,nullptr,local); auto *second=make(context(),nullptr,global); const auto before=read(first),globalBefore=read(second); QVERIFY(before.fields.value(ScreenLockKey::LockOnResume).globalPresent); auto reset=before; QString error; QVERIFY(ScreenLockTypes::restore(reset,before,ScreenLockKey::LockOnResume,error)); auto globalRequest=globalBefore; QVERIFY(ScreenLockTypes::selectGrace(globalRequest,globalBefore,30,error));
        QSignalSpy firstDone(first,&ScreenLockBackend::writeFinished),secondDone(second,&ScreenLockBackend::writeFinished); first->writeChanges(++token,before,reset); QTRY_VERIFY(held->entered.available()>0); second->writeChanges(++token,globalBefore,globalRequest); QTRY_COMPARE(secondDone.size(),1); QVERIFY(!secondDone[0][1].toBool()); held->released.release(); QTRY_COMPARE(firstDone.size(),1); QVERIFY2(firstDone[0][1].toBool(),qPrintable(firstDone[0][3].toString())); QVERIFY(contents(global).contains("Keep=global")); QVERIFY(!contents(global).contains("LockGrace=30")); QFile::remove(local); QFile::remove(global);
    }
    void fileDefaultPolicyOrProviderChangesBeforeConfigureArePreserved_data() {
        QTest::addColumn<int>("change"); QTest::newRow("foreign-file")<<0; QTest::newRow("native-default")<<1; QTest::newRow("policy")<<2; QTest::newRow("provider-epoch")<<3;
    }
    void fileDefaultPolicyOrProviderChangesBeforeConfigureArePreserved() {
        QFETCH(int,change); const QString defaults=directory.filePath("native-defaults"); save(defaults,"[Daemon]\nTimeout=5\n"); const auto held=gate(); auto config=context(); config.defaultSources={defaults}; config.beforeBoundaryRead=[held]{held->waitOnce();}; AsyncLockerService *service=nullptr; auto *backend=make(config,&service); service->online(); const auto before=read(backend),next=request(before); QSignalSpy done(backend,&ScreenLockBackend::writeFinished),pending(backend,&ScreenLockBackend::operationPending);
        backend->writeChanges(++token,before,next); QTRY_VERIFY(held->entered.available()>0); QTRY_VERIFY(!pending.isEmpty()); QCOMPARE(done.size(),0); QCOMPARE(service->configures,0);
        if (change==0) save(path,contents(path)+"\n[Outside]\nKeep=foreign-edit\n"); else if (change==1) save(defaults,"[Daemon]\nTimeout=6\n"); else if (change==2) save(defaults,"[Daemon]\nTimeout=5\nLockOnResume[$i]=true\n"); else ++service->state.epoch;
        held->released.release(); QTRY_COMPARE(done.size(),1); QVERIFY(done[0][1].toBool()); QVERIFY(!done[0][2].toBool()); QVERIFY(!done[0][3].toString().isEmpty()); QCOMPARE(service->configures,0); QTest::qWait(30); QCOMPARE(done.size(),1);
        if (change==0) QVERIFY(contents(path).contains("foreign-edit")); if (change==1) QCOMPARE(contents(defaults),QByteArray("[Daemon]\nTimeout=6\n")); if (change==2) QVERIFY(contents(defaults).contains("[$i]"));
    }
    void terminalSyncFailureDoesNotRetryWhenThePathBecomesWritableAgain() {
        auto config=context(); auto sabotaged=std::make_shared<std::atomic<bool>>(false); const QString target=path;
        config.beforeSync=[target,sabotaged]{const bool removed=QFile::remove(target);const bool directory=QDir().mkdir(target);sabotaged->store(removed&&directory);}; auto *backend=make(config); const auto before=read(backend),next=request(before); QSignalSpy done(backend,&ScreenLockBackend::writeFinished);
        backend->writeChanges(++token,before,next); QTRY_COMPARE_WITH_TIMEOUT(done.size(),1,10000); QVERIFY(sabotaged->load()); QVERIFY(!done[0][2].toBool()); QVERIFY(!done[0][3].toString().isEmpty()); QTRY_VERIFY(!OperationManager::instance()->hasActiveJobs()); QVERIFY(QDir().rmdir(path)); const QByteArray restored="[Daemon]\nLockOnResume=true\n[Outside]\nKeep=after-failure\n"; save(path,restored); QTest::qWait(80); QCOMPARE(done.size(),1); QCOMPARE(contents(path),restored);
    }
    void fixtureHooksAreIgnoredUnlessExplicitlyEnabled() {
        auto calls=std::make_shared<std::atomic<int>>(0); KdeScreenLockBackend::ConfigContext config; config.fixture=false; config.softDeadlineMs=1; config.beforeFileRead=[calls]{++*calls;}; config.beforeSync=[calls]{++*calls;}; auto *backend=make(config); const auto before=read(backend),next=request(before); QSignalSpy done(backend,&ScreenLockBackend::writeFinished); backend->writeChanges(++token,before,next); QTRY_COMPARE(done.size(),1); QVERIFY2(done[0][1].toBool(),qPrintable(done[0][3].toString())); QCOMPARE(calls->load(),0);
    }
};
int main(int argc,char **argv)
{
    QCoreApplication app(argc,argv);
    if (!qEnvironmentVariableIsSet("CONTROL_SCREEN_LOCK_ASYNC_CHILD")) {
        QTemporaryDir sandbox; if (!sandbox.isValid()) return 2; auto environment=QProcessEnvironment::systemEnvironment();
        const auto config=sandbox.filePath("config"),defaults=sandbox.filePath("defaults"),cache=sandbox.filePath("cache"); if (!QDir().mkpath(config) || !QDir().mkpath(defaults) || !QDir().mkpath(cache)) return 3;
        environment.insert("XDG_CONFIG_HOME",config); environment.insert("XDG_CONFIG_DIRS",defaults); environment.insert("XDG_CACHE_HOME",cache); environment.insert("KDE_SKIP_KDERC","1"); environment.insert("CONTROL_SCREEN_LOCK_ASYNC_CHILD","1"); QProcess child; child.setProcessEnvironment(environment); child.setProcessChannelMode(QProcess::ForwardedChannels); QStringList arguments{"--",app.applicationFilePath()}; arguments<<app.arguments().mid(1); child.start("/usr/bin/dbus-run-session",arguments);
        if (!child.waitForStarted(3000) || !child.waitForFinished(120000)) { child.kill(); child.waitForFinished(1000); return 4; } return child.exitStatus()==QProcess::NormalExit?child.exitCode():5;
    }
    ScreenLockAsyncTest test; return QTest::qExec(&test,argc,argv);
}
#include "screen_lock_async_test.moc"
