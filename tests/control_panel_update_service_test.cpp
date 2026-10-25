#include "../src/operations/ControlPanelUpdateService.h"
#include "../src/operations/OperationManager.h"
#include "../src/ui/pages/LinuxUpdatePage.h"

#include <QtTest>
#include <QApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QTemporaryDir>
#include <QStandardPaths>
#include <QDir>
#include <AeroQt/stylesheet.h>

namespace {
using Phase = ControlPanelUpdateService::Phase;
bool writeFile(const QString &path, const QByteArray &data)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    if (file.write(data) != data.size()) return false;
    file.close();
    return file.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
}
QByteArray result(const QString &status, const QString &message = "Fixture status",
                  const QString &receipt = {})
{
    QJsonObject object {{"schema", 1}, {"status", status}, {"message", message}};
    if (status == "available" || status == "ready") {
        object.insert("build_id", "fixture-next+123");
        object.insert("release_tag", "laptop-candidate-2-fixture");
    }
    if (!receipt.isEmpty()) object.insert("receipt", receipt);
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}
struct Fixture {
    QTemporaryDir dir;
    QString helper() const { return dir.filePath("updater.py"); }
    QString receipt() const { return dir.filePath("receipt.json"); }
    bool setup()
    {
        return dir.isValid() && writeFile(helper(),
            "import pathlib,sys,time\n"
            "root=pathlib.Path(__file__).parent\n"
            "with (root/'calls').open('a') as f: f.write(' '.join(sys.argv[1:])+'\\n')\n"
            "if (root/'delay').exists(): time.sleep(0.15)\n"
            "print((root/(sys.argv[1]+'.json')).read_text())\n"
            "sys.exit(int((root/'exit-code').read_text()) if (root/'exit-code').exists() else 0)\n")
            && writeFile(receipt(), "{}")
            && writeFile(dir.filePath("check.json"), result("available"))
            && writeFile(dir.filePath("prepare.json"), result("ready", "Verified fixture", receipt()))
            && writeFile(dir.filePath("status.json"), result("rolled_back", "The fixture was rolled back"));
    }
    QByteArray calls() const
    {
        QFile file(dir.filePath("calls"));
        if (!file.open(QIODevice::ReadOnly)) return {};
        return file.readAll();
    }
};
}

class ControlPanelUpdateServiceTest final : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void noImplicitProcessesAndUnconfiguredIsHonest();
    void malformedResults_data();
    void malformedResults();
    void navigationAndDuplicateClicks();
    void readyOnlyRequestsGuardedCloseAndRechecksTrust();
    void localStatusIsAnExplicitAction();
    void unsafeHelperIsDisabled();
    void nonzeroFailurePreservesDiagnostic();
    void changedPreparedIdentityIsRejected();
private:
    QTemporaryDir m_home;
};

void ControlPanelUpdateServiceTest::initTestCase()
{
    QVERIFY(m_home.isValid());
    qputenv("HOME", m_home.path().toUtf8());
    qputenv("XDG_CONFIG_HOME", m_home.filePath("config").toUtf8());
    qputenv("XDG_DATA_HOME", m_home.filePath("data").toUtf8());
    qputenv("XDG_CACHE_HOME", m_home.filePath("cache").toUtf8());
    qputenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/nonexistent/control-update-test-session");
    qputenv("DBUS_SYSTEM_BUS_ADDRESS", "unix:path=/nonexistent/control-update-test-system");
    QStandardPaths::setTestModeEnabled(true);
    // Package providers are deliberately absent. Self-update uses absolute paths.
    qputenv("PATH", m_home.path().toUtf8());
    if (qEnvironmentVariableIsSet("LINUX_CONTROL_UI_CAPTURE_DIR"))
        Aero::registerStylesheet(qApp);
}

