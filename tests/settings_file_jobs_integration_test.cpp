#include "platform/kde/ScreenLockBackend.h"
#include "platform/kde/NightLightBackend.h"
#include "platform/kde/NightTimeBackend.h"
#include "platform/kde/PowerPolicyBackend.h"
#include "operations/OperationManager.h"
#include <QtTest>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSaveFile>
#include <QSemaphore>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <atomic>
#include <functional>
#include <memory>
#include <vector>

namespace {
enum Kind { Locking, Filter, Schedule, Power };
class LockService final : public ScreenLockService {
public:
    void query(quint64 token, const QString & = {}) override {
        ScreenLockRuntime r; r.installedVerified = true; r.version = QStringLiteral("6.6.5"); r.epoch = 1;
        emit queryFinished(token, true, r, {});
    }
    void configure(quint64, const QString &, quint64, Guard) override { QFAIL("Offline locker must not be configured"); }
};
class FilterService final : public NightLightService {
public:
    void query(quint64 token, const QString & = {}) override {
        NightLightRuntime r; r.owner = QStringLiteral(":private.filter"); r.epoch = 1; r.version = QStringLiteral("6.6.6");
        r.serviceAvailable = r.schemaSupported = r.known = r.available = r.daylight = true;
        r.mode = 1; r.currentTemperature = r.targetTemperature = 6500;
        emit queryFinished(token, true, r, {});
    }
    void preview(quint64, int, const QString &, quint64) override { QFAIL("Reservation fixtures must not preview"); }
    void stopPreview(quint64, const QString &, quint64) override { QFAIL("Reservation fixtures must not stop previews"); }
};
class ScheduleService final : public NightTimeService {
public:
    void query(quint64 token, const QString & = {}) override {
        NightTimeRuntime r; r.installedVerified = true; r.version = QStringLiteral("6.6.5"); r.epoch = 1;
        emit queryFinished(token, true, r, {});
    }
};
class PolicyDaemon final : public PowerPolicyDaemon {
public:
    void queryCapabilities() override {
        PowerPolicyCapabilities c;
        c.serviceAvailable = c.schemaSupported = c.displaySupported = true;
        c.suspendActionSupported = c.canSuspendKnown = c.canSuspend = true;
        c.hibernateKnown = c.mobileKnown = c.defaultMobileKnown = c.virtualMachineKnown = true;
        c.batteryKnown = c.hasBattery = true;
        c.owner = QStringLiteral(":private.power"); c.revision = QStringLiteral("private-power-v1");
        emit capabilitiesFinished(c);
    }
    void reload() override { emit reloadFinished(true, false, {}); }
    void cancel() override { emit reloadFinished(false, true, QStringLiteral("Private reload cancelled")); }
};
struct Gate { QSemaphore release; std::atomic_bool entered{false}; };
struct Endpoint {
    QPointer<SettingsSession> session;
    QPointer<QObject> service;
    std::unique_ptr<QSignalSpy> completed;
    std::function<void()> stage;
    std::function<bool()> ready;
};
}

