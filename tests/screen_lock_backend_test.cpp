#include <QtTest>
#include "platform/kde/ScreenLockBackend.h"
#include <KConfig>
#include <KConfigGroup>
#include <KSharedConfig>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <cmath>
#include <atomic>
#include <QThread>
#include <sys/stat.h>

class FakeScreenLockService final : public ScreenLockService {
public:
    ScreenLockRuntime state;
    bool hold=false,failConfigure=false;quint64 heldToken=0;QString heldOwner;int queries=0,configures=0;std::function<void()> beforeConfigure;
    std::function<void()> beforeQuery;
    FakeScreenLockService(){state.installedVerified=true;state.version="6.6.5";state.epoch=1;}
    void query(quint64 token,const QString &owner={}) override
    {
        ++queries;if(hold){heldToken=token;heldOwner=owner;return;}
        if(beforeQuery)beforeQuery();
        const bool valid=state.installedVerified&&(!state.serviceAvailable||state.providerVerified)&&(owner.isEmpty()||owner==state.owner);
        emit queryFinished(token,valid,state,valid?QString():QStringLiteral("Native identity changed"));
    }
    void configure(quint64 token,const QString &owner,quint64 epoch,Guard guard) override
    {
        if(beforeConfigure)beforeConfigure();QPointer<FakeScreenLockService> safe(this);
        guard([safe,token,owner,epoch](bool valid,const QString &error){
            if(!safe)return;
            if(owner!=safe->state.owner||epoch!=safe->state.epoch||!valid){emit safe->configureFinished(token,false,error.isEmpty()?QStringLiteral("Provider changed"):error);return;}
            ++safe->configures;emit safe->configureFinished(token,!safe->failConfigure,safe->failConfigure?QStringLiteral("Configure unacknowledged"):QString());
        });
    }
    void release(){hold=false;query(heldToken,heldOwner);}
};

