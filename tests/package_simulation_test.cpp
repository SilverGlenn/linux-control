#include <QtTest>
#include <QFile>
#include <QTemporaryDir>
#include <QSignalSpy>
#include <QScopeGuard>
#include <QPushButton>
#include <QTreeWidget>
#include <QLabel>
#include "packages/PackageSimulationService.h"
#include "ui/pages/PackagePreviewPage.h"

namespace {
const QByteArray plan =
    "NOTE: This is only a simulation!\n"
    "      apt-get needs root privileges for real execution.\n"
    "Reading package lists...\nBuilding dependency tree...\nReading state information...\nCalculating upgrade...\n"
    "The following packages will be upgraded:\n  example\n"
    "1 upgraded, 1 newly installed, 1 to remove and 2 not upgraded.\n"
    "Remv retired:amd64 [1.2-1]\n"
    "Inst example [1:1.0-1] (1:2.0-1 Ubuntu:26.04/resolute-updates [amd64]) []\n"
    "Inst dependency (3.2.1-0ubuntu1 Ubuntu:26.04/resolute [all])\n"
    "Conf example (1:2.0-1 Ubuntu:26.04/resolute-updates [amd64])\n"
    "Conf dependency (3.2.1-0ubuntu1 Ubuntu:26.04/resolute [all])\n";
}
class FakePackageSimulationBackend : public PackageSimulationBackend {
public:
    bool available = true;
    QList<quint64> starts, cancels;
    bool isAvailable() const override { return available; }
    QString unavailableReason() const override { return available ? QString() : QStringLiteral("APT provider is missing"); }
    void start(quint64 id) override { starts << id; }
    void cancel(quint64 id) override { cancels << id; }
    void finish(quint64 id, const PackageSimulationProcessResult &result) { emit finished(id, result); }
    static PackageSimulationProcessResult successful() {
        PackageSimulationProcessResult result; result.normalExit = true; result.exitCode = 0; result.standardOutput = plan; return result;
    }
};
class PackageSimulationTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { qRegisterMetaType<PackageSimulationProcessResult>(); }
    void parsesCompleteTypedPlan() {
        const auto result = parseAptPackageSimulation(plan);
        QVERIFY2(result.success, qPrintable(result.error)); QCOMPARE(result.changes.size(), 3);
        QCOMPARE(result.upgraded, 1); QCOMPARE(result.installed, 1); QCOMPARE(result.removed, 1); QCOMPARE(result.notUpgraded, 2);
        QCOMPARE(result.changes[0].action, PackageChange::Action::Remove); QCOMPARE(result.changes[0].package, "retired:amd64");
        QCOMPARE(result.changes[1].action, PackageChange::Action::Upgrade); QCOMPARE(result.changes[1].installedVersion, "1:1.0-1");
        QCOMPARE(result.changes[1].candidateVersion, "1:2.0-1"); QCOMPARE(result.changes[2].action, PackageChange::Action::Install);
        QVERIFY(result.changes[2].source.contains("resolute"));
    }
    void parsesCapturedAuditedUbuntuSimulation() {
        QFile file(QFINDTESTDATA("fixtures/apt-3.2.0-fullupgrade-simulation.txt")); QVERIFY(file.open(QIODevice::ReadOnly));
        const auto result = parseAptPackageSimulation(file.readAll());
        QVERIFY2(result.success, qPrintable(result.error)); QCOMPARE(result.upgraded, 5);
        QCOMPARE(result.installed, 0); QCOMPARE(result.removed, 0); QCOMPARE(result.notUpgraded, 4); QCOMPARE(result.changes.size(), 5);
    }
    void zeroChangesIsAValidSimulation() {
        const auto result = parseAptPackageSimulation("Reading package lists...\n0 upgraded, 0 newly installed, 0 to remove and 4 not upgraded.\n");
        QVERIFY(result.success); QVERIFY(result.changes.isEmpty()); QCOMPARE(result.notUpgraded, 4);
    }
    void incompleteOrUnsupportedOutputCannotSucceed_data() {
        QTest::addColumn<QByteArray>("output");
        QTest::newRow("empty") << QByteArray();
        QTest::newRow("no summary") << QByteArray("Inst example [1] (2 source [amd64])\nConf example (2 source [amd64])\n");
        QTest::newRow("missing operations") << QByteArray("1 upgraded, 0 newly installed, 0 to remove and 0 not upgraded.\n");
        QTest::newRow("wrong count") << QByteArray(plan).replace("1 upgraded, 1 newly", "2 upgraded, 1 newly");
        QTest::newRow("missing configuration") << QByteArray(plan).replace("Conf example (1:2.0-1 Ubuntu:26.04/resolute-updates [amd64])\n", "");
        QTest::newRow("wrong configuration version") << QByteArray(plan).replace("Conf example (1:2.0-1", "Conf example (9.0");
        QTest::newRow("invalid package") << QByteArray(plan).replace("Inst example ", "Inst ../example ");
        QTest::newRow("invalid version") << QByteArray(plan).replace("(1:2.0-1", "(???");
        QTest::newRow("duplicate operations") << QByteArray(plan).append("Inst example [1] (2 source [amd64])\n");
        QTest::newRow("translated") << QByteArray("0 actualizados, 0 nuevos instalados, 0 para eliminar.\n");
        QTest::newRow("unsupported summary") << QByteArray("1 upgraded, 0 newly installed, 1 downgraded, 0 to remove and 0 not upgraded.\n");
        QTest::newRow("metadata error") << QByteArray(plan).append("E: The package cache is invalid\n");
        QTest::newRow("unknown operation") << QByteArray(plan).append("Purg example [1]\n");
        QTest::newRow("indented unknown operation") << QByteArray(plan).append("  Inst invalid-metadata\n");
        QTest::newRow("indented error") << QByteArray(plan).append("  E: Unreadable metadata\n");
        QTest::newRow("nul") << QByteArray(plan).append('\0');
    }
    void incompleteOrUnsupportedOutputCannotSucceed() {
        QFETCH(QByteArray, output); const auto result = parseAptPackageSimulation(output);
        QVERIFY(!result.success); QVERIFY(!result.error.isEmpty()); QVERIFY(result.changes.isEmpty());
    }
    void serviceAndJobSurviveNavigation() {
        auto *backend = new FakePackageSimulationBackend;
        PackageSimulationService service(backend);
        auto *page = new PackagePreviewPage(nullptr, &service, nullptr);
        QVERIFY(backend->starts.isEmpty());
        page->findChild<QPushButton *>("startPackagePreview")->click(); QVERIFY(service.isBusy()); QCOMPARE(backend->starts.size(), 1);
        delete page;
        PackagePreviewPage replacement(nullptr, &service, nullptr);
        QCOMPARE(replacement.simulationService(), &service); QVERIFY(replacement.findChild<QPushButton *>("cancelPackagePreview")->isEnabled());
        QVERIFY(!replacement.findChild<QPushButton *>("startPackagePreview")->isEnabled());
        backend->finish(backend->starts.last(), FakePackageSimulationBackend::successful());
        QCOMPARE(service.state(), PackageSimulationService::State::Succeeded); QCOMPARE(replacement.findChild<QTreeWidget *>("packagePreviewChanges")->topLevelItemCount(), 3);
        QVERIFY(service.result().completedAt.isValid());
    }
    void cancellationWaitsForCompletionAndStaleRepliesAreIgnored() {
        auto *backend = new FakePackageSimulationBackend; PackageSimulationService service(backend);
        service.startPreview(); const auto first = backend->starts.last(); service.cancel();
        QCOMPARE(service.state(), PackageSimulationService::State::Cancelling); QVERIFY(service.isBusy()); QCOMPARE(backend->cancels.last(), first);
        service.startPreview(); QCOMPARE(backend->starts.size(), 1);
        backend->finish(first, FakePackageSimulationBackend::successful());
        QCOMPARE(service.state(), PackageSimulationService::State::Cancelled); QVERIFY(!service.result().success);
        service.startPreview(); const auto second = backend->starts.last();
        backend->finish(first, FakePackageSimulationBackend::successful()); QVERIFY(service.isBusy());
        backend->finish(second, FakePackageSimulationBackend::successful()); QCOMPARE(service.state(), PackageSimulationService::State::Succeeded);
    }
    void failuresNeverExposeACompletedPlan_data() {
        QTest::addColumn<int>("failure");
        for (int failure = 0; failure < 6; ++failure) QTest::newRow(qPrintable(QString::number(failure))) << failure;
    }
    void failuresNeverExposeACompletedPlan() {
        QFETCH(int, failure); auto *backend = new FakePackageSimulationBackend; PackageSimulationService service(backend); service.startPreview();
        auto result = FakePackageSimulationBackend::successful();
        switch (failure) {
        case 0: result.exitCode = 100; result.standardError = "E: Unmet dependencies"; break;
        case 1: result.normalExit = false; break;
        case 2: result.timedOut = true; break;
        case 3: result.outputTruncated = true; break;
        case 4: result.standardOutput = "Unparseable metadata"; break;
        case 5: result.standardError = "E: invalid metadata even with zero exit"; break;
        }
        backend->finish(backend->starts.last(), result); QVERIFY(!service.isBusy()); QVERIFY(!service.result().success);
        QVERIFY(service.result().changes.isEmpty()); QVERIFY(!service.result().error.isEmpty());
        QCOMPARE(service.state(), failure == 2 ? PackageSimulationService::State::TimedOut : PackageSimulationService::State::Failed);
    }
    void missingProviderIsExplicit() {
        auto *backend = new FakePackageSimulationBackend; backend->available = false; PackageSimulationService service(backend);
        PackagePreviewPage page(nullptr, &service, nullptr);
        QVERIFY(!page.findChild<QPushButton *>("startPackagePreview")->isEnabled());
        QVERIFY(page.findChild<QLabel *>("packagePreviewMessage")->text().contains("missing"));
        service.startPreview(); QVERIFY(backend->starts.isEmpty()); QCOMPARE(service.state(), PackageSimulationService::State::Failed);
    }
    void immutableArgvContainsOnlyTheSimulationOperation() {
        const auto args = aptPackageSimulationArguments(QStringLiteral("/tmp/test-apt-safety.conf"));
        QCOMPARE(args[0], "-c"); QCOMPARE(args[1], "/tmp/test-apt-safety.conf");
        const auto safety = aptPackageSimulationSafetyConfiguration();
        QVERIFY(safety.contains("#clear APT::Install::Pre-Invoke;"));
        QVERIFY(safety.contains("#clear APT::Install::Post-Invoke-Success;"));
        QVERIFY(safety.contains("#clear AptCli::Hooks;"));
        QVERIFY(safety.contains("#clear Version;"));
        QVERIFY(safety.contains("#clear Binary;"));
        QVERIFY(safety.contains("#clear DPkg::Pre-Invoke;"));
        QVERIFY(args.contains("APT::Output-Version=29"));
        QVERIFY(args.contains("--simulate")); QVERIFY(args.contains("--no-download")); QVERIFY(args.contains("APT::Get::Simulate=true"));
        QVERIFY(args.contains("Debug::NoLocking=true")); QCOMPARE(args.last(), "dist-upgrade");
        QVERIFY(!args.contains("install")); QVERIFY(!args.contains("remove")); QVERIFY(!args.contains("update"));
        QVERIFY(!args.contains("--assume-yes"));
    }
    void realProcessBoundaryUsesOnlyAnInjectedFixtureExecutable() {
        QTemporaryDir temp; QVERIFY(temp.isValid()); const QString path = temp.filePath("fake apt; injected-shell-marker");
        QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly));
        const QString capturedConfig = temp.filePath("captured.conf");
        const QString capturedArgs = temp.filePath("captured.args");
        const QString marker = temp.filePath("hook-invoked");
        const QByteArray script = "#!/bin/sh\nif [ -n \"$APT_CONFIG\" ]; then /usr/bin/touch '" + marker.toUtf8() + "'; exit 8; fi\n"
            "[ \"$LC_ALL\" = C ] || exit 9\nif [ \"$1\" = --version ]; then printf 'apt 3.2.0 (amd64)\\n'; exit 0; fi\n"
            "[ \"$1\" = -c ] || exit 7\n[ -r \"$2\" ] || exit 5\n[ \"$3\" = --simulate ] || exit 4\n[ \"$4\" = --no-download ] || exit 6\n"
            "/bin/cat \"$2\" > '" + capturedConfig.toUtf8() + "'\nprintf '%s\\n' \"$@\" > '" + capturedArgs.toUtf8() + "'\n/bin/cat <<'OUTPUT'\n" + plan + "OUTPUT\n";
        QCOMPARE(file.write(script), qint64(script.size())); file.close(); QVERIFY(file.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        auto *backend = createAptPackageSimulationBackend(path, 2000); PackageSimulationService service(backend);
        const bool hadConfig = qEnvironmentVariableIsSet("APT_CONFIG"); const QByteArray oldConfig = qgetenv("APT_CONFIG");
        const auto restoreEnvironment = qScopeGuard([hadConfig, oldConfig] { if (hadConfig) qputenv("APT_CONFIG", oldConfig); else qunsetenv("APT_CONFIG"); });
        qputenv("APT_CONFIG", "must-never-be-read-or-executed"); service.startPreview();
        QTRY_VERIFY(!service.isBusy());
        QVERIFY2(service.result().success, qPrintable(service.result().error)); QVERIFY(!QFile::exists(marker));
        QFile captured(capturedConfig); QVERIFY(captured.open(QIODevice::ReadOnly)); QCOMPARE(captured.readAll(), aptPackageSimulationSafetyConfiguration());
        QFile argvFile(capturedArgs); QVERIFY(argvFile.open(QIODevice::ReadOnly)); const auto argv = QString::fromUtf8(argvFile.readAll()).trimmed().split('\n');
        QVERIFY(argv.size() > 2); QCOMPARE(argv, aptPackageSimulationArguments(argv[1])); QVERIFY(!QFile::exists(argv[1]));
        QVERIFY(!QFile::exists(temp.filePath("injected-shell-marker")));
    }
    void unknownAptVersionCannotStartSimulation() {
        QTemporaryDir temp; const QString path = temp.filePath("fake-new-apt"); const QString marker = temp.filePath("simulation-started");
        QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly));
        const QByteArray script = "#!/bin/sh\nif [ \"$1\" = --version ]; then printf 'apt 9.0.0 (amd64)\\n'; exit 0; fi\n/usr/bin/touch '" + marker.toUtf8() + "'\n";
        QCOMPARE(file.write(script), qint64(script.size())); file.close(); QVERIFY(file.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        PackageSimulationService service(createAptPackageSimulationBackend(path, 2000)); service.startPreview(); QTRY_VERIFY(!service.isBusy());
        QCOMPARE(service.state(), PackageSimulationService::State::Failed); QVERIFY(service.result().error.contains("audited")); QVERIFY(!QFile::exists(marker));
    }
    void injectedHungProcessIsTimedOut() {
        QTemporaryDir temp; const QString path = temp.filePath("fake-hung-apt"); QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly));
        const QByteArray script = "#!/bin/sh\nexec /bin/sleep 10\n"; QCOMPARE(file.write(script), qint64(script.size())); file.close(); QVERIFY(file.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        PackageSimulationService service(createAptPackageSimulationBackend(path, 30)); service.startPreview();
        QTRY_VERIFY_WITH_TIMEOUT(!service.isBusy(), 1500); QCOMPARE(service.state(), PackageSimulationService::State::TimedOut); QVERIFY(!service.result().success);
    }
    void injectedProcessCanBeCancelled() {
        QTemporaryDir temp; const QString path = temp.filePath("fake-cancel-apt"); QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly));
        const QByteArray script = "#!/bin/sh\nexec /bin/sleep 10\n"; QCOMPARE(file.write(script), qint64(script.size())); file.close(); QVERIFY(file.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        PackageSimulationService service(createAptPackageSimulationBackend(path, 3000)); service.startPreview(); service.cancel();
        QTRY_VERIFY_WITH_TIMEOUT(!service.isBusy(), 1500); QCOMPARE(service.state(), PackageSimulationService::State::Cancelled);
    }
};
QTEST_MAIN(PackageSimulationTest)
#include "package_simulation_test.moc"
