#include <QtTest>
#include "platform/kde/NightLightBackend.h"
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusVirtualObject>
#include <QProcess>
#include <memory>
#include "operations/OperationManager.h"
#include <QThreadPool>
#include <QtConcurrent/QtConcurrentRun>
#include <QSemaphore>
#include <QTemporaryDir>
#include <QFile>
#include <QPointer>

namespace {
const QString NightService=QStringLiteral("org.kde.KWin.NightLight"),NightPath=QStringLiteral("/org/kde/KWin/NightLight");
const QByteArray Schema=R"(<kcfg><kcfgfile arg="true"/><group name="NightColor"><entry name="Active" type="Bool"><default>false</default></entry><entry name="Mode" type="Enum"><choices><choice name="Constant"/><choice name="DarkLight"/></choices><default>NightLightMode::DarkLight</default></entry><entry name="DayTemperature" type="Int"><default>6500</default></entry><entry name="NightTemperature" type="Int"><default>4500</default></entry></group></kcfg>)";
// Saturate only this private test process's global pool, after discovery.
// The blocker captures shared semaphore values, never a service or backend.
class HeldSchemaPool {
    struct Gate {QSemaphore entered,released;};
    QThreadPool *pool=QThreadPool::globalInstance();int previous=pool->maxThreadCount();
    std::shared_ptr<Gate> gate=std::make_shared<Gate>();QFuture<void> future;bool released=false;
public:
    HeldSchemaPool(){pool->setMaxThreadCount(1);const auto values=gate;future=QtConcurrent::run(pool,[values]{values->entered.release();values->released.acquire();});}
    bool started() const{return gate->entered.available()>0;}
    void release(){if(!released){released=true;gate->released.release();}}
    bool drained() const{return pool->activeThreadCount()==0;}
    ~HeldSchemaPool(){release();future.waitForFinished();pool->setMaxThreadCount(previous);}
};
class FakeKWin final : public QDBusVirtualObject {
public:
    QString report=QStringLiteral("KWin version: 6.6.6\nOperation Mode: Wayland\n");
    QString introspect(const QString &) const override{return QStringLiteral("<interface name=\"org.kde.KWin\"><method name=\"supportInformation\"><arg direction=\"out\" type=\"s\"/></method></interface>");}
    bool handleMessage(const QDBusMessage &message,const QDBusConnection &bus) override
    {if(message.interface()==QLatin1String("org.kde.KWin")&&message.member()==QLatin1String("supportInformation")){bus.send(message.createReply(QVariantList{report}));return true;}return false;}
};
class FakeNight final : public QDBusVirtualObject {
public:
    QVariantMap properties{{"available",true},{"enabled",false},{"running",false},{"inhibited",false},{"daylight",true},
        {"mode",1u},{"currentTemperature",6500u},{"targetTemperature",6500u},{"previousTransitionDateTime",QVariant::fromValue<qulonglong>(0)},
        {"scheduledTransitionDateTime",QVariant::fromValue<qulonglong>(0)},{"previousTransitionDuration",0u},{"scheduledTransitionDuration",0u}};
    int previews=0,stops=0,otherMutations=0,gets=0;bool holdPreview=false,holdGet=false;QDBusMessage delayedPreview,delayedGet;
    void reset()
    {
        properties={{"available",true},{"enabled",false},{"running",false},{"inhibited",false},{"daylight",true},
            {"mode",1u},{"currentTemperature",6500u},{"targetTemperature",6500u},{"previousTransitionDateTime",QVariant::fromValue<qulonglong>(0)},
            {"scheduledTransitionDateTime",QVariant::fromValue<qulonglong>(0)},{"previousTransitionDuration",0u},{"scheduledTransitionDuration",0u}};
        previews=stops=otherMutations=gets=0;holdPreview=holdGet=false;delayedPreview={};delayedGet={};
    }
    QString introspect(const QString &) const override
    {
        QString xml="<interface name=\"org.kde.KWin.NightLight\">";
        for(auto it=properties.cbegin();it!=properties.cend();++it){
            const auto key=it.key();const QString type=key.endsWith("DateTime")?"t":key.endsWith("Temperature")||key.endsWith("Duration")||key=="mode"?"u":"b";
            xml+=QStringLiteral("<property name=\"%1\" type=\"%2\" access=\"read\"/>").arg(key,type);
        }
        xml+="<method name=\"preview\"><arg direction=\"in\" type=\"u\"/></method><method name=\"stopPreview\"/></interface>";return xml;
    }
    bool handleMessage(const QDBusMessage &message,const QDBusConnection &bus) override
    {
        if(message.interface()==QLatin1String("org.freedesktop.DBus.Properties")&&message.member()==QLatin1String("GetAll")){
            ++gets;if(holdGet)delayedGet=message;else bus.send(message.createReply(QVariantList{properties}));return true;
        }
        if(message.interface()==NightService&&message.member()==QLatin1String("preview")){
            ++previews;if(holdPreview)delayedPreview=message;else bus.send(message.createReply());return true;
        }
        if(message.interface()==NightService&&message.member()==QLatin1String("stopPreview")){++stops;bus.send(message.createReply());return true;}
        if(message.interface()==NightService){++otherMutations;bus.send(message.createErrorReply(QStringLiteral("org.freedesktop.DBus.Error.UnknownMethod"),QStringLiteral("Fixture disallows mutations")));return true;}
        return false;
    }
};
}
class NightLightDBusTest final : public QObject {
    Q_OBJECT
    QProcess daemon;QString address;
    std::unique_ptr<QDBusConnection> server,client,replacement;
    std::unique_ptr<NightLightService> service;
    FakeKWin kwin;FakeNight night;
    NightLightRuntime query(quint64 token=1)
    {
        QSignalSpy spy(service.get(),&NightLightService::queryFinished);service->query(token);
        if(spy.isEmpty())spy.wait(4000);if(spy.isEmpty())return {};
        return qvariant_cast<NightLightRuntime>(spy[0][2]);
    }
private slots:
    void initTestCase(){qRegisterMetaType<NightLightRuntime>();qRegisterMetaType<NightLightSnapshot>();}
    void init()
    {
        daemon.start(QStringLiteral("/usr/bin/dbus-daemon"),{"--session","--nofork","--print-address=1"});QVERIFY(daemon.waitForStarted(2000));QVERIFY(daemon.waitForReadyRead(2000));address=QString::fromUtf8(daemon.readLine()).trimmed();QVERIFY(!address.isEmpty());
        server=std::make_unique<QDBusConnection>(QDBusConnection::connectToBus(address,"night-server"));client=std::make_unique<QDBusConnection>(QDBusConnection::connectToBus(address,"night-client"));
        QVERIFY(server->isConnected()&&client->isConnected());QVERIFY(server->registerService("org.kde.KWin"));QVERIFY(server->registerService(NightService));
        kwin.report="KWin version: 6.6.6\nOperation Mode: Wayland\n";night.reset();
        QVERIFY(server->registerVirtualObject("/KWin",&kwin));QVERIFY(server->registerVirtualObject(NightPath,&night));service.reset(createNightLightService(*client,Schema));
    }
    void cleanup()
    {
        QTRY_VERIFY_WITH_TIMEOUT(!OperationManager::instance()->hasActiveJobs(),10000);
        service.reset();replacement.reset();client.reset();server.reset();
        QDBusConnection::disconnectFromBus("night-replacement");QDBusConnection::disconnectFromBus("night-client");QDBusConnection::disconnectFromBus("night-server");
        daemon.terminate();if(!daemon.waitForFinished(2000)){daemon.kill();daemon.waitForFinished(1000);}
    }
    void exactReadOnlyWireAndSchemaEvidence()
    {
        const auto runtime=query();QVERIFY(runtime.known&&runtime.schemaSupported&&runtime.serviceAvailable);QVERIFY(runtime.available&&runtime.previewSupported);
        QCOMPARE(runtime.owner,server->baseService());QCOMPARE(runtime.mode,1u);QCOMPARE(runtime.targetTemperature,6500u);
        QCOMPARE(night.previews,0);QCOMPARE(night.stops,0);QCOMPARE(night.otherMutations,0);
        QString error;QVERIFY(validateNightLightSchema(Schema,error));auto bad=Schema;bad.replace("4500","4600");QVERIFY(!validateNightLightSchema(bad,error));
        bad=Schema;bad.replace("<default>false</default>","<default>false</default><default>false</default>");QVERIFY(!validateNightLightSchema(bad,error));
    }
    void invalidDomainsAndSchemaRemainReadOnly()
    {
        night.properties["mode"]=3u;auto runtime=query();QVERIFY(!runtime.known);QVERIFY(!runtime.schemaSupported);
        night.properties["mode"]=1u;night.properties["targetTemperature"]=9000u;runtime=query(2);QVERIFY(!runtime.known);
        night.properties["targetTemperature"]=6500u;service.reset(createNightLightService(*client,QByteArray("<kcfg/>")));runtime=query(3);QVERIFY(!runtime.known&&!runtime.schemaSupported);
        QCOMPARE(night.previews,0);QCOMPARE(night.stops,0);
    }
    void malformedTypesAndDuplicateVersionAreRejected()
    {
        night.properties["enabled"]=QStringLiteral("true");QVERIFY(!query().known);night.properties["enabled"]=false;
        kwin.report+="KWin version: 6.6.6\n";QVERIFY(!query(2).known);QCOMPARE(night.previews,0);
        kwin.report="KWin version: 6.6.6\nOperation Mode: Wayland\nKWin version: 6.6.7\n";QVERIFY(!query(3).known);
        kwin.report="KWin version: 6.6.6\nOperation Mode: Wayland\nOperation Mode: X11\n";QVERIFY(!query(4).known);
        QCOMPARE(night.previews,0);QCOMPARE(night.stops,0);
    }
    void explicitPreviewStopArePinnedAndNeverSave()
    {
        const auto runtime=query();QSignalSpy spy(service.get(),&NightLightService::previewFinished);
        service->preview(10,4000,runtime.owner,runtime.epoch);QTRY_COMPARE(spy.size(),1);QVERIFY(spy[0][1].toBool());QCOMPARE(night.previews,1);
        service->stopPreview(11,runtime.owner,runtime.epoch);QTRY_COMPARE(spy.size(),2);QVERIFY(spy[1][1].toBool());QVERIFY(spy[1][2].toBool());QCOMPARE(night.stops,1);
        service->preview(12,999,runtime.owner,runtime.epoch);QCOMPARE(spy.size(),3);QVERIFY(!spy[2][1].toBool());QCOMPARE(night.previews,1);QCOMPARE(night.otherMutations,0);
    }
    void sameOwnerReregistrationRejectsOldEpochAndLateCallbackCannotStopNewPreview()
    {
        const auto original=query();QSignalSpy events(service.get(),&NightLightService::externalChange);QSignalSpy replies(service.get(),&NightLightService::previewFinished);
        night.holdPreview=true;service->preview(10,4000,original.owner,original.epoch);QTRY_COMPARE(night.previews,1);
        QVERIFY(server->unregisterService(NightService));QTRY_VERIFY(!events.isEmpty());QVERIFY(server->registerService(NightService));
        QTRY_VERIFY(events.size()>=2);const auto current=query(2);QVERIFY(current.known);QCOMPARE(current.owner,original.owner);QVERIFY(current.epoch!=original.epoch);
        night.holdPreview=false;service->preview(11,4500,current.owner,current.epoch);QTRY_VERIFY(!replies.isEmpty());QCOMPARE(night.previews,2);
        QVERIFY(server->send(night.delayedPreview.createReply()));QTRY_COMPARE(replies.size(),2);QCOMPARE(night.stops,0);
        service->stopPreview(12,original.owner,original.epoch);QCOMPARE(replies.size(),3);QVERIFY(!replies.last()[1].toBool());QCOMPARE(night.stops,0);
    }
    void replacementOwnerReceivesNoStalePreviewOrStop()
    {
        const auto original=query();QSignalSpy changed(service.get(),&NightLightService::externalChange);
        QVERIFY(server->unregisterService(NightService));replacement=std::make_unique<QDBusConnection>(QDBusConnection::connectToBus(address,"night-replacement"));QVERIFY(replacement->registerService(NightService));QTRY_VERIFY(!changed.isEmpty());
        const auto state=query(2);QVERIFY(!state.known);QSignalSpy replies(service.get(),&NightLightService::previewFinished);
        service->preview(3,4000,original.owner,original.epoch);service->stopPreview(4,original.owner,original.epoch);QCOMPARE(replies.size(),2);QCOMPARE(night.previews,0);QCOMPARE(night.stops,0);
    }
    void previewTimeoutIsUncertainAndExplicitStopStillWorks()
    {
        const auto runtime=query();night.holdPreview=true;QSignalSpy replies(service.get(),&NightLightService::previewFinished);
        service->preview(10,4500,runtime.owner,runtime.epoch);QTRY_COMPARE_WITH_TIMEOUT(replies.size(),1,4000);QVERIFY(!replies[0][1].toBool());QCOMPARE(night.stops,0);
        service->stopPreview(11,runtime.owner,runtime.epoch);QTRY_COMPARE(replies.size(),2);QVERIFY(replies[1][1].toBool());QCOMPARE(night.stops,1);
    }
    void queuedLocalSchemaCannotDispatchAfterExpiryStopProviderChangeOrApply_data()
    {
        QTest::addColumn<int>("retirement");QTest::newRow("tracking-expiry")<<0;QTest::newRow("explicit-stop")<<1;QTest::newRow("owner-replacement")<<2;QTest::newRow("accepted-apply")<<3;QTest::newRow("apply-observer-deletes-backend")<<4;QTest::newRow("destructor-known-unsent")<<5;
    }
    void queuedLocalSchemaCannotDispatchAfterExpiryStopProviderChangeOrApply()
    {
        QFETCH(int,retirement);QTemporaryDir files;QVERIFY(files.isValid());KdeNightLightBackend::ConfigContext context;context.fixture=true;context.previewTrackingMs=retirement==0?35:15000;
        auto backend=std::make_unique<KdeNightLightBackend>(files.filePath("kwinrc"),service.release(),context);
        QSignalSpy initial(backend.get(),&NightLightBackend::readFinished);backend->readCurrent(1);QTRY_COMPARE(initial.size(),1);QVERIFY(initial[0][1].toBool());const auto before=qvariant_cast<NightLightSnapshot>(initial[0][2]);QVERIFY(before.runtime.known);
        HeldSchemaPool blocked;QTRY_VERIFY(blocked.started());QSignalSpy replies(backend.get(),&NightLightBackend::previewFinished),writes(backend.get(),&NightLightBackend::writeFinished);
        backend->preview(10,4000,before.runtime.owner,before.runtime.epoch);QCOMPARE(night.previews,0);QCOMPARE(replies.size(),0);
        if(retirement==0){QTRY_COMPARE(replies.size(),1);QVERIFY(!replies[0][1].toBool());QVERIFY(!replies[0][2].toBool());QVERIFY(!replies[0][3].toString().isEmpty());QCOMPARE(night.stops,0);backend->stopPreview(11,before.runtime.owner,before.runtime.epoch);QCOMPARE(replies.size(),2);QVERIFY(!replies[1][1].toBool());QVERIFY(replies[1][2].toBool());QCOMPARE(night.stops,0);}
        if(retirement==1){backend->stopPreview(11,before.runtime.owner,before.runtime.epoch);QTRY_COMPARE(replies.size(),2);QCOMPARE(replies[0][0].toULongLong(),10ull);QVERIFY(!replies[0][1].toBool());QCOMPARE(replies[1][0].toULongLong(),11ull);QVERIFY(!replies[1][1].toBool()&&replies[1][2].toBool());QCOMPARE(night.stops,0);}
        if(retirement==2){QVERIFY(server->unregisterService(NightService));QTRY_COMPARE(replies.size(),1);QVERIFY(!replies[0][1].toBool());QCOMPARE(night.stops,0);}
        if(retirement==5){backend.reset();QCOMPARE(replies.size(),0);QCOMPARE(night.stops,0);}
        if(retirement==3||retirement==4){
            auto requested=before;QString error;QVERIFY(NightLightTypes::stage(requested,before,NightLightKey::NightTemperature,4700,error));
            if(retirement==4){auto *raw=backend.get();connect(raw,&NightLightBackend::previewFinished,this,[&backend](quint64 token,bool success,bool,const QString &){if(token==10&&!success)backend.reset();});}
            auto *raw=backend.get();raw->writeChanges(20,before,requested);QCOMPARE(replies.size(),1);QVERIFY(!replies[0][1].toBool());QVERIFY(OperationManager::instance()->hasActiveJobs());if(retirement==4)QVERIFY(!backend);
        }
        const auto count=replies.size();blocked.release();QTRY_VERIFY(blocked.drained());
        if(retirement==3){QTRY_COMPARE(writes.size(),1);QVERIFY2(writes[0][3].toString().isEmpty(),qPrintable(writes[0][3].toString()));QVERIFY(writes[0][1].toBool());}
        if(retirement==4)QTRY_VERIFY(!OperationManager::instance()->hasActiveJobs());
        QTest::qWait(80);QCOMPARE(replies.size(),count);QCOMPARE(night.previews,0);QCOMPARE(night.stops,0);
        if(backend){backend.reset();}QTRY_VERIFY(!OperationManager::instance()->hasActiveJobs());
    }
    void reentrantNewPreviewIsNotOverwrittenByRetiringCaller_data()
    {QTest::addColumn<bool>("outerStop");QTest::newRow("Stop-retirement")<<true;QTest::newRow("new-preview-retirement")<<false;}
    void reentrantNewPreviewIsNotOverwrittenByRetiringCaller()
    {
        QFETCH(bool,outerStop);QTemporaryDir files;QVERIFY(files.isValid());KdeNightLightBackend::ConfigContext context;context.fixture=true;
        auto backend=std::make_unique<KdeNightLightBackend>(files.filePath("kwinrc"),service.release(),context);QSignalSpy initial(backend.get(),&NightLightBackend::readFinished);backend->readCurrent(1);QTRY_COMPARE(initial.size(),1);QVERIFY(initial[0][1].toBool());const auto before=qvariant_cast<NightLightSnapshot>(initial[0][2]);
        HeldSchemaPool blocked;QTRY_VERIFY(blocked.started());QSignalSpy replies(backend.get(),&NightLightBackend::previewFinished);backend->preview(10,4000,before.runtime.owner,before.runtime.epoch);
        connect(backend.get(),&NightLightBackend::previewFinished,this,[&backend,before](quint64 token,bool success,bool,const QString &){if(token==10&&!success)backend->preview(30,4200,before.runtime.owner,before.runtime.epoch);});
        if(outerStop)backend->stopPreview(11,before.runtime.owner,before.runtime.epoch);else backend->preview(11,4100,before.runtime.owner,before.runtime.epoch);
        QCOMPARE(replies.size(),2);QCOMPARE(replies[0][0].toULongLong(),10ull);QVERIFY(!replies[0][1].toBool());QCOMPARE(replies[1][0].toULongLong(),11ull);QVERIFY(!replies[1][1].toBool());QCOMPARE(night.previews,0);QCOMPARE(night.stops,0);
        blocked.release();QTRY_COMPARE(replies.size(),3);QCOMPARE(replies[2][0].toULongLong(),30ull);QVERIFY(replies[2][1].toBool());QCOMPARE(night.previews,1);QCOMPARE(night.stops,0);
        backend->stopPreview(31,before.runtime.owner,before.runtime.epoch);QTRY_COMPARE(replies.size(),4);QVERIFY(replies[3][1].toBool()&&replies[3][2].toBool());QCOMPARE(night.stops,1);backend.reset();
    }
    void acceptedApplyFromRetirementObserverInvalidatesOlderCaller_data()
    {QTest::addColumn<bool>("outerStop");QTest::newRow("Apply-during-new-preview")<<false;QTest::newRow("Apply-during-Stop")<<true;}
    void acceptedApplyFromRetirementObserverInvalidatesOlderCaller()
    {
        QFETCH(bool,outerStop);QTemporaryDir files;QVERIFY(files.isValid());KdeNightLightBackend::ConfigContext context;context.fixture=true;
        auto backend=std::make_unique<KdeNightLightBackend>(files.filePath("kwinrc"),service.release(),context);QSignalSpy initial(backend.get(),&NightLightBackend::readFinished);backend->readCurrent(1);QTRY_COMPARE(initial.size(),1);QVERIFY(initial[0][1].toBool());const auto before=qvariant_cast<NightLightSnapshot>(initial[0][2]);
        auto requested=before;QString error;QVERIFY(NightLightTypes::stage(requested,before,NightLightKey::NightTemperature,4700,error));
        HeldSchemaPool blocked;QTRY_VERIFY(blocked.started());QSignalSpy replies(backend.get(),&NightLightBackend::previewFinished),writes(backend.get(),&NightLightBackend::writeFinished);backend->preview(10,4000,before.runtime.owner,before.runtime.epoch);
        connect(backend.get(),&NightLightBackend::previewFinished,this,[&backend,before,requested](quint64 token,bool success,bool,const QString &){if(token==10&&!success)backend->writeChanges(20,before,requested);});
        if(outerStop)backend->stopPreview(11,before.runtime.owner,before.runtime.epoch);else backend->preview(11,4100,before.runtime.owner,before.runtime.epoch);
        QCOMPARE(replies.size(),2);QCOMPARE(replies[0][0].toULongLong(),10ull);QVERIFY(!replies[0][1].toBool());QCOMPARE(replies[1][0].toULongLong(),11ull);QVERIFY(!replies[1][1].toBool());QVERIFY(OperationManager::instance()->hasActiveJobs());QCOMPARE(night.previews,0);QCOMPARE(night.stops,0);
        blocked.release();QTRY_COMPARE(writes.size(),1);QVERIFY2(writes[0][3].toString().isEmpty(),qPrintable(writes[0][3].toString()));QVERIFY(writes[0][1].toBool());QTRY_VERIFY(blocked.drained());QTest::qWait(80);QCOMPARE(replies.size(),2);QCOMPARE(night.previews,0);QCOMPARE(night.stops,0);backend.reset();QTRY_VERIFY(!OperationManager::instance()->hasActiveJobs());
    }
    void controllerPendingIsTerminalizedByLocalTrackingExpiry_data()
    {QTest::addColumn<bool>("stopWhilePending");QTest::newRow("expiry-completes-pending")<<false;QTest::newRow("stop-queued-behind-pending")<<true;}
    void controllerPendingIsTerminalizedByLocalTrackingExpiry()
    {
        QFETCH(bool,stopWhilePending);QTemporaryDir files;QVERIFY(files.isValid());KdeNightLightBackend::ConfigContext context;context.fixture=true;context.previewTrackingMs=35;
        auto session=std::make_unique<NightLightSettings>(new KdeNightLightBackend(files.filePath("kwinrc"),service.release(),context));QTRY_VERIFY(session->isReady());session->selectValue(NightLightKey::Active,true);QVERIFY(session->staged().fields.value(NightLightKey::Active).value.toBool());QVERIFY(session->canPreview(NightLightKey::NightTemperature));
        HeldSchemaPool blocked;QTRY_VERIFY(blocked.started());session->previewTemperature(NightLightKey::NightTemperature);QVERIFY(!session->canPreview(NightLightKey::NightTemperature));if(stopWhilePending)session->stopPreview();
        QTRY_VERIFY(session->canPreview(NightLightKey::NightTemperature));QVERIFY(session->previewText().contains("proved"));QCOMPARE(night.previews,0);QCOMPARE(night.stops,0);blocked.release();QTRY_VERIFY(blocked.drained());QTest::qWait(80);QCOMPARE(night.previews,0);QVERIFY(session->canPreview(NightLightKey::NightTemperature));session.reset();
    }
    void propertySignalsRefreshAndHeldReadOwnerLossDoesNotSucceed()
    {
        const auto runtime=query();QVERIFY(runtime.known);QSignalSpy changed(service.get(),&NightLightService::externalChange);
        auto signal=QDBusMessage::createSignal(NightPath,"org.freedesktop.DBus.Properties","PropertiesChanged");signal.setArguments({NightService,QVariantMap{{"inhibited",true}},QStringList{}});QVERIFY(server->send(signal));QTRY_VERIFY(!changed.isEmpty());
        night.holdGet=true;const int previous=night.gets;QSignalSpy read(service.get(),&NightLightService::queryFinished);service->query(20);QTRY_COMPARE(night.gets,previous+1);
        QVERIFY(server->unregisterService(NightService));QTRY_VERIFY(changed.size()>=2);QVERIFY(server->send(night.delayedGet.createReply(QVariantList{night.properties})));QTRY_COMPARE(read.size(),1);QVERIFY(!read[0][1].toBool());
    }
};
QTEST_GUILESS_MAIN(NightLightDBusTest)
#include "night_light_dbus_test.moc"