class ScreenLockBackendTest final : public QObject {
    Q_OBJECT
    QTemporaryDir directory;QString path;FakeScreenLockService *service=nullptr;KdeScreenLockBackend *backend=nullptr;quint64 token=0;
    void saveFile(const QString &file,const QByteArray &bytes)
    {
        QSaveFile target(file);QVERIFY(target.open(QIODevice::WriteOnly));QCOMPARE(target.write(bytes),bytes.size());QVERIFY(target.commit());
        QFile saved(file);QVERIFY(saved.open(QIODevice::ReadOnly));static qint64 stamp=1000000;QVERIFY(saved.setFileTime(QDateTime::fromMSecsSinceEpoch(++stamp),QFileDevice::FileModificationTime));
    }
    QByteArray contents(const QString &file){QFile input(file);if(!input.open(QIODevice::ReadOnly))return {};return input.readAll();}
    void replaceBackend(const KdeScreenLockBackend::ConfigContext &context={})
    {delete backend;service=new FakeScreenLockService;backend=new KdeScreenLockBackend(path,service,context);}
    ScreenLockSnapshot read()
    {
        QSignalSpy done(backend,&ScreenLockBackend::readFinished);backend->readCurrent(++token);if(done.isEmpty())done.wait(3000);
        if(done.size()!=1||!done[0][1].toBool())return {};return qvariant_cast<ScreenLockSnapshot>(done[0][2]);
    }
    QList<QVariant> write(const ScreenLockSnapshot &before,const ScreenLockSnapshot &request)
    {
        QSignalSpy done(backend,&ScreenLockBackend::writeFinished);backend->writeChanges(++token,before,request);if(done.isEmpty())done.wait(60000);
        // A fixed-size failure result makes timeout failures readable assertions.
        return done.isEmpty()?QList<QVariant>{QVariant::fromValue(token),false,false,QStringLiteral("No completion")}:done[0];
    }
private slots:
    void initTestCase(){qRegisterMetaType<ScreenLockSnapshot>();qRegisterMetaType<ScreenLockRuntime>();QStandardPaths::setTestModeEnabled(false);}
    void init(){QVERIFY(directory.isValid());path=directory.filePath("kscreenlockerrc");QFile::remove(path);token=0;replaceBackend();}
    void cleanup(){delete backend;backend=nullptr;}
    void passiveOfflineReadHasNineNativeFieldsAndCreatesNothing()
    {
        const auto snapshot=read();QCOMPARE(snapshot.fields.size(),9);QVERIFY(!QFileInfo::exists(path));
        for(const auto key:ScreenLockTypes::keys()){
            const auto field=snapshot.fields[key];QVERIFY(field.known&&field.defaultKnown&&field.defaultNoOp);QCOMPARE(field.value,ScreenLockTypes::builtinDefault(key));QCOMPARE(field.writable,ScreenLockTypes::owned(key));
        }
        QCOMPARE(service->configures,0);
    }
    void offlineTypedSavePreservesHiddenAppearanceAndForeignEntries()
    {
        saveFile(path,"[Daemon]\nLock=true\nLockOnStart=true\n[Greeter]\nTheme=approved-aero-shell\nWallpaperPlugin=foreign-plugin\n[Foreign]\nKeep=unowned\n");
        const auto before=read();auto request=before;QString error;QVERIFY(ScreenLockTypes::selectTimeout(request,before,10.0,error));QVERIFY(ScreenLockTypes::selectGrace(request,before,30,error));
        const auto result=write(before,request);QVERIFY(result[1].toBool());QVERIFY(!result[2].toBool());QVERIFY2(result[3].toString().isEmpty(),qPrintable(result[3].toString()));QCOMPARE(service->configures,0);
        const auto actual=read();QVERIFY2(ScreenLockTypes::matchesRequest(before,request,actual,error),qPrintable(error));
        for(const auto key:{ScreenLockKey::Lock,ScreenLockKey::LockOnStart,ScreenLockKey::Theme,ScreenLockKey::WallpaperPlugin})QVERIFY(actual.fields[key]==before.fields[key]);
        QVERIFY(contents(path).contains("Keep=unowned"));QCOMPARE(actual.fields[ScreenLockKey::Timeout].value.metaType().id(),int(QMetaType::Double));QCOMPARE(actual.fields[ScreenLockKey::LockGrace].value.metaType().id(),int(QMetaType::Int));
    }
    void runningProviderAcknowledgementAndFailureRemainDistinct()
    {
        service->state.owner=":fake.1";service->state.serviceAvailable=service->state.providerVerified=true;
        auto before=read();auto request=before;QString error;QVERIFY(ScreenLockTypes::stage(request,before,ScreenLockKey::LockOnResume,false,error));auto result=write(before,request);
        QVERIFY(result[1].toBool()&&result[2].toBool());QVERIFY(result[3].toString().isEmpty());QCOMPARE(service->configures,1);
        before=read();request=before;QVERIFY(ScreenLockTypes::stage(request,before,ScreenLockKey::LockOnResume,true,error));service->failConfigure=true;result=write(before,request);
        QVERIFY(result[1].toBool());QVERIFY(!result[2].toBool());QVERIFY(!result[3].toString().isEmpty());QCOMPARE(service->configures,2);QVERIFY(read().fields[ScreenLockKey::LockOnResume].value.toBool());
    }
    void malformedSameFallbackRepairIsExplicitAndNativeBooleansStayRaw()
    {
        saveFile(path,"[Daemon]\nAutolock=false\nTimeout=malformed\nRequirePassword=other-text-is-true\n");const auto before=read();QVERIFY(!before.fields[ScreenLockKey::Timeout].known);QVERIFY(before.fields[ScreenLockKey::RequirePassword].known&&before.fields[ScreenLockKey::RequirePassword].value.toBool());
        auto request=before;QString error;QVERIFY(ScreenLockTypes::stage(request,before,ScreenLockKey::Timeout,5.0,error));QVERIFY(request.intents.contains(ScreenLockKey::Timeout));
        const auto result=write(before,request);QVERIFY(result[1].toBool());QVERIFY2(result[3].toString().isEmpty(),qPrintable(result[3].toString()));QVERIFY(read().fields[ScreenLockKey::Timeout].known);QVERIFY(contents(path).contains("RequirePassword=other-text-is-true"));
    }
    void dormantFractionalAndNanTimeoutsRemainUntouched()
    {
        saveFile(path,"[Daemon]\nTimeout=1.2345678901234567\n");auto before=read();auto request=before;QString error;QVERIFY(ScreenLockTypes::stage(request,before,ScreenLockKey::LockOnResume,false,error));auto result=write(before,request);
        QVERIFY(result[1].toBool());QVERIFY(result[3].toString().isEmpty());QVERIFY(contents(path).contains("Timeout=1.2345678901234567"));QVERIFY(read().fields[ScreenLockKey::Timeout]==before.fields[ScreenLockKey::Timeout]);
        saveFile(path,"[Daemon]\nAutolock=false\nTimeout=nan\n");before=read();QVERIFY(before.fields[ScreenLockKey::Timeout].known);QVERIFY(std::isnan(before.fields[ScreenLockKey::Timeout].value.toDouble()));request=before;
        QVERIFY(ScreenLockTypes::stage(request,before,ScreenLockKey::LockOnResume,false,error));result=write(before,request);QVERIFY(result[1].toBool());QVERIFY2(result[3].toString().isEmpty(),qPrintable(result[3].toString()));QVERIFY(contents(path).contains("Timeout=nan"));
    }
    void hiddenLegacyLockRequiresSafeGraceEvenWithoutPassword()
    {
        saveFile(path,"[Daemon]\nLock=true\nLockGrace=2000000000\nRequirePassword=false\n");const auto bytes=contents(path);const auto before=read();auto request=before;QString error;
        QVERIFY(ScreenLockTypes::stage(request,before,ScreenLockKey::LockOnResume,false,error));auto result=write(before,request);QVERIFY(!result[1].toBool());QCOMPARE(contents(path),bytes);QCOMPARE(service->configures,0);
        QVERIFY(ScreenLockTypes::selectGrace(request,before,30,error));result=write(before,request);QVERIFY(result[1].toBool());QVERIFY2(result[3].toString().isEmpty(),qPrintable(result[3].toString()));QCOMPARE(read().fields[ScreenLockKey::LockGrace].value.toInt(),30);
    }
    void nativeDefaultsUseInheritedTypedValuesOrRemoveOverrides()
    {
        const auto defaults=directory.filePath("locker-defaults");saveFile(defaults,"[Daemon]\nTimeout=7.123456789012345\n");saveFile(path,"[Daemon]\nTimeout=10\nLockGrace=30\n");replaceBackend({false,{defaults}});
        const auto before=read();auto request=before;QString error;QVERIFY(ScreenLockTypes::restore(request,before,ScreenLockKey::Timeout,error));QVERIFY(ScreenLockTypes::restore(request,before,ScreenLockKey::LockGrace,error));const auto result=write(before,request);
        QVERIFY(result[1].toBool());QVERIFY2(result[3].toString().isEmpty(),qPrintable(result[3].toString()));const auto actual=read();QVERIFY(actual.fields[ScreenLockKey::Timeout].localPresent);QVERIFY(actual.fields[ScreenLockKey::Timeout].value.toDouble()==7.123456789012345);QVERIFY(!actual.fields[ScreenLockKey::LockGrace].localPresent);
        QCOMPARE(actual.fields[ScreenLockKey::Timeout].inheritedRevision,before.fields[ScreenLockKey::Timeout].inheritedRevision);QCOMPARE(contents(defaults),QByteArray("[Daemon]\nTimeout=7.123456789012345\n"));
    }
    void absentNanDefaultIsNotMistakenForNativeNoOp()
    {
        const auto defaults=directory.filePath("nan-defaults");saveFile(defaults,"[Daemon]\nTimeout=nan\n");saveFile(path,"[Daemon]\nAutolock=false\nTimeout[$d]\n");replaceBackend({false,{defaults}});
        const auto before=read();const auto field=before.fields[ScreenLockKey::Timeout];QVERIFY(!field.effectivePresent);QVERIFY(field.known&&field.defaultKnown);QVERIFY(std::isnan(field.value.toDouble()));QVERIFY(!field.defaultNoOp);QVERIFY(field.resettable);
        auto request=before;QString error;QVERIFY(ScreenLockTypes::restore(request,before,ScreenLockKey::Timeout,error));QVERIFY(request.intents.contains(ScreenLockKey::Timeout));const auto result=write(before,request);QVERIFY(result[1].toBool());QVERIFY2(result[3].toString().isEmpty(),qPrintable(result[3].toString()));QCOMPARE(contents(defaults),QByteArray("[Daemon]\nTimeout=nan\n"));QCOMPARE(service->configures,0);
    }
    void policyAndDefaultSyntaxChangesRefuseBeforeSave()
    {
        const auto defaults=directory.filePath("policy-defaults");saveFile(defaults,"[Daemon]\nLockOnResume[$i]=true\nTimeout=5.0\n");replaceBackend({false,{defaults}});const auto before=read();auto request=before;QString error;
        QVERIFY(!ScreenLockTypes::stage(request,before,ScreenLockKey::LockOnResume,false,error));QVERIFY(ScreenLockTypes::selectGrace(request,before,30,error));
        service->hold=true;QSignalSpy done(backend,&ScreenLockBackend::writeFinished);backend->writeChanges(++token,before,request);saveFile(defaults,"[Daemon]\nLockOnResume[$i]=true\nTimeout=05.0\n");service->release();QTRY_COMPARE(done.size(),1);QVERIFY(!done[0][1].toBool());QVERIFY(!QFileInfo::exists(path));
    }
    void nativeSyncFailureDoesNotReloadOrRetryAfterUnlock()
    {
        KdeScreenLockBackend::ConfigContext context;context.fixture=true;context.softDeadlineMs=20;replaceBackend(context);
        saveFile(path,"[Daemon]\nLockOnResume=true\n");const auto bytes=contents(path);const auto before=read();auto request=before;QString error;QVERIFY(ScreenLockTypes::stage(request,before,ScreenLockKey::LockOnResume,false,error));
        QSignalSpy pending(backend,&ScreenLockBackend::operationPending);int beats=0;QTimer heartbeat;heartbeat.setInterval(10);connect(&heartbeat,&QTimer::timeout,this,[&]{++beats;});heartbeat.start();
        QLockFile lock(path+".lock");QVERIFY(lock.tryLock());const auto result=write(before,request);heartbeat.stop();QVERIFY(beats>5);QVERIFY(!pending.isEmpty());QVERIFY(!result[2].toBool());QVERIFY(!result[3].toString().isEmpty());QCOMPARE(service->configures,0);lock.unlock();
        QCOMPARE(contents(path),bytes);delete backend;backend=nullptr;QCOMPARE(contents(path),bytes);
    }
    void fixtureHooksRunOnWorkersAndProductionIgnoresThem()
    {
        auto *guiThread=QThread::currentThread();std::atomic_int reads{0},syncs{0},after{0};std::atomic_bool touchedGui{false};
        KdeScreenLockBackend::ConfigContext context;context.fixture=true;
        context.beforeFileRead=[&]{++reads;if(QThread::currentThread()==guiThread)touchedGui=true;};
        context.beforeSync=[&]{++syncs;if(QThread::currentThread()==guiThread)touchedGui=true;};
        context.afterSync=[&]{++after;if(QThread::currentThread()==guiThread)touchedGui=true;};replaceBackend(context);
        const auto before=read();auto request=before;QString error;QVERIFY(ScreenLockTypes::stage(request,before,ScreenLockKey::LockOnResume,false,error));const auto result=write(before,request);
        QVERIFY2(result[3].toString().isEmpty(),qPrintable(result[3].toString()));QVERIFY(result[1].toBool());QVERIFY(reads>=4);QCOMPARE(syncs.load(),1);QCOMPARE(after.load(),1);QVERIFY(!touchedGui);
        context.fixture=false;replaceBackend(context);const auto prior=reads.load();QVERIFY(!read().fields.isEmpty());QCOMPARE(reads.load(),prior);
    }
    void foreignChangeAtConfigureBoundaryIsPreservedWithoutReload()
    {
        service->state.owner=":fake.1";service->state.serviceAvailable=service->state.providerVerified=true;
        const auto before=read();auto request=before;QString error;QVERIFY(ScreenLockTypes::stage(request,before,ScreenLockKey::LockOnResume,false,error));
        service->beforeConfigure=[&]{KConfig config(path,KConfig::SimpleConfig);config.group("Daemon").writeEntry("LockGrace",40);QVERIFY(config.sync());};
        const auto result=write(before,request);QVERIFY(result[1].toBool());QVERIFY(!result[2].toBool());QVERIFY(!result[3].toString().isEmpty());QCOMPARE(service->configures,0);QCOMPARE(read().fields[ScreenLockKey::LockGrace].value.toInt(),40);
    }
    void ownerEpochAndForgedHiddenRequestsNeverSave()
    {
        const auto before=read();auto request=before;QString error;QVERIFY(ScreenLockTypes::stage(request,before,ScreenLockKey::LockOnResume,false,error));service->state.epoch++;auto result=write(before,request);QVERIFY(!result[1].toBool());QVERIFY(!QFileInfo::exists(path));service->state.epoch--;
        request.fields[ScreenLockKey::Theme].value="unapproved";request.intents[ScreenLockKey::Theme]=ScreenLockIntent::Set;result=write(before,request);QVERIFY(!result[1].toBool());QVERIFY(!QFileInfo::exists(path));
    }
    void globalOnlyAndShadowedDefaultsUseNativeResetMasks()
    {
        delete backend;backend=nullptr;path=QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)+"/kscreenlockerrc";const auto global=QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)+"/kdeglobals";QFile::remove(path);QFile::remove(global);
        saveFile(global,"[Daemon]\nLockOnResume=false\n[Foreign]\nKeep=global\n");replaceBackend({true,{}});auto before=read();auto request=before;QString error;QVERIFY(ScreenLockTypes::restore(request,before,ScreenLockKey::LockOnResume,error));auto result=write(before,request);QVERIFY(result[1].toBool());QVERIFY(result[3].toString().isEmpty());QVERIFY(!read().fields[ScreenLockKey::LockOnResume].globalPresent);
        saveFile(global,"[Daemon]\nLockOnResume=false\n[Foreign]\nKeep=global\n");saveFile(path,"[Daemon]\nLockOnResume=false\n");before=read();request=before;QVERIFY(ScreenLockTypes::restore(request,before,ScreenLockKey::LockOnResume,error));result=write(before,request);QVERIFY(result[1].toBool());QVERIFY2(result[3].toString().isEmpty(),qPrintable(result[3].toString()));const auto actual=read();QVERIFY(!actual.fields[ScreenLockKey::LockOnResume].effectivePresent&&actual.fields[ScreenLockKey::LockOnResume].globalPresent);QVERIFY(contents(path).contains("[$d]"));QVERIFY(contents(global).contains("Keep=global"));QFile::remove(path);QFile::remove(global);
    }
    void unverifiedInstallationDoesNotInventKnownDefaults()
    {
        service->state.installedVerified=false;service->state.reason="Unsupported installation";const auto snapshot=read();QCOMPARE(snapshot.fields.size(),9);for(const auto &f:snapshot.fields)QVERIFY(!f.known&&!f.defaultKnown&&!f.writable);
    }
    void fifoBrokenLinkAndWriterAliasesFailWithoutOpeningOrMutation()
    {
        QVERIFY(::mkfifo(QFile::encodeName(path).constData(),0600)==0);QSignalSpy done(backend,&ScreenLockBackend::readFinished);backend->readCurrent(++token);QTRY_COMPARE(done.size(),1);QVERIFY(!done[0][1].toBool());QVERIFY(QFile::remove(path));
        QVERIFY(QFile::link(directory.filePath("missing-target"),path));backend->readCurrent(++token);QTRY_COMPARE(done.size(),2);QVERIFY(!done[1][1].toBool());QVERIFY(QFile::remove(path));
        const auto defaults=directory.filePath("aliased-defaults");saveFile(defaults,"[Daemon]\nLockOnResume=false\n");QVERIFY(QFile::link(defaults,path));replaceBackend({false,{defaults}});
        QSignalSpy alias(backend,&ScreenLockBackend::readFinished);backend->readCurrent(++token);QTRY_COMPARE(alias.size(),1);QVERIFY(!alias[0][1].toBool());QCOMPARE(contents(defaults),QByteArray("[Daemon]\nLockOnResume=false\n"));QCOMPARE(service->configures,0);
    }
};
int main(int argc,char **argv)
{
    QCoreApplication app(argc,argv);
    if(!qEnvironmentVariableIsSet("CONTROL_SCREEN_LOCK_CHILD")){
        QTemporaryDir sandbox;if(!sandbox.isValid())return 2;auto environment=QProcessEnvironment::systemEnvironment();const auto config=sandbox.filePath("config"),defaults=sandbox.filePath("defaults"),cache=sandbox.filePath("cache");
        if(!QDir().mkpath(config)||!QDir().mkpath(defaults)||!QDir().mkpath(cache))return 3;
        environment.insert("XDG_CONFIG_HOME",config);environment.insert("XDG_CONFIG_DIRS",defaults);environment.insert("XDG_CACHE_HOME",cache);environment.insert("KDE_SKIP_KDERC","1");environment.insert("CONTROL_SCREEN_LOCK_CHILD","1");
        QProcess child;child.setProcessEnvironment(environment);child.setProcessChannelMode(QProcess::ForwardedChannels);QStringList arguments{"--",app.applicationFilePath()};arguments<<app.arguments().mid(1);child.start("/usr/bin/dbus-run-session",arguments);
        if(!child.waitForStarted(3000)||!child.waitForFinished(60000)){child.kill();child.waitForFinished(1000);return 4;}return child.exitStatus()==QProcess::NormalExit?child.exitCode():5;
    }
    ScreenLockBackendTest test;return QTest::qExec(&test,argc,argv);
}
#include "screen_lock_backend_test.moc"
