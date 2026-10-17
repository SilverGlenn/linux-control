#include <QtTest>
#include <QDBusVirtualObject>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusMetaType>
#include <QDBusVariant>
#include <QProcess>
#include <QSignalSpy>
#include <QTemporaryDir>
#include "platform/network/NetworkManagerBackend.h"

// Private-bus protocol fixtures are defined below; no system network writes.
namespace {
const QString nm = QStringLiteral("org.freedesktop.NetworkManager");
const QString root = QStringLiteral("/org/freedesktop/NetworkManager");
const QString wired = root + QStringLiteral("/Devices/1");
const QString wifi = root + QStringLiteral("/Devices/2");
const QString profile1 = root + QStringLiteral("/Settings/1");
const QString profile2 = root + QStringLiteral("/Settings/2");
const QString apPath = root + QStringLiteral("/AccessPoint/1");
const QString checkpointPath = root + QStringLiteral("/Checkpoint/1");
QVariant paths(const QStringList &list)
{
    QList<QDBusObjectPath> value;
    for (const auto &entry : list) value << QDBusObjectPath(entry);
    return QVariant::fromValue(value);
}
QString objectPath(const QVariant &v) { return qvariant_cast<QDBusObjectPath>(v).path(); }
struct Profile {
    NetworkConnectionData settings, disk;
    quint64 version = 1;
    bool unsaved = false;
    uint flags = 0;
    QString secret = QStringLiteral("saved-secret-must-survive");
};
struct Device {
    uint type = 1, state = 30;
    QString active = QStringLiteral("/"), profile = QStringLiteral("/");
    NetworkConnectionData applied;
    quint64 appliedVersion = 1;
};
class FakeNetworkManager : public QDBusVirtualObject {
    Q_OBJECT
public:
    explicit FakeNetworkManager(const QDBusConnection &connection) : bus(connection) { reset(); }
    QDBusConnection bus;
    QMap<QString, Profile> profiles, checkpointProfiles;
    QMap<QString, Device> devices, checkpointDevices;
    QMap<QString, QString> permissions;
    QStringList mutations, methods, signatures;
    QList<bool> interactive;
    bool checkpointSupported = true, checkpoint = false, delayActivation = false, activationFails = false;
    bool raceRead = false, omitActivation = false, dropCheckpoint = false;
    bool externalEditDuringAuthorization = false;
    bool transientReadFailureAfterRollback = false, failNextSettingsRead = false;
    QString checkpointError, activationError;
    QString receivedPassword;
    uint checkpointFlags = 999, checkpointTimeout = 0;
    QStringList checkpointDevicePaths;
    int nextActive = 1, nextProfile = 3, cpSerial = 0;
    int scanCalls = 0;
    NetworkConnectionData lastUpdate;
    uint lastUpdateFlags = 0;
    quint64 lastCas = 0;
    void reset()
    {
        mutations.clear(); methods.clear(); signatures.clear(); interactive.clear();
        profiles.clear(); devices.clear(); checkpointProfiles.clear(); checkpointDevices.clear();
        checkpointSupported = true; checkpoint = delayActivation = activationFails = raceRead = omitActivation = dropCheckpoint = false;
        externalEditDuringAuthorization = false;
        transientReadFailureAfterRollback = failNextSettingsRead = false;
        checkpointError.clear(); activationError.clear(); receivedPassword.clear();
        checkpointFlags = 999; checkpointTimeout = 0; nextActive = 1; nextProfile = 3; scanCalls = 0;
        checkpointDevicePaths.clear();
        lastUpdate.clear(); lastUpdateFlags = 0; lastCas = 0;
        ++cpSerial;
        Profile ethernet;
        ethernet.settings = {{"connection", {{"id", "Ethernet"}, {"uuid", "00000000-0000-0000-0000-000000000001"}, {"type", "802-3-ethernet"}, {"autoconnect", true}, {"zone", "trusted"}}},
            {"802-3-ethernet", {{"mtu", uint(1400)}}}, {"ipv4", {{"method", "auto"}, {"route-metric", qint64(55)}, {"never-default", true}}},
            {"ipv6", {{"method", "auto"}, {"ip6-privacy", int(2)}}}, {"proxy", {{"method", uint(0)}, {"pac-url", "https://example.invalid/proxy"}}}};
        ethernet.disk = ethernet.settings;
        profiles.insert(profile1, ethernet);
        Profile wireless;
        wireless.settings = {{"connection", {{"id", "Saved Wi-Fi"}, {"uuid", "00000000-0000-0000-0000-000000000002"}, {"type", "802-11-wireless"}, {"autoconnect", true}}},
            {"802-11-wireless", {{"ssid", QByteArray("Saved Wi-Fi")}, {"mode", "infrastructure"}, {"hidden", false}}},
            {"802-11-wireless-security", {{"key-mgmt", "wpa-psk"}, {"psk-flags", uint(0)}}},
            {"ipv4", {{"method", "auto"}}}, {"ipv6", {{"method", "auto"}}}};
        wireless.disk = wireless.settings;
        profiles.insert(profile2, wireless);
        devices.insert(wired, {1, 100, root + "/ActiveConnection/1", profile1});
        devices.insert(wifi, {2, 30, "/", "/"});
        devices[wired].applied = ethernet.settings;
        permissions = {{nm + ".network-control", "yes"}, {nm + ".checkpoint-rollback", "auth"},
            {nm + ".settings.modify.system", "yes"}, {nm + ".settings.modify.own", "yes"}, {nm + ".wifi.scan", "yes"}};
    }
    QString introspect(const QString &path) const override
    {
        if (path == root) {
            QString xml = QStringLiteral("<interface name='org.freedesktop.NetworkManager'>"
                "<method name='GetPermissions'><arg type='a{ss}' direction='out'/></method>"
                "<method name='ActivateConnection'><arg type='o' direction='in'/><arg type='o' direction='in'/><arg type='o' direction='in'/><arg type='o' direction='out'/></method>"
                "<method name='AddAndActivateConnection2'><arg type='a{sa{sv}}' direction='in'/><arg type='o' direction='in'/><arg type='o' direction='in'/><arg type='a{sv}' direction='in'/><arg type='o' direction='out'/><arg type='o' direction='out'/><arg type='a{sv}' direction='out'/></method>");
            if (checkpointSupported) xml += QStringLiteral("<method name='CheckpointCreate'><arg type='ao' direction='in'/><arg type='u' direction='in'/><arg type='u' direction='in'/><arg type='o' direction='out'/></method>"
                "<method name='CheckpointRollback'><arg type='o' direction='in'/><arg type='a{su}' direction='out'/></method>"
                "<method name='CheckpointDestroy'><arg type='o' direction='in'/></method>");
            return xml + QStringLiteral("</interface>");
        }
        if (path.startsWith(root + "/Settings/")) return QStringLiteral("<interface name='org.freedesktop.NetworkManager.Settings.Connection'><method name='GetSettings'><arg type='a{sa{sv}}' direction='out'/></method><method name='Update2'><arg type='a{sa{sv}}' direction='in'/><arg type='u' direction='in'/><arg type='a{sv}' direction='in'/><arg type='a{sv}' direction='out'/></method></interface>");
        return QString();
    }
    void rollback()
    {
        checkpoint = false; ++cpSerial;
        devices = checkpointDevices;
        for (auto it = checkpointProfiles.begin(); it != checkpointProfiles.end(); ++it) {
            Profile copy = it.value();
            copy.version = profiles.value(it.key()).version + 1;
            profiles[it.key()] = copy;
        }
        for (const QString &path : profiles.keys()) if (profiles[path].flags & 4u) profiles.remove(path);
        if (transientReadFailureAfterRollback) failNextSettingsRead = true;
    }
    bool handleMessage(const QDBusMessage &message, const QDBusConnection &connection) override
    {
        const QString method = message.member(), path = message.path(), interface = message.interface();
        methods << method;
        auto reply = [&](const QVariantList &arguments = QVariantList()) { connection.send(message.createReply(arguments)); };
        auto error = [&](const QString &name) { connection.send(message.createErrorReply(name, "untrusted diagnostic saved-secret-must-survive")); };
        if (interface == "org.freedesktop.DBus.Introspectable" && method == "Introspect") {
            reply({"<node>" + introspect(path) + "</node>"}); return true;
        }
        if (interface == "org.freedesktop.DBus.Properties" && method == "GetAll") {
            QVariantMap p;
            const QString requested = message.arguments().value(0).toString();
            if (path == root && requested == nm) {
                p = {{"Version", "1.54.3"}, {"Devices", paths(devices.keys())}, {"Checkpoints", paths(checkpoint ? QStringList{checkpointPath} : QStringList())},
                    {"Connectivity", uint(4)}, {"State", uint(70)}, {"ConnectivityCheckEnabled", false}, {"ConnectivityCheckAvailable", false},
                    {"WirelessEnabled", true}, {"WirelessHardwareEnabled", true}, {"NetworkingEnabled", true}};
            } else if (devices.contains(path) && requested == nm + ".Device") {
                const auto d = devices[path];
                p = {{"Interface", d.type == 1 ? "eth0" : "wlan0"}, {"DeviceType", d.type}, {"State", d.state}, {"Managed", true},
                    {"ActiveConnection", QVariant::fromValue(QDBusObjectPath(d.active))}, {"AvailableConnections", paths({d.type == 1 ? profile1 : profile2})},
                    {"Ip4Config", QVariant::fromValue(QDBusObjectPath("/"))}, {"Ip6Config", QVariant::fromValue(QDBusObjectPath("/"))}};
            } else if (path == wired && requested == nm + ".Device.Wired") p = {{"Carrier", true}};
            else if (path == wifi && requested == nm + ".Device.Wireless") p = {{"AccessPoints", paths({apPath})}, {"ActiveAccessPoint", QVariant::fromValue(QDBusObjectPath(devices[wifi].state == 100 ? apPath : "/"))}};
            else if (path == apPath) p = {{"Ssid", QByteArray("A new network")}, {"Strength", uint(81)}, {"Mode", uint(2)}, {"Flags", uint(1)}, {"WpaFlags", uint(0)}, {"RsnFlags", uint(0x100)}};
            else if (profiles.contains(path)) p = {{"VersionId", QVariant::fromValue(qulonglong(profiles[path].version))}, {"Flags", profiles[path].flags}, {"Unsaved", profiles[path].unsaved}, {"Filename", profiles[path].unsaved ? "/run/fake.nmconnection" : "/etc/fake.nmconnection"}};
            else if (path.startsWith(root + "/ActiveConnection/")) {
                for (const auto &d : devices) if (d.active == path) p = {{"Connection", QVariant::fromValue(QDBusObjectPath(d.profile))}, {"State", uint(d.state == 100 ? 2 : 1)}};
            } else { error("org.freedesktop.DBus.Error.UnknownObject"); return true; }
            reply({p}); return true;
        }
        if (method == "GetPermissions") { reply({QVariant::fromValue(permissions)}); return true; }
        if (method == "ListConnections") { reply({paths(profiles.keys())}); return true; }
        if (method == "GetSettings" && profiles.contains(path)) {
            if (failNextSettingsRead) { failNextSettingsRead = false; error("org.freedesktop.DBus.Error.UnknownObject"); return true; }
            reply({QVariant::fromValue(profiles[path].settings)});
            if (raceRead) { raceRead = false; profiles[path].version++; profiles[path].settings["connection"]["id"] = "external edit"; }
            return true;
        }
        if (method == "GetAppliedConnection" && devices.contains(path)) {
            reply({QVariant::fromValue(devices[path].applied), QVariant::fromValue(qulonglong(devices[path].appliedVersion))}); return true;
        }
        mutations << method; signatures << message.signature(); interactive << message.isInteractiveAuthorizationAllowed();
        if (method == "RequestScan") { ++scanCalls; reply(); return true; }
        if (method == "CheckpointCreate") {
            checkpointFlags = message.arguments().value(2).toUInt(); checkpointTimeout = message.arguments().value(1).toUInt();
            for (const auto &p : qdbus_cast<QList<QDBusObjectPath>>(message.arguments().value(0))) checkpointDevicePaths << p.path();
            for (const auto &p : checkpointDevicePaths) if (devices[p].type != 1 && devices[p].type != 2) {
                error(nm + ".UnknownDevice"); return true;
            }
            if (!checkpointError.isEmpty()) {
                if (externalEditDuringAuthorization) {
                    profiles[profile1].settings["connection"]["id"] = "Edited during authorization";
                    profiles[profile1].disk = profiles[profile1].settings;
                    ++profiles[profile1].version;
                }
                error(checkpointError); return true;
            }
            if (!checkpointSupported || checkpoint) { error(nm + ".InvalidArguments"); return true; }
            checkpoint = true; checkpointDevices = devices; checkpointProfiles.clear();
            for (const auto &d : devices) if (profiles.contains(d.profile)) checkpointProfiles[d.profile] = profiles[d.profile];
            const int serial = ++cpSerial;
            QTimer::singleShot(int(checkpointTimeout * 1000), this, [this, serial] { if (checkpoint && serial == cpSerial) rollback(); });
            reply({QVariant::fromValue(QDBusObjectPath(checkpointPath))}); return true;
        }
        if (method == "CheckpointRollback") {
            rollback(); reply({QVariant::fromValue(QMap<QString, uint>{{wired, 0}, {wifi, 0}})}); return true;
        }
        if (method == "CheckpointDestroy") { checkpoint = false; ++cpSerial; reply(); return true; }
        if (method == "Update2" && profiles.contains(path)) {
            lastUpdate = qdbus_cast<NetworkConnectionData>(message.arguments().value(0));
            lastUpdateFlags = message.arguments().value(1).toUInt();
            const auto args = qdbus_cast<QVariantMap>(message.arguments().value(2));
            lastCas = args.value("version-id").toULongLong();
            if (lastCas != profiles[path].version) { error(nm + ".Settings.VersionIdMismatch"); return true; }
            auto &p = profiles[path];
            // Exact NM 1.54.3 update_auth_cb semantic: a settings replacement
            // containing no secrets retains existing system/agent secrets.
            if (!lastUpdate.isEmpty()) p.settings = lastUpdate;
            ++p.version;
            p.unsaved = !(lastUpdateFlags & 1u); p.flags = p.unsaved ? 1u : 0u;
            if (!p.unsaved) p.disk = p.settings;
            reply({QVariantMap()}); return true;
        }
        if (method == "Disconnect" && devices.contains(path)) { devices[path].state = 30; devices[path].active = "/"; devices[path].profile = "/"; reply(); return true; }
        if (method == "ActivateConnection" || method == "AddAndActivateConnection2") {
            if (!activationError.isEmpty()) { error(activationError); return true; }
            const bool adding = method == "AddAndActivateConnection2";
            const QString devicePath = objectPath(message.arguments().value(1));
            QString target = adding ? root + "/Settings/" + QString::number(nextProfile++) : objectPath(message.arguments().value(0));
            if (adding) {
                Profile p; p.settings = qdbus_cast<NetworkConnectionData>(message.arguments().value(0));
                receivedPassword = p.settings["802-11-wireless-security"].take("psk").toString();
                if (p.settings["802-11-wireless-security"].isEmpty()) p.settings.remove("802-11-wireless-security");
                p.secret = receivedPassword; p.unsaved = true; p.flags = 1 | 4; profiles[target] = p;
                const auto options = qdbus_cast<QVariantMap>(message.arguments().value(3));
                if (options.value("persist") != "volatile") { error(nm + ".InvalidArguments"); return true; }
            }
            const QString active = root + "/ActiveConnection/" + QString::number(++nextActive);
            auto complete = [this, message, connection, devicePath, target, active, adding] {
                if (!omitActivation) { devices[devicePath].active = active; devices[devicePath].profile = target; devices[devicePath].state = activationFails ? 120u : 100u; devices[devicePath].applied = profiles[target].settings; ++devices[devicePath].appliedVersion; }
                if (dropCheckpoint) { checkpoint = false; ++cpSerial; }
                connection.send(message.createReply(adding ? QVariantList{QVariant::fromValue(QDBusObjectPath(target)), QVariant::fromValue(QDBusObjectPath(active)), QVariantMap()}
                    : QVariantList{QVariant::fromValue(QDBusObjectPath(active))}));
            };
            if (delayActivation) QTimer::singleShot(500, this, complete); else complete();
            return true;
        }
        error("org.freedesktop.DBus.Error.UnknownMethod"); return true;
    }
};
}

class NetworkBackendTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase()
    {
        NetworkTypes::registerDbusTypes();
        qDBusRegisterMetaType<QList<QDBusObjectPath>>(); qDBusRegisterMetaType<QMap<QString, QString>>(); qDBusRegisterMetaType<QMap<QString, uint>>();
        daemon.start("dbus-daemon", {"--session", "--nofork", "--print-address=1", "--address=unix:path=" + dir.path() + "/bus"});
        QVERIFY(daemon.waitForStarted(2000)); QVERIFY(daemon.waitForReadyRead(2000));
        const QString address = QString::fromUtf8(daemon.readLine()).trimmed();
        server = new QDBusConnection(QDBusConnection::connectToBus(address, "network-fake-server"));
        client = new QDBusConnection(QDBusConnection::connectToBus(address, "network-fake-client"));
        QVERIFY(server->isConnected()); QVERIFY(client->isConnected());
        QVERIFY(server->registerService(service));
        fake = new FakeNetworkManager(*server);
        QVERIFY(server->registerVirtualObject(root, fake, QDBusConnection::SubPath));
    }
    void cleanupTestCase()
    {
        delete fake;
        QDBusConnection::disconnectFromBus("network-fake-client"); QDBusConnection::disconnectFromBus("network-fake-server");
        delete client; delete server;
        daemon.terminate(); daemon.waitForFinished(2000);
    }
    void init() { fake->reset(); }
    void inventoryIsReadOnly()
    {
        NetworkManagerBackend backend(*client, options());
        QSignalSpy reads(&backend, &NetworkBackend::readFinished);
        backend.readCurrent(); QTRY_COMPARE(reads.size(), 1);
        QVERIFY(reads.first().at(0).toBool());
        const auto snapshot = reads.first().at(1).value<NetworkSnapshot>();
        QCOMPARE(snapshot.devices.size(), 2); QCOMPARE(snapshot.profiles.size(), 2); QCOMPARE(snapshot.accessPoints.size(), 1);
        QCOMPARE(snapshot.accessPoints.first().security, NetworkAccessPoint::Security::WpaPsk);
        QVERIFY(snapshot.checkpointAvailable); QVERIFY(snapshot.profile(profile1)->editable);
        QVERIFY(!snapshot.connectivityCheckEnabled); QVERIFY(fake->mutations.isEmpty());
        QVERIFY(!fake->methods.contains("GetSecrets"));
    }
    void activePreviewKeepsMemoryUntilKeep()
    {
        NetworkManagerBackend backend(*client, options());
        const auto baseline = read(backend); QVERIFY(baseline.isValid());
        auto request = edit(baseline, profile1); request.preferences.id = "Changed safely";
        QSignalSpy preview(&backend, &NetworkBackend::previewReady), finished(&backend, &NetworkBackend::transactionFinished);
        backend.beginChange(baseline, request); QTRY_COMPARE(preview.size(), 1);
        QCOMPARE(fake->mutations, QStringList({"CheckpointCreate", "Update2", "ActivateConnection"}));
        QCOMPARE(fake->signatures, QStringList({"aouu", "a{sa{sv}}ua{sv}", "ooo"}));
        QVERIFY(fake->interactive[0]); QVERIFY(!fake->interactive[1]); QVERIFY(!fake->interactive[2]);
        QCOMPARE(fake->checkpointFlags, 0u); QCOMPARE(fake->checkpointTimeout, 3u);
        QVERIFY(fake->profiles[profile1].unsaved); QCOMPARE(fake->profiles[profile1].disk, baseline.profile(profile1)->settings);
        QCOMPARE(fake->lastUpdateFlags, uint(2 | 0x40)); QCOMPARE(fake->lastCas, quint64(1));
        QCOMPARE(fake->lastUpdate["proxy"], baseline.profile(profile1)->settings["proxy"]);
        backend.keepChange(); QTRY_COMPARE(finished.size(), 1);
        QVERIFY(finished.first()[0].toBool()); QVERIFY(!finished.first()[1].toBool());
        QCOMPARE(fake->mutations.last(), QString("CheckpointDestroy")); QVERIFY(!fake->profiles[profile1].unsaved);
        QCOMPARE(fake->profiles[profile1].disk["connection"]["id"].toString(), QString("Changed safely"));
    }
    void activeRevertRestoresProfileAndRuntime()
    {
        NetworkManagerBackend backend(*client, options()); const auto baseline = read(backend);
        auto request = edit(baseline, profile1); request.preferences.autoconnect = false;
        QSignalSpy preview(&backend, &NetworkBackend::previewReady), finished(&backend, &NetworkBackend::transactionFinished);
        backend.beginChange(baseline, request); QTRY_COMPARE(preview.size(), 1);
        backend.revertChange(); QTRY_COMPARE(finished.size(), 1);
        QVERIFY(!finished.first()[0].toBool()); QVERIFY(finished.first()[1].toBool());
        QVERIFY(NetworkTypes::sameSettings(fake->profiles[profile1].settings, baseline.profile(profile1)->settings));
        QVERIFY(!fake->checkpoint); QCOMPARE(fake->devices[wired].profile, profile1);
    }
    void checkpointExcludesUnsupportedManagedPseudoDevices()
    {
        const QString p2p = root + "/Devices/30"; fake->devices.insert(p2p, {30, 30, "/", "/"});
        NetworkManagerBackend backend(*client, options()); const auto baseline = read(backend); QVERIFY(baseline.device(p2p));
        QSignalSpy preview(&backend, &NetworkBackend::previewReady), finished(&backend, &NetworkBackend::transactionFinished);
        backend.beginChange(baseline, edit(baseline, profile1)); QTRY_COMPARE(preview.size(), 1);
        QVERIFY(!fake->checkpointDevicePaths.contains(p2p)); QVERIFY(fake->checkpointDevicePaths.contains(wired)); QVERIFY(fake->checkpointDevicePaths.contains(wifi));
        backend.revertChange(); QTRY_COMPARE(finished.size(), 1); QVERIFY(finished.first()[1].toBool());
    }
    void expandedIpv6AddressMatchesNormalizedAppliedSettings()
    {
        NetworkManagerBackend backend(*client, options()); const auto baseline = read(backend);
        auto request = edit(baseline, profile1); request.preferences.ipv6.method = "manual";
        request.preferences.ipv6.addresses = {"2001:0db8:0000:0000:0000:0000:0000:0010/64"};
        request.preferences.ipv6.gateway = "2001:0db8:0000:0000:0000:0000:0000:0001";
        request.preferences.ipv6.dns = {"2001:4860:4860:0000:0000:0000:0000:8888"};
        QSignalSpy preview(&backend, &NetworkBackend::previewReady), finished(&backend, &NetworkBackend::transactionFinished);
        backend.beginChange(baseline, request); QTRY_COMPARE(preview.size(), 1);
        const auto addresses = NetworkTypes::addressData(fake->profiles[profile1].settings["ipv6"]["address-data"]);
        QCOMPARE(addresses.first().value("address").toString(), QString("2001:db8::10"));
        QCOMPARE(addresses.first().value("prefix").metaType(), QMetaType::fromType<uint>());
        backend.keepChange(); QTRY_COMPARE(finished.size(), 1); QVERIFY(finished.first()[0].toBool());
    }
    void inactiveSavePreservesSecretsAndNoConnectivityMutation()
    {
        NetworkManagerBackend backend(*client, options()); const auto baseline = read(backend);
        auto request = edit(baseline, profile2); request.preferences.ipv4.dns = {"1.1.1.1"};
        QSignalSpy preview(&backend, &NetworkBackend::previewReady), finished(&backend, &NetworkBackend::transactionFinished);
        backend.beginChange(baseline, request); QTRY_COMPARE(finished.size(), 1);
        QVERIFY(finished.first()[0].toBool()); QVERIFY(preview.isEmpty());
        QCOMPARE(fake->mutations, QStringList{"Update2"}); QCOMPARE(fake->lastUpdateFlags, uint(1 | 0x20 | 0x40));
        QCOMPARE(fake->profiles[profile2].secret, QString("saved-secret-must-survive"));
        QVERIFY(!fake->lastUpdate["802-11-wireless-security"].contains("psk"));
        QVERIFY(!fake->methods.contains("GetSecrets"));
        QVERIFY(fake->interactive.first());
    }
    void nativeWifiCredentialsAndScopedRollback()
    {
        NetworkManagerBackend backend(*client, options()); const auto baseline = read(backend);
        NetworkRequest request; request.action = NetworkRequest::Action::ConnectWifi; request.devicePath = wifi; request.accessPointPath = apPath; request.password = "transient-password-123";
        QSignalSpy preview(&backend, &NetworkBackend::previewReady), finished(&backend, &NetworkBackend::transactionFinished);
        backend.beginChange(baseline, request); request.password.clear(); QTRY_COMPARE(preview.size(), 1);
        QCOMPARE(fake->receivedPassword, QString("transient-password-123")); QCOMPARE(fake->checkpointFlags, 0u);
        QCOMPARE(fake->signatures.last(), QString("a{sa{sv}}ooa{sv}"));
        const auto current = preview.first()[0].value<NetworkSnapshot>();
        QVERIFY(!current.profile(fake->devices[wifi].profile)->settings["802-11-wireless-security"].contains("psk"));
        Profile external = fake->profiles[profile2]; external.settings["connection"]["uuid"] = "external";
        const QString externalPath = root + "/Settings/99"; fake->profiles[externalPath] = external;
        backend.revertChange(); QTRY_COMPARE(finished.size(), 1);
        QVERIFY(finished.first()[1].toBool()); QVERIFY(fake->profiles.contains(externalPath));
        QVERIFY(!fake->profiles.contains(root + "/Settings/3"));
    }
    void wifiKeepClearsVolatileAndPersists()
    {
        NetworkManagerBackend backend(*client, options()); const auto baseline = read(backend);
        NetworkRequest request; request.action = NetworkRequest::Action::ConnectWifi; request.devicePath = wifi; request.accessPointPath = apPath; request.password = "native-password-456";
        QSignalSpy preview(&backend, &NetworkBackend::previewReady), finished(&backend, &NetworkBackend::transactionFinished);
        backend.beginChange(baseline, request); QTRY_COMPARE(preview.size(), 1); backend.keepChange(); QTRY_COMPARE(finished.size(), 1);
        QVERIFY(finished.first()[0].toBool()); const auto p = fake->profiles[devicesProfile()];
        QVERIFY(!(p.flags & 4u)); QVERIFY(!p.unsaved); QCOMPARE(p.secret, QString("native-password-456")); QVERIFY(fake->lastUpdate.isEmpty());
    }
    void canceledAuthorizationDoesNotMutateOrExposeSecrets()
    {
        NetworkManagerBackend backend(*client, options()); const auto baseline = read(backend);
        fake->checkpointError = "org.freedesktop.PolicyKit1.Error.Cancelled";
        QSignalSpy finished(&backend, &NetworkBackend::transactionFinished);
        backend.beginChange(baseline, edit(baseline, profile1)); QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(fake->mutations, QStringList{"CheckpointCreate"}); QVERIFY(finished.first()[1].toBool());
        QVERIFY(finished.first()[3].toString().contains("canceled")); QVERIFY(!finished.first()[3].toString().contains("saved-secret"));
    }
    void deniedPermissionDoesNotCreateCheckpoint()
    {
        fake->permissions[nm + ".network-control"] = "auth";
        NetworkManagerBackend backend(*client, options()); const auto baseline = read(backend);
        QSignalSpy finished(&backend, &NetworkBackend::transactionFinished);
        NetworkRequest request; request.action = NetworkRequest::Action::Disconnect; request.devicePath = wired;
        backend.beginChange(baseline, request); QTRY_COMPARE(finished.size(), 1); QVERIFY(fake->mutations.isEmpty());
    }
    void unavailableCheckpointFailsClosed()
    {
        fake->checkpointSupported = false;
        NetworkManagerBackend backend(*client, options()); const auto baseline = read(backend);
        QSignalSpy finished(&backend, &NetworkBackend::transactionFinished);
        NetworkRequest request; request.action = NetworkRequest::Action::Disconnect; request.devicePath = wired;
        backend.beginChange(baseline, request); QTRY_COMPARE(finished.size(), 1); QVERIFY(fake->mutations.isEmpty());
    }
    void daemonAuthorizationDenialIsDistinctFromCancel()
    {
        NetworkManagerBackend backend(*client, options()); const auto baseline = read(backend);
        fake->checkpointError = nm + ".PermissionDenied";
        QSignalSpy finished(&backend, &NetworkBackend::transactionFinished);
        backend.beginChange(baseline, edit(baseline, profile1)); QTRY_COMPARE(finished.size(), 1);
        QVERIFY(finished.first()[3].toString().contains("denied")); QVERIFY(!finished.first()[3].toString().contains("canceled"));
        QCOMPARE(fake->mutations, QStringList{"CheckpointCreate"});
    }
    void externalEditDuringCanceledAuthorizationIsNotReportedRestored()
    {
        NetworkManagerBackend backend(*client, options()); const auto baseline = read(backend);
        fake->checkpointError = "org.freedesktop.PolicyKit1.Error.Cancelled"; fake->externalEditDuringAuthorization = true;
        QSignalSpy finished(&backend, &NetworkBackend::transactionFinished);
        backend.beginChange(baseline, edit(baseline, profile1)); QTRY_COMPARE(finished.size(), 1);
        QVERIFY(!finished.first()[0].toBool()); QVERIFY(!finished.first()[1].toBool());
        QCOMPARE(finished.first()[2].value<NetworkSnapshot>().profile(profile1)->id, QString("Edited during authorization"));
        QCOMPARE(fake->mutations, QStringList{"CheckpointCreate"});
    }
    void revertDuringPendingMutationDoesNotSendCompensatingWrite()
    {
        NetworkManagerBackend backend(*client, options()); const auto baseline = read(backend);
        fake->delayActivation = true;
        NetworkRequest request; request.action = NetworkRequest::Action::ActivateProfile; request.devicePath = wired; request.profilePath = profile1;
        QSignalSpy finished(&backend, &NetworkBackend::transactionFinished), preview(&backend, &NetworkBackend::previewReady);
        backend.beginChange(baseline, request); QTRY_VERIFY(fake->mutations.contains("ActivateConnection"));
        backend.revertChange(); QTRY_COMPARE(finished.size(), 1);
        QVERIFY(!finished.first()[0].toBool()); QVERIFY(!finished.first()[1].toBool());
        QVERIFY(fake->checkpoint); QVERIFY(!fake->mutations.contains("CheckpointRollback")); QVERIFY(!fake->mutations.contains("CheckpointDestroy"));
        QTest::qWait(600); QVERIFY(preview.isEmpty()); QCOMPARE(finished.size(), 1);
    }
    void appliedRuntimeConflictRefusesKeepAndStaleRollback()
    {
        NetworkManagerBackend backend(*client, options()); const auto baseline = read(backend);
        QSignalSpy preview(&backend, &NetworkBackend::previewReady), finished(&backend, &NetworkBackend::transactionFinished);
        backend.beginChange(baseline, edit(baseline, profile1)); QTRY_COMPARE(preview.size(), 1);
        ++fake->devices[wired].appliedVersion;
        fake->devices[wired].applied["ipv4"]["route-metric"] = qint64(999);
        backend.keepChange(); QTRY_COMPARE(finished.size(), 1);
        QVERIFY(!finished.first()[0].toBool()); QVERIFY(!finished.first()[1].toBool());
        QVERIFY(!fake->checkpoint); QVERIFY(fake->mutations.contains("CheckpointDestroy")); QVERIFY(!fake->mutations.contains("CheckpointRollback"));
        QTest::qWait(3100);
        QCOMPARE(fake->devices[wired].applied["ipv4"]["route-metric"].toLongLong(), qint64(999));
    }
    void secretOnlyExternalRevisionCancelsOwnedRecovery()
    {
        NetworkManagerBackend backend(*client, options()); const auto baseline = read(backend);
        QSignalSpy preview(&backend, &NetworkBackend::previewReady), finished(&backend, &NetworkBackend::transactionFinished);
        backend.beginChange(baseline, edit(baseline, profile1)); QTRY_COMPARE(preview.size(), 1);
        fake->profiles[profile1].secret = "new external secret";
        ++fake->profiles[profile1].version;
        backend.revertChange(); QTRY_COMPARE(finished.size(), 1);
        QVERIFY(!finished.first()[0].toBool()); QVERIFY(!finished.first()[1].toBool());
        QVERIFY(!fake->checkpoint); QVERIFY(fake->mutations.contains("CheckpointDestroy")); QVERIFY(!fake->mutations.contains("CheckpointRollback"));
        QTest::qWait(3100); QCOMPARE(fake->profiles[profile1].secret, QString("new external secret"));
    }
    void lostCheckpointPreventsKeepMutation()
    {
        NetworkManagerBackend backend(*client, options()); const auto baseline = read(backend);
        QSignalSpy preview(&backend, &NetworkBackend::previewReady), finished(&backend, &NetworkBackend::transactionFinished);
        backend.beginChange(baseline, edit(baseline, profile1)); QTRY_COMPARE(preview.size(), 1);
        fake->rollback();
        const int writes = fake->mutations.size();
        backend.keepChange(); QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(fake->mutations.size(), writes); QVERIFY(finished.first()[1].toBool());
    }
    void staleVersionAndPathInjectionRejected()
    {
        NetworkManagerBackend backend(*client, options()); const auto baseline = read(backend);
        auto request = edit(baseline, profile1);
        fake->profiles[profile1].version++;
        QSignalSpy finished(&backend, &NetworkBackend::transactionFinished);
        backend.beginChange(baseline, request); QTRY_COMPARE(finished.size(), 1); QVERIFY(fake->mutations.isEmpty());
        request.devicePath = "/org/freedesktop/systemd1";
        backend.beginChange(baseline, request); QTRY_COMPARE(finished.size(), 2); QVERIFY(fake->mutations.isEmpty());
    }
    void mixedRevisionSnapshotRejected()
    {
        fake->raceRead = true;
        NetworkManagerBackend backend(*client, options()); QSignalSpy reads(&backend, &NetworkBackend::readFinished);
        backend.readCurrent(); QTRY_COMPARE(reads.size(), 1); QVERIFY(!reads.first()[0].toBool()); QVERIFY(fake->mutations.isEmpty());
    }
    void lateActivationTimeoutLocksFurtherWrites()
    {
        auto opt = options(); opt.writeTimeoutMs = 100;
        NetworkManagerBackend backend(*client, opt); const auto baseline = read(backend);
        fake->delayActivation = true;
        NetworkRequest request; request.action = NetworkRequest::Action::ActivateProfile; request.devicePath = wired; request.profilePath = profile1;
        QSignalSpy finished(&backend, &NetworkBackend::transactionFinished), preview(&backend, &NetworkBackend::previewReady);
        backend.beginChange(baseline, request); QTRY_COMPARE(finished.size(), 1);
        QVERIFY(!finished.first()[0].toBool()); QVERIFY(!finished.first()[1].toBool()); QVERIFY(fake->checkpoint);
        QTest::qWait(650); QVERIFY(preview.isEmpty());
        const int writes = fake->mutations.size();
        const auto refreshed = read(backend); QVERIFY(refreshed.isValid()); QVERIFY(!refreshed.checkpointAvailable);
        QVERIFY(!refreshed.profile(profile1)->editable); QVERIFY(refreshed.profile(profile1)->reason.contains("locked"));
        QVERIFY(!finished.first()[2].value<NetworkSnapshot>().checkpointAvailable);
        backend.beginChange(refreshed, request); QTRY_COMPARE(finished.size(), 2);
        QCOMPARE(fake->mutations.size(), writes); QVERIFY(!fake->mutations.contains("CheckpointDestroy")); QVERIFY(!fake->mutations.contains("CheckpointRollback"));
    }
    void daemonTimeoutRestoresWithoutKeep()
    {
        NetworkManagerBackend backend(*client, options()); const auto baseline = read(backend);
        auto request = edit(baseline, profile1); request.preferences.id = "Temporary";
        QSignalSpy preview(&backend, &NetworkBackend::previewReady), finished(&backend, &NetworkBackend::transactionFinished);
        backend.beginChange(baseline, request); QTRY_COMPARE(preview.size(), 1);
        // Rollback legitimately replaces exported objects mid-read. Force one
        // transient read failure and require a stable, verified restoration.
        fake->transientReadFailureAfterRollback = true;
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 6000);
        QVERIFY(!finished.first()[0].toBool()); QVERIFY(finished.first()[1].toBool()); QVERIFY(!fake->checkpoint);
    }
    void scanUsesNativeAsyncProtocol()
    {
        NetworkManagerBackend backend(*client, options()); QSignalSpy scanned(&backend, &NetworkBackend::scanFinished);
        backend.scan(wifi); QTRY_COMPARE(scanned.size(), 1); QVERIFY(scanned.first()[0].toBool());
        QCOMPARE(fake->scanCalls, 1); QCOMPARE(fake->signatures.last(), QString("a{sv}")); QVERIFY(!fake->checkpoint);
    }
