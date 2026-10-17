#include <QtTest>
#include "platform/kde/ScreenLockBackend.h"
#include <QDBusMessage>
#include <QDBusVirtualObject>
#include <QProcess>
#include <memory>
namespace {
const QString Service=QStringLiteral("org.kde.screensaver"),Alias=QStringLiteral("org.freedesktop.ScreenSaver"),KWin=QStringLiteral("org.kde.KWin");
class FakeKWin final : public QDBusVirtualObject {
public:
    QString report="KWin version: 6.6.6\nOperation Mode: Wayland\n";int gets=0;
    QString introspect(const QString &) const override{return "<interface name=\"org.kde.KWin\"><method name=\"supportInformation\"><arg direction=\"out\" type=\"s\"/></method></interface>";}
    bool handleMessage(const QDBusMessage &message,const QDBusConnection &bus) override
    {if(message.interface()==KWin&&message.member()==QLatin1String("supportInformation")){++gets;bus.send(message.createReply(QVariantList{report}));return true;}return false;}
};
class FakeLocker final : public QDBusVirtualObject {
public:
    QString arguments;int configures=0,forbidden=0;bool fail=false,hold=false;QDBusMessage held;
    void reset(){arguments.clear();configures=forbidden=0;fail=hold=false;held={};}
    QString introspect(const QString &) const override
    {return QStringLiteral("<interface name=\"org.kde.screensaver\"><method name=\"configure\">%1</method></interface><interface name=\"org.freedesktop.ScreenSaver\"><method name=\"Lock\"/></interface>").arg(arguments);}
    bool handleMessage(const QDBusMessage &message,const QDBusConnection &bus) override
    {
        if(message.interface()==Service&&message.member()==QLatin1String("configure")){
            ++configures;if(!message.arguments().isEmpty()){++forbidden;bus.send(message.createErrorReply("org.freedesktop.DBus.Error.InvalidArgs","Configure takes no arguments"));return true;}
            if(hold)held=message;else if(fail)bus.send(message.createErrorReply("org.freedesktop.DBus.Error.Failed","Fixture reload failure"));else bus.send(message.createReply());return true;
        }
        if(message.interface()==Service||message.interface()==Alias){++forbidden;bus.send(message.createErrorReply("org.freedesktop.DBus.Error.AccessDenied","Fixture forbids locking, inhibition, and other actions"));return true;}return false;
    }
};
}
class ScreenLockDBusTest final : public QObject {
    Q_OBJECT
    QProcess daemon;QString address;std::unique_ptr<QDBusConnection> server,client,replacement;std::unique_ptr<ScreenLockService> service;
    FakeKWin kwin;FakeLocker locker;QList<uint> pids;bool installed=true,verified=true,holdIdentity=false;std::function<void(bool,QString)> heldIdentity;
    ScreenLockIdentityVerifier verifier()
    {return [this](uint pid,std::function<void(bool,QString)> done){pids<<pid;if(pid&&holdIdentity){heldIdentity=std::move(done);return;}const bool valid=pid?verified:installed;done(valid,valid?QString():QStringLiteral("Unverified native identity"));};}
    QList<QVariant> query(quint64 token=1,const QString &owner={})
    {QSignalSpy done(service.get(),&ScreenLockService::queryFinished);service->query(token,owner);if(done.isEmpty())done.wait(5000);return done.isEmpty()?QList<QVariant>{QVariant::fromValue(token),false,QVariant::fromValue(ScreenLockRuntime{}),QStringLiteral("No completion")}:done[0];}
    QList<QVariant> configure(const ScreenLockRuntime &runtime,ScreenLockService::Guard guard=[](ScreenLockService::GuardCompletion done){done(true,{});})
    {QSignalSpy done(service.get(),&ScreenLockService::configureFinished);service->configure(10,runtime.owner,runtime.epoch,std::move(guard));if(done.isEmpty())done.wait(5000);return done.isEmpty()?QList<QVariant>{10,false,QStringLiteral("No completion")}:done[0];}
private slots:
    void initTestCase(){qRegisterMetaType<ScreenLockRuntime>();}
    void init()
    {
        pids.clear();installed=verified=true;holdIdentity=false;heldIdentity={};locker.reset();kwin.report="KWin version: 6.6.6\nOperation Mode: Wayland\n";kwin.gets=0;
        daemon.start("/usr/bin/dbus-daemon",{"--session","--nofork","--print-address=1"});QVERIFY(daemon.waitForStarted(2000));QVERIFY(daemon.waitForReadyRead(2000));address=QString::fromUtf8(daemon.readLine()).trimmed();QVERIFY(!address.isEmpty());
        server=std::make_unique<QDBusConnection>(QDBusConnection::connectToBus(address,"screen-lock-server"));client=std::make_unique<QDBusConnection>(QDBusConnection::connectToBus(address,"screen-lock-client"));QVERIFY(server->isConnected()&&client->isConnected());
        QVERIFY(server->registerService(Service));QVERIFY(server->registerService(Alias));QVERIFY(server->registerService(KWin));QVERIFY(server->registerVirtualObject("/ScreenSaver",&locker));QVERIFY(server->registerVirtualObject("/KWin",&kwin));service.reset(createScreenLockService(*client,verifier()));
    }
    void cleanup()
    {
        QCOMPARE(locker.forbidden,0);service.reset();replacement.reset();client.reset();server.reset();QDBusConnection::disconnectFromBus("screen-lock-replacement");QDBusConnection::disconnectFromBus("screen-lock-client");QDBusConnection::disconnectFromBus("screen-lock-server");QDBusConnection::disconnectFromBus("screen-lock-invalid");daemon.terminate();if(!daemon.waitForFinished(2000)){daemon.kill();daemon.waitForFinished(1000);}
    }
    void passiveDiscoveryPinsThreeAliasesAndNativeProcess()
    {
        const auto result=query();QVERIFY2(result[1].toBool(),qPrintable(result[3].toString()));const auto runtime=qvariant_cast<ScreenLockRuntime>(result[2]);QVERIFY(runtime.installedVerified&&runtime.serviceAvailable&&runtime.providerVerified);QCOMPARE(runtime.owner,server->baseService());QCOMPARE(pids.size(),2);QCOMPARE(pids[0],0u);QVERIFY(pids[1]>0);QCOMPARE(locker.configures,0);
    }
    void bothAliasesAbsentAllowOfflineWithoutActivation()
    {
        QSignalSpy changed(service.get(),&ScreenLockService::externalChange);QVERIFY(server->unregisterService(Service));QVERIFY(server->unregisterService(Alias));QTRY_VERIFY(changed.size()>=2);const auto result=query();QVERIFY(result[1].toBool());const auto runtime=qvariant_cast<ScreenLockRuntime>(result[2]);QVERIFY(runtime.installedVerified&&!runtime.serviceAvailable&&!runtime.providerVerified);QCOMPARE(kwin.gets,0);QCOMPARE(locker.configures,0);QVERIFY(!configure(runtime)[1].toBool());
    }
    void partialAndSplitAliasesFailClosed()
    {
        QSignalSpy changed(service.get(),&ScreenLockService::externalChange);QVERIFY(server->unregisterService(Alias));QTRY_VERIFY(!changed.isEmpty());auto result=query();QVERIFY(!result[1].toBool());QVERIFY(qvariant_cast<ScreenLockRuntime>(result[2]).serviceAvailable);
        replacement=std::make_unique<QDBusConnection>(QDBusConnection::connectToBus(address,"screen-lock-replacement"));QVERIFY(replacement->registerService(Alias));QTRY_VERIFY(changed.size()>=2);QVERIFY(!query(2)[1].toBool());QCOMPARE(locker.configures,0);
    }
    void strictVersionModeAndVoidConfigureContract()
    {
        kwin.report+="KWin version: 6.6.7\n";QVERIFY(!query()[1].toBool());kwin.report="KWin version: 6.6.6\nOperation Mode: Wayland\nOperation Mode: X11\n";QVERIFY(!query(2)[1].toBool());kwin.report="KWin version: 6.6.6\nOperation Mode: Wayland\n";locker.arguments="<arg direction=\"out\" type=\"b\"/>";QVERIFY(!query(3)[1].toBool());QCOMPARE(locker.configures,0);
    }
    void explicitConfigureAcknowledgesVoidReplyOnly()
    {
        const auto runtime=qvariant_cast<ScreenLockRuntime>(query()[2]);int guards=0;const auto result=configure(runtime,[&](ScreenLockService::GuardCompletion done){++guards;done(true,{});});QVERIFY2(result[1].toBool(),qPrintable(result[2].toString()));QCOMPARE(guards,1);QCOMPARE(locker.configures,1);
    }
    void rejectedFileGuardSendsNoConfigure()
    {
        const auto runtime=qvariant_cast<ScreenLockRuntime>(query()[2]);const auto result=configure(runtime,[](ScreenLockService::GuardCompletion done){done(false,QStringLiteral("Outside file edit"));});QVERIFY(!result[1].toBool());QVERIFY(result[2].toString().contains("Outside file edit"));QCOMPARE(locker.configures,0);
    }
    void observedEpochChangeInsideGuardSendsNoConfigure()
    {
        const auto runtime=qvariant_cast<ScreenLockRuntime>(query()[2]);ScreenLockService::GuardCompletion held;QSignalSpy reply(service.get(),&ScreenLockService::configureFinished),changed(service.get(),&ScreenLockService::externalChange);
        service->configure(10,runtime.owner,runtime.epoch,[&](ScreenLockService::GuardCompletion done){held=std::move(done);});QTRY_VERIFY(bool(held));
        QVERIFY(server->unregisterService(Service));QTRY_VERIFY(!changed.isEmpty());QVERIFY(server->registerService(Service));QTRY_VERIFY(changed.size()>=2);held(true,{});QTRY_COMPARE(reply.size(),1);QVERIFY(!reply[0][1].toBool());QCOMPARE(locker.configures,0);
        held(true,{});QCoreApplication::processEvents();QCOMPARE(reply.size(),1);QCOMPARE(locker.configures,0);
    }
    void heldAsyncGuardKeepsTheEventLoopResponsiveAndCompletesOnlyOnce()
    {
        const auto runtime=qvariant_cast<ScreenLockRuntime>(query()[2]);ScreenLockService::GuardCompletion held;QSignalSpy reply(service.get(),&ScreenLockService::configureFinished);int beats=0;QTimer heartbeat;heartbeat.setInterval(1);connect(&heartbeat,&QTimer::timeout,this,[&]{++beats;});heartbeat.start();
        service->configure(10,runtime.owner,runtime.epoch,[&](ScreenLockService::GuardCompletion done){held=std::move(done);});QTRY_VERIFY(bool(held));QTRY_VERIFY(beats>=3);QCOMPARE(reply.size(),0);QCOMPARE(locker.configures,0);
        held(true,{});QTRY_COMPARE(reply.size(),1);QVERIFY(reply[0][1].toBool());held(false,QStringLiteral("late duplicate"));held(true,{});QCoreApplication::processEvents();QCOMPARE(reply.size(),1);QCOMPARE(locker.configures,1);
    }
    void duplicateImmediateGuardCompletionsSendAtMostOneRpcAndTerminalSignal()
    {
        const auto runtime=qvariant_cast<ScreenLockRuntime>(query()[2]);QSignalSpy reply(service.get(),&ScreenLockService::configureFinished);
        service->configure(10,runtime.owner,runtime.epoch,[](ScreenLockService::GuardCompletion done){done(true,{});done(true,{});done(false,QStringLiteral("duplicate rejection"));});QTRY_COMPARE(reply.size(),1);QCoreApplication::processEvents();QVERIFY(reply[0][1].toBool());QCOMPARE(locker.configures,1);
    }
    void differentOwnerDuringHeldGuardNeverReceivesConfigure()
    {
        const auto runtime=qvariant_cast<ScreenLockRuntime>(query()[2]);ScreenLockService::GuardCompletion held;QSignalSpy reply(service.get(),&ScreenLockService::configureFinished),changed(service.get(),&ScreenLockService::externalChange);
        service->configure(10,runtime.owner,runtime.epoch,[&](ScreenLockService::GuardCompletion done){held=std::move(done);});QTRY_VERIFY(bool(held));QVERIFY(server->unregisterService(Service));QVERIFY(server->unregisterService(Alias));
        replacement=std::make_unique<QDBusConnection>(QDBusConnection::connectToBus(address,"screen-lock-replacement"));QVERIFY(replacement->registerService(Service));QVERIFY(replacement->registerService(Alias));QTRY_VERIFY(changed.size()>=4);held(true,{});QTRY_COMPARE(reply.size(),1);QVERIFY(!reply[0][1].toBool());QCOMPARE(locker.configures,0);
    }
    void destructionDiscardsHeldGuardAndAllDuplicateCompletions()
    {
        const auto runtime=qvariant_cast<ScreenLockRuntime>(query()[2]);ScreenLockService::GuardCompletion held;QSignalSpy reply(service.get(),&ScreenLockService::configureFinished);
        service->configure(10,runtime.owner,runtime.epoch,[&](ScreenLockService::GuardCompletion done){held=std::move(done);});QTRY_VERIFY(bool(held));service.reset();held(true,{});held(false,QStringLiteral("late"));QCoreApplication::processEvents();QCOMPARE(reply.size(),0);QCOMPARE(locker.configures,0);
    }
    void reloadErrorAndTimeoutNeverCountAsAcknowledgedOrRetry()
    {
        const auto runtime=qvariant_cast<ScreenLockRuntime>(query()[2]);locker.fail=true;auto result=configure(runtime);QVERIFY(!result[1].toBool());QCOMPARE(locker.configures,1);locker.fail=false;locker.hold=true;result=configure(runtime);QVERIFY(!result[1].toBool());QVERIFY(result[2].toString().contains("does not cancel queued execution"));QCOMPARE(locker.configures,2);
    }
    void sameOwnerReregistrationRejectsOldEpoch()
    {
        const auto runtime=qvariant_cast<ScreenLockRuntime>(query()[2]);QSignalSpy changed(service.get(),&ScreenLockService::externalChange);QVERIFY(server->unregisterService(Service));QTRY_VERIFY(!changed.isEmpty());QVERIFY(server->registerService(Service));QTRY_VERIFY(changed.size()>=2);QVERIFY(!configure(runtime)[1].toBool());QCOMPARE(locker.configures,0);const auto fresh=qvariant_cast<ScreenLockRuntime>(query(2)[2]);QCOMPARE(fresh.owner,runtime.owner);QVERIFY(fresh.epoch!=runtime.epoch);
    }
    void ownerChangeAfterRpcCannotBecomeAcknowledged()
    {
        const auto runtime=qvariant_cast<ScreenLockRuntime>(query()[2]);locker.hold=true;QSignalSpy reply(service.get(),&ScreenLockService::configureFinished),changed(service.get(),&ScreenLockService::externalChange);service->configure(10,runtime.owner,runtime.epoch,[](ScreenLockService::GuardCompletion done){done(true,{});});QTRY_COMPARE(locker.configures,1);QVERIFY(server->unregisterService(Service));QTRY_VERIFY(!changed.isEmpty());QVERIFY(server->registerService(Service));QTRY_VERIFY(changed.size()>=2);QVERIFY(server->send(locker.held.createReply()));QTRY_COMPARE(reply.size(),1);QVERIFY(!reply[0][1].toBool());QCOMPARE(locker.configures,1);
    }
    void identityAndDisconnectedBusErrorsAreReadOnly()
    {
        verified=false;auto result=query();QVERIFY(!result[1].toBool());QVERIFY(!qvariant_cast<ScreenLockRuntime>(result[2]).providerVerified);const auto disconnected=QDBusConnection::connectToBus("unix:path=/tmp/control-screen-lock-no-such-bus","screen-lock-invalid");service.reset(createScreenLockService(disconnected,verifier()));result=query(2);QVERIFY(!result[1].toBool());QVERIFY(!qvariant_cast<ScreenLockRuntime>(result[2]).installedVerified);QCOMPARE(locker.configures,0);
    }
    void destructionDiscardsHeldIdentity()
    {
        holdIdentity=true;service->query(10);QTRY_VERIFY(bool(heldIdentity));service.reset();auto release=std::move(heldIdentity);release(true,{});QCOMPARE(locker.configures,0);
    }
};
QTEST_GUILESS_MAIN(ScreenLockDBusTest)
#include "screen_lock_dbus_test.moc"
