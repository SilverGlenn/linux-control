#include <QtTest>
#include <QDBusContext>
#include <QDBusMetaType>
#include <QDBusVariant>
#include <QProcess>
#include <QTemporaryDir>
#include <QScopeGuard>
#include "platform/kde/PowerPolicyBackend.h"

using PowerPortalSettings = QMap<QString, QVariantMap>;
Q_DECLARE_METATYPE(PowerPortalSettings)

class FakePowerObject final : public QObject, protected QDBusContext {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kde.Solid.PowerManagement")
public:
    bool display = true, sleep = true, sendSignal = true, failReload = false, delay = false;
    bool badActionReply = false, holdProfile = false, profileHeld = false;
    bool badReloadReply = false;
    QDBusMessage profileRequest;
    int reloads = 0;
    QDBusMessage delayed;
    QDBusConnection *bus = nullptr;
    void completeDelayed()
    {
        emit configurationReloaded();
        bus->send(delayed.createReply());
    }
public slots:
    QString currentProfile() {
        if (holdProfile) { setDelayedReply(true); profileHeld = true; profileRequest = message(); return {}; }
        return QStringLiteral("AC");
    }
    bool isLidPresent() const { return true; }
    bool isActionSupported(const QString &id) {
        if (badActionReply) { setDelayedReply(true); connection().send(message().createReply(QVariantList{QStringLiteral("true")})); return false; }
        return id == QLatin1String("DPMSControl") ? display : sleep;
    }
    void refreshStatus()
    {
        ++reloads;
        if (delay) {
            setDelayedReply(true);
            delayed = message();
            return;
        }
        if (sendSignal)
            emit configurationReloaded();
        if (badReloadReply) {
            setDelayedReply(true); connection().send(message().createReply(QVariantList{false})); return;
        }
        if (failReload)
            sendErrorReply(QStringLiteral("org.kde.PowerDevil.ReloadDenied"), QStringLiteral("Fake daemon denied reload"));
    }
signals:
    void configurationReloaded();
    void supportedActionsChanged();
    void profileChanged(const QString &profile);
};
class FakeFdoPowerObject final : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.PowerManagement")
public:
    bool canSuspend = true, canHibernate = true, hybrid = true, thenHibernate = true;
public slots:
    bool CanSuspend() const { return canSuspend; }
    bool CanHibernate() const { return canHibernate; }
    bool CanHybridSuspend() const { return hybrid; }
    bool CanSuspendThenHibernate() const { return thenHibernate; }
};
class FakeUPowerProperties final : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.UPower.Device")
    Q_PROPERTY(uint Type READ type)
    Q_PROPERTY(bool IsPresent READ isPresent)
    Q_PROPERTY(bool PowerSupply READ isPowerSupply)
public:
    bool hasBattery = true;
    uint type() const { return hasBattery ? 2u : 0u; }
    bool isPresent() const { return hasBattery; }
    bool isPowerSupply() const { return hasBattery; }
};
class FakeSystemdProperties final : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.systemd1.Manager")
    Q_PROPERTY(QString Virtualization MEMBER virtualization)
public:
    QString virtualization = QStringLiteral("");
};
class MalformedUPowerProperties final : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.UPower.Device")
    Q_PROPERTY(QString Type READ type CONSTANT)
    Q_PROPERTY(QString IsPresent READ present CONSTANT)
    Q_PROPERTY(QString PowerSupply READ present CONSTANT)
public:
    QString type() const { return QStringLiteral("2"); }
    QString present() const { return QStringLiteral("true"); }
};
class FakePortalSettings final : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.portal.Settings")
public:
    bool mobile = false;
public slots:
    PowerPortalSettings ReadAll(const QStringList &) const
    { return {{"org.kde.TabletMode", {{"enabled", mobile}, {"available", true}}}}; }
signals:
    void SettingChanged(const QString &group, const QString &key, const QDBusVariant &value);
};

class PowerPolicyDBusTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase()
    {
        qDBusRegisterMetaType<PowerPortalSettings>();
        QVERIFY(dir.isValid());
        // A private bus ensures every method and signal below is fake; no
        // actual PowerDevil configuration, reload or suspend is involved.
        process.start(QStringLiteral("dbus-daemon"), {QStringLiteral("--session"),
            QStringLiteral("--nofork"), QStringLiteral("--print-address=1"),
            QStringLiteral("--address=unix:path=%1/bus").arg(dir.path())});
        QVERIFY2(process.waitForStarted(2000), qPrintable(process.errorString()));
        QVERIFY2(process.waitForReadyRead(2000), qPrintable(QString::fromUtf8(process.readAllStandardError())));
        const QString address = QString::fromUtf8(process.readLine()).trimmed();
        QVERIFY(!address.isEmpty());
        server = std::make_unique<QDBusConnection>(QDBusConnection::connectToBus(address, QStringLiteral("power-policy-fake-server")));
        client = std::make_unique<QDBusConnection>(QDBusConnection::connectToBus(address, QStringLiteral("power-policy-fake-client")));
        QVERIFY(server->isConnected());
        QVERIFY(client->isConnected());
        for (const QString &service : {QStringLiteral("org.kde.Solid.PowerManagement"), QStringLiteral("org.freedesktop.PowerManagement"),
             QStringLiteral("org.freedesktop.UPower"), QStringLiteral("org.freedesktop.systemd1"), QStringLiteral("org.freedesktop.portal.Desktop")})
            QVERIFY(server->registerService(service));
        const auto flags = QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllSignals
            | QDBusConnection::ExportAllProperties;
        QVERIFY(server->registerObject(QStringLiteral("/org/kde/Solid/PowerManagement"), &power, flags));
        QVERIFY(server->registerObject(QStringLiteral("/org/freedesktop/PowerManagement"), &fdo, flags));
        QVERIFY(server->registerObject(QStringLiteral("/org/freedesktop/UPower/devices/DisplayDevice"), &upower, flags));
        QVERIFY(server->registerObject(QStringLiteral("/org/freedesktop/systemd1"), &systemd, flags));
        QVERIFY(server->registerObject(QStringLiteral("/org/freedesktop/portal/desktop"), &portal, flags));
        power.bus = server.get();
    }
    void cleanupTestCase()
    {
        client.reset();
        server.reset();
        QDBusConnection::disconnectFromBus(QStringLiteral("power-policy-fake-client"));
        QDBusConnection::disconnectFromBus(QStringLiteral("power-policy-fake-server"));
        process.terminate();
        if (!process.waitForFinished(1000)) {
            process.kill();
            process.waitForFinished(1000);
        }
    }
    void init()
    {
        power.display = power.sleep = power.sendSignal = true;
        power.failReload = power.delay = power.badActionReply = power.badReloadReply = power.holdProfile = power.profileHeld = false;
        fdo.canHibernate = fdo.hybrid = fdo.thenHibernate = true;
        fdo.canSuspend = upower.hasBattery = true;
        portal.mobile = false;
        systemd.virtualization = QStringLiteral("");
    }
    void discoversRealInterfaceShapesAndDefaultInputs()
    {
        PowerPolicyDBus daemon(*client, *client, true);
        QSignalSpy read(&daemon, &PowerPolicyDaemon::capabilitiesFinished);
        daemon.queryCapabilities();
        QTRY_COMPARE(read.size(), 1);
        const auto caps = qvariant_cast<PowerPolicyCapabilities>(read.first()[0]);
        QVERIFY(caps.serviceAvailable);
        QVERIFY(caps.schemaSupported);
        QVERIFY(caps.displaySupported);
        QVERIFY(caps.sleepSupported);
        QVERIFY(caps.suspendActionSupported && caps.canSuspendKnown && caps.canSuspend);
        QVERIFY(caps.hibernateKnown && caps.hibernateSupported && caps.hybridSuspendSupported && caps.suspendThenHibernateSupported);
        QVERIFY(caps.dimSupported && caps.brightnessSupported && caps.keyboardBrightnessSupported && caps.buttonSupported);
        QVERIFY(caps.lidKnown && caps.hasLid); QCOMPARE(caps.owner, server->baseService()); QVERIFY(!caps.revision.isEmpty());
        QVERIFY(caps.batteryKnown);
        QVERIFY(caps.hasBattery);
        QVERIFY(caps.mobileKnown);
        QVERIFY(!caps.mobile);
        QVERIFY(caps.virtualMachineKnown);
        QVERIFY(!caps.virtualMachine);
        portal.mobile = true;
        systemd.virtualization = QStringLiteral("kvm");
        fdo.canSuspend = false;
        upower.hasBattery = false;
        daemon.queryCapabilities();
        QTRY_COMPARE(read.size(), 2);
        const auto changed = qvariant_cast<PowerPolicyCapabilities>(read.last()[0]);
        QVERIFY(changed.mobile);
        QVERIFY(changed.virtualMachine);
        QVERIFY(!changed.sleepSupported);
        QVERIFY(!changed.hasBattery);
    }
    void reloadNeedsMethodReplyAndSignal()
    {
        PowerPolicyDBus daemon(*client, *client, true);
        prime(daemon);
        QSignalSpy finished(&daemon, &PowerPolicyDaemon::reloadFinished);
        daemon.reload();
        QTRY_COMPARE(finished.size(), 1);
        QVERIFY(finished.first()[0].toBool());
        QVERIFY(!finished.first()[1].toBool());
    }
    void hardwareAndPortalChangesInvalidateCapabilities()
    {
        PowerPolicyDBus daemon(*client, *client, true);
        QSignalSpy changed(&daemon, &PowerPolicyDaemon::changed);
        // Complete a round trip so AddMatch subscriptions are installed first.
        QSignalSpy ready(&daemon, &PowerPolicyDaemon::capabilitiesFinished);
        daemon.queryCapabilities();
        QTRY_COMPARE(ready.size(), 1);
        auto hardwareSignal = QDBusMessage::createSignal(
            QStringLiteral("/org/freedesktop/UPower/devices/DisplayDevice"),
            QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("PropertiesChanged"));
        hardwareSignal.setArguments({QStringLiteral("org.freedesktop.UPower.Device"),
            QVariantMap{{"IsPresent", false}}, QStringList{}});
        QVERIFY(server->send(hardwareSignal));
        QTRY_COMPARE(changed.size(), 1);
        emit portal.SettingChanged(QStringLiteral("org.kde.TabletMode"), QStringLiteral("enabled"), QDBusVariant(true));
        QTRY_COMPARE(changed.size(), 2);
        emit power.profileChanged(QStringLiteral("Battery"));
        QTRY_COMPARE(changed.size(), 3);
    }
    void signalWithFailedReplyDoesNotAcknowledgeReload()
    {
        PowerPolicyDBus daemon(*client, *client, true);
        prime(daemon);
        QSignalSpy finished(&daemon, &PowerPolicyDaemon::reloadFinished);
        power.failReload = true; // emits signal, then method error
        daemon.reload();
        QTRY_COMPARE(finished.size(), 1);
        QVERIFY(!finished.first()[0].toBool());
        QVERIFY(finished.first()[2].toString().contains("denied"));
    }
    void replyWithoutSignalTimesOut()
    {
        PowerPolicyDBus daemon(*client, *client, true);
        prime(daemon);
        QSignalSpy finished(&daemon, &PowerPolicyDaemon::reloadFinished);
        power.sendSignal = false;
        daemon.reload();
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 6000);
        QVERIFY(!finished.first()[0].toBool());
        QVERIFY(finished.first()[2].toString().contains("timeout"));
    }
    void cancellationAndLateReplyCompleteOnlyOnce()
    {
        PowerPolicyDBus daemon(*client, *client, true);
        prime(daemon);
        QSignalSpy finished(&daemon, &PowerPolicyDaemon::reloadFinished);
        power.delay = true;
        const int prior = power.reloads;
        daemon.reload();
        QTRY_COMPARE(power.reloads, prior + 1);
        daemon.cancel();
        QCOMPARE(finished.size(), 1);
        QVERIFY(!finished.first()[0].toBool());
        QVERIFY(finished.first()[1].toBool());
        power.completeDelayed();
        QTest::qWait(100);
        QCOMPARE(finished.size(), 1);
        power.delay = false;
        daemon.reload();
        QTRY_COMPARE(finished.size(), 2);
        QVERIFY(finished.last()[0].toBool());
    }
    void missingServiceAndSchemaAreUnavailable()
    {
        const QString missingName = QStringLiteral("power-policy-missing-connection");
        const auto missing = QDBusConnection::connectToBus(QStringLiteral("unix:path=%1/missing").arg(dir.path()), missingName);
        PowerPolicyDBus daemon(missing, missing, false);
        QSignalSpy read(&daemon, &PowerPolicyDaemon::capabilitiesFinished);
        daemon.queryCapabilities();
        QTRY_COMPARE(read.size(), 1);
        const auto caps = qvariant_cast<PowerPolicyCapabilities>(read.first()[0]);
        QVERIFY(!caps.serviceAvailable);
        QVERIFY(!caps.schemaSupported);
        QVERIFY(!caps.reason.isEmpty());
        QDBusConnection::disconnectFromBus(missingName);
    }
    void malformedBooleanCapabilitiesDoNotBecomeTruthy()
    {
        power.badActionReply = true; PowerPolicyDBus daemon(*client, *client, true);
        QSignalSpy read(&daemon, &PowerPolicyDaemon::capabilitiesFinished); daemon.queryCapabilities(); QTRY_COMPARE(read.size(), 1);
        const auto caps = qvariant_cast<PowerPolicyCapabilities>(read.first()[0]);
        QVERIFY(caps.serviceAvailable); QVERIFY(!caps.displaySupported && !caps.suspendActionSupported && !caps.sleepSupported);
        QVERIFY(!caps.dimSupported && !caps.brightnessSupported && !caps.keyboardBrightnessSupported && !caps.buttonSupported);
        QVERIFY(caps.canSuspendKnown && caps.canSuspend); // Independently typed facade replies remain valid.
    }
    void malformedUPowerPropertyTypesCannotEnableBatteryPolicies()
    {
        const auto path = QStringLiteral("/org/freedesktop/UPower/devices/DisplayDevice");
        MalformedUPowerProperties malformed; server->unregisterObject(path);
        const auto restore = qScopeGuard([&] { server->unregisterObject(path); server->registerObject(path, &upower, QDBusConnection::ExportAllProperties); });
        QVERIFY(server->registerObject(path, &malformed, QDBusConnection::ExportAllProperties));
        PowerPolicyDBus daemon(*client, *client, true); QSignalSpy read(&daemon, &PowerPolicyDaemon::capabilitiesFinished);
        daemon.queryCapabilities(); QTRY_COMPARE(read.size(), 1); const auto caps = qvariant_cast<PowerPolicyCapabilities>(read.first()[0]);
        QVERIFY(caps.serviceAvailable); QVERIFY(!caps.batteryKnown); QVERIFY(!caps.hasBattery);
    }
    void clientMobileEnvironmentHasIndependentNativeEditorDefaults_data()
    {
        QTest::addColumn<QByteArray>("name");
        QTest::newRow("quick-controls") << QByteArray("QT_QUICK_CONTROLS_MOBILE");
        QTest::newRow("kirigami") << QByteArray("KDE_KIRIGAMI_TABLET_MODE");
        QTest::newRow("portal-bypass") << QByteArray("QT_NO_XDG_DESKTOP_PORTAL");
    }
    void clientMobileEnvironmentHasIndependentNativeEditorDefaults()
    {
        QFETCH(QByteArray, name); const bool existed = qEnvironmentVariableIsSet(name.constData()); const auto original = qgetenv(name.constData());
        const auto restore = qScopeGuard([&] { if (existed) qputenv(name.constData(), original); else qunsetenv(name.constData()); });
        qputenv(name.constData(), "1"); portal.mobile = false;
        PowerPolicyDBus daemon(*client, *client, true); QSignalSpy read(&daemon, &PowerPolicyDaemon::capabilitiesFinished);
        daemon.queryCapabilities(); QTRY_COMPARE(read.size(), 1); const auto caps = qvariant_cast<PowerPolicyCapabilities>(read.first()[0]);
        QVERIFY(caps.serviceAvailable);
        QVERIFY(caps.defaultMobileKnown);
        QCOMPARE(caps.defaultMobile, name != QByteArray("QT_NO_XDG_DESKTOP_PORTAL"));
        // qputenv changes client editor inputs; /proc exposes the daemon's
        // original process environment and is queried independently.
        QVERIFY(caps.mobileKnown); QVERIFY(!caps.mobile);
        QVERIFY(caps.canSuspendKnown); QVERIFY(caps.virtualMachineKnown);
    }
    void ownerEpochRejectsACompositeCapabilityReplyFromBeforeReplacement()
    {
        PowerPolicyDBus daemon(*client, *client, true); power.holdProfile = true;
        QSignalSpy read(&daemon, &PowerPolicyDaemon::capabilitiesFinished), changed(&daemon, &PowerPolicyDaemon::changed);
        daemon.queryCapabilities(); QTRY_VERIFY(power.profileHeld); QVERIFY(read.isEmpty());
        QVERIFY(server->unregisterService(QStringLiteral("org.kde.Solid.PowerManagement"))); QTRY_VERIFY(!changed.isEmpty());
        QVERIFY(server->send(power.profileRequest.createReply(QVariantList{QStringLiteral("AC")}))); QTRY_COMPARE(read.size(), 1);
        const auto stale = qvariant_cast<PowerPolicyCapabilities>(read.first()[0]); QVERIFY(!stale.serviceAvailable); QVERIFY(!stale.reason.isEmpty());
        const int observed = changed.size();
        QVERIFY(server->registerService(QStringLiteral("org.kde.Solid.PowerManagement"))); power.holdProfile = false;
        QTRY_VERIFY(changed.size() > observed);
        daemon.queryCapabilities(); QTRY_COMPARE(read.size(), 2); QVERIFY(qvariant_cast<PowerPolicyCapabilities>(read.last()[0]).serviceAvailable);
    }
    void booleanFalseReplyCannotAcknowledgeTheNativeVoidReloadMethod()
    {
        PowerPolicyDBus daemon(*client, *client, true); prime(daemon); power.badReloadReply = true;
        QSignalSpy finished(&daemon, &PowerPolicyDaemon::reloadFinished); daemon.reload(); QTRY_COMPARE(finished.size(), 1);
        QVERIFY(!finished.first()[0].toBool());
    }
    void unrelatedUniqueSenderCannotAcknowledgeReload()
    {
        PowerPolicyDBus daemon(*client, *client, true); prime(daemon); power.delay = true;
        QSignalSpy finished(&daemon, &PowerPolicyDaemon::reloadFinished); const int calls = power.reloads; daemon.reload(); QTRY_COMPARE(power.reloads, calls + 1);
        auto spoof = QDBusMessage::createSignal(QStringLiteral("/org/kde/Solid/PowerManagement"), QStringLiteral("org.kde.Solid.PowerManagement"), QStringLiteral("configurationReloaded"));
        QVERIFY(client->send(spoof)); QTest::qWait(25); QVERIFY(finished.isEmpty());
        power.completeDelayed(); QTRY_COMPARE(finished.size(), 1); QVERIFY(finished.first()[0].toBool());
    }
    void guardedReloadRetiresUnsentRequests_data()
    {
        QTest::addColumn<int>("retirement");
        QTest::newRow("file-receipt-failure")<<0; QTest::newRow("owner-replacement")<<1;
        QTest::newRow("capability-change")<<2; QTest::newRow("explicit-cancel")<<3;
        QTest::newRow("daemon-deletion")<<4; QTest::newRow("success-once")<<5;
    }
    void guardedReloadRetiresUnsentRequests()
    {
        QFETCH(int,retirement);
        auto daemon=std::make_unique<PowerPolicyDBus>(*client,*client,true);
        QSignalSpy discovery(daemon.get(),&PowerPolicyDaemon::capabilitiesFinished);
        daemon->queryCapabilities(); QTRY_COMPARE(discovery.size(),1);
        const auto expected=qvariant_cast<PowerPolicyCapabilities>(discovery[0][0]); QVERIFY(expected.serviceAvailable);
        QSignalSpy finished(daemon.get(),&PowerPolicyDaemon::reloadFinished);
        QSignalSpy invalidated(daemon.get(),&PowerPolicyDaemon::providerChanged);
        PowerPolicyDaemon::GuardCompletion release;
        const int calls=power.reloads;
        daemon->reloadGuarded(expected,[&release](PowerPolicyDaemon::GuardCompletion done){release=std::move(done);});
        QTRY_VERIFY(bool(release)); QCOMPARE(power.reloads,calls); QVERIFY(finished.isEmpty());
        bool removed=false;
        const auto restore=qScopeGuard([&]{if(removed)server->registerService(QStringLiteral("org.kde.Solid.PowerManagement"));});
        if(retirement==0) release(false,QStringLiteral("Private source receipt changed"));
        else if(retirement==1) {
            QVERIFY(server->unregisterService(QStringLiteral("org.kde.Solid.PowerManagement"))); removed=true;
            QTRY_VERIFY(!invalidated.isEmpty());
            release(true,{});
        } else if(retirement==2) {
            emit power.supportedActionsChanged(); QTRY_VERIFY(!invalidated.isEmpty()); release(true,{});
        } else if(retirement==3) { daemon->cancel(); release(true,{}); }
        else if(retirement==4) { daemon.reset(); release(true,{}); }
        else { release(true,{}); release(true,{}); }
        if(retirement==4) { QTest::qWait(50); QVERIFY(finished.isEmpty()); }
        else {
            QTRY_COMPARE(finished.size(),1);
            QCOMPARE(finished[0][0].toBool(),retirement==5);
            QCOMPARE(finished[0][1].toBool(),retirement==3);
            if(retirement!=5) QVERIFY(!finished[0][2].toString().isEmpty());
        }
        QTest::qWait(30); QCOMPARE(power.reloads,calls+(retirement==5?1:0));
        if(retirement!=4) QCOMPARE(finished.size(),1);
    }
private:
    void prime(PowerPolicyDBus &daemon) {
        QSignalSpy ready(&daemon, &PowerPolicyDaemon::capabilitiesFinished); daemon.queryCapabilities(); QTRY_COMPARE(ready.size(), 1);
        QVERIFY(qvariant_cast<PowerPolicyCapabilities>(ready.first()[0]).serviceAvailable);
    }
    QTemporaryDir dir;
    QProcess process;
    std::unique_ptr<QDBusConnection> server, client;
    FakePowerObject power;
    FakeFdoPowerObject fdo;
    FakeUPowerProperties upower;
    FakeSystemdProperties systemd;
    FakePortalSettings portal;
};
QTEST_GUILESS_MAIN(PowerPolicyDBusTest)
#include "power_policy_dbus_test.moc"