private:
    NetworkManagerBackend::Options options() const
    {
        NetworkManagerBackend::Options opt; opt.service = service; opt.rollbackSeconds = 3;
        opt.readTimeoutMs = 500; opt.snapshotTimeoutMs = 2000; opt.writeTimeoutMs = 200; opt.pollIntervalMs = 25; return opt;
    }
    NetworkSnapshot read(NetworkManagerBackend &backend)
    {
        QSignalSpy reads(&backend, &NetworkBackend::readFinished); backend.readCurrent();
        if (!reads.wait(3000) || !reads.first()[0].toBool()) return {};
        return reads.first()[1].value<NetworkSnapshot>();
    }
    NetworkRequest edit(const NetworkSnapshot &baseline, const QString &path)
    {
        NetworkRequest request; request.action = NetworkRequest::Action::UpdateProfile; request.profilePath = path;
        if (path == profile1) request.devicePath = wired;
        request.preferences = NetworkTypes::preferences(*baseline.profile(path));
        request.preferences.id += " edited"; return request;
    }
    QString devicesProfile() const { return fake->devices.value(wifi).profile; }
    QTemporaryDir dir;
    QProcess daemon;
    QDBusConnection *server = nullptr, *client = nullptr;
    FakeNetworkManager *fake = nullptr;
    const QString service = QStringLiteral("org.example.NetworkManagerFixture");
};

QTEST_GUILESS_MAIN(NetworkBackendTest)
#include "network_backend_test.moc"
