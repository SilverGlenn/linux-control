#include <QtTest>
#include <AeroQt/stylesheet.h>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QTabWidget>
#include <QTimer>
#include <QTreeWidget>
#include "ui/LinkLabel.h"
#include "ui/pages/NetworkConnectionsPage.h"
#include "ui/pages/NetworkSharingPage.h"
#include "settings/NetworkSettings.h"

namespace {
NetworkSnapshot fixture()
{
    NetworkSnapshot snapshot;
    snapshot.serviceAvailable = true;
    snapshot.serviceOwner = QStringLiteral(":1.fake");
    snapshot.version = QStringLiteral("1.54.3");
    snapshot.checkpointAvailable = true;
    snapshot.networkingEnabled = snapshot.wirelessEnabled = snapshot.wirelessHardwareEnabled = true;
    snapshot.connectivity = 4;
    snapshot.connectivityCheckAvailable = snapshot.connectivityCheckEnabled = false;
    for (int i = 1; i <= 2; ++i) {
        NetworkProfile profile;
        profile.path = QStringLiteral("/profiles/%1").arg(i);
        profile.id = QStringLiteral("Shared name");
        profile.uuid = QStringLiteral("uuid-%1").arg(i);
        profile.type = i == 1 ? QStringLiteral("802-11-wireless") : QStringLiteral("802-3-ethernet");
        profile.editable = true;
        profile.settings[QStringLiteral("connection")] = {{QStringLiteral("id"), profile.id}, {QStringLiteral("uuid"), profile.uuid}, {QStringLiteral("type"), profile.type}, {QStringLiteral("autoconnect"), true}};
        profile.settings[QStringLiteral("ipv4")] = {{QStringLiteral("method"), QStringLiteral("auto")}};
        profile.settings[QStringLiteral("ipv6")] = {{QStringLiteral("method"), QStringLiteral("auto")}};
        if (i == 1) {
            profile.settings[QStringLiteral("802-11-wireless")] = {{QStringLiteral("ssid"), QByteArray("Same name")}};
            profile.settings[QStringLiteral("802-11-wireless-security")] = {{QStringLiteral("key-mgmt"), QStringLiteral("wpa-psk")}};
        }
        snapshot.profiles << profile;
        NetworkDevice device;
        device.path = QStringLiteral("/devices/%1").arg(i);
        device.interfaceName = i == 1 ? QStringLiteral("wlan0") : QStringLiteral("eth0");
        device.type = i == 1 ? 2 : 1;
        device.state = i == 1 ? 100 : 30;
        device.managed = true;
        device.availableProfilePaths << profile.path;
        if (i == 1) {
            device.activeConnectionPath = QStringLiteral("/active/1");
            device.activeProfilePath = profile.path;
            device.activeAccessPointPath = QStringLiteral("/aps/1");
            device.ipv4Addresses << QStringLiteral("192.0.2.10/24");
        }
        snapshot.devices << device;
    }
    NetworkDevice loopback;
    loopback.path = QStringLiteral("/devices/3"); loopback.interfaceName = QStringLiteral("lo");
    loopback.type = 32; loopback.state = 10;
    loopback.reason = QStringLiteral("Loopback is managed outside this application.");
    snapshot.devices << loopback;
    for (int i = 1; i <= 3; ++i) {
        NetworkAccessPoint ap;
        ap.path = QStringLiteral("/aps/%1").arg(i); ap.devicePath = QStringLiteral("/devices/1");
        ap.name = QStringLiteral("Same name"); ap.ssid = QByteArray("Same name");
        ap.strength = i == 1 ? 82 : 51;
        ap.security = i == 3 ? NetworkAccessPoint::Security::Unsupported : NetworkAccessPoint::Security::WpaPsk;
        if (i == 3) ap.reason = QStringLiteral("Enterprise Wi-Fi is unsupported.");
        snapshot.accessPoints << ap;
    }
    return snapshot;
}
QScrollArea *sidebar()
{
    auto *area = new QScrollArea; area->setFixedWidth(190); return area;
}
void choose(QTreeWidget *tree, const QString &path)
{
    for (int i = 0; i < tree->topLevelItemCount(); ++i)
        if (tree->topLevelItem(i)->data(0, Qt::UserRole).toString() == path) {
            tree->setCurrentItem(tree->topLevelItem(i)); return;
        }
    QFAIL("Fixture row missing");
}
void capture(QWidget &widget, const QString &name)
{
    const QString directory = qEnvironmentVariable("LINUX_CONTROL_UI_CAPTURE_DIR");
    if (directory.isEmpty()) return;
    QDir().mkpath(directory);
    widget.resize(qobject_cast<QDialog *>(&widget) ? QSize(440, 190) : QSize(1050, 790));
    widget.show(); QApplication::processEvents();
    QVERIFY(widget.grab().save(directory + QLatin1Char('/') + name + QStringLiteral(".png")));
}
}

