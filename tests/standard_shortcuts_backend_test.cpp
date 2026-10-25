#include <QtTest>
#include <KConfig>
#include <KConfigGroup>
#include <KStandardShortcut>
#include <QFile>
#include <QSemaphore>
#include <QTemporaryDir>
#include <memory>
#include <unistd.h>
#include <sys/stat.h>
#include "platform/shortcuts/StandardShortcutsBackend.h"
#include "platform/apps/DefaultAppsBackend.h"
#include "operations/OperationManager.h"
#include <QScopeGuard>
#include <QThread>
#include <QDir>
#include <QTimer>
#include <atomic>

namespace {
StandardShortcutsBackendOptions options(QTemporaryDir &dir)
{ StandardShortcutsBackendOptions o; o.isolated = true; o.config = dir.filePath(QStringLiteral("kdeglobals")); return o; }
void save(const QString &path, const QByteArray &data)
{ QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(data), data.size()); }
QByteArray data(const QString &path)
{ QFile file(path); if (!file.open(QIODevice::ReadOnly)) return {}; return file.readAll(); }
ShortcutsSnapshot read(KdeStandardShortcutsBackend &backend)
{
    QSignalSpy spy(&backend, &ShortcutsBackend::readFinished); backend.readCurrent(17);
    if (spy.isEmpty()) spy.wait(6000);
    return spy.isEmpty() ? ShortcutsSnapshot{} : qvariant_cast<ShortcutsSnapshot>(spy.first().at(2));
}
QList<QVariant> write(KdeStandardShortcutsBackend &backend, const ShortcutsSnapshot &expected, const ShortcutChanges &changes)
{
    QSignalSpy spy(&backend, &ShortcutsBackend::writeFinished); backend.writeChanges(19, expected, changes);
    if (spy.isEmpty()) spy.wait(6000);
    return spy.isEmpty() ? QList<QVariant>{} : spy.first();
}
ShortcutActionId identity(const ShortcutsSnapshot &snapshot, const QString &name)
{ for (const auto &action : snapshot.actions) if (action.id.action == name) return action.id; return {}; }
QList<QKeySequence> unusual()
{ return {QKeySequence(QStringLiteral("Meta+Ctrl+F24")), QKeySequence(QStringLiteral("Meta+Ctrl+F23, Meta+Ctrl+F22, Meta+Ctrl+F21, Meta+Ctrl+F20"))}; }
}
class StandardShortcutsBackendTest : public QObject {
    Q_OBJECT
private slots:
    void completeInstalledCatalog()
    {
        QTemporaryDir dir; auto o = options(dir); KdeStandardShortcutsBackend backend(o);
        const auto snapshot = read(backend); QVERIFY(snapshot.available); QVERIFY(snapshot.writable);
        int supported = 0;
        for (int i = KStandardShortcut::AccelNone + 1; i < KStandardShortcut::StandardShortcutCount; ++i) {
            const auto native = static_cast<KStandardShortcut::StandardShortcut>(i);
            if (KStandardShortcut::name(native).isEmpty() || KStandardShortcut::category(native) == KStandardShortcut::Category::InvalidCategory) continue;
            ++supported;
            const auto id = identity(snapshot, KStandardShortcut::name(native));
            const auto *action = snapshot.find(id); QVERIFY(action);
            QCOMPARE(id.context, QStringLiteral("default"));
            QCOMPARE(action->defaults, KStandardShortcut::hardcodedDefaultShortcut(native));
            QCOMPARE(action->current, action->defaults);
            QCOMPARE(action->friendlyName, KStandardShortcut::label(native));
        }
        QCOMPARE(snapshot.actions.size(), supported);
        QVERIFY(snapshot.reason.contains(QStringLiteral("restart")));
        QVERIFY(!QFileInfo::exists(o.config));
    }
    void alternativesChordsDisableResetAndPreserve()
    {
        QTemporaryDir dir; const auto o = options(dir);
        save(o.config, "[General]\nColorScheme=Aero\n[AppOverride][Shortcuts]\nCopy=Alt+C\n[Shortcuts]\nUnrecognizedFutureAction=Ctrl+Alt+F24\n");
        int notified = 0; auto isolated = o;
        isolated.notify = [&](const QByteArrayList &keys, QString &) { ++notified; return keys == QByteArrayList{"Open"}; };
        KdeStandardShortcutsBackend backend(isolated); auto snapshot = read(backend);
        const auto id = identity(snapshot, QStringLiteral("Open"));
        auto reply = write(backend, snapshot, {{id, unusual()}}); QCOMPARE(reply.size(), 4);
        QCOMPARE(qvariant_cast<ShortcutsOutcome>(reply[1]), ShortcutsOutcome::Success); QCOMPARE(notified, 1);
        snapshot = read(backend); QCOMPARE(snapshot.find(id)->current, unusual());
        KConfig file(o.config, KConfig::SimpleConfig);
        QCOMPARE(file.group("General").readEntry("ColorScheme"), QStringLiteral("Aero"));
        QCOMPARE(file.group("AppOverride").group("Shortcuts").readEntry("Copy"), QStringLiteral("Alt+C"));
        QCOMPARE(file.group("Shortcuts").readEntry("UnrecognizedFutureAction"), QStringLiteral("Ctrl+Alt+F24"));
        QCOMPARE(file.group("Shortcuts").readEntry("Open"), QKeySequence::listToString(unusual(), QKeySequence::PortableText));
        reply = write(backend, snapshot, {{id, {}}}); QCOMPARE(qvariant_cast<ShortcutsOutcome>(reply[1]), ShortcutsOutcome::Success);
        snapshot = read(backend); QVERIFY(snapshot.find(id)->current.isEmpty());
        reply = write(backend, snapshot, {{id, snapshot.find(id)->defaults}});
        QCOMPARE(qvariant_cast<ShortcutsOutcome>(reply[1]), ShortcutsOutcome::Success);
        file.reparseConfiguration(); QVERIFY(!file.group("Shortcuts").hasKey("Open"));
        QCOMPARE(read(backend).find(id)->current, KStandardShortcut::hardcodedDefaultShortcut(KStandardShortcut::Open));
    }
    void freshExternalReadAndStaleGuard()
    {
        QTemporaryDir dir; const auto o = options(dir); KdeStandardShortcutsBackend backend(o);
        const auto initial = read(backend); const auto id = identity(initial, QStringLiteral("Open"));
        save(o.config, "[Shortcuts]\nOpen=Meta+F24; Meta+F23, Meta+F22\n[General]\nOther=external\n");
        const auto bytes = data(o.config); const auto fresh = read(backend);
        QCOMPARE(fresh.find(id)->current.size(), 2); QCOMPARE(fresh.find(id)->current.last().count(), 2);
        QVERIFY(fresh.revision != initial.revision);
        const auto reply = write(backend, initial, {{id, unusual()}});
        QCOMPARE(qvariant_cast<ShortcutsOutcome>(reply[1]), ShortcutsOutcome::Failed); QCOMPARE(data(o.config), bytes);
    }
    void nativePolicyLayersAndDeletionMarker()
    {
        QTemporaryDir dir; auto o = options(dir); const auto system = dir.filePath(QStringLiteral("system"));
        save(system, "[Shortcuts]\nOpen=Meta+F24\nCopy[$i]=Meta+F23\n"); o.systemConfigs = {system};
        save(o.config, "[Shortcuts]\nCopy=Ctrl+Alt+C\n[General]\nUnrelated=retained\n");
        KdeStandardShortcutsBackend backend(o); auto snapshot = read(backend);
        const auto open = identity(snapshot, QStringLiteral("Open")), copy = identity(snapshot, QStringLiteral("Copy"));
        QCOMPARE(snapshot.find(open)->current, QList<QKeySequence>{QKeySequence(QStringLiteral("Meta+F24"))});
        QCOMPARE(snapshot.find(copy)->current, QList<QKeySequence>{QKeySequence(QStringLiteral("Meta+F23"))});
        QVERIFY(!snapshot.find(copy)->available); QVERIFY(snapshot.find(open)->available);
        auto reply = write(backend, snapshot, {{copy, unusual()}});
        QCOMPARE(qvariant_cast<ShortcutsOutcome>(reply[1]), ShortcutsOutcome::Failed);
        reply = write(backend, snapshot, {{open, snapshot.find(open)->defaults}});
        QCOMPARE(qvariant_cast<ShortcutsOutcome>(reply[1]), ShortcutsOutcome::Success);
        snapshot = read(backend); QCOMPARE(snapshot.find(open)->current, snapshot.find(open)->defaults);
        QVERIFY(data(o.config).contains("Open[$d]"));
        QCOMPARE(data(system), QByteArray("[Shortcuts]\nOpen=Meta+F24\nCopy[$i]=Meta+F23\n"));
    }
    void crossCategoryConflictAndExplicitRelease()
    {
        QTemporaryDir dir; const auto o = options(dir); KdeStandardShortcutsBackend backend(o);
        auto snapshot = read(backend);
        const auto open = identity(snapshot, QStringLiteral("Open")), copy = identity(snapshot, QStringLiteral("Copy"));
        const auto key = snapshot.find(copy)->current.first();
        auto reply = write(backend, snapshot, {{open, {key}}});
        QCOMPARE(qvariant_cast<ShortcutsOutcome>(reply[1]), ShortcutsOutcome::Failed); QVERIFY(!QFileInfo::exists(o.config));
        reply = write(backend, snapshot, {{open, {key}}, {copy, {}}});
        QCOMPARE(qvariant_cast<ShortcutsOutcome>(reply[1]), ShortcutsOutcome::Success);
        snapshot = read(backend); QCOMPARE(snapshot.find(open)->current, QList<QKeySequence>{key}); QVERIFY(snapshot.find(copy)->current.isEmpty());
    }
    void staleBeforeCommitPreservesExternalFile()
    {
        QTemporaryDir dir; auto o = options(dir); bool mutate = false;
        o.beforeCommit = [&] { if (mutate) save(o.config, "[General]\nChanged=external\n"); };
        KdeStandardShortcutsBackend backend(o); const auto initial = read(backend); mutate = true;
        const auto reply = write(backend, initial, {{identity(initial, QStringLiteral("Open")), unusual()}});
        QCOMPARE(qvariant_cast<ShortcutsOutcome>(reply[1]), ShortcutsOutcome::Failed);
        QCOMPARE(data(o.config), QByteArray("[General]\nChanged=external\n"));
    }
    void verificationDoesNotOverwriteExternalEdit()
    {
        QTemporaryDir dir; auto o = options(dir);
        o.afterCommit = [&] { save(o.config, "[General]\nChanged=external-after-write\n"); };
        KdeStandardShortcutsBackend backend(o); const auto initial = read(backend);
        const auto reply = write(backend, initial, {{identity(initial, QStringLiteral("Open")), unusual()}});
        QCOMPARE(qvariant_cast<ShortcutsOutcome>(reply[1]), ShortcutsOutcome::Partial);
        QCOMPARE(data(o.config), QByteArray("[General]\nChanged=external-after-write\n"));
    }
    void policyVerificationRollsBackOnlyOwnFile()
    {
        QTemporaryDir dir; auto o = options(dir); const auto system = dir.filePath(QStringLiteral("system"));
        o.systemConfigs = {system}; save(system, "[General]\nPolicy=before\n");
        save(o.config, "[General]\nOriginal=retained\n"); const auto original = data(o.config);
        o.afterCommit = [&] { save(system, "[General]\nPolicy=after\n"); };
        KdeStandardShortcutsBackend backend(o); const auto initial = read(backend);
        const auto reply = write(backend, initial, {{identity(initial, QStringLiteral("Open")), unusual()}});
        QCOMPARE(qvariant_cast<ShortcutsOutcome>(reply[1]), ShortcutsOutcome::Failed); QCOMPARE(data(o.config), original);
        QCOMPARE(data(system), QByteArray("[General]\nPolicy=after\n"));
    }
    void notificationFailureIsSavedPartial()
    {
        QTemporaryDir dir; auto o = options(dir);
        o.notify = [](const QByteArrayList &, QString &) { return false; };
        KdeStandardShortcutsBackend backend(o); const auto initial = read(backend); const auto id = identity(initial, QStringLiteral("Open"));
        const auto reply = write(backend, initial, {{id, unusual()}});
        QCOMPARE(qvariant_cast<ShortcutsOutcome>(reply[1]), ShortcutsOutcome::Partial);
        QCOMPARE(read(backend).find(id)->current, unusual()); QVERIFY(reply[3].toString().contains(QStringLiteral("saved")));
    }
    void terminalDeliveryStopsOldJobProgressBeforeNestedEvents() {
        QTemporaryDir dir; auto o=options(dir); o.softDeadlineMs=100;
        KdeStandardShortcutsBackend backend(o); const auto initial=read(backend);
        QSignalSpy pending(&backend,&ShortcutsBackend::operationPending),done(&backend,&ShortcutsBackend::writeFinished);
        bool stable=false;
        connect(&backend,&ShortcutsBackend::writeFinished,this,[&] {
            const int before=pending.size(); QTest::qWait(150); stable=pending.size()==before;
        });
        backend.writeChanges(69,initial,{{identity(initial,QStringLiteral("Open")),unusual()}});
        QTRY_COMPARE(done.size(),1); QVERIFY(stable);
    }
    void sameByteForeignReplacementDuringRetentionNeverBecomesOwnedRollback() {
        QTemporaryDir dir; auto o=options(dir); const auto system=dir.filePath(QStringLiteral("policy"));
        save(system,"[General]\nPolicy=before\n"); o.systemConfigs={system};
        save(o.config,"[General]\nOriginal=retained\n");
        struct Gate { QSemaphore entered,release; }; const auto gate=std::make_shared<Gate>();
        const auto release=qScopeGuard([gate] { gate->release.release(); });
        o.afterReplaceBeforeRetention=[gate] { gate->entered.release(); gate->release.acquire(); };
        int notifications=0; o.notify=[&notifications](const QByteArrayList &,QString &) { ++notifications; return true; };
        KdeStandardShortcutsBackend backend(o); const auto initial=read(backend);
        QSignalSpy done(&backend,&ShortcutsBackend::writeFinished);
        backend.writeChanges(70,initial,{{identity(initial,QStringLiteral("Open")),unusual()}});
        QTRY_VERIFY(gate->entered.available()>0);
        const auto written=data(o.config); const auto nativeAlias=dir.filePath(QStringLiteral("native-written-inode"));
        QVERIFY(QFile::rename(o.config,nativeAlias)); save(o.config,written);
        struct stat foreign{}; QCOMPARE(::stat(QFile::encodeName(o.config).constData(),&foreign),0);
        // With a receipt sampled after the bridge this policy change would
        // trigger rollback over the same-byte foreign replacement.
        save(system,"[General]\nPolicy=changed-externally\n"); gate->release.release();
        QTRY_COMPARE(done.size(),1); QCOMPARE(qvariant_cast<ShortcutsOutcome>(done[0][1]),ShortcutsOutcome::Uncertain);
        struct stat after{}; QCOMPARE(::stat(QFile::encodeName(o.config).constData(),&after),0);
        QCOMPARE(after.st_dev,foreign.st_dev); QCOMPARE(after.st_ino,foreign.st_ino);
        QCOMPARE(data(o.config),written); QCOMPARE(data(nativeAlias),written); QCOMPARE(notifications,0);
        QTRY_VERIFY(!OperationManager::instance()->hasActiveJobs());
    }
    void immutableGroupAndUnsafePath()
    {
        QTemporaryDir dir; auto o = options(dir); save(o.config, "[Shortcuts][$i]\nOpen=Ctrl+O\n");
        KdeStandardShortcutsBackend backend(o); auto snapshot = read(backend); QVERIFY(snapshot.available); QVERIFY(!snapshot.writable);
        QFile::remove(o.config); const auto target = dir.filePath(QStringLiteral("target")); save(target, "[Shortcuts]\nOpen=Meta+F24\n");
        QVERIFY(QFile::link(target, o.config)); snapshot = read(backend); QVERIFY(snapshot.available); QVERIFY(!snapshot.writable);
        const auto original = data(target);
        auto reply = write(backend, snapshot, {{identity(snapshot, QStringLiteral("Open")), unusual()}});
        QCOMPARE(qvariant_cast<ShortcutsOutcome>(reply[1]), ShortcutsOutcome::Failed); QCOMPARE(data(target), original);
    }
    void hardlinkedConfigurationIsReadOnly()
    {
        QTemporaryDir dir; const auto o = options(dir); const auto target = dir.filePath(QStringLiteral("linked-original"));
        save(target, "[Shortcuts]\nOpen=Meta+F24\n");
        QCOMPARE(::link(QFile::encodeName(target).constData(), QFile::encodeName(o.config).constData()), 0);
        KdeStandardShortcutsBackend backend(o); const auto snapshot = read(backend);
        QVERIFY(snapshot.available); QVERIFY(!snapshot.writable);
        const auto original = data(target);
        const auto reply = write(backend, snapshot, {{identity(snapshot, QStringLiteral("Open")), unusual()}});
        QCOMPARE(qvariant_cast<ShortcutsOutcome>(reply[1]), ShortcutsOutcome::Failed); QCOMPARE(data(target), original);
    }
    void readRunsOffEventLoopAndSurvivesBackendDestruction()
    {
        QTemporaryDir dir; auto o = options(dir); auto entered = std::make_shared<QSemaphore>(); auto release = std::make_shared<QSemaphore>();
        o.beforeRead = [entered, release] { entered->release(); release->tryAcquire(1, 3000); };
        auto *backend = new KdeStandardShortcutsBackend(o); backend->readCurrent(1);
        QTRY_VERIFY_WITH_TIMEOUT(entered->available() > 0, 1000);
        bool event = false; QTimer::singleShot(0, [&] { event = true; }); QTRY_VERIFY(event);
        delete backend; release->release();
        QTRY_VERIFY(!OperationManager::instance()->hasActiveJobs());
    }
    void orphanWriteKeepsAliasesAndGuiNotificationThroughTerminal_data() {
        QTest::addColumn<bool>("rollback"); QTest::newRow("saved-notified")<<false; QTest::newRow("owned-rollback")<<true;
    }
    void orphanWriteKeepsAliasesAndGuiNotificationThroughTerminal() {
        QFETCH(bool,rollback); QTemporaryDir dir; auto o=options(dir); o.softDeadlineMs=10;
        const auto system=dir.filePath(QStringLiteral("policy")); save(system,"[General]\nPolicy=before\n"); o.systemConfigs={system};
        save(o.config,"[General]\nOriginal=retained\n"); const auto original=data(o.config);
        struct Gate { QSemaphore entered,release; }; const auto gate=std::make_shared<Gate>();
        const auto release=qScopeGuard([gate] { gate->release.release(); });
        auto notifications=std::make_shared<std::atomic_int>(0); auto gui=std::make_shared<std::atomic_bool>(true);
        o.notify=[notifications,gui](const QByteArrayList &,QString &) { ++*notifications; *gui=QThread::currentThread()==QCoreApplication::instance()->thread(); return true; };
        o.afterCommit=[gate] { gate->entered.release(); gate->release.acquire(); };
        auto *backend=new KdeStandardShortcutsBackend(o); const auto initial=read(*backend);
        QSignalSpy done(backend,&ShortcutsBackend::writeFinished),pending(backend,&ShortcutsBackend::operationPending);
        int beats=0; QTimer heartbeat; heartbeat.setInterval(1); connect(&heartbeat,&QTimer::timeout,this,[&] { ++beats; }); heartbeat.start();
        backend->writeChanges(20,initial,{{identity(initial,QStringLiteral("Open")),unusual()}});
        QTRY_VERIFY(gate->entered.available()>0); QTRY_VERIFY(!pending.isEmpty()); QTRY_VERIFY(beats>=3); QVERIFY(done.isEmpty());
        delete backend; QVERIFY(OperationManager::instance()->hasActiveJobs());
        const auto alias=dir.filePath(QStringLiteral("replacement-alias"));
        QCOMPARE(::link(QFile::encodeName(o.config).constData(),QFile::encodeName(alias).constData()),0);
        auto other=o; other.config=alias; other.afterCommit={}; other.notify={};
        KdeStandardShortcutsBackend contender(other); const auto current=read(contender);
        QSignalSpy refused(&contender,&ShortcutsBackend::writeFinished);
        contender.writeChanges(21,current,{{identity(current,QStringLiteral("Open")),{}}});
        QTRY_COMPARE(refused.size(),1); QCOMPARE(qvariant_cast<ShortcutsOutcome>(refused[0][1]),ShortcutsOutcome::Failed);
        QVERIFY(refused[0][3].toString().contains("tracked")); QVERIFY(QFile::remove(alias));
        if (rollback) save(system,"[General]\nPolicy=changed-externally\n");
        gate->release.release(); QTRY_VERIFY(!OperationManager::instance()->hasActiveJobs());
        QCOMPARE(done.size(),0); QCOMPARE(notifications->load(),rollback?0:1); QVERIFY(gui->load());
        if (rollback) { QCOMPARE(data(o.config),original); QVERIFY(data(system).contains("changed-externally")); }
        else QVERIFY(data(o.config).contains("Open="));
    }
    void defaultTerminalAndStandardShortcutsShareKdeglobalsReservation() {
        QTemporaryDir dir; auto o=options(dir); save(o.config,"[General]\nTerminalService=old.desktop\nTerminalApplication=old-command\n");
        DefaultAppsPaths paths; paths.isolated=true; paths.configHome=dir.path(); paths.dataDirs={dir.filePath(QStringLiteral("data"))};
        QVERIFY(QDir().mkpath(paths.dataDirs.first()+QStringLiteral("/applications")));
        save(paths.dataDirs.first()+QStringLiteral("/applications/new.desktop"),"[Desktop Entry]\nType=Application\nName=Terminal\nExec=never-run\nCategories=TerminalEmulator;\nTerminal=true\n");
        KdeDefaultAppsBackend apps(paths); QSignalSpy inventory(&apps,&DefaultAppsBackend::readFinished); apps.readCurrent(1); QTRY_COMPARE(inventory.size(),1);
        const auto appsBefore=qvariant_cast<DefaultAppsSnapshot>(inventory[0][2]); QVERIFY(appsBefore.writable);
        struct Gate { QSemaphore entered,release; }; const auto gate=std::make_shared<Gate>(); const auto release=qScopeGuard([gate] { gate->release.release(); });
        o.beforeCommit=[gate] { gate->entered.release(); gate->release.acquire(); };
        KdeStandardShortcutsBackend standard(o); const auto initial=read(standard); QSignalSpy saved(&standard,&ShortcutsBackend::writeFinished);
        standard.writeChanges(2,initial,{{identity(initial,QStringLiteral("Open")),unusual()}}); QTRY_VERIFY(gate->entered.available()>0);
        auto requested=appsBefore.preferences; requested.terminalService=QStringLiteral("new.desktop");
        QSignalSpy denied(&apps,&DefaultAppsBackend::writeFinished); apps.writeChanges(3,appsBefore,requested); QTRY_COMPARE(denied.size(),1);
        QCOMPARE(qvariant_cast<DefaultAppsOutcome>(denied[0][1]),DefaultAppsOutcome::Failed); QVERIFY(denied[0][3].toString().contains("tracked"));
        gate->release.release(); QTRY_COMPARE(saved.size(),1); QCOMPARE(qvariant_cast<ShortcutsOutcome>(saved[0][1]),ShortcutsOutcome::Success);
        QVERIFY(data(o.config).contains("TerminalService=old.desktop")); QTRY_VERIFY(!OperationManager::instance()->hasActiveJobs());
    }
    void cancelDropsUnsentWriteWithoutCancelingRunningTransaction() {
        QTemporaryDir dir; auto o=options(dir); struct Gate { QSemaphore entered,release; }; const auto gate=std::make_shared<Gate>();
        const auto release=qScopeGuard([gate] { gate->release.release(); });
        o.beforeCommit=[gate] { gate->entered.release(); gate->release.acquire(); };
        KdeStandardShortcutsBackend backend(o); const auto initial=read(backend); const auto id=identity(initial,QStringLiteral("Open"));
        QSignalSpy done(&backend,&ShortcutsBackend::writeFinished),pending(&backend,&ShortcutsBackend::operationPending);
        backend.writeChanges(30,initial,{{id,unusual()}}); QTRY_VERIFY(gate->entered.available()>0);
        backend.writeChanges(31,initial,{{id,{}}}); backend.cancel();
        QCOMPARE(done.size(),1); QCOMPARE(done[0][0].toULongLong(),31u); QVERIFY(done[0][3].toString().contains("before it started"));
        QVERIFY(!pending.isEmpty()); QVERIFY(OperationManager::instance()->hasActiveJobs());
        gate->release.release(); QTRY_COMPARE(done.size(),2); QCOMPARE(done[1][0].toULongLong(),30u);
        QCOMPARE(qvariant_cast<ShortcutsOutcome>(done[1][1]),ShortcutsOutcome::Success); QTRY_VERIFY(!OperationManager::instance()->hasActiveJobs());
    }
};
QTEST_MAIN(StandardShortcutsBackendTest)
#include "standard_shortcuts_backend_test.moc"
