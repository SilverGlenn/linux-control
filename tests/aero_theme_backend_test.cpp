#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusServiceWatcher>
#include <QProcess>
#include "operations/OperationManager.h"
#include <QLockFile>
#include <QPointer>
#include <QSemaphore>
#include <QSaveFile>
#include <QThread>
#include <atomic>
#include <unistd.h>
#include <sys/stat.h>
#include "platform/kde/AeroThemeBackend.h"
#include "settings/AeroThemeContract.h"
#include <KConfig>
#include <KConfigGroup>
#include <QSignalSpy>
#include <QFile>
#include <functional>
#include <QTemporaryDir>
#include <QTest>

class WireThemeKWin final : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface","org.kde.KWin")
public:
    int reloads=0;
public slots:
    QString supportInformation()const{return QStringLiteral("KWin version: 6.6.6\nOperation Mode: Wayland\n");}
    Q_NOREPLY void reconfigure(){++reloads;}
};
class WireThemeEffects final : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface","org.kde.kwin.Effects")
public:
    int reloads=0;
    std::function<void()> beforeLoadedReply;
public slots:
    bool isEffectLoaded(const QString &id)const{if(beforeLoadedReply)beforeLoadedReply();return id==QLatin1String("aeroglassblur");}
    void reconfigureEffect(const QString &id){if(id==QLatin1String("aeroglassblur"))++reloads;}
};
class FakeThemeAssets final : public AeroThemeAssetProbe {
public:
    bool supported = true;
    bool validate(QStringList &issues) const override
    { issues = supported ? QStringList() : QStringList{QStringLiteral("Asset changed")}; return supported; }
};
class FakeThemeRuntime final : public AeroThemeRuntime {
public:
    bool supported = true, reloadSuccess = true, replyToReload = true;
    int probes = 0, reloads = 0;
    std::function<void()> beforeProbeFinished;
    std::function<void()> beforeReloadFinished;
    void probe() override {
        ++probes;
        if (beforeProbeFinished) beforeProbeFinished();
        emit probeFinished(supported, supported ? QString() : QStringLiteral("Runtime incompatible"));
    }
    void reload(const QSet<QString> &) override {
        ++reloads;
        if (beforeReloadFinished) beforeReloadFinished();
        if (!replyToReload) return;
        emit reloadFinished(reloadSuccess, reloadSuccess ? QString() : QStringLiteral("Saved, but reload failed"));
    }
};
static void seed(const QString &directory, const QVariantMap &values)
{
    for (const auto &field : AeroThemeContract::fields()) {
        KConfig config(directory + QLatin1Char('/') + field.file, KConfig::SimpleConfig);
        if (field.key == QLatin1String("palette")) {
            const auto palette = values.value(field.key).toMap();
            for (auto group = palette.cbegin(); group != palette.cend(); ++group) {
                KConfigGroup target(&config, group.key()); const auto entries = group.value().toMap();
                for (auto entry = entries.cbegin(); entry != entries.cend(); ++entry) target.writeEntry(entry.key(), entry.value());
            }
        } else KConfigGroup(&config, field.group).writeEntry(field.entry, values.value(field.key));
        QVERIFY(config.sync());
    }
}
static AeroThemeState read(KdeAeroThemeBackend &backend)
{
    QSignalSpy spy(&backend, &AeroThemeBackend::readFinished); backend.readCurrent();
    if (spy.isEmpty()) spy.wait(15000);
    if (spy.isEmpty() || !spy.first().at(0).toBool()) return {};
    return qvariant_cast<AeroThemeState>(spy.first().at(1));
}
static bool write(KdeAeroThemeBackend &backend,const QVariantMap &expected,const QVariantMap &requested)
{
    QSignalSpy spy(&backend,&AeroThemeBackend::writeFinished);backend.writeSettings(expected,requested);
    return !spy.isEmpty() || spy.wait(15000);
}

struct AsyncGate {
    std::atomic_bool armed{true},entered{false};
    QSemaphore released;
    void once(){if(armed.exchange(false)){entered=true;released.acquire();}}
};

class AeroThemeBackendTest : public QObject {
    Q_OBJECT
    QList<std::shared_ptr<AsyncGate>> gates;
    std::shared_ptr<AsyncGate> gate(){auto result=std::make_shared<AsyncGate>();gates<<result;return result;}
    static QByteArray bytes(const QString &path){QFile file(path);return file.open(QIODevice::ReadOnly)?file.readAll():QByteArray();}
    static AeroThemeBackendOptions options(){AeroThemeBackendOptions result;result.softDeadlineMs=20;result.runtimeDeadlineMs=500;return result;}
private slots:
    void cleanup(){for(const auto &held:gates)held->released.release(32);QTRY_VERIFY_WITH_TIMEOUT(!OperationManager::instance()->hasActiveJobs(),10000);gates.clear();}

