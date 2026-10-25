#include <QtTest>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusServiceWatcher>
#include <QSignalBlocker>
#include <QDir>
#include <QFile>
#include <QLockFile>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSaveFile>
#include <QSemaphore>
#include <QTemporaryDir>
#include <QTimer>
#include <atomic>
#include <memory>
#include <sys/stat.h>
#include <unistd.h>
#include "platform/kde/AppearanceBackend.h"
#include "operations/OperationManager.h"

namespace {
struct Gate {
    std::atomic_bool armed{true}; QSemaphore entered, released;
    void waitOnce() { if (armed.exchange(false)) { entered.release(); released.acquire(); } }
};
class Reload final : public AppearanceReloadService {
public:
    bool available = true, automatic = true; int requests = 0; QString revision = QStringLiteral("fixture-owner-1");
    bool isAvailable() const override { return available; }
    QString unavailableReason() const override { return QStringLiteral("Private reload unavailable"); }
    QString identity() const override { return revision; }
    void requestReload(const QString &) override { ++requests; if (automatic) emit finished(true, {}); }
};
}
class PrivateKWin final : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kde.KWin")
public:
    int reloads = 0; std::function<void()> onReload;
public slots:
    void reconfigure() { ++reloads; if (onReload) onReload(); }
    QString supportInformation() const {
        return QStringLiteral("KWin version: 6.6.6\n\nOptions\n=======\noperationTitlebarDblClick: %1\n\n").arg(reloads ? 5002 : 5000);
    }
};
class AppearanceAsyncTest final : public QObject {
    Q_OBJECT
    QTemporaryDir directory;
    QList<QPointer<KdeAppearanceBackend>> backends;
    QList<std::shared_ptr<Gate>> gates;
    QString path() const { return directory.filePath("kwinrc"); }
    void put(const QByteArray &bytes) {
        QSaveFile file(path()); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(bytes), qint64(bytes.size())); QVERIFY(file.commit());
    }
    QByteArray contents() const { QFile file(path()); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray(); }
    std::shared_ptr<Gate> gate() { auto value = std::make_shared<Gate>(); gates << value; return value; }
    KdeAppearanceBackend::FileContext context() const {
        KdeAppearanceBackend::FileContext result; result.fixture = true; result.softDeadlineMs = 25; return result;
    }
    KdeAppearanceBackend *make(const KdeAppearanceBackend::FileContext &options, Reload **service = nullptr, const QString &file = {}) {
        auto *reload = new Reload; auto *backend = new KdeAppearanceBackend(file.isEmpty() ? path() : file, reload, options);
        backends << backend; if (service) *service = reload; return backend;
    }
