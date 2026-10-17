#include <QtTest>
#include "platform/kde/NightTimeBackend.h"
#include <KConfig>
#include <KConfigGroup>
#include <KSharedConfig>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTemporaryDir>

class FakeNightTimeService final : public NightTimeService {
public:
    NightTimeRuntime state;
    bool hold=false;quint64 heldToken=0;QString heldOwner;int queries=0;
    std::function<void()> beforeQuery;
    FakeNightTimeService(){state.installedVerified=true;state.version="6.6.5";state.epoch=1;}
    void query(quint64 token,const QString &owner={}) override
    {
        ++queries;if(hold){heldToken=token;heldOwner=owner;return;}
        if(beforeQuery)beforeQuery();
        const bool valid=state.installedVerified&&(!state.serviceAvailable||state.providerVerified)&&(owner.isEmpty()||owner==state.owner);
        emit queryFinished(token,valid,state,valid?QString():QStringLiteral("Native identity changed"));
    }
    void release(){hold=false;query(heldToken,heldOwner);}
};

class NightTimeBackendTest final : public QObject {
    Q_OBJECT
    QTemporaryDir directory;QString path;FakeNightTimeService *service=nullptr;KdeNightTimeBackend *backend=nullptr;quint64 token=0;
    void saveFile(const QString &file,const QByteArray &bytes)
    {
        QSaveFile target(file);QVERIFY(target.open(QIODevice::WriteOnly));QCOMPARE(target.write(bytes),bytes.size());QVERIFY(target.commit());
        QFile saved(file);QVERIFY(saved.open(QIODevice::ReadOnly));static qint64 stamp=1000000;QVERIFY(saved.setFileTime(QDateTime::fromMSecsSinceEpoch(++stamp),QFileDevice::FileModificationTime));
    }
    QByteArray contents(const QString &file){QFile input(file);if(!input.open(QIODevice::ReadOnly))return {};return input.readAll();}
    void replaceBackend(const KdeNightTimeBackend::ConfigContext &context={})
    {delete backend;service=new FakeNightTimeService;backend=new KdeNightTimeBackend(path,service,context);}
    NightTimeSnapshot read()
    {
        QSignalSpy done(backend,&NightTimeBackend::readFinished);backend->readCurrent(++token);if(done.isEmpty())done.wait(3000);
        if(done.size()!=1||!done[0][1].toBool())return {};return qvariant_cast<NightTimeSnapshot>(done[0][2]);
    }
    QList<QVariant> write(const NightTimeSnapshot &before,const NightTimeSnapshot &request)
    {
        QSignalSpy done(backend,&NightTimeBackend::writeFinished);backend->writeChanges(++token,before,request);if(done.isEmpty())done.wait(5000);
        // A fixed-size failure result makes timeout failures readable assertions.
        return done.isEmpty()?QList<QVariant>{QVariant::fromValue(token),false,false,QStringLiteral("No completion")}:done[0];
    }
private slots:
    void initTestCase(){qRegisterMetaType<NightTimeSnapshot>();qRegisterMetaType<NightTimeRuntime>();QStandardPaths::setTestModeEnabled(false);}
    void init(){QVERIFY(directory.isValid());path=directory.filePath("knighttimerc");QFile::remove(path);token=0;replaceBackend();}
    void cleanup(){delete backend;backend=nullptr;}
    void offlineReadHasNativeDefaultsWithoutCreatingFile()
    {
        const auto state=read();QCOMPARE(state.fields.size(),7);QCOMPARE(state.daemonFields.size(),7);QVERIFY(!QFileInfo::exists(path));
        QVERIFY(state.runtime.installedVerified&&!state.runtime.serviceAvailable);
        for(const auto key:NightTimeTypes::keys()){
            const auto field=state.fields[key],daemon=state.daemonFields[key];QVERIFY(field.known&&field.defaultKnown&&field.writable&&field.defaultNoOp);
            QCOMPARE(field.value,NightTimeTypes::builtinDefault(key));QCOMPARE(daemon.value,field.value);QVERIFY(!field.effectivePresent&&!field.localPresent);
        }
    }
    void explicitNativeTypedWritesPreserveAbsentAndForeignEntries()
    {
        saveFile(path,"[Foreign]\nKeep=unowned\n");const auto before=read();auto request=before;QString error;
        QVERIFY(NightTimeTypes::stage(request,before,NightTimeKey::Source,1,error));
        QVERIFY(NightTimeTypes::stage(request,before,NightTimeKey::SunriseStart,QTime(7,15,12),error));
        QVERIFY(NightTimeTypes::stage(request,before,NightTimeKey::TransitionDuration,QVariant::fromValue(2400u),error));
        const auto result=write(before,request);QVERIFY(result[1].toBool());QVERIFY(result[2].toBool());QVERIFY2(result[3].toString().isEmpty(),qPrintable(result[3].toString()));
        const auto actual=read();QVERIFY2(NightTimeTypes::matchesRequest(before,request,actual,error),qPrintable(error));
        KConfig config(path,KConfig::SimpleConfig);QCOMPARE(config.group("General").readEntry("Source",QString()),QStringLiteral("Times"));
        QCOMPARE(config.group("Times").readEntry("SunriseStart",QTime()),QTime(7,15,12));QCOMPARE(config.group("Times").readEntry("TransitionDuration",0u),2400u);
        QVERIFY(!config.group("Location").hasKey("Automatic"));QVERIFY(!config.group("Times").hasKey("SunsetStart"));QCOMPARE(config.group("Foreign").readEntry("Keep",QString()),QStringLiteral("unowned"));
    }
    void separateEditorAndDaemonInheritedSourcesArePreserved()
    {
        const auto editor=directory.filePath("editor-defaults"),daemon=directory.filePath("daemon-defaults");
        saveFile(editor,"[Location]\nLatitude=40.25\n");saveFile(daemon,"[Location]\nLatitude=12.5\n");
        saveFile(path,"[General]\nSource=Times\n[Location]\nLatitude=30\n");replaceBackend({false,{editor},{daemon},{}});
        const auto before=read();QCOMPARE(before.fields[NightTimeKey::Latitude].defaultValue.toDouble(),40.25);QCOMPARE(before.daemonFields[NightTimeKey::Latitude].defaultValue.toDouble(),12.5);
        auto request=before;QString error;QVERIFY(NightTimeTypes::restore(request,before,NightTimeKey::Latitude,error));const auto result=write(before,request);
        QVERIFY(result[1].toBool());QVERIFY2(result[3].toString().isEmpty(),qPrintable(result[3].toString()));const auto actual=read();
        QCOMPARE(actual.fields[NightTimeKey::Latitude].value.toDouble(),40.25);QCOMPARE(actual.daemonFields[NightTimeKey::Latitude].value.toDouble(),40.25);
        QCOMPARE(actual.daemonFields[NightTimeKey::Latitude].inheritedRevision,before.daemonFields[NightTimeKey::Latitude].inheritedRevision);
        QCOMPARE(contents(editor),QByteArray("[Location]\nLatitude=40.25\n"));QCOMPARE(contents(daemon),QByteArray("[Location]\nLatitude=12.5\n"));
    }
    void nativeDefaultRemovalAndSameValueNoOpPreserveRawSyntax()
    {
        saveFile(path,"[General]\nSource=Times\n[Location]\nLatitude=0.0000\n[Times]\nTransitionDuration=2400\n");const auto before=read();
        QVERIFY(before.fields[NightTimeKey::Latitude].defaultNoOp);auto request=before;QString error;
        QVERIFY(NightTimeTypes::restore(request,before,NightTimeKey::TransitionDuration,error));const auto result=write(before,request);QVERIFY(result[1].toBool());QVERIFY2(result[3].toString().isEmpty(),qPrintable(result[3].toString()));
        QVERIFY(contents(path).contains("Latitude=0.0000"));QVERIFY(!read().fields[NightTimeKey::TransitionDuration].localPresent);
    }
    void malformedSourceBlocksNotifyUntilExplicitlyRepaired()
    {
        saveFile(path,"[General]\nSource=9\n[Foreign]\nKeep=yes\n");const auto bytes=contents(path);const auto before=read();auto request=before;QString error;
        QVERIFY(NightTimeTypes::stage(request,before,NightTimeKey::TransitionDuration,QVariant::fromValue(2400u),error));
        auto result=write(before,request);QVERIFY(!result[1].toBool());QCOMPARE(contents(path),bytes);
        QVERIFY(NightTimeTypes::stage(request,before,NightTimeKey::Source,1,error));result=write(before,request);QVERIFY(result[1].toBool());QVERIFY2(result[3].toString().isEmpty(),qPrintable(result[3].toString()));
        QCOMPARE(read().daemonFields[NightTimeKey::Source].value.toInt(),1);
    }
    void malformedDormantCoordinateDoesNotForceCompanionRewrite()
    {
        saveFile(path,"[General]\nSource=Times\n[Location]\nLatitude=broken\n");const auto before=read();QVERIFY(!before.fields[NightTimeKey::Latitude].known);
        auto request=before;QString error;QVERIFY(NightTimeTypes::stage(request,before,NightTimeKey::TransitionDuration,QVariant::fromValue(2400u),error));const auto result=write(before,request);
        QVERIFY(result[1].toBool());QVERIFY2(result[3].toString().isEmpty(),qPrintable(result[3].toString()));QVERIFY(contents(path).contains("Latitude=broken"));
    }
    void perKeyPolicyAndForgedMetadataRejectWrites()
    {
        saveFile(path,"[General]\nSource=Times\n[Location]\nAutomatic[$i]=false\n");const auto before=read();QVERIFY(!before.fields[NightTimeKey::Automatic].writable);
        auto request=before;QString error;QVERIFY(!NightTimeTypes::stage(request,before,NightTimeKey::Automatic,true,error));
        QVERIFY(NightTimeTypes::stage(request,before,NightTimeKey::Latitude,25.0,error));auto forged=request;forged.fields[NightTimeKey::Automatic].immutable=false;
        auto result=write(before,forged);QVERIFY(!result[1].toBool());result=write(before,request);QVERIFY(result[1].toBool());QVERIFY2(result[3].toString().isEmpty(),qPrintable(result[3].toString()));
        QVERIFY(contents(path).contains("Automatic[$i]=false"));
    }
    void bothDefaultSourcesAndOwnerEpochArePreflightGuards()
    {
        const auto daemon=directory.filePath("race-defaults");saveFile(daemon,"[Location]\nLatitude=10.0\n");replaceBackend({false,{},{daemon},{}});
        const auto before=read();auto request=before;QString error;QVERIFY(NightTimeTypes::stage(request,before,NightTimeKey::TransitionDuration,QVariant::fromValue(2400u),error));
        service->hold=true;QSignalSpy done(backend,&NightTimeBackend::writeFinished);backend->writeChanges(++token,before,request);
        saveFile(daemon,"[Location]\nLatitude=010.0\n");service->release();QTRY_COMPARE(done.size(),1);QVERIFY(!done[0][1].toBool());QVERIFY(!QFileInfo::exists(path));
        const auto fresh=read();request=fresh;QVERIFY(NightTimeTypes::stage(request,fresh,NightTimeKey::TransitionDuration,QVariant::fromValue(2400u),error));service->state.epoch++;
        const auto result=write(fresh,request);QVERIFY(!result[1].toBool());QVERIFY(!QFileInfo::exists(path));
    }
    void separateDaemonPolicyBlocksEditorWritesBeforeMutation()
    {
        const auto daemon=directory.filePath("daemon-policy");saveFile(daemon,"[Location]\nLatitude[$i]=10\n");
        saveFile(path,"[General]\nSource=Times\n[Location]\nLatitude=30\n");replaceBackend({false,{},{daemon},{}});
        const auto bytes=contents(path),policy=contents(daemon);const auto before=read();
        QVERIFY(before.fields[NightTimeKey::Latitude].writable);QVERIFY(before.daemonFields[NightTimeKey::Latitude].immutable);
        auto request=before;QString error;QVERIFY(!NightTimeTypes::stage(request,before,NightTimeKey::Latitude,35.0,error));
        request.fields[NightTimeKey::Latitude].value=35.0;request.fields[NightTimeKey::Latitude].known=true;request.intents[NightTimeKey::Latitude]=NightTimeIntent::Set;
        const auto result=write(before,request);QVERIFY(!result[1].toBool());QCOMPARE(contents(path),bytes);QCOMPARE(contents(daemon),policy);
    }
    void nativeBooleanConversionAndUnknownSameFallbackRepairAreExplicit()
    {
        saveFile(path,"[General]\nSource=Times\n[Location]\nAutomatic=unrecognized-but-true\n[Times]\nTransitionDuration=malformed\n");
        const auto before=read();QVERIFY(before.fields[NightTimeKey::Automatic].known&&before.fields[NightTimeKey::Automatic].value.toBool());
        QVERIFY(!before.fields[NightTimeKey::TransitionDuration].known);auto request=before;QString error;
        QVERIFY(NightTimeTypes::stage(request,before,NightTimeKey::TransitionDuration,QVariant::fromValue(1800u),error));QVERIFY(request.intents.contains(NightTimeKey::TransitionDuration));
        const auto result=write(before,request);QVERIFY(result[1].toBool());QVERIFY2(result[3].toString().isEmpty(),qPrintable(result[3].toString()));
        QVERIFY(read().fields[NightTimeKey::TransitionDuration].known);QVERIFY(contents(path).contains("Automatic=unrecognized-but-true"));
    }
    void nativeDoubleWriterRoundTripsSeventeenDigitCoordinates()
    {
        saveFile(path,"[General]\nSource=Times\n[Foreign]\nKeep=precision\n");const auto before=read();auto request=before;QString error;
        const double latitude=12.345678901234567,longitude=-123.45678901234567;
        QVERIFY(NightTimeTypes::stage(request,before,NightTimeKey::Latitude,latitude,error));
        QVERIFY(NightTimeTypes::stage(request,before,NightTimeKey::Longitude,longitude,error));
        const auto result=write(before,request);QVERIFY(result[1].toBool());QVERIFY2(result[3].toString().isEmpty(),qPrintable(result[3].toString()));
        const auto actual=read();QCOMPARE(actual.fields[NightTimeKey::Latitude].value.toDouble(),latitude);QCOMPARE(actual.fields[NightTimeKey::Longitude].value.toDouble(),longitude);
        QCOMPARE(actual.daemonFields[NightTimeKey::Latitude].value.toDouble(),latitude);QCOMPARE(actual.daemonFields[NightTimeKey::Longitude].value.toDouble(),longitude);
        QVERIFY(actual.fields[NightTimeKey::Latitude].value.toDouble()==latitude);QVERIFY(actual.fields[NightTimeKey::Longitude].value.toDouble()==longitude);
        QVERIFY(actual.daemonFields[NightTimeKey::Latitude].value.toDouble()==latitude);QVERIFY(actual.daemonFields[NightTimeKey::Longitude].value.toDouble()==longitude);
        QVERIFY(contents(path).contains("Keep=precision"));QVERIFY2(NightTimeTypes::matchesRequest(before,request,actual,error),qPrintable(error));
    }
    void nativeTimeWriterCanonicalizesOnlyExplicitChanges()
    {
        saveFile(path,"[General]\nSource=Times\n[Times]\nSunriseStart=06:00:00.123\nSunsetStart=18:00:00.987\n");
        const auto before=read();QCOMPARE(before.fields[NightTimeKey::SunriseStart].value.toTime(),QTime(6,0,0,123));
        auto unchanged=before;QString error;QVERIFY(NightTimeTypes::stage(unchanged,before,NightTimeKey::SunriseStart,QTime(6,0,0,123),error));QVERIFY(unchanged.intents.isEmpty());
        auto request=before;QVERIFY(NightTimeTypes::stage(request,before,NightTimeKey::TransitionDuration,QVariant::fromValue(2400u),error));
        auto result=write(before,request);QVERIFY(result[1].toBool());QVERIFY2(result[3].toString().isEmpty(),qPrintable(result[3].toString()));
        auto actual=read();QCOMPARE(actual.fields[NightTimeKey::SunsetStart].value.toTime(),QTime(18,0,0,987));
        QCOMPARE(actual.fields[NightTimeKey::SunsetStart].revision,before.fields[NightTimeKey::SunsetStart].revision);
        QVERIFY(contents(path).contains("SunriseStart=06:00:00.123"));QVERIFY(contents(path).contains("SunsetStart=18:00:00.987"));
        request=actual;QVERIFY(NightTimeTypes::stage(request,actual,NightTimeKey::SunriseStart,QTime(7,15,12,345),error));
        QCOMPARE(request.fields[NightTimeKey::SunriseStart].value.toTime(),QTime(7,15,12));
        result=write(actual,request);QVERIFY(result[1].toBool());QVERIFY2(result[3].toString().isEmpty(),qPrintable(result[3].toString()));
        const auto written=read();QCOMPARE(written.fields[NightTimeKey::SunriseStart].value.toTime(),QTime(7,15,12));QCOMPARE(written.daemonFields[NightTimeKey::SunriseStart].value.toTime(),QTime(7,15,12));
        QCOMPARE(written.fields[NightTimeKey::SunsetStart].revision,before.fields[NightTimeKey::SunsetStart].revision);QVERIFY(contents(path).contains("SunsetStart=18:00:00.987"));
        QVERIFY2(NightTimeTypes::matchesRequest(actual,request,written,error),qPrintable(error));
    }
    void inheritedFractionalTimeDefaultUsesNativePersistedPrecision()
    {
        const auto defaults=directory.filePath("fractional-time-defaults");const QByteArray raw="[Times]\nSunriseStart=07:00:12.345\n";
        saveFile(defaults,raw);saveFile(path,"[General]\nSource=Times\n[Times]\nSunriseStart=06:00:00.123\nSunsetStart=18:00:00.987\n");replaceBackend({false,{defaults},{defaults},{}});
        const auto before=read();QCOMPARE(before.fields[NightTimeKey::SunriseStart].defaultValue.toTime(),QTime(7,0,12,345));
        auto request=before;QString error;QVERIFY(NightTimeTypes::restore(request,before,NightTimeKey::SunriseStart,error));
        QCOMPARE(request.fields[NightTimeKey::SunriseStart].value.toTime(),QTime(7,0,12));QCOMPARE(request.fields[NightTimeKey::SunriseStart].defaultValue.toTime(),QTime(7,0,12,345));
        const auto result=write(before,request);QVERIFY(result[1].toBool());QVERIFY2(result[3].toString().isEmpty(),qPrintable(result[3].toString()));
        const auto actual=read();QCOMPARE(actual.fields[NightTimeKey::SunriseStart].value.toTime(),QTime(7,0,12));QVERIFY(actual.fields[NightTimeKey::SunriseStart].localPresent);
        QCOMPARE(actual.daemonFields[NightTimeKey::SunriseStart].value.toTime(),QTime(7,0,12));QCOMPARE(actual.fields[NightTimeKey::SunriseStart].defaultValue.toTime(),QTime(7,0,12,345));
        QCOMPARE(actual.fields[NightTimeKey::SunriseStart].inheritedRevision,before.fields[NightTimeKey::SunriseStart].inheritedRevision);QCOMPARE(contents(defaults),raw);
        QVERIFY(contents(path).contains("SunsetStart=18:00:00.987"));QVERIFY2(NightTimeTypes::matchesRequest(before,request,actual,error),qPrintable(error));
    }
    void postSaveForeignChangeIsPreservedAndReported()
    {
        saveFile(path,"[General]\nSource=Times\n");const auto before=read();auto request=before;QString error;QVERIFY(NightTimeTypes::stage(request,before,NightTimeKey::TransitionDuration,QVariant::fromValue(2400u),error));
        const auto observation=service->queries+2;service->beforeQuery=[&]{if(service->queries==observation){KConfig config(path,KConfig::SimpleConfig);config.group("Times").writeEntry("SunsetStart",QTime(20,0));QVERIFY(config.sync());}};
        const auto result=write(before,request);QVERIFY(result[1].toBool()&&result[2].toBool());QVERIFY(!result[3].toString().isEmpty());
        QCOMPARE(read().fields[NightTimeKey::SunsetStart].value.toTime(),QTime(20,0));QCOMPARE(read().fields[NightTimeKey::TransitionDuration].value.toUInt(),2400u);
    }
    void fullCascadeGlobalsAreNotDaemonPolicyAndNativeMasksAreVerified()
    {
        delete backend;backend=nullptr;path=QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)+"/knighttimerc";
        const auto global=QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)+"/kdeglobals";QFile::remove(path);QFile::remove(global);
        saveFile(global,"[Location]\nAutomatic=false\n[Foreign]\nKeep=global\n");replaceBackend({true,{},{},{}});
        auto before=read();QVERIFY(!before.fields[NightTimeKey::Automatic].value.toBool());QVERIFY(before.daemonFields[NightTimeKey::Automatic].value.toBool());
        auto request=before;QString error;QVERIFY(NightTimeTypes::restore(request,before,NightTimeKey::Automatic,error));auto result=write(before,request);
        QVERIFY(result[1].toBool());QVERIFY2(result[3].toString().isEmpty(),qPrintable(result[3].toString()));QVERIFY(!read().fields[NightTimeKey::Automatic].globalPresent);
        saveFile(global,"[Location]\nAutomatic=false\n[Foreign]\nKeep=global\n");saveFile(path,"[Location]\nAutomatic=false\n");before=read();request=before;
        QVERIFY(NightTimeTypes::restore(request,before,NightTimeKey::Automatic,error));result=write(before,request);QVERIFY(result[1].toBool());QVERIFY2(result[3].toString().isEmpty(),qPrintable(result[3].toString()));
        const auto actual=read();QVERIFY(actual.fields[NightTimeKey::Automatic].globalPresent&&!actual.fields[NightTimeKey::Automatic].localPresent);QVERIFY(actual.daemonFields[NightTimeKey::Automatic].value.toBool());QVERIFY(contents(path).contains("[$d]"));
        KConfig config(global,KConfig::SimpleConfig);QCOMPARE(config.group("Foreign").readEntry("Keep",QString()),QStringLiteral("global"));QFile::remove(path);QFile::remove(global);
    }
    void identityMismatchIsReadOnlyAndAbsentDefaultsUnknown()
    {
        service->state.installedVerified=false;service->state.reason="Unverified installation";const auto snapshot=read();QCOMPARE(snapshot.fields.size(),7);
        for(const auto &field:snapshot.fields){QVERIFY(!field.known&&!field.defaultKnown&&!field.writable);}
        saveFile(path,"[General]\nSource=9\n");const auto explicitSaved=read();QVERIFY(explicitSaved.fields[NightTimeKey::Source].known);QCOMPARE(explicitSaved.fields[NightTimeKey::Source].value.toInt(),9);
    }
    void atomicReplaceAndCachedLocationStayReadOnly()
    {
        const auto cache=directory.filePath("knighttimestaterc");saveFile(cache,"[AutomaticLocation]\nAvailable=true\nLatitude=51.5\nLongitude=-0.1\n");replaceBackend({false,{},{},cache});
        const auto bytes=contents(cache);const auto before=read();QVERIFY(before.cachedLocation.available);QCOMPARE(before.cachedLocation.latitude,51.5);QVERIFY(before.cachedLocation.reason.contains("age is unknown"));QCOMPARE(contents(cache),bytes);
        QSignalSpy changed(backend,&NightTimeBackend::externalChange);saveFile(path,"[General]\nSource=Times\n");QTRY_VERIFY(!changed.isEmpty());QCOMPARE(contents(cache),bytes);
    }
    void fullCascadeDiscoversNativeGenericStateCache()
    {
        QVERIFY(!QCoreApplication::applicationName().isEmpty());
        QVERIFY(QStandardPaths::writableLocation(QStandardPaths::StateLocation)!=QStandardPaths::writableLocation(QStandardPaths::GenericStateLocation));
        const auto statePath=QStandardPaths::writableLocation(QStandardPaths::GenericStateLocation)+QStringLiteral("/knighttimestaterc");QFile::remove(statePath);
        {
            const auto state=KSharedConfig::openStateConfig(QStringLiteral("knighttimestaterc"));auto group=state->group("AutomaticLocation");
            group.writeEntry("Available",true);group.writeEntry("Latitude",12.345678901234567);group.writeEntry("Longitude",-98.76543210987654);QVERIFY(state->sync());
        }
        QVERIFY(QFileInfo::exists(statePath));const auto bytes=contents(statePath);
        delete backend;backend=nullptr;path=QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)+QStringLiteral("/knighttimerc");QFile::remove(path);replaceBackend({true,{},{},{}});
        const auto snapshot=read();QVERIFY(snapshot.cachedLocation.available);QVERIFY(snapshot.cachedLocation.latitude==12.345678901234567);QVERIFY(snapshot.cachedLocation.longitude==-98.76543210987654);
        QVERIFY(snapshot.cachedLocation.reason.contains("age is unknown"));QCOMPARE(contents(statePath),bytes);QVERIFY(!QFileInfo::exists(path));QFile::remove(statePath);
    }
};
int main(int argc,char **argv)
{
    QCoreApplication app(argc,argv);
    if(!qEnvironmentVariableIsSet("CONTROL_NIGHT_TIME_CHILD")){
        QTemporaryDir sandbox;if(!sandbox.isValid())return 2;auto environment=QProcessEnvironment::systemEnvironment();
        const auto config=sandbox.filePath("config"),defaults=sandbox.filePath("defaults"),cache=sandbox.filePath("cache"),state=sandbox.filePath("state");
        if(!QDir().mkpath(config)||!QDir().mkpath(defaults)||!QDir().mkpath(cache)||!QDir().mkpath(state))return 3;
        environment.insert("XDG_CONFIG_HOME",config);environment.insert("XDG_CONFIG_DIRS",defaults);environment.insert("XDG_CACHE_HOME",cache);environment.insert("XDG_STATE_HOME",state);environment.insert("KDE_SKIP_KDERC","1");environment.insert("CONTROL_NIGHT_TIME_CHILD","1");
        QProcess child;child.setProcessEnvironment(environment);child.setProcessChannelMode(QProcess::ForwardedChannels);QStringList arguments{"--",app.applicationFilePath()};arguments<<app.arguments().mid(1);
        child.start("/usr/bin/dbus-run-session",arguments);if(!child.waitForStarted(3000)||!child.waitForFinished(60000)){child.kill();child.waitForFinished(1000);return 4;}
        return child.exitStatus()==QProcess::NormalExit?child.exitCode():5;
    }
    NightTimeBackendTest test;return QTest::qExec(&test,argc,argv);
}
#include "night_time_backend_test.moc"
