#include <QtTest>
#include "platform/kde/NightTimeBackend.h"
#include <QDBusMessage>
#include <QDBusVariant>
#include <QDBusVirtualObject>
#include <QFile>
#include <QProcess>
#include <QSaveFile>
#include <QTemporaryDir>
#include <memory>

namespace {
const QString Service=QStringLiteral("org.kde.NightTime"),Path=QStringLiteral("/org/kde/NightTime/Manager"),Interface=QStringLiteral("org.kde.NightTime.Manager");
class FakeNightTime final : public QDBusVirtualObject {
public:
    QVariant version=1u;QString propertyType="u",subscribeArguments="<arg direction=\"in\" type=\"a{sv}\"/><arg direction=\"out\" type=\"a{sv}\"/>";
    int gets=0,subscribes=0,unsubscribes=0,otherCalls=0;bool hold=false;QDBusMessage held;
    void reset(){version=1u;propertyType="u";subscribeArguments="<arg direction=\"in\" type=\"a{sv}\"/><arg direction=\"out\" type=\"a{sv}\"/>";gets=subscribes=unsubscribes=otherCalls=0;hold=false;held={};}
    QString introspect(const QString &) const override
    {return QStringLiteral("<interface name=\"org.kde.NightTime.Manager\"><property name=\"Version\" type=\"%1\" access=\"read\"/><method name=\"Subscribe\">%2</method><method name=\"Unsubscribe\"><arg direction=\"in\" type=\"u\"/></method></interface>").arg(propertyType,subscribeArguments);}
    bool handleMessage(const QDBusMessage &message,const QDBusConnection &bus) override
    {
        if(message.interface()==QLatin1String("org.freedesktop.DBus.Properties")&&message.member()==QLatin1String("Get")){
            ++gets;if(message.arguments()!=QVariantList{Interface,QStringLiteral("Version")}){bus.send(message.createErrorReply("org.freedesktop.DBus.Error.InvalidArgs","Wrong getter"));return true;}
            if(hold)held=message;else bus.send(message.createReply(QVariantList{QVariant::fromValue(QDBusVariant(version))}));return true;
        }
        if(message.interface()==Interface){
            if(message.member()==QLatin1String("Subscribe"))++subscribes;else if(message.member()==QLatin1String("Unsubscribe"))++unsubscribes;else ++otherCalls;
            bus.send(message.createErrorReply("org.freedesktop.DBus.Error.AccessDenied","Read-only fixture forbids native subscriptions and changes"));return true;
        }
        return false;
    }
};
}
class NightTimeDBusTest final : public QObject {
    Q_OBJECT
    QTemporaryDir directory;QProcess daemon;QString address;std::unique_ptr<QDBusConnection> server,client,replacement;
    std::unique_ptr<NightTimeService> service;FakeNightTime native;
    QList<uint> identityPids;bool installed=true,processVerified=true;std::function<void(bool,QString)> heldIdentity;
    bool holdProcessIdentity=false;
    NightTimeIdentityVerifier verifier()
    {
        return [this](uint pid,std::function<void(bool,QString)> done){
            identityPids<<pid;
            if(pid&&holdProcessIdentity){heldIdentity=std::move(done);return;}
            const bool valid=pid?processVerified:installed;done(valid,valid?QString():QStringLiteral("Unverified native identity"));
        };
    }
    QList<QVariant> query(quint64 token=1,const QString &expected={})
    {
        QSignalSpy done(service.get(),&NightTimeService::queryFinished);service->query(token,expected);if(done.isEmpty())done.wait(4000);
        return done.isEmpty()?QList<QVariant>{QVariant::fromValue(token),false,QVariant::fromValue(NightTimeRuntime{}),QStringLiteral("No completion")}:done[0];
    }
    void assertNoNativeChanges(){QCOMPARE(native.subscribes,0);QCOMPARE(native.unsubscribes,0);QCOMPARE(native.otherCalls,0);}
private slots:
    void initTestCase(){qRegisterMetaType<NightTimeRuntime>();}
    void init()
    {
        QVERIFY(directory.isValid());identityPids.clear();installed=processVerified=true;holdProcessIdentity=false;heldIdentity={};native.reset();
        daemon.start("/usr/bin/dbus-daemon",{"--session","--nofork","--print-address=1"});QVERIFY(daemon.waitForStarted(2000));QVERIFY(daemon.waitForReadyRead(2000));address=QString::fromUtf8(daemon.readLine()).trimmed();QVERIFY(!address.isEmpty());
        server=std::make_unique<QDBusConnection>(QDBusConnection::connectToBus(address,"night-time-server"));client=std::make_unique<QDBusConnection>(QDBusConnection::connectToBus(address,"night-time-client"));
        QVERIFY(server->isConnected()&&client->isConnected());QVERIFY(server->registerService(Service));QVERIFY(server->registerVirtualObject(Path,&native));service.reset(createNightTimeService(*client,verifier()));
    }
    void cleanup()
    {
        assertNoNativeChanges();service.reset();replacement.reset();client.reset();server.reset();QDBusConnection::disconnectFromBus("night-time-replacement");QDBusConnection::disconnectFromBus("night-time-client");QDBusConnection::disconnectFromBus("night-time-server");
        daemon.terminate();if(!daemon.waitForFinished(2000)){daemon.kill();daemon.waitForFinished(1000);}
    }
    void exactGetterWireAndPinnedProcessIdentity()
    {
        const auto result=query();QVERIFY2(result[1].toBool(),qPrintable(result[3].toString()));const auto runtime=qvariant_cast<NightTimeRuntime>(result[2]);
        QVERIFY(runtime.installedVerified&&runtime.serviceAvailable&&runtime.providerVerified);QCOMPARE(runtime.owner,server->baseService());QCOMPARE(runtime.version,QStringLiteral("6.6.5"));QCOMPARE(runtime.apiVersion,1u);
        QCOMPARE(native.gets,1);QCOMPARE(identityPids.size(),2);QCOMPARE(identityPids[0],0u);QVERIFY(identityPids[1]>0);
    }
    void offlineReadDoesNotActivateOrSubscribe()
    {
        QSignalSpy changed(service.get(),&NightTimeService::externalChange);QVERIFY(server->unregisterService(Service));QTRY_VERIFY(!changed.isEmpty());
        const auto result=query();QVERIFY(result[1].toBool());const auto runtime=qvariant_cast<NightTimeRuntime>(result[2]);
        QVERIFY(runtime.installedVerified&&!runtime.serviceAvailable&&!runtime.providerVerified);QVERIFY(runtime.owner.isEmpty());QCOMPARE(native.gets,0);QCOMPARE(identityPids,QList<uint>{0u});
        const auto stale=query(2,server->baseService());QVERIFY(!stale[1].toBool());
    }
    void nativeReturnFirstOrderingIsAcceptedWithoutSubscribing()
    {
        native.subscribeArguments="<arg direction=\"out\" type=\"a{sv}\"/><arg direction=\"in\" type=\"a{sv}\"/>";
        const auto result=query();QVERIFY2(result[1].toBool(),qPrintable(result[3].toString()));QVERIFY(qvariant_cast<NightTimeRuntime>(result[2]).providerVerified);
        QCOMPARE(native.gets,1);assertNoNativeChanges();
    }
    void duplicateAndInvalidArgumentDirectionsAreRejected()
    {
        native.subscribeArguments="<arg direction=\"out\" type=\"a{sv}\"/><arg direction=\"in\" type=\"a{sv}\"/><arg direction=\"in\" type=\"a{sv}\"/>";
        QVERIFY(!query()[1].toBool());QCOMPARE(native.gets,0);
        native.subscribeArguments="<arg direction=\"sideways\" type=\"a{sv}\"/><arg direction=\"in\" type=\"a{sv}\"/>";
        QVERIFY(!query(2)[1].toBool());QCOMPARE(native.gets,0);assertNoNativeChanges();
    }
    void unavailableInstallationDoesNotContactNativeObject()
    {
        installed=false;const auto result=query();QVERIFY(!result[1].toBool());QVERIFY(!qvariant_cast<NightTimeRuntime>(result[2]).installedVerified);QCOMPARE(native.gets,0);QCOMPARE(identityPids,QList<uint>{0u});
    }
    void futureVersionWrongTypeAndWrongIntrospectionAreReadOnly()
    {
        native.version=2u;QVERIFY(!query()[1].toBool());native.version=QStringLiteral("1");QVERIFY(!query(2)[1].toBool());
        native.version=1u;native.propertyType="s";const int previous=native.gets;QVERIFY(!query(3)[1].toBool());QCOMPARE(native.gets,previous);
    }
    void processIdentityFailureAndExpectedOwnerMismatchAreRejected()
    {
        processVerified=false;auto result=query();QVERIFY(!result[1].toBool());const auto runtime=qvariant_cast<NightTimeRuntime>(result[2]);QVERIFY(runtime.installedVerified&&runtime.serviceAvailable&&!runtime.providerVerified);
        processVerified=true;const int previous=native.gets;result=query(2,QStringLiteral(":1.99999"));QVERIFY(!result[1].toBool());QCOMPARE(native.gets,previous);
    }
    void sameOwnerReregistrationInvalidatesInFlightIdentity()
    {
        const auto first=query();QVERIFY(first[1].toBool());const auto before=qvariant_cast<NightTimeRuntime>(first[2]);
        holdProcessIdentity=true;QSignalSpy replies(service.get(),&NightTimeService::queryFinished),changed(service.get(),&NightTimeService::externalChange);service->query(10,before.owner);QTRY_VERIFY(bool(heldIdentity));
        QVERIFY(server->unregisterService(Service));QTRY_VERIFY(!changed.isEmpty());QVERIFY(server->registerService(Service));QTRY_VERIFY(changed.size()>=2);
        auto release=std::move(heldIdentity);release(true,{});QTRY_COMPARE(replies.size(),1);QVERIFY(!replies[0][1].toBool());QVERIFY(!qvariant_cast<NightTimeRuntime>(replies[0][2]).providerVerified);
        holdProcessIdentity=false;const auto fresh=query(11);QVERIFY(fresh[1].toBool());const auto after=qvariant_cast<NightTimeRuntime>(fresh[2]);QCOMPARE(after.owner,before.owner);QVERIFY(after.epoch!=before.epoch);
    }
    void ownerReplacementDuringHeldGetterCannotSucceed()
    {
        native.hold=true;QSignalSpy replies(service.get(),&NightTimeService::queryFinished),changed(service.get(),&NightTimeService::externalChange);service->query(10);QTRY_COMPARE(native.gets,1);
        QVERIFY(server->unregisterService(Service));replacement=std::make_unique<QDBusConnection>(QDBusConnection::connectToBus(address,"night-time-replacement"));QVERIFY(replacement->registerService(Service));QTRY_VERIFY(!changed.isEmpty());
        QVERIFY(server->send(native.held.createReply(QVariantList{QVariant::fromValue(QDBusVariant(1u))})));QTRY_COMPARE(replies.size(),1);QVERIFY(!replies[0][1].toBool());
    }
    void boundedGetterTimeoutHasNoActivationOrCleanupCalls()
    {
        native.hold=true;const auto result=query();QVERIFY(!result[1].toBool());QCOMPARE(native.gets,1);QCOMPARE(identityPids,QList<uint>{0u});
    }
    void destroyingServiceDiscardsHeldIdentityCompletion()
    {
        holdProcessIdentity=true;service->query(10);QTRY_VERIFY(bool(heldIdentity));service.reset();auto release=std::move(heldIdentity);release(true,{});assertNoNativeChanges();
    }
};
QTEST_GUILESS_MAIN(NightTimeDBusTest)
#include "night_time_dbus_test.moc"