private slots:
    void initTestCase() { qRegisterMetaType<AppearanceState>(); }
    void init() { QVERIFY(directory.isValid()); put("[Windows]\nTitlebarDoubleClickCommand=Maximize\n[Other]\nKeep=original\n"); }
    void cleanup() {
        ::chmod(QFile::encodeName(directory.path()).constData(), 0700);
        for (const auto &held : gates) held->released.release(32);
        QTRY_VERIFY_WITH_TIMEOUT(!OperationManager::instance()->hasActiveJobs(), 15000);
        for (auto backend : backends) delete backend;
        backends.clear(); gates.clear();
    }
    void slowReadKeepsHeartbeatAndSurvivesFacadeDeletion() {
        const auto held = gate(); auto options = context(); options.beforeRead = [held] { held->waitOnce(); };
        Reload *reload = nullptr; auto *backend = make(options, &reload); QPointer<Reload> retained(reload);
        int beats = 0; QTimer heartbeat; heartbeat.setInterval(1); connect(&heartbeat, &QTimer::timeout, this, [&] { ++beats; }); heartbeat.start();
        QSignalSpy done(backend, &AppearanceBackend::readFinished), pending(backend, &AppearanceBackend::operationPending);
        backend->readCurrent(); QTRY_VERIFY(held->entered.available()); QTRY_VERIFY(beats >= 3); QTRY_VERIFY(!pending.isEmpty());
        QCOMPARE(done.size(), 0); delete backend; QVERIFY(retained); QVERIFY(OperationManager::instance()->hasActiveJobs());
        held->released.release(); QTRY_VERIFY(!OperationManager::instance()->hasActiveJobs()); QTRY_VERIFY(!retained); QCOMPARE(done.size(), 0);
    }
    void nativeLockWaitHasResponsiveTimerAndDelayedSuccess() {
        auto *backend = make(context()); QLockFile lock(path() + ".lock"); QVERIFY(lock.tryLock());
        int beats = 0; bool unlocked = false; QTimer heartbeat; heartbeat.setInterval(1);
        connect(&heartbeat, &QTimer::timeout, this, [&] { ++beats; }); heartbeat.start();
        QTimer::singleShot(200, this, [&] { unlocked = true; lock.unlock(); });
        QSignalSpy done(backend, &AppearanceBackend::writeFinished), pending(backend, &AppearanceBackend::operationPending);
        backend->writeAction("Maximize", "Minimize"); QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 10000);
        QVERIFY(unlocked); QVERIFY(beats >= 3); QVERIFY(!pending.isEmpty()); QVERIFY2(done[0][0].toBool(), qPrintable(done[0][1].toString()));
        QVERIFY(contents().contains("TitlebarDoubleClickCommand=Minimize")); QVERIFY(contents().contains("Keep=original"));
    }
    void orphanedSaveRetainsServiceActivityAndReservation() {
        const auto held = gate(); auto options = context(); options.beforeSync = [held] { held->waitOnce(); };
        Reload *reload = nullptr; auto *first = make(options, &reload); QPointer<Reload> retained(reload);
        QSignalSpy gone(first, &AppearanceBackend::writeFinished); first->writeAction("Maximize", "Minimize"); QTRY_VERIFY(held->entered.available());
        delete first; QVERIFY(retained); QVERIFY(OperationManager::instance()->hasActiveJobs());
        auto *second = make(context(), nullptr, directory.filePath("./kwinrc")); QSignalSpy refused(second, &AppearanceBackend::writeFinished);
        second->writeAction("Maximize", "Nothing"); QTRY_COMPARE(refused.size(), 1); QVERIFY(!refused[0][0].toBool());
        held->released.release(); QTRY_VERIFY(!OperationManager::instance()->hasActiveJobs()); QTRY_VERIFY(!retained); QCOMPARE(gone.size(), 0);
        QVERIFY(contents().contains("TitlebarDoubleClickCommand=Minimize"));
        second->writeAction("Minimize", "Nothing"); QTRY_COMPARE(refused.size(), 2); QVERIFY2(refused[1][0].toBool(), qPrintable(refused[1][1].toString()));
    }
    void oldAndReplacementHardlinksRemainOwnedThroughReload() {
        const auto old = directory.filePath("old"), replacement = directory.filePath("new"); QFile::remove(old); QFile::remove(replacement);
        QVERIFY(::link(QFile::encodeName(path()).constData(), QFile::encodeName(old).constData()) == 0);
        Reload *reload = nullptr; auto *first = make(context(), &reload); reload->automatic = false;
        QSignalSpy done(first, &AppearanceBackend::writeFinished); first->writeAction("Maximize", "Minimize"); QTRY_COMPARE(reload->requests, 1);
        QVERIFY(::link(QFile::encodeName(path()).constData(), QFile::encodeName(replacement).constData()) == 0);
        for (const auto &alias : {old, replacement}) {
            auto *second = make(context(), nullptr, alias); QSignalSpy refused(second, &AppearanceBackend::writeFinished);
            second->writeAction(alias == old ? "Maximize" : "Minimize", "Nothing"); QTRY_COMPARE(refused.size(), 1); QVERIFY(!refused[0][0].toBool());
        }
        emit reload->finished(true, {}); QTRY_COMPARE(done.size(), 1); QVERIFY(done[0][0].toBool());
    }
    void changedProviderOrForeignSourcePreventsUnsentReload_data() {
        QTest::addColumn<bool>("provider"); QTest::newRow("provider") << true; QTest::newRow("foreign-source") << false;
    }
    void changedProviderOrForeignSourcePreventsUnsentReload() {
        QFETCH(bool, provider); const auto held = gate(); auto options = context(); options.afterSync = [held] { held->waitOnce(); };
        Reload *reload = nullptr; auto *backend = make(options, &reload); QSignalSpy done(backend, &AppearanceBackend::writeFinished);
        backend->writeAction("Maximize", "Minimize"); QTRY_VERIFY(held->entered.available());
        if (provider) { reload->revision = "fixture-owner-2"; emit reload->availabilityChanged(); }
        else put(contents() + "\n[Outside]\nForeign=preserve\n");
        held->released.release(); QTRY_COMPARE(done.size(), 1); QVERIFY(!done[0][0].toBool()); QVERIFY(!done[0][1].toString().isEmpty()); QCOMPARE(reload->requests, 0);
        QVERIFY(contents().contains("TitlebarDoubleClickCommand=Minimize")); if (!provider) QVERIFY(contents().contains("Foreign=preserve"));
    }
    void nativeFailureCleansBeforeItsFailureObserverCanRestore() {
        if (::geteuid() == 0) QSKIP("Permission fixture requires an unprivileged process.");
        const auto parent = directory.path(); auto options = context();
        options.beforeSync = [parent] { ::chmod(QFile::encodeName(parent).constData(), 0500); };
        options.afterSync = [parent] { ::chmod(QFile::encodeName(parent).constData(), 0700); };
        auto *backend = make(options); QSignalSpy done(backend, &AppearanceBackend::writeFinished);
        backend->writeAction("Maximize", "Minimize"); QTRY_COMPARE(done.size(), 1); QVERIFY(!done[0][0].toBool());
        const QByteArray restored = "[Windows]\nTitlebarDoubleClickCommand=Nothing\n[Other]\nKeep=restored\n";
        put(restored); QTest::qWait(80); QCOMPARE(contents(), restored); QCOMPARE(done.size(), 1);
    }
    void nativeWirePinsOwnerAndGuardsReload_data() {
        QTest::addColumn<int>("replacementPoint"); QTest::newRow("stable-owner") << 0;
        QTest::newRow("replacement") << 1; QTest::newRow("before-dispatch-notification-delayed") << 2;
        QTest::newRow("after-dispatch-notification-delayed") << 3;
    }
    void nativeWirePinsOwnerAndGuardsReload() {
        QFETCH(int, replacementPoint); const bool replaceOwner = replacementPoint != 0;
        const auto address = qgetenv("DBUS_SESSION_BUS_ADDRESS"); QVERIFY(!address.isEmpty());
        auto old = QDBusConnection::connectToBus(QString::fromUtf8(address), "appearance-old");
        auto replacement = QDBusConnection::connectToBus(QString::fromUtf8(address), "appearance-new");
        auto client = QDBusConnection::connectToBus(QString::fromUtf8(address), "appearance-client");
        PrivateKWin first, second; QVERIFY(old.registerObject("/KWin", &first, QDBusConnection::ExportAllSlots));
        QVERIFY(replacement.registerObject("/KWin", &second, QDBusConnection::ExportAllSlots)); QVERIFY(old.registerService("org.kde.KWin"));
        auto *service = createKWinAppearanceReloadService(client); QTRY_VERIFY(service->isAvailable());
        const auto held = gate(); auto options = context(); options.afterSync = [held] { held->waitOnce(); };
        auto *backend = new KdeAppearanceBackend(path(), service, options); backends << backend;
        QSignalSpy done(backend, &AppearanceBackend::writeFinished); backend->writeAction("Maximize", "Minimize"); QTRY_VERIFY(held->entered.available());
        std::unique_ptr<QSignalBlocker> delay;
        if (replacementPoint >= 2) {
            auto *watcher = service->findChild<QDBusServiceWatcher *>(); QVERIFY(watcher);
            delay = std::make_unique<QSignalBlocker>(watcher);
        }
        if (replacementPoint == 3) {
            first.onReload = [&] {
                QVERIFY(old.unregisterService("org.kde.KWin")); QVERIFY(replacement.registerService("org.kde.KWin"));
            };
        } else if (replaceOwner) {
            const auto identity = service->identity(); QVERIFY(old.unregisterService("org.kde.KWin")); QVERIFY(replacement.registerService("org.kde.KWin"));
            if (replacementPoint == 1) { QTRY_VERIFY(service->identity() != identity); QTRY_VERIFY(service->isAvailable()); }
            else { QTest::qWait(30); QCOMPARE(service->identity(), identity); }
        }
        held->released.release();
        QTRY_COMPARE(done.size(), 1); QCOMPARE(done[0][0].toBool(), !replaceOwner);
        QCOMPARE(first.reloads, replacementPoint == 0 || replacementPoint == 3 ? 1 : 0); QCOMPARE(second.reloads, 0);
        QTRY_VERIFY(!OperationManager::instance()->hasActiveJobs()); delay.reset(); delete backend;
        if (replaceOwner) QVERIFY(replacement.unregisterService("org.kde.KWin"));
        else QVERIFY(old.unregisterService("org.kde.KWin"));
        QDBusConnection::disconnectFromBus("appearance-old"); QDBusConnection::disconnectFromBus("appearance-new"); QDBusConnection::disconnectFromBus("appearance-client");
    }
};
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (!qEnvironmentVariableIsSet("CONTROL_APPEARANCE_ASYNC_CHILD")) {
        QTemporaryDir sandbox; if (!sandbox.isValid()) return 2; auto env = QProcessEnvironment::systemEnvironment();
        for (const auto &name : {"XDG_CONFIG_HOME", "XDG_CONFIG_DIRS", "XDG_CACHE_HOME", "XDG_STATE_HOME"}) {
            const auto directory = sandbox.filePath(QString::fromLatin1(name)); if (!QDir().mkpath(directory)) return 3; env.insert(name, directory);
        }
        env.insert("KDE_SKIP_KDERC", "1"); env.insert("CONTROL_APPEARANCE_ASYNC_CHILD", "1");
        QProcess child; child.setProcessEnvironment(env); child.setProcessChannelMode(QProcess::ForwardedChannels);
        QStringList args{"--", app.applicationFilePath()}; args << app.arguments().mid(1); child.start("/usr/bin/dbus-run-session", args);
        if (!child.waitForStarted(3000) || !child.waitForFinished(90000)) { child.kill(); child.waitForFinished(1000); return 4; }
        return child.exitStatus() == QProcess::NormalExit ? child.exitCode() : 5;
    }
    AppearanceAsyncTest test; return QTest::qExec(&test, argc, argv);
}
#include "appearance_async_test.moc"