class PageNetworkBackend : public NetworkBackend {
public:
    NetworkSnapshot current = fixture(), baseline, preview;
    int changes = 0, scans = 0, keeps = 0, reverts = 0;
    QString changedPath;
    NetworkRequest::Action action = NetworkRequest::Action::UpdateProfile;
    bool expectedSecret = false;
    bool failAuthentication = false;
    void readCurrent() override { emit readFinished(true, current, {}); }
    void scan(const QString &devicePath) override {
        QCOMPARE(devicePath, QStringLiteral("/devices/1")); ++scans;
        emit scanFinished(true, {});
    }
    void beginChange(const NetworkSnapshot &before, const NetworkRequest &request) override {
        ++changes; action = request.action;
        changedPath = request.accessPointPath.isEmpty() ? request.profilePath : request.accessPointPath;
        expectedSecret = request.password == QStringLiteral("correct horse");
        baseline = before; preview = before;
        if (failAuthentication) {
            emit transactionFinished(false, true, baseline, QStringLiteral("Authentication failed. Check the security key.")); return;
        }
        if (request.action == NetworkRequest::Action::UpdateProfile) {
            for (auto &profile : preview.profiles)
                if (profile.path == request.profilePath) {
                    profile.settings = NetworkTypes::mergePreferences(profile, request.preferences);
                    profile.id = request.preferences.id;
                }
        } else if (request.action == NetworkRequest::Action::Disconnect) {
            for (auto &device : preview.devices)
                if (device.path == request.devicePath) {
                    device.activeConnectionPath.clear(); device.activeProfilePath.clear();
                    device.activeAccessPointPath.clear(); device.state = 30;
                }
        } else if (request.action == NetworkRequest::Action::ActivateProfile) {
            for (auto &device : preview.devices)
                if (device.path == request.devicePath) {
                    device.activeConnectionPath = QStringLiteral("/active/new");
                    device.activeProfilePath = request.profilePath; device.state = 100;
                }
        } else {
            for (auto &device : preview.devices)
                if (device.path == request.devicePath) {
                    device.activeConnectionPath = QStringLiteral("/active/new");
                    device.activeProfilePath = QStringLiteral("/profiles/1");
                    device.activeAccessPointPath = request.accessPointPath; device.state = 100;
                }
        }
        emit previewReady(preview, 20);
    }
    void keepChange() override {
        ++keeps; current = preview; emit transactionFinished(true, false, current, {});
    }
    void revertChange() override {
        ++reverts; current = baseline; emit transactionFinished(false, true, current, {});
    }
};

class NetworkPagesTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase()
    {
        if (qEnvironmentVariableIsSet("LINUX_CONTROL_UI_CAPTURE_DIR"))
            Aero::registerStylesheet(qApp);
    }
    void sharingReportsServiceConnectivityAndNativeNavigation()
    {
        auto *backend = new PageNetworkBackend;
        NetworkSettings session(backend);
        QTRY_VERIFY(session.isReady());
        NetworkSharingPage page(sidebar(), &session, nullptr);
        QCOMPARE(page.settingsSession(), static_cast<SettingsSession *>(&session));
        QVERIFY(page.findChild<QLabel *>("networkConnectivity")->text().contains(QStringLiteral("reported")));
        QVERIFY(page.findChild<QLabel *>("networkSharingStatus")->text().contains(QStringLiteral("may be cached")));
        QVERIFY(page.findChild<QLabel *>("networkActiveConnections")->text().contains(QStringLiteral("wlan0")));
        QSignalSpy navigation(&page, &NetworkSharingPage::navigationRequested);
        page.findChild<LinkLabel *>("networkAdapterLink")->activate();
        QCOMPARE(navigation.count(), 1);
        QCOMPARE(qvariant_cast<PageId>(navigation.first().first()), PageId::NetworkConnections);
        QCOMPARE(backend->changes, 0); QCOMPARE(backend->scans, 0);
        capture(page, QStringLiteral("network-sharing"));
    }
    void activeLoopbackAndWifiDirectDoNotCountAsUserNetworks()
    {
        auto *backend = new PageNetworkBackend;
        auto &loopback = backend->current.devices[2];
        loopback.managed = true; loopback.state = 100;
        loopback.activeConnectionPath = QStringLiteral("/active/loopback");
        loopback.ipv4Addresses << QStringLiteral("127.0.0.1/8");
        NetworkDevice discovery;
        discovery.path = QStringLiteral("/devices/4");
        discovery.interfaceName = QStringLiteral("p2p-dev-wlan0");
        discovery.type = 30; discovery.state = 100; discovery.managed = true;
        discovery.activeConnectionPath = QStringLiteral("/active/discovery");
        backend->current.devices << discovery;
        NetworkSettings session(backend);
        QTRY_VERIFY(session.isReady());
        NetworkSharingPage sharing(sidebar(), &session, nullptr);
        QCOMPARE(sharing.findChild<QLabel *>("networkMapNetwork")->text(), QStringLiteral("1 active connection(s)"));
        const QString networks = sharing.findChild<QLabel *>("networkActiveConnections")->text();
        QVERIFY(networks.contains(QStringLiteral("wlan0")));
        QVERIFY(!networks.contains(QStringLiteral("127.0.0.1")));
        QVERIFY(!networks.contains(QStringLiteral("p2p-dev-wlan0")));
        NetworkConnectionsPage connections(sidebar(), &session, nullptr);
        QCOMPARE(connections.findChild<QTreeWidget *>("networkDevices")->topLevelItemCount(), 4);
        choose(connections.findChild<QTreeWidget *>("networkDevices"), QStringLiteral("/devices/3"));
        QVERIFY(!connections.findChild<QPushButton *>("networkDisconnect")->isEnabled());
        backend->current.devices[0].activeConnectionPath.clear();
        backend->current.devices[0].activeProfilePath.clear();
        backend->current.devices[0].activeAccessPointPath.clear();
        backend->current.devices[0].state = 30;
        session.refresh();
        QCOMPARE(sharing.findChild<QLabel *>("networkMapNetwork")->text(), QStringLiteral("No active network"));
        QVERIFY(sharing.findChild<QLabel *>("networkActiveConnections")->text().contains(QStringLiteral("not currently connected")));
    }
    void sharingCanKeepAndRevertSharedCheckpointWithoutNavigation()
    {
        auto *backend = new PageNetworkBackend;
        NetworkSettings session(backend);
        QTRY_VERIFY(session.isReady());
        NetworkSharingPage sharing(sidebar(), &session, nullptr);
        NetworkConnectionsPage connections(sidebar(), &session, nullptr);
        connections.findChild<QPlainTextEdit *>("networkProfileName")->setPlainText(QStringLiteral("Keep from sharing"));
        connections.findChild<QPushButton *>("networkApply")->click();
        QVERIFY(session.awaitingConfirmation());
        auto *keep = sharing.findChild<QPushButton *>("networkSharingKeep");
        auto *undo = sharing.findChild<QPushButton *>("networkSharingRevertPreview");
        QVERIFY(!keep->isHidden()); QVERIFY(keep->isEnabled());
        QVERIFY(sharing.findChild<QLabel *>("networkSharingConfirmation")->text().contains(QStringLiteral("20 seconds")));
        keep->click();
        QCOMPARE(backend->keeps, 1); QVERIFY(!session.isBusy());
        QVERIFY(keep->isHidden()); QVERIFY(undo->isHidden());
        connections.findChild<QPlainTextEdit *>("networkProfileName")->setPlainText(QStringLiteral("Revert from sharing"));
        connections.findChild<QPushButton *>("networkApply")->click();
        QVERIFY(session.awaitingConfirmation());
        undo->click();
        QCOMPARE(backend->reverts, 1); QVERIFY(!session.isBusy());
        QCOMPARE(session.stagedPreferences().id, QStringLiteral("Keep from sharing"));
    }
    void stagedFormAndCheckpointSurvivePageRecreation()
    {
        auto *backend = new PageNetworkBackend;
        NetworkSettings session(backend);
        QTRY_VERIFY(session.isReady());
        auto *page = new NetworkConnectionsPage(sidebar(), &session, nullptr);
        auto *name = page->findChild<QPlainTextEdit *>("networkProfileName");
        name->setPlainText(QStringLiteral("New Wi-Fi name"));
        QVERIFY(session.isDirty()); QCOMPARE(backend->changes, 0);
        QVERIFY(!page->findChild<QPushButton *>("networkWifiConnect")->isEnabled());
        delete page;
        page = new NetworkConnectionsPage(sidebar(), &session, nullptr);
        QCOMPARE(page->findChild<QPlainTextEdit *>("networkProfileName")->toPlainText(), QStringLiteral("New Wi-Fi name"));
        QCOMPARE(page->settingsSession(), static_cast<SettingsSession *>(&session));
        page->findChild<QPushButton *>("networkApply")->click();
        QCOMPARE(backend->changes, 1); QVERIFY(session.awaitingConfirmation());
        delete page;
        page = new NetworkConnectionsPage(sidebar(), &session, nullptr);
        QVERIFY(!page->findChild<QPushButton *>("networkKeep")->isHidden());
        QVERIFY(!page->findChild<QPushButton *>("networkApply")->isEnabled());
        page->findChild<QPushButton *>("networkRevertPreview")->click();
        QCOMPARE(backend->reverts, 1); QVERIFY(!session.isBusy());
        QCOMPARE(page->findChild<QPlainTextEdit *>("networkProfileName")->toPlainText(), QStringLiteral("Shared name"));
        delete page;
    }
    void passwordIsMaskedCancelDoesNotDispatchAndStablePathConnects()
    {
        auto *backend = new PageNetworkBackend;
        NetworkSettings session(backend);
        QTRY_VERIFY(session.isReady());
        NetworkConnectionsPage page(sidebar(), &session, nullptr);
        page.findChild<QTabWidget *>("networkTabs")->setCurrentIndex(1);
        auto *aps = page.findChild<QTreeWidget *>("networkAccessPoints");
        choose(aps, QStringLiteral("/aps/2"));
        QTimer::singleShot(0, &page, [] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            auto *password = dialog->findChild<QLineEdit *>("networkPassword");
            QVERIFY(password); QCOMPARE(password->echoMode(), QLineEdit::Password);
            password->setText(QStringLiteral("must never be dispatched"));
            capture(*dialog, QStringLiteral("network-password"));
            dialog->reject();
        });
        page.findChild<QPushButton *>("networkWifiConnect")->click();
        QCOMPARE(backend->changes, 0);
        QVERIFY(!page.findChild<QLineEdit *>("networkPassword"));
        QTimer::singleShot(0, &page, [] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            dialog->findChild<QLineEdit *>("networkPassword")->setText(QStringLiteral("correct horse"));
            dialog->accept();
        });
        page.findChild<QPushButton *>("networkWifiConnect")->click();
        QCOMPARE(backend->changedPath, QStringLiteral("/aps/2"));
        QVERIFY(backend->expectedSecret); QVERIFY(session.awaitingConfirmation());
        page.findChild<QPushButton *>("networkKeep")->click();
        QCOMPARE(backend->keeps, 1); QVERIFY(!session.isBusy());
    }
    void unsupportedAdaptersAndSecurityAreReadOnlyAndScanIsExplicit()
    {
        auto *backend = new PageNetworkBackend;
        NetworkSettings session(backend);
        QTRY_VERIFY(session.isReady());
        NetworkConnectionsPage page(sidebar(), &session, nullptr);
        auto *devices = page.findChild<QTreeWidget *>("networkDevices");
        QCOMPARE(devices->topLevelItemCount(), 3);
        choose(devices, QStringLiteral("/devices/3"));
        QVERIFY(!page.findChild<QPushButton *>("networkDisconnect")->isEnabled());
        QVERIFY(page.findChild<QLabel *>("networkDeviceDetails")->text().contains(QStringLiteral("Loopback")));
        choose(page.findChild<QTreeWidget *>("networkAccessPoints"), QStringLiteral("/aps/3"));
        QVERIFY(!page.findChild<QPushButton *>("networkWifiConnect")->isEnabled());
        QVERIFY(page.findChild<QPushButton *>("networkWifiConnect")->toolTip().contains(QStringLiteral("unsupported")));
        QCOMPARE(backend->scans, 0);
        page.findChild<QPushButton *>("networkScan")->click();
        QCOMPARE(backend->scans, 1);
        page.findChild<QTabWidget *>("networkTabs")->setCurrentIndex(2);
        choose(page.findChild<QTreeWidget *>("networkProfiles"), QStringLiteral("/profiles/2"));
        QCOMPARE(session.selectedProfile(), QStringLiteral("/profiles/2"));
        capture(page, QStringLiteral("network-connections"));
    }
    void plainInputsKeepSeparatorsAndUnavailableServiceCanBeRetried()
    {
        auto *backend = new PageNetworkBackend;
        NetworkSettings session(backend);
        QTRY_VERIFY(session.isReady());
        NetworkConnectionsPage page(sidebar(), &session, nullptr);
        page.findChild<QTabWidget *>("networkTabs")->setCurrentIndex(2);
        page.resize(1050, 790); page.show();
        auto *dns = page.findChild<QPlainTextEdit *>("networkIpv4Dns");
        dns->setFocus();
        QTest::keyClicks(dns, "1.1.1.1, 9.9.9.9");
        QCOMPARE(dns->toPlainText(), QStringLiteral("1.1.1.1, 9.9.9.9"));
        QCOMPARE(session.stagedPreferences().ipv4.dns, QStringList({QStringLiteral("1.1.1.1"), QStringLiteral("9.9.9.9")}));
        page.findChild<QPushButton *>("networkRevert")->click();
        backend->current.serviceAvailable = false;
        session.refresh();
        QVERIFY(!session.isReady());
        QVERIFY(page.findChild<QPushButton *>("networkRefresh")->isEnabled());
        QVERIFY(!page.findChild<QPushButton *>("networkApply")->isEnabled());
        backend->current.serviceAvailable = true;
        page.findChild<QPushButton *>("networkRefresh")->click();
        QVERIFY(session.isReady());
    }
    void invalidIpAndAuthenticationErrorsStayVisible()
    {
        auto *backend = new PageNetworkBackend;
        NetworkSettings session(backend);
        QTRY_VERIFY(session.isReady());
        NetworkConnectionsPage page(sidebar(), &session, nullptr);
        auto *method = page.findChild<QComboBox *>("networkIpv4Method");
        method->setCurrentIndex(method->findData(QStringLiteral("manual")));
        page.findChild<QPlainTextEdit *>("networkIpv4Addresses")->setPlainText(QStringLiteral("not-an-address"));
        page.findChild<QPushButton *>("networkApply")->click();
        QCOMPARE(backend->changes, 0);
        QVERIFY(page.findChild<QLabel *>("networkStatus")->text().contains(QStringLiteral("valid address")));
        page.findChild<QPushButton *>("networkRevert")->click();
        QVERIFY(!session.isDirty());
        backend->failAuthentication = true;
        choose(page.findChild<QTreeWidget *>("networkAccessPoints"), QStringLiteral("/aps/2"));
        QTimer::singleShot(0, &page, [] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget()); QVERIFY(dialog);
            dialog->findChild<QLineEdit *>("networkPassword")->setText(QStringLiteral("correct horse")); dialog->accept();
        });
        page.findChild<QPushButton *>("networkWifiConnect")->click();
        QVERIFY(page.findChild<QLabel *>("networkStatus")->text().contains(QStringLiteral("Authentication failed")));
        session.refresh();
        QVERIFY(page.findChild<QLabel *>("networkStatus")->text().contains(QStringLiteral("Authentication failed")));
        QVERIFY(!page.findChild<QLabel *>("networkStatus")->text().contains(QStringLiteral("correct horse")));
    }
};
QTEST_MAIN(NetworkPagesTest)
#include "network_pages_test.moc"
