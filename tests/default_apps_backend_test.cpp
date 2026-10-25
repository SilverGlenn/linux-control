#include <QtTest>
#include <KConfig>
#include <KConfigGroup>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QSemaphore>
#include <QThreadPool>
#include <QTimer>
#include <QElapsedTimer>
#include <QAtomicInteger>
#include <memory>
#include "platform/apps/DefaultAppsBackend.h"
#include "operations/OperationManager.h"
#include <QScopeGuard>
#include <unistd.h>

namespace {
void put(const QString &path, const QByteArray &data) { QFile f(path); QVERIFY(f.open(QIODevice::WriteOnly)); QCOMPARE(f.write(data), data.size()); }
QByteArray get(const QString &path) { QFile f(path); if (!f.open(QIODevice::ReadOnly)) return {}; return f.readAll(); }
class Fixture {
public:
    QTemporaryDir temp;
    DefaultAppsPaths paths;
    Fixture() {
        paths.configHome = temp.filePath("config"); paths.dataDirs = {temp.filePath("data")}; paths.isolated = true;
        QDir().mkpath(paths.configHome); QDir().mkpath(paths.dataDirs[0] + "/applications");
        app("old.desktop", "Old app", "text/plain;x-scheme-handler/http;x-scheme-handler/https;x-scheme-handler/mailto;", "TextEditor;Email;");
        app("new.desktop", "New app", "text/plain;x-scheme-handler/http;x-scheme-handler/https;x-scheme-handler/mailto;", "TextEditor;Email;");
        app("terminal.desktop", "Terminal", "", "TerminalEmulator;", true);
        app("org.kde.kmail2.desktop", "KMail", "x-scheme-handler/mailto;", "Email;");
        put(config("mimeapps.list"), "[Default Applications]\ntext/plain=old.desktop;\nx-scheme-handler/http=old.desktop;\nx-scheme-handler/https=old.desktop;\nx-scheme-handler/mailto=old.desktop;\n[Added Associations]\ntext/plain=old.desktop;kept.desktop;\n[Removed Associations]\ntext/plain=new.desktop;other.desktop;\n[Unrelated]\nPreserve=unchanged\n");
        put(config("kdeglobals"), "[General]\nTerminalService=old-terminal.desktop\nTerminalApplication=old-command\nColorScheme=Aero\n");
        put(config("emaildefaults"), "[Defaults]\nProfile=Work\n[PROFILE_Work]\nEmailClient=old.desktop\nTerminalClient=false\nFullName=Private Name\n[PROFILE_Other]\nEmailClient=keep.desktop\n");
        put(config("filetypesrc"), "[EmbedSettings]\nembed-text/plain=true\nembed-application/pdf=true\n[Unrelated]\nValue=keep\n");
    }
    QString config(const QString &name) const { return paths.configHome + '/' + name; }
    QString app(const QString &id, const QString &name, const QByteArray &mimes, const QByteArray &categories, bool terminal = false) {
        const auto path = paths.dataDirs[0] + "/applications/" + id;
        put(path, "[Desktop Entry]\nType=Application\nName=" + name.toUtf8() + "\nExec=never-launch-this %U\nMimeType=" + mimes + "\nCategories=" + categories + "\nTerminal=" + (terminal ? "true" : "false") + "\n");
        return path;
    }
    DefaultAppsSnapshot read(KdeDefaultAppsBackend &backend) {
        QSignalSpy spy(&backend, &DefaultAppsBackend::readFinished); backend.readCurrent(1);
        if (!spy.wait(10000)) return {};
        if (!spy[0][1].toBool()) qWarning() << spy[0][3].toString();
        return qvariant_cast<DefaultAppsSnapshot>(spy[0][2]);
    }
};
}
class DefaultAppsBackendTest final : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { qRegisterMetaType<DefaultAppsSnapshot>(); qRegisterMetaType<DefaultAppsOutcome>(); }
    void readIsIsolatedAndDoesNotCreateMissingFiles() {
        Fixture f; QFile::remove(f.config("emaildefaults")); QFile::remove(f.config("filetypesrc"));
        const auto original = get(f.config("mimeapps.list")); KdeDefaultAppsBackend backend(f.paths); const auto state = f.read(backend);
        QVERIFY(!state.revision.isEmpty()); QVERIFY2(state.writable, qPrintable(state.reason)); QCOMPARE(state.applications.size(), 4);
        QCOMPARE(state.preferences.mimeDefaults.value("text/plain"), "old.desktop");
        QCOMPARE(get(f.config("mimeapps.list")), original); QVERIFY(!QFileInfo::exists(f.config("emaildefaults"))); QVERIFY(!QFileInfo::exists(f.config("filetypesrc")));
        QVERIFY(!QFileInfo::exists(f.temp.filePath("data/ksycoca6")));
    }
    void ordinaryGroupWritableUserSettingsRemainEditable() {
        Fixture f;
        const auto directoryMode = QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner
            | QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ExeGroup | QFileDevice::ReadOther | QFileDevice::ExeOther;
        const auto fileMode = QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ReadOther;
        QVERIFY(QFile::setPermissions(f.paths.configHome, directoryMode));
        QVERIFY(QFile::setPermissions(f.config("mimeapps.list"), fileMode));
        KdeDefaultAppsBackend backend(f.paths); const auto before = f.read(backend);
        QVERIFY2(before.writable, qPrintable(before.reason)); QVERIFY(before.canChangeMime("text/plain"));
        auto requested = before.preferences; requested.mimeDefaults["text/plain"] = "new.desktop";
        QSignalSpy spy(&backend, &DefaultAppsBackend::writeFinished); backend.writeChanges(24, before, requested); QVERIFY(spy.wait(10000));
        QVERIFY2(qvariant_cast<DefaultAppsOutcome>(spy[0][1]) == DefaultAppsOutcome::Success, qPrintable(spy[0][3].toString()));
        QVERIFY(QFileInfo(f.config("mimeapps.list")).permissions().testFlag(QFileDevice::WriteGroup));
    }
    void worldWritableTargetsRemainProtected() {
        Fixture f;
        QVERIFY(QFile::setPermissions(f.config("mimeapps.list"), QFileInfo(f.config("mimeapps.list")).permissions() | QFileDevice::WriteOther));
        KdeDefaultAppsBackend backend(f.paths); auto before = f.read(backend); QVERIFY(!before.canChangeMime("text/plain"));
        const auto original = get(f.config("mimeapps.list")); auto requested = before.preferences; requested.mimeDefaults["text/plain"] = "new.desktop";
        QSignalSpy spy(&backend, &DefaultAppsBackend::writeFinished); backend.writeChanges(25, before, requested); QVERIFY(spy.wait(10000));
        QCOMPARE(qvariant_cast<DefaultAppsOutcome>(spy[0][1]), DefaultAppsOutcome::Failed); QCOMPARE(get(f.config("mimeapps.list")), original);
        QVERIFY(QFile::setPermissions(f.paths.configHome, QFileInfo(f.paths.configHome).permissions() | QFileDevice::WriteOther));
        before = f.read(backend); QVERIFY(!before.writable);
    }
    void appliesMultiFilePlanAndPreservesUnrelatedValues() {
        Fixture f; KdeDefaultAppsBackend backend(f.paths); const auto before = f.read(backend); auto requested = before.preferences;
        requested.mimeDefaults["text/plain"] = "new.desktop"; requested.mimeDefaults["x-scheme-handler/mailto"] = "new.desktop";
        requested.terminalService = "terminal.desktop";
        QSignalSpy spy(&backend, &DefaultAppsBackend::writeFinished); backend.writeChanges(2, before, requested); QVERIFY(spy.wait(10000));
        QVERIFY2(qvariant_cast<DefaultAppsOutcome>(spy[0][1]) == DefaultAppsOutcome::Success, qPrintable(spy[0][3].toString()));
        const auto saved = qvariant_cast<DefaultAppsSnapshot>(spy[0][2]); QVERIFY(saved.preferences == requested);
        KConfig mime(f.config("mimeapps.list"), KConfig::SimpleConfig);
        QCOMPARE(mime.group("Added Associations").readXdgListEntry("text/plain"), QStringList({"new.desktop", "old.desktop", "kept.desktop"}));
        QCOMPARE(mime.group("Removed Associations").readXdgListEntry("text/plain"), QStringList({"other.desktop"}));
        QCOMPARE(mime.group("Unrelated").readEntry("Preserve", QString()), "unchanged");
        KConfig general(f.config("kdeglobals"), KConfig::SimpleConfig);
        QCOMPARE(general.group("General").readEntry("TerminalApplication", QString()), "never-launch-this %U");
        QCOMPARE(general.group("General").readEntry("ColorScheme", QString()), "Aero");
        KConfig email(f.config("emaildefaults"), KConfig::SimpleConfig);
        QCOMPARE(email.group("Defaults").readEntry("Profile", QString()), "Work");
        QCOMPARE(email.group("PROFILE_Work").readEntry("EmailClient", QString()), "new.desktop");
        QCOMPARE(email.group("PROFILE_Work").readEntry("FullName", QString()), "Private Name");
        QCOMPARE(email.group("PROFILE_Other").readEntry("EmailClient", QString()), "keep.desktop");
        KConfig embed(f.config("filetypesrc"), KConfig::SimpleConfig);
        QVERIFY(!embed.group("EmbedSettings").readEntry("embed-text/plain", true));
        QVERIFY(embed.group("EmbedSettings").readEntry("embed-application/pdf", false));
        QCOMPARE(embed.group("Unrelated").readEntry("Value", QString()), "keep");
    }
    void desktopSpecificDefaultIsUpdatedWithoutTouchingOtherTypes() {
        Fixture f; put(f.config("kde-mimeapps.list"), "[Default Applications]\ntext/plain=old.desktop;\napplication/pdf=kept.desktop;\n");
        KdeDefaultAppsBackend backend(f.paths); const auto before = f.read(backend); auto requested = before.preferences; requested.mimeDefaults["text/plain"] = "new.desktop";
        QSignalSpy spy(&backend, &DefaultAppsBackend::writeFinished); backend.writeChanges(3, before, requested); QVERIFY(spy.wait(10000));
        QVERIFY2(qvariant_cast<DefaultAppsOutcome>(spy[0][1]) == DefaultAppsOutcome::Success, qPrintable(spy[0][3].toString()));
        KConfig config(f.config("kde-mimeapps.list"), KConfig::SimpleConfig);
        QCOMPARE(config.group("Default Applications").readXdgListEntry("text/plain"), QStringList{"new.desktop"});
        QCOMPARE(config.group("Default Applications").readXdgListEntry("application/pdf"), QStringList{"kept.desktop"});
    }
    void externalEditAndDesktopIdentityChangePreventAllWrites() {
        for (bool changeDesktop : {false, true}) {
            Fixture f; KdeDefaultAppsBackend backend(f.paths); auto before = f.read(backend); auto requested = before.preferences; requested.mimeDefaults["text/plain"] = "new.desktop";
            const auto oldGeneral = get(f.config("kdeglobals"));
            const QString target = changeDesktop ? f.paths.dataDirs[0] + "/applications/new.desktop" : f.config("mimeapps.list");
            const auto edited = get(target) + "\n# external edit\n"; put(target, edited);
            QSignalSpy spy(&backend, &DefaultAppsBackend::writeFinished); backend.writeChanges(4, before, requested); QVERIFY(spy.wait(10000));
            QCOMPARE(qvariant_cast<DefaultAppsOutcome>(spy[0][1]), DefaultAppsOutcome::Failed);
            QCOMPARE(get(target), edited); QCOMPARE(get(f.config("kdeglobals")), oldGeneral);
        }
    }
    void invalidSelectionAndUnsupportedMimeNeverWrite() {
        Fixture f; KdeDefaultAppsBackend backend(f.paths); const auto before = f.read(backend); const auto original = get(f.config("mimeapps.list"));
        for (const auto &entry : QList<QPair<QString, QString>>{{"text/plain", "missing.desktop"}, {"image/png", "new.desktop"}, {"bad\nkey", "new.desktop"}}) {
            auto requested = before.preferences; requested.mimeDefaults[entry.first] = entry.second;
            QSignalSpy spy(&backend, &DefaultAppsBackend::writeFinished); backend.writeChanges(5, before, requested); QVERIFY(spy.wait(10000));
            QCOMPARE(qvariant_cast<DefaultAppsOutcome>(spy[0][1]), DefaultAppsOutcome::Failed); QCOMPARE(get(f.config("mimeapps.list")), original);
        }
    }
    void configurationPolicyPreventsTransaction() {
        Fixture f; put(f.config("filetypesrc"), "[EmbedSettings]\nembed-text/plain[$i]=true\n");
        KdeDefaultAppsBackend backend(f.paths); const auto before = f.read(backend); const auto original = get(f.config("mimeapps.list")); auto requested = before.preferences;
        requested.mimeDefaults["text/plain"] = "new.desktop";
        QSignalSpy spy(&backend, &DefaultAppsBackend::writeFinished); backend.writeChanges(6, before, requested); QVERIFY(spy.wait(10000));
        QCOMPARE(qvariant_cast<DefaultAppsOutcome>(spy[0][1]), DefaultAppsOutcome::Failed); QCOMPARE(get(f.config("mimeapps.list")), original);
    }
    void symlinkAndProtectedDirectoryAreNotWritable() {
        Fixture f; const auto target = f.temp.filePath("victim"); put(target, get(f.config("mimeapps.list")));
        QVERIFY(QFile::remove(f.config("mimeapps.list"))); QVERIFY(QFile::link(target, f.config("mimeapps.list")));
        KdeDefaultAppsBackend backend(f.paths); const auto before = f.read(backend); QVERIFY(!before.canChangeMime("text/plain"));
        const auto original = get(target); auto requested = before.preferences; requested.mimeDefaults["text/plain"] = "new.desktop";
        QSignalSpy spy(&backend, &DefaultAppsBackend::writeFinished); backend.writeChanges(7, before, requested); QVERIFY(spy.wait(10000));
        QCOMPARE(qvariant_cast<DefaultAppsOutcome>(spy[0][1]), DefaultAppsOutcome::Failed); QCOMPARE(get(target), original);
    }
    void kmailClearsLegacyClientAndTerminalFlag() {
        Fixture f; KdeDefaultAppsBackend backend(f.paths); const auto before = f.read(backend); auto requested = before.preferences;
        requested.mimeDefaults["x-scheme-handler/mailto"] = "org.kde.kmail2.desktop";
        QSignalSpy spy(&backend, &DefaultAppsBackend::writeFinished); backend.writeChanges(8, before, requested); QVERIFY(spy.wait(10000));
        QVERIFY2(qvariant_cast<DefaultAppsOutcome>(spy[0][1]) == DefaultAppsOutcome::Success, qPrintable(spy[0][3].toString()));
        KConfig config(f.config("emaildefaults"), KConfig::SimpleConfig); QVERIFY(config.group("PROFILE_Work").readEntry("EmailClient", QString()).isEmpty());
        QVERIFY(!config.group("PROFILE_Work").readEntry("TerminalClient", true));
    }
    void missingDefaultIdsDoNotConsumeKdePreferenceSlots() {
        Fixture f; QByteArray missing;
        for (int i = 0; i < 30; ++i) missing += "missing-" + QByteArray::number(i) + ".desktop;";
        put(f.config("mimeapps.list"), "[Added Associations]\ntext/plain=old.desktop;\n[Default Applications]\ntext/plain=" + missing + "new.desktop;\n");
        KdeDefaultAppsBackend backend(f.paths); const auto state = f.read(backend);
        QCOMPARE(state.preferences.mimeDefaults.value("text/plain"), "new.desktop");
    }
    void heldScanKeepsEventLoopResponsiveAndCoalescesQueuedReads() {
        Fixture f;
        struct Gate { QSemaphore entered, release; QAtomicInteger<int> scans{0}; };
        auto gate = std::make_shared<Gate>();
        f.paths.beforeScan = [gate] {
            if (gate->scans.fetchAndAddRelaxed(1) == 0) { gate->entered.release(); gate->release.tryAcquire(1, 10000); }
        };
        KdeDefaultAppsBackend backend(f.paths); QSignalSpy spy(&backend, &DefaultAppsBackend::readFinished);
        int heartbeats = 0; QTimer heartbeat; heartbeat.setInterval(5); connect(&heartbeat, &QTimer::timeout, this, [&] { ++heartbeats; }); heartbeat.start();
        backend.readCurrent(10); QTRY_VERIFY(gate->entered.available() > 0);
        backend.readCurrent(11); backend.readCurrent(12); QTest::qWait(60);
        const bool responsive = heartbeats >= 4 && spy.isEmpty(); gate->release.release();
        QVERIFY(responsive); QTRY_COMPARE(spy.size(), 3); QCOMPARE(gate->scans.loadRelaxed(), 2);
        QCOMPARE(spy[0][0].toULongLong(), 10u); QCOMPARE(spy[1][0].toULongLong(), 11u); QCOMPARE(spy[2][0].toULongLong(), 12u);
    }
    void deletingBackendDuringHeldReadNeverBlocksOrCapturesItsOwner() {
        Fixture f;
        struct Gate { QSemaphore entered, release; };
        auto gate = std::make_shared<Gate>();
        f.paths.beforeScan = [gate] { gate->entered.release(); gate->release.tryAcquire(1, 10000); };
        auto *backend = new KdeDefaultAppsBackend(f.paths); backend->readCurrent(20); QTRY_VERIFY(gate->entered.available() > 0);
        QElapsedTimer timer; timer.start(); delete backend; const qint64 elapsed = timer.elapsed(); gate->release.release();
        QVERIFY(elapsed < 150); QVERIFY(QThreadPool::globalInstance()->waitForDone(10000));
        QTRY_VERIFY(!OperationManager::instance()->hasActiveJobs());
    }
    void orphanMultiFileWriteRetainsAliasesThroughOwnedRollback_data() {
        QTest::addColumn<bool>("failLater"); QTest::newRow("success")<<false; QTest::newRow("owned-rollback")<<true;
    }
    void orphanMultiFileWriteRetainsAliasesThroughOwnedRollback() {
        QFETCH(bool,failLater); Fixture f; const auto original=get(f.config("filetypesrc"));
        struct Gate { QSemaphore entered,release; }; const auto gate=std::make_shared<Gate>();
        const auto release=qScopeGuard([gate] { gate->release.release(); });
        f.paths.softDeadlineMs=10;
        f.paths.beforeCommit=[gate,failLater](int index,const QString &path) {
            if (index!=1) return;
            gate->entered.release(); gate->release.acquire();
            if (failLater) { QFile file(path); if (file.open(QIODevice::Append)) file.write("\n# external later-file edit\n"); }
        };
        auto *backend=new KdeDefaultAppsBackend(f.paths); const auto before=f.read(*backend);
        auto requested=before.preferences; requested.mimeDefaults["text/plain"]="new.desktop";
        QSignalSpy done(backend,&DefaultAppsBackend::writeFinished),pending(backend,&DefaultAppsBackend::operationPending);
        backend->writeChanges(30,before,requested); QTRY_VERIFY(gate->entered.available()>0); QTRY_VERIFY(!pending.isEmpty());
        QVERIFY(done.isEmpty()); QVERIFY(OperationManager::instance()->hasActiveJobs());
        delete backend; QVERIFY(OperationManager::instance()->hasActiveJobs());
        // The first atomic replacement is already complete. Its replacement
        // inode must stay reserved while the second file and possible rollback
        // are pending, even after facade deletion.
        const auto alias=f.config("intermediate-alias");
        QCOMPARE(::link(QFile::encodeName(f.config("filetypesrc")).constData(),QFile::encodeName(alias).constData()),0);
        DefaultAppsPaths alternate=f.paths; alternate.beforeCommit={}; alternate.configHome=f.config("./");
        KdeDefaultAppsBackend contender(alternate); const auto fresh=f.read(contender);
        QSignalSpy refused(&contender,&DefaultAppsBackend::writeFinished); contender.writeChanges(31,fresh,requested);
        QTRY_COMPARE(refused.size(),1); QCOMPARE(qvariant_cast<DefaultAppsOutcome>(refused[0][1]),DefaultAppsOutcome::Failed);
        QVERIFY(refused[0][3].toString().contains("tracked"));
        QVERIFY(QFile::remove(alias)); gate->release.release(); QTRY_VERIFY(!OperationManager::instance()->hasActiveJobs());
        QCOMPARE(done.size(),0);
        if (failLater) { QCOMPARE(get(f.config("filetypesrc")),original); QVERIFY(get(f.config("mimeapps.list")).contains("external later-file edit")); }
        else QVERIFY(get(f.config("mimeapps.list")).contains("text/plain=new.desktop"));
    }
    void cancelDropsUnsentQueuedWriteAndKeepsRunningOwner() {
        Fixture f; struct Gate { QSemaphore entered,release; }; const auto gate=std::make_shared<Gate>();
        const auto release=qScopeGuard([gate] { gate->release.release(); });
        f.paths.beforeCommit=[gate](int index,const QString &) { if (!index) { gate->entered.release(); gate->release.acquire(); } };
        KdeDefaultAppsBackend backend(f.paths); const auto before=f.read(backend);
        auto requested=before.preferences; requested.mimeDefaults["text/plain"]="new.desktop";
        QSignalSpy done(&backend,&DefaultAppsBackend::writeFinished),pending(&backend,&DefaultAppsBackend::operationPending);
        backend.writeChanges(40,before,requested); QTRY_VERIFY(gate->entered.available()>0);
        auto queued=before.preferences; queued.terminalService="terminal.desktop"; backend.writeChanges(41,before,queued); backend.cancel();
        QCOMPARE(done.size(),1); QCOMPARE(done[0][0].toULongLong(),41u); QVERIFY(done[0][3].toString().contains("before it started"));
        QVERIFY(!pending.isEmpty()); QVERIFY(OperationManager::instance()->hasActiveJobs());
        gate->release.release(); QTRY_COMPARE(done.size(),2); QCOMPARE(done[1][0].toULongLong(),40u);
        QCOMPARE(qvariant_cast<DefaultAppsOutcome>(done[1][1]),DefaultAppsOutcome::Success);
        QVERIFY(get(f.config("kdeglobals")).contains("TerminalService=old-terminal.desktop"));
        QTRY_VERIFY(!OperationManager::instance()->hasActiveJobs());
    }
    void heldApplyKeepsEventLoopResponsive() {
        Fixture f;
        struct Gate { QSemaphore entered, release; };
        auto gate = std::make_shared<Gate>();
        f.paths.beforeCommit = [gate](int index, const QString &) { if (index == 0) { gate->entered.release(); gate->release.tryAcquire(1, 10000); } };
        KdeDefaultAppsBackend backend(f.paths); const auto before = f.read(backend); auto requested = before.preferences; requested.mimeDefaults["text/plain"] = "new.desktop";
        QSignalSpy spy(&backend, &DefaultAppsBackend::writeFinished);
        int ticks = 0; QTimer heartbeat; heartbeat.setInterval(5); connect(&heartbeat, &QTimer::timeout, this, [&] { ++ticks; }); heartbeat.start();
        backend.writeChanges(21, before, requested); QTRY_VERIFY(gate->entered.available() > 0); QTest::qWait(60);
        const bool responsive = ticks >= 4 && spy.isEmpty(); gate->release.release(); QVERIFY(responsive); QVERIFY(spy.wait(10000));
        QVERIFY2(qvariant_cast<DefaultAppsOutcome>(spy[0][1]) == DefaultAppsOutcome::Success, qPrintable(spy[0][3].toString()));
    }
    void laterWriteFailureRollsBackCompletedFilesWithoutOverwritingExternalEdit() {
        Fixture f; const auto original = get(f.config("filetypesrc"));
        f.paths.beforeCommit = [](int index, const QString &path) {
            if (index == 1) { QFile file(path); if (file.open(QIODevice::Append)) file.write("\n# external change during Apply\n"); }
        };
        KdeDefaultAppsBackend backend(f.paths); const auto before = f.read(backend); auto requested = before.preferences; requested.mimeDefaults["text/plain"] = "new.desktop";
        QSignalSpy spy(&backend, &DefaultAppsBackend::writeFinished); backend.writeChanges(22, before, requested); QVERIFY(spy.wait(10000));
        QCOMPARE(qvariant_cast<DefaultAppsOutcome>(spy[0][1]), DefaultAppsOutcome::Failed);
        QCOMPARE(get(f.config("filetypesrc")), original); QVERIFY(get(f.config("mimeapps.list")).contains("external change during Apply"));
        QVERIFY(spy[0][3].toString().contains("rolled back"));
    }
    void ownedRollbackDoesNotReplaceForeignSameByteInode() {
        Fixture f; f.paths.beforeCommit=[&f](int index,const QString &path) {
            if (index!=1) return;
            const auto first=f.config("filetypesrc"),foreign=f.config("foreign-replacement");
            put(foreign,get(first)); QFile::remove(first); QFile::rename(foreign,first);
            QFile file(path); if (file.open(QIODevice::Append)) file.write("\n# fail the later file\n");
        };
        KdeDefaultAppsBackend backend(f.paths); const auto before=f.read(backend); auto requested=before.preferences;
        requested.mimeDefaults["text/plain"]="new.desktop";
        QSignalSpy done(&backend,&DefaultAppsBackend::writeFinished); backend.writeChanges(50,before,requested); QVERIFY(done.wait(10000));
        QCOMPARE(qvariant_cast<DefaultAppsOutcome>(done[0][1]),DefaultAppsOutcome::Partial);
        QVERIFY(get(f.config("filetypesrc")).contains("embed-text/plain=false"));
        QVERIFY(get(f.config("mimeapps.list")).contains("fail the later file"));
    }
    void aliasOverridesAreCanonicalAndPreserveUnrelatedEntries() {
        Fixture f; put(f.config("mimeapps.list"), "[Default Applications]\naudio/x-flac=old.desktop;\ntext/plain=old.desktop;\n");
        f.app("new.desktop", "New app", "audio/flac;text/plain;", "Player;TextEditor;");
        KdeDefaultAppsBackend backend(f.paths); const auto before = f.read(backend); auto requested = before.preferences;
        requested.mimeDefaults["audio/flac"] = "new.desktop";
        QSignalSpy spy(&backend, &DefaultAppsBackend::writeFinished); backend.writeChanges(23, before, requested); QVERIFY(spy.wait(10000));
        QVERIFY2(qvariant_cast<DefaultAppsOutcome>(spy[0][1]) == DefaultAppsOutcome::Success, qPrintable(spy[0][3].toString()));
        KConfig config(f.config("mimeapps.list"), KConfig::SimpleConfig);
        QVERIFY(!config.group("Default Applications").hasKey("audio/x-flac"));
        QCOMPARE(config.group("Default Applications").readXdgListEntry("audio/flac"), QStringList{"new.desktop"});
        QCOMPARE(config.group("Default Applications").readXdgListEntry("text/plain"), QStringList{"old.desktop"});
    }
    void hiddenDesktopEntryShadowsLowerPrecedenceApplication() {
        Fixture f; const QString lower = f.temp.filePath("system-data"); QDir().mkpath(lower + "/applications");
        put(lower + "/applications/new.desktop", get(f.paths.dataDirs[0] + "/applications/new.desktop")); f.paths.dataDirs << lower;
        put(f.paths.dataDirs[0] + "/applications/new.desktop", "[Desktop Entry]\nType=Application\nHidden=true\n");
        KdeDefaultAppsBackend backend(f.paths); const auto state = f.read(backend); QVERIFY(!state.application("new.desktop"));
    }
};
QTEST_GUILESS_MAIN(DefaultAppsBackendTest)
#include "default_apps_backend_test.moc"
