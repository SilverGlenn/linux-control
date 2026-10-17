#include <QtTest>
#include "platform/kde/NightLightBackend.h"
#include <KConfig>
#include <KConfigGroup>
#include <QFile>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QStandardPaths>
#include <QProcess>
#include <QProcessEnvironment>
#include <QDir>
#include <QDateTime>
#include <QFileInfo>
#include <functional>
#include <sys/stat.h>

class FakeNightLightService final : public NightLightService {
public:
    NightLightRuntime state;
    int queries=0,previews=0,stops=0;bool hold=false,failPreview=false,failStop=false;int *externalStops=nullptr;
    quint64 heldToken=0;QString heldOwner;
    std::function<void()> beforeQuery;
    FakeNightLightService(){state.owner=":fake.1";state.epoch=1;state.version="6.6.6";state.serviceAvailable=state.schemaSupported=state.known=state.available=state.previewSupported=true;state.mode=1;state.daylight=true;state.currentTemperature=state.targetTemperature=6500;}
    void query(quint64 token,const QString &expectedOwner={}) override
    {
        ++queries;if(hold){heldToken=token;heldOwner=expectedOwner;return;}
        if(beforeQuery)beforeQuery();
        emit queryFinished(token,expectedOwner.isEmpty()||expectedOwner==state.owner,state,expectedOwner.isEmpty()||expectedOwner==state.owner?QString():QStringLiteral("Owner changed"));
    }
    void release(){hold=false;query(heldToken,heldOwner);}
    void preview(quint64 token,int temperature,const QString &owner,quint64 epoch) override
    {const bool ok=owner==state.owner&&epoch==state.epoch&&temperature>=1000&&temperature<=6500;if(ok)++previews;emit previewFinished(token,ok&&!failPreview,false,ok&&!failPreview?QString():QStringLiteral("Uncertain preview"));}
    void stopPreview(quint64 token,const QString &owner,quint64 epoch) override
    {const bool ok=owner==state.owner&&epoch==state.epoch;if(ok){++stops;if(externalStops)++*externalStops;}emit previewFinished(token,ok&&!failStop,true,ok&&!failStop?QString():QStringLiteral("Uncertain stop"));}
};