class SettingsFileJobsIntegrationTest final : public QObject {
    Q_OBJECT
    QTemporaryDir files;
    std::vector<std::unique_ptr<Endpoint>> endpoints;
    QList<std::shared_ptr<Gate>> gates;
    void save(const QString &path, const QByteArray &bytes) {
        QSaveFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(bytes), qint64(bytes.size())); QVERIFY(file.commit());
    }
    Endpoint *make(Kind kind, const QString &path, bool full, const std::shared_ptr<Gate> &gate = {}) {
        auto entry = std::make_unique<Endpoint>(); auto *e = entry.get();
        const std::function<void()> beforeSync = gate ? std::function<void()>([gate] { gate->entered = true; gate->release.acquire(); }) : std::function<void()>();
        if (kind == Locking) {
            auto *service = new LockService; KdeScreenLockBackend::ConfigContext context; context.fullCascade = full; context.fixture = true; context.beforeSync = beforeSync; context.softDeadlineMs = 20;
            auto *backend = new KdeScreenLockBackend(path, service, context); auto *s = new ScreenLockSettings(backend);
            e->session = s; e->service = service; e->completed = std::make_unique<QSignalSpy>(backend, &ScreenLockBackend::writeFinished);
            e->stage = [s] { s->selectValue(ScreenLockKey::LockOnResume, false); }; e->ready = [s] { return s->isReady(); };
        } else if (kind == Filter) {
            auto *service = new FilterService; KdeNightLightBackend::ConfigContext context; context.fullCascade = full; context.fixture = true; context.beforeSync = beforeSync; context.softDeadlineMs = 20;
            auto *backend = new KdeNightLightBackend(path, service, context); auto *s = new NightLightSettings(backend);
            e->session = s; e->service = service; e->completed = std::make_unique<QSignalSpy>(backend, &NightLightBackend::writeFinished);
            e->stage = [s] { s->selectValue(NightLightKey::NightTemperature, 4200); }; e->ready = [s] { return s->isReady(); };
        } else if (kind == Schedule) {
            auto *service = new ScheduleService; KdeNightTimeBackend::ConfigContext context; context.fullCascade = full; context.fixture = true; context.beforeSync = beforeSync; context.softDeadlineMs = 20;
            auto *backend = new KdeNightTimeBackend(path, service, context); auto *s = new NightTimeSettings(backend);
            e->session = s; e->service = service; e->completed = std::make_unique<QSignalSpy>(backend, &NightTimeBackend::writeFinished);
            e->stage = [s] { s->selectValue(NightTimeKey::Source, 1); }; e->ready = [s] { return s->isReady(); };
        } else {
            auto *service = new PolicyDaemon; KdePowerPolicyBackend::ConfigContext context; context.fullCascade = full; context.fixture = true; context.beforeSync = beforeSync; context.softDeadlineMs = 20;
            auto *backend = new KdePowerPolicyBackend(path, service, context); auto *s = new PowerPolicySettings(backend);
            e->session = s; e->service = service; e->completed = std::make_unique<QSignalSpy>(backend, &PowerPolicyBackend::writeFinished);
            e->stage = [s] { s->selectDisplayTimeout(QStringLiteral("AC"), 1200); }; e->ready = [s] { return s->isReady(); };
        }
        endpoints.push_back(std::move(entry)); return e;
    }
    static QString name(Kind kind) {
        return kind == Locking ? QStringLiteral("kscreenlockerrc") : kind == Filter ? QStringLiteral("kwinrc") : kind == Schedule ? QStringLiteral("knighttimerc") : QStringLiteral("powerdevilrc");
    }
