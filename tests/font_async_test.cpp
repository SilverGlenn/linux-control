#include <QtTest>
#include <KConfig>
#include <QDir>
#include <QFile>
#include <QLockFile>
#include <QPointer>
#include <QSaveFile>
#include <QSemaphore>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <atomic>
#include <memory>
#include <unistd.h>
#include "platform/kde/FontBackend.h"
#include "operations/OperationManager.h"

namespace {
struct FontGate {
    std::atomic_bool armed{true};
    QSemaphore entered, released;
    void waitOnce() { if (armed.exchange(false)) { entered.release(); released.acquire(); } }
};
class AsyncFontReload final : public FontReloadService {
public:
    bool available = true, automatic = true, waiting = false;
    QString generation = QStringLiteral("fixture-font-provider-1");
    std::shared_ptr<std::atomic_int> reloads = std::make_shared<std::atomic_int>(0);
    bool isAvailable() const override { return available; }
    QString unavailableReason() const override { return QStringLiteral("Private font provider unavailable"); }
    QString identity() const override { return generation; }
    void requestReload() override {
        ++*reloads; waiting = true;
        if (automatic) QTimer::singleShot(0, this, [this] { acknowledge(); });
    }
    void acknowledge() { if (waiting) { waiting = false; emit finished(true, {}); } }
};
}

class FontAsyncTest final : public QObject {
    Q_OBJECT
    QTemporaryDir directory;
    QString config, xml;
    QList<QPointer<KdeFontBackend>> backends;
    QList<QPointer<AsyncFontReload>> reloads;
    QList<std::shared_ptr<FontGate>> gates;
    std::shared_ptr<FontGate> gate() { auto result = std::make_shared<FontGate>(); gates << result; return result; }
    KdeFontBackend::FileContext context() { KdeFontBackend::FileContext result; result.fixture = true; result.softDeadlineMs = 25; return result; }
    void save(const QString &path, const QByteArray &bytes) {
        QSaveFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(bytes), bytes.size()); QVERIFY(file.commit());
    }
    QByteArray contents(const QString &path) { QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray(); }
    KdeFontBackend *make(const KdeFontBackend::FileContext &fixture, AsyncFontReload **service = nullptr,
                         const QString &file = {}, const QString &fontFile = {}) {
        auto *reload = new AsyncFontReload;
        auto *backend = new KdeFontBackend(file.isEmpty() ? config : file, fontFile.isEmpty() ? xml : fontFile, reload, fixture);
        backends << backend; reloads << reload; if (service) *service = reload; return backend;
    }
    FontState read(KdeFontBackend *backend) {
        QSignalSpy result(backend, &FontBackend::readFinished); backend->readCurrent(); if (result.isEmpty()) result.wait(10000);
        if (result.size() != 1 || !result[0][0].toBool()) {
            QTest::qFail(qPrintable(result.isEmpty() ? QStringLiteral("No font read response") : result[0][2].toString()), __FILE__, __LINE__); return {};
        }
        return qvariant_cast<FontState>(result[0][1]);
    }
    FontPreferences change(const FontState &state) { auto result = state.preferences; result.fonts[0].setPointSize(13); return result; }