class NightLightBackendTest : public QObject {
    Q_OBJECT
    QTemporaryDir directory;
    QString path;
    FakeNightLightService *service=nullptr;
    KdeNightLightBackend *backend=nullptr;
    quint64 token=0;
    void saveFile(const QString &file,const QByteArray &bytes)
    {QSaveFile target(file);QVERIFY(target.open(QIODevice::WriteOnly));QCOMPARE(target.write(bytes),bytes.size());QVERIFY(target.commit());QFile saved(file);QVERIFY(saved.open(QIODevice::ReadOnly));static qint64 timestamp=1000000;QVERIFY(saved.setFileTime(QDateTime::fromMSecsSinceEpoch(++timestamp),QFileDevice::FileModificationTime));}
    QByteArray contents(const QString &file)
    {QFile source(file);if(!source.open(QIODevice::ReadOnly))return {};return source.readAll();}
    NightLightSnapshot read()
    {
        QSignalSpy spy(backend,&NightLightBackend::readFinished);backend->readCurrent(++token);
        if(spy.isEmpty())spy.wait(3000);
        if(spy.size()!=1||!spy[0][1].toBool())return {};
        return qvariant_cast<NightLightSnapshot>(spy[0][2]);
    }
    QList<QVariant> write(const NightLightSnapshot &before,const NightLightSnapshot &request)
    {
        QSignalSpy spy(backend,&NightLightBackend::writeFinished);backend->writeChanges(++token,before,request);
        if(spy.isEmpty())spy.wait(5000);
        return spy.isEmpty()?QList<QVariant>{QVariant::fromValue(token),false,false,QStringLiteral("No terminal completion")}:spy[0];
    }
    void replaceBackend(const KdeNightLightBackend::ConfigContext &context={})
    {delete backend;service=new FakeNightLightService;backend=new KdeNightLightBackend(path,service,context);}
private slots:
    void initTestCase(){qRegisterMetaType<NightLightSnapshot>();qRegisterMetaType<NightLightRuntime>();QStandardPaths::setTestModeEnabled(false);}
    void init(){QVERIFY(directory.isValid());path=directory.filePath("kwinrc");QFile::remove(path);token=0;replaceBackend();}
    void cleanup(){delete backend;backend=nullptr;}
    void absentReadHasExactNativeDefaultsAndNoMutation()
    {
        const auto state=read();QCOMPARE(state.fields.size(),4);QVERIFY(!QFileInfo::exists(path));
        for(const auto key:NightLightTypes::keys()){
            const auto field=state.fields[key];QVERIFY(field.known&&field.defaultKnown&&field.writable&&field.defaultNoOp);
            QVERIFY(!field.localPresent&&!field.effectivePresent&&!field.resettable);
            QCOMPARE(field.value,NightLightTypes::builtinDefault(key));
        }
        QCOMPARE(service->previews,0);QCOMPARE(service->stops,0);
    }
    void firstExplicitSetPreservesAbsentCompanionsAndUnknownKeys()
    {
        saveFile(path,"[Foreign]\nKeep=still-here\n");const auto before=read();auto request=before;QString error;
        QVERIFY(NightLightTypes::stage(request,before,NightLightKey::NightTemperature,4700,error));
        const auto result=write(before,request);QCOMPARE(result.size(),4);QVERIFY(result[1].toBool());QVERIFY(result[2].toBool());QVERIFY2(result[3].toString().isEmpty(),qPrintable(result[3].toString()));
        const auto actual=read();QVERIFY(NightLightTypes::matchesRequest(before,request,actual,error));
        KConfig config(path,KConfig::SimpleConfig);const auto group=config.group("NightColor");
        QCOMPARE(group.readEntry("NightTemperature",0),4700);QVERIFY(!group.hasKey("Active"));QVERIFY(!group.hasKey("Mode"));QVERIFY(!group.hasKey("DayTemperature"));
        QCOMPARE(config.group("Foreign").readEntry("Keep",QString()),QStringLiteral("still-here"));
    }
    void enumUsesNativeNamesAndUnknownSameFallbackRepairIsExplicit()
    {
        saveFile(path,"[NightColor]\nMode=Constant\nDayTemperature=not-a-number\n");
        const auto before=read();QCOMPARE(before.fields[NightLightKey::Mode].value.toInt(),0);QVERIFY(!before.fields[NightLightKey::DayTemperature].known);
        auto request=before;QString error;QVERIFY(NightLightTypes::stage(request,before,NightLightKey::DayTemperature,6500,error));
        QVERIFY(request.intents.contains(NightLightKey::DayTemperature));service->state.mode=0;service->state.targetTemperature=4500;
        auto result=write(before,request);QVERIFY(result[1].toBool());QVERIFY2(result[3].toString().isEmpty(),qPrintable(result[3].toString()));
        auto actual=read();QVERIFY(actual.fields[NightLightKey::DayTemperature].known);QCOMPARE(actual.fields[NightLightKey::DayTemperature].value.toInt(),6500);
        request=actual;QVERIFY(NightLightTypes::stage(request,actual,NightLightKey::Mode,1,error));service->state.mode=1;service->state.targetTemperature=6500;
        result=write(actual,request);QVERIFY(result[1].toBool());QVERIFY(result[3].toString().isEmpty());
        KConfig config(path,KConfig::SimpleConfig);QCOMPARE(config.group("NightColor").readEntry("Mode",QString()),QStringLiteral("DarkLight"));
    }
    void defaultsMatchClampedEditorNoOpAndNativeRemoval()
    {
        saveFile(path,"[NightColor]\nDayTemperature=20000\nNightTemperature=4000\n");const auto before=read();
        QCOMPARE(before.fields[NightLightKey::DayTemperature].value.toInt(),20000);QVERIFY(before.fields[NightLightKey::DayTemperature].defaultNoOp);QVERIFY(!before.fields[NightLightKey::DayTemperature].resettable);
        auto request=before;QString error;QVERIFY(NightLightTypes::restore(request,before,NightLightKey::NightTemperature,error));
        const auto result=write(before,request);QVERIFY(result[1].toBool());QVERIFY2(result[3].toString().isEmpty(),qPrintable(result[3].toString()));
        KConfig config(path,KConfig::SimpleConfig);QCOMPARE(config.group("NightColor").readEntry("DayTemperature",0),20000);QVERIFY(!config.group("NightColor").hasKey("NightTemperature"));
        QVERIFY(!read().fields[NightLightKey::NightTemperature].localPresent);
    }
    void inheritedDefaultsUseNativeEditorClampAndKeepProvenance()
    {
        const auto defaults=directory.filePath("night-defaults");saveFile(defaults,"[NightColor]\nNightTemperature=20000\n");
        saveFile(path,"[NightColor]\nNightTemperature=3500\n");replaceBackend({false,{defaults}});
        const auto before=read();const auto field=before.fields[NightLightKey::NightTemperature];QVERIFY(field.inheritedPresent);QCOMPARE(field.defaultValue.toInt(),6500);QVERIFY(field.resettable);
        auto request=before;QString error;QVERIFY(NightLightTypes::restore(request,before,NightLightKey::NightTemperature,error));
        const auto result=write(before,request);QVERIFY(result[1].toBool());QVERIFY2(result[3].toString().isEmpty(),qPrintable(result[3].toString()));
        const auto actual=read();QVERIFY(actual.fields[NightLightKey::NightTemperature].localPresent);QCOMPARE(actual.fields[NightLightKey::NightTemperature].value.toInt(),6500);
        QCOMPARE(actual.fields[NightLightKey::NightTemperature].inheritedRevision,field.inheritedRevision);QCOMPARE(contents(defaults),QByteArray("[NightColor]\nNightTemperature=20000\n"));
    }
    void oneImmutableKeyDoesNotLockItsCompanions()
    {
        saveFile(path,"[NightColor]\nNightTemperature[$i]=4500\n");const auto before=read();
        QVERIFY(before.fields[NightLightKey::NightTemperature].immutable);QVERIFY(!before.fields[NightLightKey::NightTemperature].writable);
        QVERIFY(before.fields[NightLightKey::DayTemperature].writable);auto request=before;QString error;
        QVERIFY(!NightLightTypes::stage(request,before,NightLightKey::NightTemperature,4000,error));
        QVERIFY(NightLightTypes::stage(request,before,NightLightKey::Mode,0,error));service->state.mode=0;service->state.targetTemperature=4500;
        const auto result=write(before,request);QVERIFY(result[1].toBool());QVERIFY(result[3].toString().isEmpty());
        QVERIFY(contents(path).contains("NightTemperature[$i]=4500"));
    }
    void preflightRejectsExternalAndNumericEquivalentDefaultSourceEdits()
    {
        const auto defaults=directory.filePath("race-defaults");saveFile(defaults,"[NightColor]\nNightTemperature=4000\n");replaceBackend({false,{defaults}});
        const auto before=read();auto request=before;QString error;QVERIFY(NightLightTypes::stage(request,before,NightLightKey::NightTemperature,4300,error));
        QSignalSpy finished(backend,&NightLightBackend::writeFinished);service->hold=true;backend->writeChanges(++token,before,request);
        saveFile(defaults,"[NightColor]\nNightTemperature=04000\n");service->release();QTRY_COMPARE(finished.size(),1);QVERIFY(!finished[0][1].toBool());QVERIFY(!QFileInfo::exists(path));
        const auto fresh=read();request=fresh;QVERIFY(NightLightTypes::stage(request,fresh,NightLightKey::NightTemperature,4300,error));
        service->hold=true;backend->writeChanges(++token,fresh,request);saveFile(path,"[NightColor]\nActive=true\n");service->release();QTRY_COMPARE(finished.size(),2);QVERIFY(!finished[1][1].toBool());QVERIFY(!contents(path).contains("NightTemperature"));
    }
    void ownerEpochAndForgedRequestsCauseNoSave()
    {
        const auto before=read();auto request=before;QString error;QVERIFY(NightLightTypes::stage(request,before,NightLightKey::NightTemperature,4700,error));
        service->state.epoch++;auto result=write(before,request);QVERIFY(!result[1].toBool());QVERIFY(!QFileInfo::exists(path));service->state.epoch--;
        auto forged=request;forged.fields[NightLightKey::Active].value=true;result=write(before,forged);QVERIFY(!result[1].toBool());QVERIFY(!QFileInfo::exists(path));
    }
    void postSaveForeignChangeIsReportedWithoutOverwritingIt()
    {
        const auto before=read();auto request=before;QString error;QVERIFY(NightLightTypes::stage(request,before,NightLightKey::NightTemperature,4700,error));
        const int preflight=service->queries+1;
        service->beforeQuery=[&]{if(service->queries==preflight+1){KConfig config(path,KConfig::SimpleConfig);config.group("NightColor").writeEntry("Active",true);QVERIFY(config.sync());}};
        const auto result=write(before,request);QVERIFY(result[1].toBool());QVERIFY(result[2].toBool());QVERIFY(!result[3].toString().isEmpty());
        KConfig config(path,KConfig::SimpleConfig);QVERIFY(config.group("NightColor").readEntry("Active",false));QCOMPARE(config.group("NightColor").readEntry("NightTemperature",0),4700);
    }
    void atomicReplacementIsObservedAndPreviewHasNoConfigSideEffects()
    {
        const auto before=read();QSignalSpy changed(backend,&NightLightBackend::externalChange);saveFile(path,"[NightColor]\nNightTemperature=4600\n");QTRY_VERIFY(!changed.isEmpty());
        const auto bytes=contents(path);QSignalSpy preview(backend,&NightLightBackend::previewFinished);
        backend->preview(100,4000,before.runtime.owner,before.runtime.epoch);QCOMPARE(service->previews,1);QCOMPARE(preview.size(),1);QCOMPARE(contents(path),bytes);
        backend->stopPreview(101,before.runtime.owner,before.runtime.epoch);QCOMPARE(service->stops,1);QCOMPARE(contents(path),bytes);
        backend->preview(102,4000,before.runtime.owner,before.runtime.epoch);QCOMPARE(service->previews,2);
        int cleanupStops=0;service->externalStops=&cleanupStops;
        delete backend;backend=nullptr;QCOMPARE(cleanupStops,1);
    }
    void fullCascadeGlobalOriginsAndDeletionMasksAreNative()
    {
        delete backend;backend=nullptr;
        path=QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)+QStringLiteral("/kwinrc");
        const auto global=QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)+QStringLiteral("/kdeglobals");
        QFile::remove(path);QFile::remove(global);
        saveFile(global,"[NightColor]\nNightTemperature=3500\n[Foreign]\nKeep=global-unowned\n");
        replaceBackend({true,{}});auto before=read();auto request=before;QString error;
        QVERIFY(before.fields[NightLightKey::NightTemperature].globalPresent);QVERIFY(!before.fields[NightLightKey::NightTemperature].localPresent);
        QVERIFY(NightLightTypes::restore(request,before,NightLightKey::NightTemperature,error));
        auto result=write(before,request);QVERIFY(result[1].toBool());QVERIFY2(result[3].toString().isEmpty(),qPrintable(result[3].toString()));
        auto actual=read();QVERIFY(!actual.fields[NightLightKey::NightTemperature].globalPresent);QVERIFY(!actual.fields[NightLightKey::NightTemperature].effectivePresent);
        saveFile(global,"[NightColor]\nNightTemperature=3500\n[Foreign]\nKeep=global-unowned\n");
        saveFile(path,"[NightColor]\nNightTemperature=4000\n");
        before=read();request=before;QVERIFY(before.fields[NightLightKey::NightTemperature].globalPresent&&before.fields[NightLightKey::NightTemperature].localPresent);
        QVERIFY(NightLightTypes::restore(request,before,NightLightKey::NightTemperature,error));
        result=write(before,request);QVERIFY(result[1].toBool());QVERIFY2(result[3].toString().isEmpty(),qPrintable(result[3].toString()));
        actual=read();QVERIFY(!actual.fields[NightLightKey::NightTemperature].effectivePresent);QVERIFY(!actual.fields[NightLightKey::NightTemperature].localPresent);QVERIFY(actual.fields[NightLightKey::NightTemperature].globalPresent);
        QVERIFY(contents(path).contains("[$d]"));KConfig globals(global,KConfig::SimpleConfig);QCOMPARE(globals.group("Foreign").readEntry("Keep",QString()),QStringLiteral("global-unowned"));
        QFile::remove(path);QFile::remove(global);
    }
    void nativeBooleanTruthAndMalformedInheritedDefaultRemainExplicit()
    {
        const auto defaults=directory.filePath("malformed-defaults");saveFile(defaults,"[NightColor]\nNightTemperature=broken\n");
        saveFile(path,"[NightColor]\nActive=unrecognized-but-true\n");replaceBackend({false,{defaults}});
        const auto before=read();QVERIFY(before.fields[NightLightKey::Active].known);QVERIFY(before.fields[NightLightKey::Active].value.toBool());
        QVERIFY(!before.fields[NightLightKey::NightTemperature].defaultKnown);QVERIFY(before.fields[NightLightKey::NightTemperature].writable);
        auto request=before;QString error;QVERIFY(!NightLightTypes::restore(request,before,NightLightKey::NightTemperature,error));
        QVERIFY(NightLightTypes::stage(request,before,NightLightKey::NightTemperature,4500,error));service->state.enabled=true;
        const auto result=write(before,request);QVERIFY(result[1].toBool());QVERIFY2(result[3].toString().isEmpty(),qPrintable(result[3].toString()));
    }
    void nonregularBrokenAndAliasedSourcesRefuseWithoutMutation()
    {
        QVERIFY(::mkfifo(QFile::encodeName(path).constData(),0600)==0);QSignalSpy done(backend,&NightLightBackend::readFinished);backend->readCurrent(++token);QTRY_COMPARE(done.size(),1);QVERIFY(!done[0][1].toBool());QVERIFY(QFile::remove(path));
        QVERIFY(QFile::link(directory.filePath("missing-target"),path));backend->readCurrent(++token);QTRY_COMPARE(done.size(),2);QVERIFY(!done[1][1].toBool());QVERIFY(QFile::remove(path));
        const auto defaults=directory.filePath("alias-defaults");saveFile(defaults,"[NightColor]\nNightTemperature=4500\n");QVERIFY(QFile::link(defaults,path));replaceBackend({false,{defaults}});QSignalSpy alias(backend,&NightLightBackend::readFinished);backend->readCurrent(++token);QTRY_COMPARE(alias.size(),1);QVERIFY(!alias[0][1].toBool());QCOMPARE(contents(defaults),QByteArray("[NightColor]\nNightTemperature=4500\n"));QCOMPARE(service->previews,0);QCOMPARE(service->stops,0);
    }
    void failedPreviewAndStopRetainOwnedCleanupLease()
    {
        const auto before=read();service->failPreview=true;
        backend->preview(100,4500,before.runtime.owner,before.runtime.epoch);QCOMPARE(service->previews,1);
        service->failStop=true;backend->stopPreview(101,before.runtime.owner,before.runtime.epoch);QCOMPARE(service->stops,1);
        service->failStop=false;backend->stopPreview(102,before.runtime.owner,before.runtime.epoch);QCOMPARE(service->stops,2);
        backend->stopPreview(103,before.runtime.owner,before.runtime.epoch);QCOMPARE(service->stops,2);
    }
    void unsupportedAbsentKeysDoNotInventVerifiedBuiltinDefaults()
    {
        service->state.schemaSupported=false;service->state.reason="Unverified current schema";
        const auto snapshot=read();QCOMPARE(snapshot.fields.size(),4);
        for(const auto &field:snapshot.fields){QVERIFY(!field.known);QVERIFY(!field.defaultKnown);QVERIFY(!field.defaultNoOp);QVERIFY(!field.writable);}
    }
    void unsupportedRuntimeReadsSavedValuesButDisablesWriting()
    {
        saveFile(path,"[NightColor]\nMode=9\nNightTemperature=3300\n");service->state.schemaSupported=false;service->state.reason="Unsupported runtime";
        const auto snapshot=read();QCOMPARE(snapshot.fields[NightLightKey::Mode].value.toInt(),9);QCOMPARE(snapshot.fields[NightLightKey::NightTemperature].value.toInt(),3300);QVERIFY(!snapshot.fields[NightLightKey::NightTemperature].writable);
    }
};
int main(int argc,char **argv)
{
    QCoreApplication app(argc,argv);
    if(!qEnvironmentVariableIsSet("CONTROL_NIGHT_BACKEND_CHILD")){
        QTemporaryDir sandbox;if(!sandbox.isValid())return 2;
        auto environment=QProcessEnvironment::systemEnvironment();
        const auto config=sandbox.filePath("config"),defaults=sandbox.filePath("defaults"),cache=sandbox.filePath("cache");
        if(!QDir().mkpath(config)||!QDir().mkpath(defaults)||!QDir().mkpath(cache))return 3;
        environment.insert("XDG_CONFIG_HOME",config);environment.insert("XDG_CONFIG_DIRS",defaults);environment.insert("XDG_CACHE_HOME",cache);
        environment.insert("KDE_SKIP_KDERC","1");environment.insert("CONTROL_NIGHT_BACKEND_CHILD","1");
        QProcess child;child.setProcessEnvironment(environment);child.setProcessChannelMode(QProcess::ForwardedChannels);
        QStringList arguments{QStringLiteral("--"),app.applicationFilePath()};arguments<<app.arguments().mid(1);
        child.start(QStringLiteral("/usr/bin/dbus-run-session"),arguments);
        if(!child.waitForStarted(3000)||!child.waitForFinished(60000)){child.kill();child.waitForFinished(1000);return 4;}
        return child.exitStatus()==QProcess::NormalExit?child.exitCode():5;
    }
    NightLightBackendTest test;return QTest::qExec(&test,argc,argv);
}
#include "night_light_backend_test.moc"