void ControlPanelUpdateServiceTest::noImplicitProcessesAndUnconfiguredIsHonest()
{
    Fixture fixture;
    QVERIFY(fixture.setup());
    const auto jobs = OperationManager::instance()->jobs().size();
    ControlPanelUpdateService configured(fixture.helper(), qApp);
    QCOMPARE(configured.state().phase, Phase::Idle);
    {
        LinuxUpdatePage page(new QScrollArea, nullptr, &configured);
        page.show();
        QTest::qWait(30);
        QVERIFY(fixture.calls().isEmpty());
        QCOMPARE(OperationManager::instance()->jobs().size(), jobs);
        auto *build = page.findChild<QLabel *>("controlPanelCurrentBuild");
        QVERIFY(build);
        QVERIFY(build->text().contains(ControlPanelUpdateService::currentBuildId()));
    }
    ControlPanelUpdateService unavailable(m_home.filePath("missing.py"));
    QCOMPARE(unavailable.state().phase, Phase::NotConfigured);
    LinuxUpdatePage page(new QScrollArea, nullptr, &unavailable);
    page.show();
    auto *check = page.findChild<QPushButton *>("controlPanelCheckUpdates");
    auto *install = page.findChild<QPushButton *>("controlPanelCloseAndInstall");
    QVERIFY(check && install);
    QVERIFY(!check->isEnabled());
    QVERIFY(!install->isVisible());
    QVERIFY(page.findChild<QLabel *>("controlPanelUpdateStatus")->text().contains("not configured"));
    unavailable.checkForUpdates();
    unavailable.prepareUpdate();
    QCOMPARE(OperationManager::instance()->jobs().size(), jobs);
}

void ControlPanelUpdateServiceTest::malformedResults_data()
{
    QTest::addColumn<QByteArray>("json");
    QTest::newRow("non-json") << QByteArray("a log line\n{}");
    QTest::newRow("unknown-status") << result("trusted_maybe");
    QTest::newRow("no-message") << QByteArray("{\"schema\":1,\"status\":\"up_to_date\"}");
    QTest::newRow("wrong-schema") << QByteArray("{\"schema\":2,\"status\":\"idle\",\"message\":\"x\"}");
    QTest::newRow("missing-identity") << QByteArray("{\"schema\":1,\"status\":\"available\",\"message\":\"x\"}");
    QTest::newRow("unknown-field") << QByteArray("{\"schema\":1,\"status\":\"idle\",\"message\":\"x\",\"exec\":\"oops\"}");
    QTest::newRow("oversize") << QByteArray(17000, 'x');
}

void ControlPanelUpdateServiceTest::malformedResults()
{
    QFETCH(QByteArray, json);
    Fixture fixture;
    QVERIFY(fixture.setup());
    QVERIFY(writeFile(fixture.dir.filePath("check.json"), json));
    ControlPanelUpdateService service(fixture.helper());
    service.checkForUpdates();
    QTRY_VERIFY(!service.busy());
    QCOMPARE(service.state().phase, Phase::Error);
    QVERIFY(service.state().receipt.isEmpty());
    QString error;
    QVERIFY(!service.launchInstaller(&error));
    QVERIFY(!error.isEmpty());
}

void ControlPanelUpdateServiceTest::navigationAndDuplicateClicks()
{
    Fixture fixture;
    QVERIFY(fixture.setup());
    QVERIFY(writeFile(fixture.dir.filePath("delay"), ""));
    ControlPanelUpdateService service(fixture.helper(), qApp);
    auto *page = new LinuxUpdatePage(new QScrollArea, nullptr, &service);
    page->findChild<QPushButton *>("controlPanelCheckUpdates")->click();
    QCOMPARE(service.state().phase, Phase::Checking);
    QVERIFY(OperationManager::instance()->hasActiveJobs());
    service.checkForUpdates();
    service.prepareUpdate();
    delete page;
    QTRY_COMPARE(service.state().phase, Phase::Available);
    QCOMPARE(fixture.calls().count('\n'), 1);
    page = new LinuxUpdatePage(new QScrollArea, nullptr, &service);
    page->show();
    auto *download = page->findChild<QPushButton *>("controlPanelDownloadUpdate");
    QVERIFY(download && download->isVisible() && download->isEnabled());
    download->click();
    QCOMPARE(service.state().phase, Phase::Downloading);
    service.prepareUpdate();
    service.checkForUpdates();
    QString error;
    QVERIFY(!service.launchInstaller(&error));
    delete page;
    QTRY_COMPARE(service.state().phase, Phase::Ready);
    QCOMPARE(fixture.calls().count('\n'), 2);
    QVERIFY(fixture.calls().contains("prepare --release-tag laptop-candidate-2-fixture --current-build "));
    QVERIFY(!OperationManager::instance()->hasActiveJobs());
}