private slots:
    void initTestCase() { qRegisterMetaType<FontState>(); }
    void init() {
        QVERIFY(directory.isValid()); config = directory.filePath("kdeglobals"); xml = directory.filePath("fonts.conf");
        save(config, "[Unrelated]\nKeep=original\n"); save(xml, "<fontconfig><!--preserve--></fontconfig>");
    }
    void cleanup() {
        for (const auto &held : gates) held->released.release(32);
        for (auto reload : reloads) if (reload) { reload->automatic = true; reload->acknowledge(); }
        QTRY_VERIFY_WITH_TIMEOUT(!OperationManager::instance()->hasActiveJobs(), 10000);
        for (auto backend : backends) delete backend;
        backends.clear(); reloads.clear(); gates.clear();
    }
    void readDiscoveryRunsInWorkerWithHeartbeatPendingAndOrphanLifetime() {
        const auto held = gate(); auto fixture = context(); fixture.beforeRead = [held] { held->waitOnce(); };
        AsyncFontReload *reload = nullptr; auto *backend = make(fixture, &reload); QPointer<AsyncFontReload> retained(reload);
        int beats = 0; QTimer heartbeat; heartbeat.setInterval(1); connect(&heartbeat, &QTimer::timeout, this, [&] { ++beats; }); heartbeat.start();
        QSignalSpy done(backend, &FontBackend::readFinished), pending(backend, &FontBackend::operationPending);
        backend->readCurrent(); QTRY_VERIFY(held->entered.available()); QTRY_VERIFY(beats >= 3); QTRY_VERIFY(!pending.isEmpty());
        QCOMPARE(done.size(), 0); delete backend; QVERIFY(retained); QVERIFY(OperationManager::instance()->hasActiveJobs());
        held->released.release(); QTRY_VERIFY(!OperationManager::instance()->hasActiveJobs()); QTRY_VERIFY(!retained); QCOMPARE(done.size(), 0);
    }
    void nativeLockWaitDoesNotBlockGuiOrFinishAtSoftDeadline() {
        auto *backend = make(context()); const auto before = read(backend);
        QLockFile lock(config + QStringLiteral(".lock")); QVERIFY(lock.tryLock());
        int beats = 0; bool unlocked = false; QTimer heartbeat; heartbeat.setInterval(1);
        connect(&heartbeat, &QTimer::timeout, this, [&] { ++beats; }); heartbeat.start();
        QTimer::singleShot(180, this, [&] { unlocked = true; lock.unlock(); });
        QSignalSpy done(backend, &FontBackend::writeFinished), pending(backend, &FontBackend::operationPending);
        backend->writePreferences(before, change(before)); QTRY_VERIFY(!pending.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 10000); QVERIFY(unlocked); QVERIFY(beats >= 3);
        QVERIFY2(done[0][0].toBool(), qPrintable(done[0][1].toString())); QVERIFY(contents(config).contains("Keep=original"));
        QCOMPARE(read(backend).preferences, change(before));
    }
    void orphanSaveRetainsServiceActivityAndCanonicalReservation() {
        const auto held = gate(); auto fixture = context(); fixture.beforeSync = [held] { held->waitOnce(); };
        AsyncFontReload *reload = nullptr; auto *first = make(fixture, &reload); QPointer<AsyncFontReload> retained(reload);
        const auto count = reload->reloads; const auto before = read(first);
        QSignalSpy done(first, &FontBackend::writeFinished); first->writePreferences(before, change(before));
        QTRY_VERIFY(held->entered.available()); delete first; QVERIFY(retained); QVERIFY(OperationManager::instance()->hasActiveJobs());
        auto *other = make(context(), nullptr, directory.filePath("./kdeglobals")); const auto aliasBefore = read(other);
        QSignalSpy refused(other, &FontBackend::writeFinished); other->writePreferences(aliasBefore, change(aliasBefore));
        QTRY_COMPARE(refused.size(), 1); QVERIFY(!refused[0][0].toBool()); QVERIFY(!refused[0][1].toString().isEmpty());
        held->released.release(); QTRY_VERIFY(!OperationManager::instance()->hasActiveJobs()); QTRY_VERIFY(!retained);
        QCOMPARE(done.size(), 0); QCOMPARE(count->load(), 1); QCOMPARE(read(other).preferences, change(before));
    }
    void replacedOldAndNewInodesStayReservedThroughRefresh_data() {
        QTest::addColumn<bool>("oldInode"); QTest::newRow("old") << true; QTest::newRow("new") << false;
    }
    void replacedOldAndNewInodesStayReservedThroughRefresh() {
        QFETCH(bool, oldInode);
        const auto oldAlias = directory.filePath("old-font-inode"), newAlias = directory.filePath("new-font-inode");
        QFile::remove(oldAlias); QFile::remove(newAlias);
        QVERIFY(::link(QFile::encodeName(config).constData(), QFile::encodeName(oldAlias).constData()) == 0);
        AsyncFontReload *reload = nullptr; auto *first = make(context(), &reload); reload->automatic = false;
        const auto before = read(first); QSignalSpy done(first, &FontBackend::writeFinished);
        first->writePreferences(before, change(before)); QTRY_COMPARE(reload->reloads->load(), 1); QCOMPARE(done.size(), 0);
        QVERIFY(::link(QFile::encodeName(config).constData(), QFile::encodeName(newAlias).constData()) == 0);
        auto *other = make(context(), nullptr, oldInode ? oldAlias : newAlias, directory.filePath("alias-fonts.conf"));
        const auto aliasBefore = read(other); QSignalSpy refused(other, &FontBackend::writeFinished);
        other->writePreferences(aliasBefore, change(aliasBefore)); QTRY_COMPARE(refused.size(), 1); QVERIFY(!refused[0][0].toBool());
        reload->acknowledge(); QTRY_COMPARE(done.size(), 1); QVERIFY2(done[0][0].toBool(), qPrintable(done[0][1].toString()));
    }
    void includeDirectoryDriftBeforeMutationIsReceiptConflictNotReservationFailure() {
        const auto includes = directory.filePath("conf.d"); QVERIFY(QDir().mkpath(includes));
        const auto held = gate(); auto fixture = context(); fixture.additionalSources = {includes}; fixture.beforeSync = [held] { held->waitOnce(); };
        AsyncFontReload *reload = nullptr; auto *backend = make(fixture, &reload); const auto before = read(backend), requested = before;
        const auto original = contents(config), originalXml = contents(xml);
        QSignalSpy done(backend, &FontBackend::writeFinished); backend->writePreferences(before, change(requested));
        QTRY_VERIFY(held->entered.available()); save(includes + QStringLiteral("/50-new.conf"), "<fontconfig/>"); held->released.release();
        QTRY_COMPARE(done.size(), 1); QVERIFY(!done[0][0].toBool()); QVERIFY(done[0][1].toString().contains("sources"));
        QCOMPARE(contents(config), original); QCOMPARE(contents(xml), originalXml); QCOMPARE(reload->reloads->load(), 0);
    }
    void intermediateXmlOldAndNewAliasesRemainReservedAfterFacadeDeletion_data() {
        QTest::addColumn<bool>("oldInode"); QTest::newRow("old-xml") << true; QTest::newRow("replacement-xml") << false;
    }
    void intermediateXmlOldAndNewAliasesRemainReservedAfterFacadeDeletion() {
        QFETCH(bool, oldInode);
        const auto oldAlias = directory.filePath("old-xml-inode"), newAlias = directory.filePath("new-xml-inode");
        QFile::remove(oldAlias); QFile::remove(newAlias);
        QVERIFY(::link(QFile::encodeName(xml).constData(), QFile::encodeName(oldAlias).constData()) == 0);
        const auto held = gate(); auto fixture = context(); fixture.afterXml = [held] { held->waitOnce(); };
        AsyncFontReload *reload = nullptr; auto *first = make(fixture, &reload); QPointer<AsyncFontReload> retained(reload);
        const auto before = read(first); const auto configBytes = contents(config);
        auto requested = change(before); requested.antialias = "true";
        QSignalSpy gone(first, &FontBackend::writeFinished); first->writePreferences(before, requested);
        QTRY_VERIFY(held->entered.available()); QCOMPARE(contents(config), configBytes); QVERIFY(contents(xml).contains("antialias"));
        QVERIFY(::link(QFile::encodeName(xml).constData(), QFile::encodeName(newAlias).constData()) == 0);
        delete first; QVERIFY(retained); QVERIFY(OperationManager::instance()->hasActiveJobs());
        const auto otherConfig = directory.filePath("intermediate-competitor-kdeglobals"); QFile::remove(otherConfig);
        auto *other = make(context(), nullptr, otherConfig, oldInode ? oldAlias : newAlias);
        const auto aliasBefore = read(other); QSignalSpy refused(other, &FontBackend::writeFinished);
        other->writePreferences(aliasBefore, change(aliasBefore)); QTRY_COMPARE(refused.size(), 1); QVERIFY(!refused[0][0].toBool());
        QVERIFY(!QFile::exists(otherConfig)); QCOMPARE(contents(config), configBytes);
        held->released.release(); QTRY_VERIFY(!OperationManager::instance()->hasActiveJobs()); QTRY_VERIFY(!retained);
        QCOMPARE(gone.size(), 0); QCOMPARE(read(make(context())).preferences, requested);
    }
    void foreignXmlOrRawMirrorAfterXmlCommitIsNotAdopted_data() {
        QTest::addColumn<bool>("xmlChange"); QTest::newRow("same-byte-xml-replacement") << true; QTest::newRow("raw-rendering-mirror") << false;
    }
    void foreignXmlOrRawMirrorAfterXmlCommitIsNotAdopted() {
        QFETCH(bool, xmlChange);
        const auto held = gate(); auto fixture = context(); fixture.afterXml = [held] { held->waitOnce(); };
        AsyncFontReload *reload = nullptr; auto *backend = make(fixture, &reload); const auto before = read(backend);
        auto requested = change(before); requested.antialias = "true";
        const auto original = contents(config); QSignalSpy done(backend, &FontBackend::writeFinished);
        backend->writePreferences(before, requested); QTRY_VERIFY(held->entered.available());
        if (xmlChange) save(xml, contents(xml));
        else save(config, original + "\n[General]\nXftAntialias=false\nForeign=preserve\n");
        held->released.release(); QTRY_COMPARE(done.size(), 1); QVERIFY(!done[0][0].toBool());
        QVERIFY(done[0][1].toString().contains("partial save")); QCOMPARE(reload->reloads->load(), 0);
        if (xmlChange) QCOMPARE(contents(config), original);
        else { QVERIFY(contents(config).contains("Foreign=preserve")); QVERIFY(!contents(config).contains("font=")); }
    }
    void xmlSameValueReplacementDuringSecondFileSyncIsNotAdopted() {
        AsyncFontReload *reload = nullptr; auto *backend = make(context(), &reload); const auto before = read(backend);
        QLockFile lock(config + QStringLiteral(".lock")); QVERIFY(lock.tryLock());
        auto requested = change(before); requested.antialias = "true";
        QSignalSpy done(backend, &FontBackend::writeFinished), pending(backend, &FontBackend::operationPending);
        backend->writePreferences(before, requested);
        QTRY_VERIFY(contents(xml).contains("antialias")); QTRY_VERIFY(!pending.isEmpty()); QCOMPARE(done.size(), 0);
        save(xml, contents(xml)); lock.unlock();
        QTRY_COMPARE(done.size(), 1); QVERIFY(!done[0][0].toBool()); QCOMPARE(reload->reloads->load(), 0);
        QVERIFY(done[0][1].toString().contains("could not be verified"));
    }
    void foreignSourceOrProviderAfterSavePreventsUnsentRefresh_data() {
        QTest::addColumn<bool>("provider"); QTest::newRow("source") << false; QTest::newRow("provider") << true;
    }
    void foreignSourceOrProviderAfterSavePreventsUnsentRefresh() {
        QFETCH(bool, provider);
        const auto source = directory.filePath("inherited-font-policy"); save(source, "original");
        const auto held = gate(); auto fixture = context(); fixture.additionalSources = {source}; fixture.afterSync = [held] { held->waitOnce(); };
        AsyncFontReload *reload = nullptr; auto *backend = make(fixture, &reload); const auto before = read(backend);
        QSignalSpy done(backend, &FontBackend::writeFinished); backend->writePreferences(before, change(before)); QTRY_VERIFY(held->entered.available());
        if (provider) { reload->generation = "replacement-provider"; emit reload->availabilityChanged(); }
        else save(source, "foreign changed source");
        held->released.release(); QTRY_COMPARE(done.size(), 1); QVERIFY(!done[0][0].toBool());
        QVERIFY(!done[0][1].toString().isEmpty()); QCOMPARE(reload->reloads->load(), 0); QVERIFY(contents(config).contains("Keep=original"));
    }
    void isolatedSyncHookRunsOffGuiAndFailureHasNoDestructorRetry() {
        auto *backend = make(context()); const auto before = read(backend); const auto original = contents(config);
        std::atomic_bool worker{false}, dirty{false};
        backend->setSyncForTest([&](KConfig &config) {
            worker = QThread::currentThread() != QCoreApplication::instance()->thread(); dirty = config.isDirty(); return false;
        });
        auto requested = change(before); requested.antialias = "true";
        QSignalSpy done(backend, &FontBackend::writeFinished); backend->writePreferences(before, requested); QTRY_COMPARE(done.size(), 1);
        QVERIFY(worker.load()); QVERIFY(dirty.load()); QVERIFY(!done[0][0].toBool()); QVERIFY(done[0][1].toString().contains("partial save"));
        QCOMPARE(contents(config), original); QVERIFY(contents(xml).contains("antialias")); delete backend;
        QCoreApplication::processEvents(); QCOMPARE(contents(config), original);
    }
};
QTEST_GUILESS_MAIN(FontAsyncTest)
#include "font_async_test.moc"