    void initTestCase() { qRegisterMetaType<AeroThemeState>(); }
    void privateBusRuntimePinsOwnerAndHonorsUnsentReloadGuard()
    {
        QProcess process;process.start(QStringLiteral("dbus-daemon"),{QStringLiteral("--session"),QStringLiteral("--nofork"),QStringLiteral("--print-address=1")});
        QVERIFY(process.waitForStarted());QVERIFY(process.waitForReadyRead());const QString address=QString::fromUtf8(process.readLine()).trimmed();
        const auto serverName=QStringLiteral("aero-theme-wire-server"),clientName=QStringLiteral("aero-theme-wire-client"),replacementName=QStringLiteral("aero-theme-wire-replacement");
        auto server=QDBusConnection::connectToBus(address,serverName),client=QDBusConnection::connectToBus(address,clientName),replacement=QDBusConnection::connectToBus(address,replacementName);
        QVERIFY(server.isConnected()&&client.isConnected()&&replacement.isConnected());WireThemeKWin kwin,newKwin;WireThemeEffects effects,newEffects;
        QVERIFY(server.registerService("org.kde.KWin"));QVERIFY(server.registerService("org.kde.plasmashell"));QVERIFY(server.registerObject("/KWin",&kwin,QDBusConnection::ExportAllSlots));QVERIFY(server.registerObject("/Effects",&effects,QDBusConnection::ExportAllSlots));
        QVERIFY(replacement.registerObject("/KWin",&newKwin,QDBusConnection::ExportAllSlots));QVERIFY(replacement.registerObject("/Effects",&newEffects,QDBusConnection::ExportAllSlots));
        std::unique_ptr<AeroThemeRuntime> runtime(createKdeAeroThemeRuntime(client));QSignalSpy probed(runtime.get(),&AeroThemeRuntime::probeFinished),done(runtime.get(),&AeroThemeRuntime::reloadFinished),changed(runtime.get(),&AeroThemeRuntime::externalChange);
        runtime->probe();QTRY_COMPARE(probed.size(),1);QVERIFY2(probed[0][0].toBool(),qPrintable(probed[0][1].toString()));
        runtime->reloadGuarded({QStringLiteral("blurStrength")},[]{return false;});QTRY_COMPARE(done.size(),1);QVERIFY(!done[0][0].toBool());QCOMPARE(kwin.reloads,0);QCOMPARE(effects.reloads,0);
        runtime->reloadGuarded({QStringLiteral("blurStrength")},[]{return true;});QTRY_COMPARE(done.size(),2);QVERIFY2(done[1][0].toBool(),qPrintable(done[1][1].toString()));QCOMPARE(kwin.reloads,1);QCOMPARE(effects.reloads,1);
        QVERIFY(server.unregisterService("org.kde.KWin"));QVERIFY(replacement.registerService("org.kde.KWin"));QTRY_VERIFY(!changed.isEmpty());
        runtime->reload({QStringLiteral("blurStrength")});QTRY_COMPARE(done.size(),3);QVERIFY(!done[2][0].toBool());QCOMPARE(newKwin.reloads,0);QCOMPARE(newEffects.reloads,0);
        runtime.reset();QDBusConnection::disconnectFromBus(clientName);QDBusConnection::disconnectFromBus(serverName);QDBusConnection::disconnectFromBus(replacementName);process.terminate();QVERIFY(process.waitForFinished());
    }
    void finalRuntimeAliasRecheckRejectsRetiredOwnerWithDelayedWatcher_data()
    {
        QTest::addColumn<QString>("alias");
        QTest::newRow("KWin")<<QStringLiteral("org.kde.KWin");
        QTest::newRow("Plasma")<<QStringLiteral("org.kde.plasmashell");
    }
    void finalRuntimeAliasRecheckRejectsRetiredOwnerWithDelayedWatcher()
    {
        QFETCH(QString,alias);QProcess process;process.start(QStringLiteral("dbus-daemon"),{QStringLiteral("--session"),QStringLiteral("--nofork"),QStringLiteral("--print-address=1")});
        QVERIFY(process.waitForStarted());QVERIFY(process.waitForReadyRead());const auto address=QString::fromUtf8(process.readLine()).trimmed();
        const auto serverName=QStringLiteral("aero-final-owner-old"),clientName=QStringLiteral("aero-final-owner-client"),replacementName=QStringLiteral("aero-final-owner-new");
        auto server=QDBusConnection::connectToBus(address,serverName),client=QDBusConnection::connectToBus(address,clientName),replacement=QDBusConnection::connectToBus(address,replacementName);
        QVERIFY(server.isConnected()&&client.isConnected()&&replacement.isConnected());WireThemeKWin kwin,newKwin;WireThemeEffects effects,newEffects;
        QVERIFY(server.registerService("org.kde.KWin"));QVERIFY(server.registerService("org.kde.plasmashell"));
        QVERIFY(server.registerObject("/KWin",&kwin,QDBusConnection::ExportAllSlots));QVERIFY(server.registerObject("/Effects",&effects,QDBusConnection::ExportAllSlots));
        QVERIFY(replacement.registerObject("/KWin",&newKwin,QDBusConnection::ExportAllSlots));QVERIFY(replacement.registerObject("/Effects",&newEffects,QDBusConnection::ExportAllSlots));
        auto *runtime=createKdeAeroThemeRuntime(client);const auto watchers=runtime->findChildren<QDBusServiceWatcher *>();QVERIFY(!watchers.isEmpty());
        // Freeze watcher delivery: the old unique connection and its objects
        // remain alive, so only a fresh alias lookup can reject their replies.
        for(auto *watcher:watchers)watcher->blockSignals(true);
        bool moved=false;effects.beforeLoadedReply=[&]{if(effects.reloads&&!moved)moved=server.unregisterService(alias)&&replacement.registerService(alias);};
        QTemporaryDir directory;const auto defaults=AeroThemeContract::defaults();seed(directory.path(),defaults);
        {KConfig config(directory.filePath("kwinrc"),KConfig::SimpleConfig);config.group("Unrelated").writeEntry("Keep","unowned");QVERIFY(config.sync());}
        {
            KdeAeroThemeBackend backend(directory.path(),runtime,std::make_unique<FakeThemeAssets>());QSignalSpy done(&backend,&AeroThemeBackend::writeFinished);
            auto requested=defaults;requested["blurStrength"]=8;backend.writeSettings(defaults,requested);QTRY_COMPARE_WITH_TIMEOUT(done.size(),1,10000);
            QVERIFY(moved);QVERIFY(!done[0][0].toBool());QVERIFY2(done[0][1].toString().contains("pinned runtime owner changed"),qPrintable(done[0][1].toString()));
            QVERIFY(done[0][1].toString().contains("uncertain"));QCOMPARE(kwin.reloads,1);QCOMPARE(effects.reloads,1);QCOMPARE(newKwin.reloads,0);QCOMPARE(newEffects.reloads,0);
            KConfig saved(directory.filePath("kwinrc"),KConfig::SimpleConfig);QCOMPARE(saved.group("Effect-aeroglassblur").readEntry("BlurStrength",0),8);
            QCOMPARE(saved.group("Unrelated").readEntry("Keep",QString()),QStringLiteral("unowned"));QTest::qWait(30);QCOMPARE(done.size(),1);
        }
        QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
        QDBusConnection::disconnectFromBus(clientName);QDBusConnection::disconnectFromBus(serverName);QDBusConnection::disconnectFromBus(replacementName);process.terminate();QVERIFY(process.waitForFinished());
    }
    void fifoAndBrokenSourceFailClosedWithoutBlocking_data(){QTest::addColumn<bool>("broken");QTest::newRow("fifo")<<false;QTest::newRow("broken-symlink")<<true;}
    void fifoAndBrokenSourceFailClosedWithoutBlocking()
    {
        QFETCH(bool,broken);QTemporaryDir directory;seed(directory.path(),AeroThemeContract::defaults());const auto path=directory.filePath("kwinrc");QVERIFY(QFile::remove(path));
        if(broken)QVERIFY(::symlink("missing-native-source",QFile::encodeName(path).constData())==0);else QVERIFY(::mkfifo(QFile::encodeName(path).constData(),0600)==0);
        auto *runtime=new FakeThemeRuntime;KdeAeroThemeBackend backend(directory.path(),runtime,std::make_unique<FakeThemeAssets>(),options());
        QSignalSpy done(&backend,&AeroThemeBackend::readFinished);backend.readCurrent();QTRY_COMPARE_WITH_TIMEOUT(done.size(),1,2000);QVERIFY(!done[0][0].toBool());QVERIFY(!done[0][2].toString().isEmpty());QCOMPARE(runtime->reloads,0);
    }
    void sameByteSourceReplacementAfterSyncPreventsReload()
    {
        QTemporaryDir directory;const auto defaults=AeroThemeContract::defaults();seed(directory.path(),defaults);const auto held=gate();auto config=options();config.afterSync=[held]{held->once();};
        auto *runtime=new FakeThemeRuntime;KdeAeroThemeBackend backend(directory.path(),runtime,std::make_unique<FakeThemeAssets>(),config);auto requested=defaults;requested["blurStrength"]=8;
        QSignalSpy done(&backend,&AeroThemeBackend::writeFinished);backend.writeSettings(defaults,requested);QTRY_VERIFY(held->entered.load());
        const auto path=directory.filePath("smodrc");const auto same=bytes(path);QSaveFile file(path);QVERIFY(file.open(QIODevice::WriteOnly));QCOMPARE(file.write(same),same.size());QVERIFY(file.commit());
        held->released.release();QTRY_COMPARE(done.size(),1);QVERIFY(!done[0][0].toBool());QCOMPARE(runtime->reloads,0);QCOMPARE(bytes(path),same);
    }
    void nativeReadAndLockWaitKeepGuiResponsiveAndCompleteOnce()
    {
        QTemporaryDir directory;const auto defaults=AeroThemeContract::defaults();seed(directory.path(),defaults);
        const auto held=gate();auto config=options();config.beforeRead=[held]{held->once();};
        auto *runtime=new FakeThemeRuntime;KdeAeroThemeBackend backend(directory.path(),runtime,std::make_unique<FakeThemeAssets>(),config);
        QSignalSpy reads(&backend,&AeroThemeBackend::readFinished),pending(&backend,&AeroThemeBackend::operationPending);
        int beats=0;QTimer heartbeat;heartbeat.setInterval(1);connect(&heartbeat,&QTimer::timeout,this,[&]{++beats;});heartbeat.start();
        backend.readCurrent();QTRY_VERIFY(held->entered.load());QTRY_VERIFY(beats>=3);QTRY_VERIFY(!pending.isEmpty());QCOMPARE(reads.size(),0);
        held->released.release();QTRY_COMPARE(reads.size(),1);QVERIFY(reads[0][0].toBool());
        QLockFile lock(directory.filePath("kwinrc.lock"));QVERIFY(lock.tryLock());pending.clear();beats=0;
        auto requested=defaults;requested["blurStrength"]=8;QSignalSpy done(&backend,&AeroThemeBackend::writeFinished);
        QTimer unlock;unlock.setSingleShot(true);connect(&unlock,&QTimer::timeout,this,[&]{lock.unlock();});unlock.start(180);
        backend.writeSettings(defaults,requested);QTRY_VERIFY(!pending.isEmpty());QVERIFY(beats>=3);QCOMPARE(done.size(),0);
        QTRY_COMPARE_WITH_TIMEOUT(done.size(),1,10000);QVERIFY2(done[0][0].toBool(),qPrintable(done[0][1].toString()));
        QCOMPARE(read(backend).values,requested);QTest::qWait(30);QCOMPARE(done.size(),1);
    }
    void orphanedSaveKeepsRuntimeActivityAndCanonicalReservation()
    {
        QTemporaryDir directory;const auto defaults=AeroThemeContract::defaults();seed(directory.path(),defaults);
        const auto held=gate();auto config=options();config.beforeSync=[held]{held->once();};
        auto *runtime=new FakeThemeRuntime;QPointer<FakeThemeRuntime> retained(runtime);
        auto *first=new KdeAeroThemeBackend(directory.path(),runtime,std::make_unique<FakeThemeAssets>(),config);
        auto requested=defaults;requested["blurStrength"]=6;QSignalSpy gone(first,&AeroThemeBackend::writeFinished);
        first->writeSettings(defaults,requested);QTRY_VERIFY(held->entered.load());delete first;
        QVERIFY(retained);QVERIFY(OperationManager::instance()->hasActiveJobs());
        KdeAeroThemeBackend second(directory.path()+"/.",new FakeThemeRuntime,std::make_unique<FakeThemeAssets>());
        QSignalSpy refused(&second,&AeroThemeBackend::writeFinished);auto competing=defaults;competing["blurStrength"]=9;
        QVERIFY(write(second,defaults,competing));QVERIFY(!refused[0][0].toBool());QVERIFY(!refused[0][1].toString().isEmpty());
        held->released.release();QTRY_VERIFY_WITH_TIMEOUT(!OperationManager::instance()->hasActiveJobs(),10000);QTRY_VERIFY(!retained);QCOMPARE(gone.size(),0);
        QCOMPARE(read(second).values,requested);QVERIFY(write(second,requested,competing));QVERIFY(refused.last()[0].toBool());
    }
    void cancellationOrReplacementBeforeSyncRetiresUnsentMutation_data(){QTest::addColumn<bool>("replace");QTest::newRow("cancel")<<false;QTest::newRow("provider")<<true;}
    void cancellationOrReplacementBeforeSyncRetiresUnsentMutation()
    {
        QFETCH(bool,replace);QTemporaryDir directory;const auto defaults=AeroThemeContract::defaults();seed(directory.path(),defaults);
        const auto original=bytes(directory.filePath("kwinrc"));const auto held=gate();auto config=options();config.beforeSync=[held]{held->once();};
        auto *runtime=new FakeThemeRuntime;KdeAeroThemeBackend backend(directory.path(),runtime,std::make_unique<FakeThemeAssets>(),config);
        auto requested=defaults;requested["blurStrength"]=8;QSignalSpy done(&backend,&AeroThemeBackend::writeFinished);
        backend.writeSettings(defaults,requested);QTRY_VERIFY(held->entered.load());
        if(replace)emit runtime->externalChange();else backend.cancel();QCOMPARE(done.size(),0);QVERIFY(OperationManager::instance()->hasActiveJobs());
        held->released.release();QTRY_COMPARE(done.size(),1);QVERIFY(!done[0][0].toBool());QCOMPARE(runtime->reloads,0);QCOMPARE(bytes(directory.filePath("kwinrc")),original);
    }
    void cancellationOrReplacementDuringNativeWaitKeepsLeaseUntilActualCompletion_data(){cancellationOrReplacementBeforeSyncRetiresUnsentMutation_data();}
    void cancellationOrReplacementDuringNativeWaitKeepsLeaseUntilActualCompletion()
    {
        QFETCH(bool,replace);QTemporaryDir directory;const auto defaults=AeroThemeContract::defaults();seed(directory.path(),defaults);
        auto entered=std::make_shared<std::atomic_bool>(false);auto config=options();config.syncForTest=[entered](KConfig &config){*entered=true;return config.sync();};
        auto *runtime=new FakeThemeRuntime;KdeAeroThemeBackend backend(directory.path(),runtime,std::make_unique<FakeThemeAssets>(),config);
        QLockFile lock(directory.filePath("kwinrc.lock"));QVERIFY(lock.tryLock());auto requested=defaults;requested["blurStrength"]=8;
        QSignalSpy done(&backend,&AeroThemeBackend::writeFinished),pending(&backend,&AeroThemeBackend::operationPending);backend.writeSettings(defaults,requested);
        QTRY_VERIFY(entered->load());QTRY_VERIFY(!pending.isEmpty());if(replace)emit runtime->externalChange();else backend.cancel();
        KdeAeroThemeBackend competitor(directory.path(),new FakeThemeRuntime,std::make_unique<FakeThemeAssets>());QSignalSpy refused(&competitor,&AeroThemeBackend::writeFinished);
        auto competing=defaults;competing["blurStrength"]=10;QVERIFY(write(competitor,defaults,competing));QVERIFY(!refused[0][0].toBool());QCOMPARE(done.size(),0);
        lock.unlock();QTRY_COMPARE_WITH_TIMEOUT(done.size(),1,10000);QVERIFY(!done[0][0].toBool());QCOMPARE(runtime->reloads,0);
        QCOMPARE(read(competitor).values,requested);QVERIFY(done[0][1].toString().contains("uncertain"));
    }
    void failedNativeSyncDestroysDirtyConfigBeforeFailureObserver()
    {
        QTemporaryDir directory;const auto defaults=AeroThemeContract::defaults();seed(directory.path(),defaults);
        const auto target=directory.filePath("kwinrc"),parked=directory.path()+"-parked";const auto original=bytes(target);
        auto dirty=std::make_shared<std::atomic_bool>(false),blocked=std::make_shared<std::atomic_bool>(false),failed=std::make_shared<std::atomic_bool>(false);
        auto config=options();const auto root=directory.path();config.syncForTest=[root,parked,dirty,blocked,failed](KConfig &config){
            *dirty=config.isDirty();if(!QDir().rename(root,parked))return false;QFile blocker(root);*blocked=blocker.open(QIODevice::WriteOnly);blocker.close();const bool ok=config.sync();*failed=!ok;return ok;
        };
        auto *runtime=new FakeThemeRuntime;KdeAeroThemeBackend backend(directory.path(),runtime,std::make_unique<FakeThemeAssets>(),config);
        bool restored=false;connect(&backend,&AeroThemeBackend::writeFinished,this,[&](bool ok,const QString &){if(!ok&&blocked->load())restored=QFile::remove(root)&&QDir().rename(parked,root);});
        auto requested=defaults;requested["blurStrength"]=8;QSignalSpy done(&backend,&AeroThemeBackend::writeFinished);QVERIFY(write(backend,defaults,requested));
        QVERIFY(!done[0][0].toBool());QVERIFY(dirty->load()&&blocked->load()&&failed->load()&&restored);QCOMPARE(bytes(target),original);QCOMPARE(runtime->reloads,0);
        QCoreApplication::processEvents();QCOMPARE(bytes(target),original);
    }
    void unrelatedEditAfterSyncIsPreservedAndPreventsReload()
    {
        QTemporaryDir directory;const auto defaults=AeroThemeContract::defaults();seed(directory.path(),defaults);
        const auto held=gate();auto config=options();config.afterSync=[held]{held->once();};auto *runtime=new FakeThemeRuntime;
        KdeAeroThemeBackend backend(directory.path(),runtime,std::make_unique<FakeThemeAssets>(),config);
        auto requested=defaults;requested["blurStrength"]=9;QSignalSpy done(&backend,&AeroThemeBackend::writeFinished);backend.writeSettings(defaults,requested);
        QTRY_VERIFY(held->entered.load());{QFile file(directory.filePath("kwinrc"));QVERIFY(file.open(QIODevice::Append));file.write("\n[Unrelated]\nPreserve=foreign\n");}
        held->released.release();QTRY_COMPARE(done.size(),1);QVERIFY(!done[0][0].toBool());QCOMPARE(runtime->reloads,0);QVERIFY(bytes(directory.filePath("kwinrc")).contains("Preserve=foreign"));
    }
    void intermediateInodeAliasStaysReservedAcrossRepeatedNativeReplacement()
    {
        QTemporaryDir directory,other;const auto defaults=AeroThemeContract::defaults();seed(directory.path(),defaults);seed(other.path(),defaults);
        const auto held=gate();auto count=std::make_shared<std::atomic_int>(0);auto linked=std::make_shared<std::atomic_bool>(false);
        const auto alias=directory.filePath("middle-inode"),target=directory.filePath("kwinrc");auto config=options();
        config.afterSync=[held,count,linked,alias,target]{const auto step=++*count;if(step==1)*linked=::link(QFile::encodeName(target).constData(),QFile::encodeName(alias).constData())==0;if(step==2)held->once();};
        auto *runtime=new FakeThemeRuntime;KdeAeroThemeBackend first(directory.path(),runtime,std::make_unique<FakeThemeAssets>(),config);
        auto requested=defaults;requested["aeroHue"]=15;requested["blurStrength"]=8;QSignalSpy done(&first,&AeroThemeBackend::writeFinished);first.writeSettings(defaults,requested);
        QTRY_VERIFY(held->entered.load());QVERIFY(linked->load());QVERIFY(QFile::remove(other.filePath("kwinrc")));QVERIFY(::link(QFile::encodeName(alias).constData(),QFile::encodeName(other.filePath("kwinrc")).constData())==0);
        KdeAeroThemeBackend second(other.path(),new FakeThemeRuntime,std::make_unique<FakeThemeAssets>());const auto before=read(second);QVERIFY(before.canApply());
        auto competing=before.values;competing["blurStrength"]=11;QSignalSpy refused(&second,&AeroThemeBackend::writeFinished);QVERIFY(write(second,before.values,competing));QVERIFY(!refused[0][0].toBool());
        held->released.release();QTRY_COMPARE(done.size(),1);QVERIFY2(done[0][0].toBool(),qPrintable(done[0][1].toString()));QVERIFY(write(second,before.values,competing));QVERIFY(refused.last()[0].toBool());
    }
    void absentSmodResourceUsesItsRealAeroDefault()
    {
        QTemporaryDir temporary;
        const auto defaults = AeroThemeContract::defaults(); seed(temporary.path(), defaults);
        {
            KConfig config(temporary.filePath(QStringLiteral("smodrc")), KConfig::SimpleConfig);
            KConfigGroup(&config, "Windeco").deleteEntry("DecorationTheme");
            QVERIFY(config.sync());
        }
        auto *runtime = new FakeThemeRuntime;
        KdeAeroThemeBackend backend(temporary.path(), runtime, std::make_unique<FakeThemeAssets>());
        const auto state = read(backend);
        QCOMPARE(state.values, defaults);
        auto requested = defaults; requested["blurStrength"] = 6;
        QSignalSpy writes(&backend, &AeroThemeBackend::writeFinished);
        QVERIFY(write(backend,state.values, requested));
        QVERIFY(writes.last().at(0).toBool());
        QCOMPARE(read(backend).values, requested);
    }
    void restoreAndTunePreserveUnrelatedSettings()
    {
        QTemporaryDir temporary; QVERIFY(temporary.isValid());
        auto defaults = AeroThemeContract::defaults(); seed(temporary.path(), defaults);
        {
            KConfig config(temporary.filePath(QStringLiteral("kdeglobals")), KConfig::SimpleConfig);
            KConfigGroup(&config, "General").writeEntry("font", "My font,11");
            KConfigGroup(&config, "WM").writeEntry("activeFont", "My title font,11");
            KConfigGroup(&config, "Colors:Header").writeEntry("BackgroundNormal", "1,2,3");
            QVERIFY(config.sync());
            KConfig shell(temporary.filePath(QStringLiteral("plasma-org.kde.plasma.desktop-appletsrc")), KConfig::SimpleConfig);
            KConfigGroup(&shell, "Containments").writeEntry("Wallpaper", "keep-me"); QVERIFY(shell.sync());
        }
        auto *runtime = new FakeThemeRuntime;
        KdeAeroThemeBackend backend(temporary.path(), runtime, std::make_unique<FakeThemeAssets>());
        const auto baseline = read(backend); QVERIFY(baseline.canApply());
        QVERIFY(baseline.values.value(QStringLiteral("palette")).toMap().contains(QStringLiteral("Colors:Header")));
        QSignalSpy writes(&backend, &AeroThemeBackend::writeFinished);
        QVERIFY(write(backend,baseline.values, defaults)); QCOMPARE(writes.count(), 1); QVERIFY(writes.first().at(0).toBool());
        QCOMPARE(read(backend).values, defaults);
        KConfig config(temporary.filePath(QStringLiteral("kdeglobals")), KConfig::SimpleConfig);
        QCOMPARE(KConfigGroup(&config, "General").readEntry("font", QString()), QStringLiteral("My font,11"));
        QCOMPARE(KConfigGroup(&config, "WM").readEntry("activeFont", QString()), QStringLiteral("My title font,11"));
        QVERIFY(!KConfigGroup(&config, "Colors:Header").hasKey("BackgroundNormal"));
        auto tuned = defaults; tuned[QStringLiteral("blurStrength")] = 8; tuned[QStringLiteral("smodEnableShadow")] = false;
        QVERIFY(write(backend,defaults, tuned)); QVERIFY(writes.last().at(0).toBool()); QCOMPARE(read(backend).values, tuned);
        KConfig shell(temporary.filePath(QStringLiteral("plasma-org.kde.plasma.desktop-appletsrc")), KConfig::SimpleConfig);
        QCOMPARE(KConfigGroup(&shell, "Containments").readEntry("Wallpaper", QString()), QStringLiteral("keep-me"));
        QCOMPARE(runtime->reloads, 2);
    }
    void rejectUnapprovedIncompleteStaleAndLockedBeforeAnyWrite()
    {
        QTemporaryDir temporary; auto defaults = AeroThemeContract::defaults(); seed(temporary.path(), defaults);
        auto *runtime = new FakeThemeRuntime;
        KdeAeroThemeBackend backend(temporary.path(), runtime, std::make_unique<FakeThemeAssets>());
        QSignalSpy writes(&backend, &AeroThemeBackend::writeFinished);
        auto requested = defaults; requested[QStringLiteral("iconTheme")] = QStringLiteral("breeze");
        QVERIFY(write(backend,defaults, requested)); QVERIFY(!writes.last().at(0).toBool()); QCOMPARE(runtime->probes, 0);
        requested = defaults; requested.remove(QStringLiteral("palette"));
        QVERIFY(write(backend,defaults, requested)); QVERIFY(!writes.last().at(0).toBool()); QCOMPARE(runtime->probes, 0);
        requested = defaults; requested[QStringLiteral("blurStrength")] = 9;
        { KConfig config(temporary.filePath(QStringLiteral("kwinrc")), KConfig::SimpleConfig);
          KConfigGroup(&config, "Effect-aeroglassblur").writeEntry("AeroHue", 7); QVERIFY(config.sync()); }
        QVERIFY(write(backend,defaults, requested)); QVERIFY(!writes.last().at(0).toBool()); QCOMPARE(runtime->reloads, 0);
        QCOMPARE(read(backend).values.value(QStringLiteral("blurStrength")).toInt(), 3);
        seed(temporary.path(), defaults);
        QFile locked(temporary.filePath(QStringLiteral("smodrc"))); QVERIFY(locked.open(QIODevice::WriteOnly));
        locked.write("[Common][$i]\nEnableShadow=true\nOutlineCloseButton=false\n[Windeco]\nDecorationTheme=Aero\n"); locked.close();
        QVERIFY(write(backend,defaults, requested)); QVERIFY(!writes.last().at(0).toBool()); QCOMPARE(runtime->reloads, 0);
        QCOMPARE(read(backend).values.value(QStringLiteral("blurStrength")).toInt(), 3);
    }
    void revalidateAssetsAndRuntimeAndReportSavedReloadFailure()
    {
        QTemporaryDir temporary; const auto defaults = AeroThemeContract::defaults(); seed(temporary.path(), defaults);
        auto assets = std::make_unique<FakeThemeAssets>(); auto *probe = assets.get(); auto *runtime = new FakeThemeRuntime;
        KdeAeroThemeBackend backend(temporary.path(), runtime, std::move(assets)); QVERIFY(read(backend).canApply());
        auto requested = defaults; requested[QStringLiteral("blurStrength")] = 6;
        QSignalSpy writes(&backend, &AeroThemeBackend::writeFinished);
        probe->supported = false; QVERIFY(write(backend,defaults, requested)); QVERIFY(!writes.last().at(0).toBool());
        QCOMPARE(read(backend).values, defaults); QCOMPARE(runtime->reloads, 0);
        probe->supported = true; runtime->supported = false; QVERIFY(write(backend,defaults, requested)); QVERIFY(!writes.last().at(0).toBool());
        QCOMPARE(read(backend).values, defaults);
        runtime->supported = true; runtime->reloadSuccess = false; QVERIFY(write(backend,defaults, requested));
        QVERIFY(!writes.last().at(0).toBool()); QCOMPARE(read(backend).values, requested);
        QVERIFY(writes.last().at(1).toString().contains(QStringLiteral("Saved")));
    }
    void reloadTimeoutKeepsSavedValuesAndPoisonsLateCallbacks()
    {
        QTemporaryDir temporary; const auto defaults = AeroThemeContract::defaults(); seed(temporary.path(), defaults);
        auto *runtime = new FakeThemeRuntime; runtime->replyToReload = false;
        KdeAeroThemeBackend backend(temporary.path(), runtime, std::make_unique<FakeThemeAssets>());
        auto requested = defaults; requested[QStringLiteral("blurStrength")] = 7;
        QSignalSpy writes(&backend, &AeroThemeBackend::writeFinished);
        backend.writeSettings(defaults, requested);
        QVERIFY(writes.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(writes.count(), 1, 14000);
        QVERIFY(!writes.last().at(0).toBool());
        QVERIFY(writes.last().at(1).toString().contains(QStringLiteral("timed out")));
        emit runtime->reloadFinished(true, QString());
        emit runtime->probeFinished(true, QString());
        QCOMPARE(writes.count(), 1);
        const auto saved = read(backend);
        QCOMPARE(saved.values, requested);
        QVERIFY(!saved.runtimeSupported); QVERIFY(!saved.canApply());
        QVERIFY(saved.issues.join(QLatin1Char('\n')).contains(QStringLiteral("restart")));
        const int probes = runtime->probes;
        backend.writeSettings(requested, defaults);
        QCOMPARE(writes.count(), 2); QVERIFY(!writes.last().at(0).toBool());
        QCOMPARE(runtime->probes, probes); QCOMPARE(runtime->reloads, 1);
        QCOMPARE(read(backend).values, requested);
    }
    void savedReadbackDetectsConcurrentChangesDuringReload()
    {
        QTemporaryDir temporary; const auto defaults = AeroThemeContract::defaults(); seed(temporary.path(), defaults);
        auto *runtime = new FakeThemeRuntime;
        KdeAeroThemeBackend backend(temporary.path(), runtime, std::make_unique<FakeThemeAssets>());
        runtime->beforeReloadFinished = [&] {
            KConfig config(temporary.filePath(QStringLiteral("kcminputrc")), KConfig::SimpleConfig);
            KConfigGroup(&config, "Mouse").writeEntry("cursorSize", 48); QVERIFY(config.sync());
        };
        auto requested = defaults; requested[QStringLiteral("blurStrength")] = 5;
        QSignalSpy writes(&backend, &AeroThemeBackend::writeFinished); QVERIFY(write(backend,defaults, requested));
        QVERIFY(!writes.last().at(0).toBool());
        QCOMPARE(read(backend).values.value(QStringLiteral("blurStrength")).toInt(), 5);
        QCOMPARE(read(backend).values.value(QStringLiteral("cursorSize")).toInt(), 48);
    }
};
QTEST_GUILESS_MAIN(AeroThemeBackendTest)
#include "aero_theme_backend_test.moc"