private slots:
    void init() {
        QVERIFY(files.isValid());
        const auto home = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
        for (const auto &file : {QStringLiteral("kscreenlockerrc"), QStringLiteral("kwinrc"), QStringLiteral("knighttimerc"), QStringLiteral("powerdevilrc"), QStringLiteral("kdeglobals")}) QFile::remove(home + '/' + file);
    }
    void cleanup() {
        for (const auto &gate : gates) gate->release.release(16);
        QTRY_VERIFY_WITH_TIMEOUT(!OperationManager::instance()->hasActiveJobs(), 10000);
        for (const auto &entry : endpoints) delete entry->session;
        endpoints.clear(); gates.clear();
    }
    void differentProvidersShareReservationUntilOrphanedJobFinishes_data() {
        QTest::addColumn<int>("first"); QTest::addColumn<int>("second"); QTest::addColumn<bool>("full");
        QTest::newRow("locker-filter-same-file") << int(Locking) << int(Filter) << false;
        QTest::newRow("filter-schedule-same-file") << int(Filter) << int(Schedule) << false;
        QTest::newRow("schedule-locker-same-file") << int(Schedule) << int(Locking) << false;
        QTest::newRow("locker-filter-shared-global") << int(Locking) << int(Filter) << true;
        QTest::newRow("filter-schedule-shared-global") << int(Filter) << int(Schedule) << true;
        QTest::newRow("schedule-locker-shared-global") << int(Schedule) << int(Locking) << true;
        for (const auto other : {Locking, Filter, Schedule}) {
            for (const bool full : {false, true}) {
                const auto suffix = QByteArray::number(int(other)) + (full ? "-shared-global" : "-same-file");
                QTest::newRow(("power-first-" + suffix).constData()) << int(Power) << int(other) << full;
                QTest::newRow(("power-second-" + suffix).constData()) << int(other) << int(Power) << full;
            }
        }
    }
    void differentProvidersShareReservationUntilOrphanedJobFinishes() {
        QFETCH(int, first); QFETCH(int, second); QFETCH(bool, full);
        const auto home = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
        const QString firstPath = full ? home + '/' + name(Kind(first)) : files.filePath(QStringLiteral("shared-settings"));
        const QString secondPath = full ? home + '/' + name(Kind(second)) : firstPath;
        save(firstPath, "[Foreign]\nKeep=unowned\n"); if (secondPath != firstPath) save(secondPath, "[Foreign]\nKeep=unowned\n");
        if (full) save(home + QStringLiteral("/kdeglobals"), "[Foreign]\nKeep=global\n");
        const auto gate = std::make_shared<Gate>(); gates << gate;
        auto *a = make(Kind(first), firstPath, full, gate); auto *b = make(Kind(second), secondPath, full);
        QTRY_VERIFY(!a->session->isBusy() && !b->session->isBusy()); QVERIFY(a->ready()); QVERIFY(b->ready());
        a->stage(); QVERIFY(a->session->isDirty()); a->session->apply(); QTRY_VERIFY(gate->entered.load());
        QVERIFY(a->session->isBusy()); delete a->session; QVERIFY(!a->session); QVERIFY(a->service);
        b->stage(); QVERIFY(b->session->isDirty()); b->session->apply(); QTRY_VERIFY(!b->session->isBusy());
        QCOMPARE(b->completed->size(), 1); QVERIFY(!b->completed->at(0).at(1).toBool());
        QVERIFY(b->session->hasConflict()); QVERIFY(b->session->isDirty()); QVERIFY(!b->session->errorText().isEmpty());
        QVERIFY(OperationManager::instance()->hasActiveJobs()); QVERIFY(a->service); QCOMPARE(a->completed->size(), 0);
        gate->release.release(); QTRY_VERIFY_WITH_TIMEOUT(!OperationManager::instance()->hasActiveJobs(), 10000); QTRY_VERIFY(!a->service);
        b->session->revert(); QTRY_VERIFY(!b->session->isBusy()); QVERIFY(!b->session->hasConflict()); QVERIFY(!b->session->isDirty());
        b->stage(); QVERIFY(b->session->isDirty()); b->session->apply(); QTRY_VERIFY(!b->session->isBusy());
        QCOMPARE(b->completed->size(), 2); QVERIFY2(b->completed->at(1).at(3).toString().isEmpty(), qPrintable(b->session->errorText()));
        QVERIFY(!b->session->isDirty()); QVERIFY(!b->session->hasConflict());
        for (const auto &path : {firstPath, secondPath}) { QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly)); QVERIFY(file.readAll().contains("Keep=unowned")); }
    }
};

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (!qEnvironmentVariableIsSet("CONTROL_SETTINGS_FILE_JOBS_CHILD")) {
        QTemporaryDir sandbox; if (!sandbox.isValid()) return 2; auto environment = QProcessEnvironment::systemEnvironment();
        for (const auto &pair : {qMakePair("XDG_CONFIG_HOME", "config"), qMakePair("XDG_CONFIG_DIRS", "defaults"), qMakePair("XDG_STATE_HOME", "state"), qMakePair("XDG_CACHE_HOME", "cache")}) {
            const auto path = sandbox.filePath(QString::fromLatin1(pair.second)); if (!QDir().mkpath(path)) return 3; environment.insert(QString::fromLatin1(pair.first), path);
        }
        environment.insert("KDE_SKIP_KDERC", "1"); environment.insert("CONTROL_SETTINGS_FILE_JOBS_CHILD", "1");
        QProcess child; child.setProcessEnvironment(environment); child.setProcessChannelMode(QProcess::ForwardedChannels);
        QStringList args{"--", app.applicationFilePath()}; args << app.arguments().mid(1); child.start("/usr/bin/dbus-run-session", args);
        if (!child.waitForStarted(3000) || !child.waitForFinished(80000)) { child.kill(); child.waitForFinished(1000); return 4; }
        return child.exitStatus() == QProcess::NormalExit ? child.exitCode() : 5;
    }
    SettingsFileJobsIntegrationTest test; return QTest::qExec(&test, argc, argv);
}
#include "settings_file_jobs_integration_test.moc"