void ControlPanelUpdateServiceTest::readyOnlyRequestsGuardedCloseAndRechecksTrust()
{
    Fixture fixture;
    QVERIFY(fixture.setup());
    ControlPanelUpdateService service(fixture.helper());
    service.checkForUpdates();
    QTRY_COMPARE(service.state().phase, Phase::Available);
    service.prepareUpdate();
    QTRY_COMPARE(service.state().phase, Phase::Ready);
    LinuxUpdatePage page(new QScrollArea, nullptr, &service);
    page.resize(1000, 650);
    page.show();
    QSignalSpy closeRequested(&page, &LinuxUpdatePage::controlPanelInstallRequested);
    auto *button = page.findChild<QPushButton *>("controlPanelCloseAndInstall");
    QVERIFY(button && button->isVisible() && button->isEnabled());
    const QString capture = qEnvironmentVariable("LINUX_CONTROL_UI_CAPTURE_DIR");
    if (!capture.isEmpty()) {
        QVERIFY(QDir().mkpath(capture));
        QTest::qWait(300);
        QVERIFY(page.grab().save(capture + QStringLiteral("/linux-update-manual-ready.png")));
    }
    button->click();
    QCOMPARE(closeRequested.count(), 1);
    QCOMPARE(fixture.calls().count('\n'), 2); // No installer before the shell guard.
    QVERIFY(QFile::remove(fixture.receipt()));
    QString error;
    QVERIFY(!service.launchInstaller(&error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!service.state().installerLaunched);
    QCOMPARE(fixture.calls().count('\n'), 2);
}

void ControlPanelUpdateServiceTest::localStatusIsAnExplicitAction()
{
    Fixture fixture;
    QVERIFY(fixture.setup());
    ControlPanelUpdateService service(fixture.helper());
    LinuxUpdatePage page(new QScrollArea, nullptr, &service);
    QVERIFY(fixture.calls().isEmpty());
    page.findChild<QPushButton *>("controlPanelLocalStatus")->click();
    QTRY_COMPARE(service.state().phase, Phase::RolledBack);
    QVERIFY(fixture.calls().startsWith("status --current-build "));
    QVERIFY(!fixture.calls().contains("check "));
    auto *status = page.findChild<QLabel *>("controlPanelUpdateStatus");
    QCOMPARE(status->textFormat(), Qt::PlainText);
    QCOMPARE(status->text(), QString("The fixture was rolled back"));
}

void ControlPanelUpdateServiceTest::unsafeHelperIsDisabled()
{
    Fixture fixture;
    QVERIFY(fixture.setup());
    QVERIFY(QFile::setPermissions(fixture.helper(), QFile::ReadOwner | QFile::WriteOwner | QFile::WriteOther));
    ControlPanelUpdateService service(fixture.helper());
    QCOMPARE(service.state().phase, Phase::NotConfigured);
    service.checkForUpdates();
    QVERIFY(fixture.calls().isEmpty());
}

void ControlPanelUpdateServiceTest::nonzeroFailurePreservesDiagnostic()
{
    Fixture fixture;
    QVERIFY(fixture.setup());
    QVERIFY(writeFile(fixture.dir.filePath("exit-code"), "3"));
    QVERIFY(writeFile(fixture.dir.filePath("check.json"), result("incompatible", "Fixture Qt ABI dependency mismatch <details>")));
    ControlPanelUpdateService service(fixture.helper());
    LinuxUpdatePage page(new QScrollArea, nullptr, &service);
    service.checkForUpdates();
    QTRY_COMPARE(service.state().phase, Phase::Incompatible);
    QCOMPARE(page.findChild<QLabel *>("controlPanelUpdateStatus")->text(), QString("Fixture Qt ABI dependency mismatch <details>"));
    QVERIFY(service.state().receipt.isEmpty());
    QVERIFY(writeFile(fixture.dir.filePath("check.json"), result("available")));
    service.checkForUpdates();
    QTRY_COMPARE(service.state().phase, Phase::Error); // Nonzero may not authorize actions.
}

void ControlPanelUpdateServiceTest::changedPreparedIdentityIsRejected()
{
    Fixture fixture;
    QVERIFY(fixture.setup());
    ControlPanelUpdateService service(fixture.helper());
    service.checkForUpdates();
    QTRY_COMPARE(service.state().phase, Phase::Available);
    auto prepared = QJsonDocument::fromJson(result("ready", "Verified fixture", fixture.receipt())).object();
    prepared.insert("build_id", "different-build+456");
    QVERIFY(writeFile(fixture.dir.filePath("prepare.json"), QJsonDocument(prepared).toJson()));
    service.prepareUpdate();
    QTRY_COMPARE(service.state().phase, Phase::Error);
    QVERIFY(service.state().receipt.isEmpty());
}

QTEST_MAIN(ControlPanelUpdateServiceTest)
#include "control_panel_update_service_test.moc"
