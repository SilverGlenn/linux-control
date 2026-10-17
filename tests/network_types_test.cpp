#include <QtTest>
#include <QDBusMetaType>
#include "settings/NetworkTypes.h"

namespace {
NetworkProfile fixture()
{
    NetworkProfile profile;
    profile.path = QStringLiteral("/profiles/1"); profile.id = QStringLiteral("Wired");
    profile.uuid = QStringLiteral("11111111-2222-3333-4444-555555555555");
    profile.type = QStringLiteral("802-3-ethernet"); profile.editable = true;
    profile.settings = {
        {QStringLiteral("connection"), {{QStringLiteral("id"), profile.id}, {QStringLiteral("uuid"), profile.uuid}, {QStringLiteral("type"), profile.type}, {QStringLiteral("autoconnect"), true}, {QStringLiteral("zone"), QStringLiteral("home")}}},
        {QStringLiteral("ipv4"), {{QStringLiteral("method"), QStringLiteral("auto")}, {QStringLiteral("route-metric"), qint64(42)}, {QStringLiteral("dns-search"), QStringList{QStringLiteral("example.test")}}}},
        {QStringLiteral("ipv6"), {{QStringLiteral("method"), QStringLiteral("auto")}, {QStringLiteral("addr-gen-mode"), 1}}},
        {QStringLiteral("802-3-ethernet"), {{QStringLiteral("mtu"), uint(1400)}, {QStringLiteral("cloned-mac-address"), QStringLiteral("stable")}}}
    };
    return profile;
}
}

class NetworkTypesTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { NetworkTypes::registerDbusTypes(); }
    void validateAddressFamiliesAndPrefixes()
    {
        QString error;
        NetworkIpConfig config;
        config.method = QStringLiteral("manual");
        QVERIFY(!NetworkTypes::validateIp(config, false, error));
        config.addresses = {QStringLiteral("192.0.2.10/24")};
        config.gateway = QStringLiteral("192.0.2.1"); config.dns = {QStringLiteral("192.0.2.53")};
        QVERIFY(NetworkTypes::validateIp(config, false, error));
        config.addresses = {QStringLiteral("192.0.2.10/33")};
        QVERIFY(!NetworkTypes::validateIp(config, false, error));
        config.addresses = {QStringLiteral("2001:db8::1/64")};
        QVERIFY(!NetworkTypes::validateIp(config, false, error));
        config.gateway = QStringLiteral("2001:db8::ff"); config.dns = {QStringLiteral("2001:db8::53")};
        QVERIFY(NetworkTypes::validateIp(config, true, error));
        config.addresses = {QStringLiteral("2001:db8::1/129")};
        QVERIFY(!NetworkTypes::validateIp(config, true, error));
        config.addresses = {QStringLiteral("2001:db8::1/64"), QStringLiteral("2001:0db8:0:0:0:0:0:1/64")};
        QVERIFY(!NetworkTypes::validateIp(config, true, error));
    }
    void rejectsDnsGatewayDuplicatesAndLimits()
    {
        QString error;
        NetworkIpConfig config;
        config.dns = {QStringLiteral("1.1.1.1"), QStringLiteral("1.1.1.1")};
        QVERIFY(!NetworkTypes::validateIp(config, false, error));
        config.dns = {QStringLiteral("2001:db8::53")};
        QVERIFY(!NetworkTypes::validateIp(config, false, error));
        config.dns = {QStringLiteral("resolver.example")};
        QVERIFY(!NetworkTypes::validateIp(config, false, error));
        config.dns.clear(); config.gateway = QStringLiteral("192.0.2.1");
        QVERIFY(!NetworkTypes::validateIp(config, false, error));
        config.gateway.clear(); config.method = QStringLiteral("shared");
        QVERIFY(!NetworkTypes::validateIp(config, false, error));
        config.method = QStringLiteral("manual");
        for (int i = 1; i <= 65; ++i) config.addresses.append(QStringLiteral("192.0.2.%1/24").arg(i));
        QVERIFY(!NetworkTypes::validateIp(config, false, error));
    }
    void passwordsFollowSecurityType()
    {
        QString error;
        using Security = NetworkAccessPoint::Security;
        QVERIFY(NetworkTypes::validatePassword(Security::Open, {}, error));
        QVERIFY(!NetworkTypes::validatePassword(Security::Unsupported, QStringLiteral("secret123"), error));
        QVERIFY(!NetworkTypes::validatePassword(Security::WpaPsk, QStringLiteral("short"), error));
        QVERIFY(NetworkTypes::validatePassword(Security::WpaPsk, QStringLiteral("secret123"), error));
        QVERIFY(NetworkTypes::validatePassword(Security::WpaPsk, QString(64, 'a'), error));
        QVERIFY(!NetworkTypes::validatePassword(Security::WpaPsk, QString(64, 'z'), error));
        QVERIFY(!NetworkTypes::validatePassword(Security::WpaPsk, QStringLiteral("1234567é"), error));
        QVERIFY(NetworkTypes::validatePassword(Security::Sae, QStringLiteral("x"), error));
        QVERIFY(NetworkTypes::validatePassword(Security::Sae, QString(100, 'x'), error));
        QVERIFY(!NetworkTypes::validatePassword(Security::Sae, {}, error));
    }
    void mergePreservesAdvancedSettingsAndAddressAttributes()
    {
        auto profile = fixture();
        NetworkAddressData addresses{{{QStringLiteral("address"), QStringLiteral("192.0.2.10")}, {QStringLiteral("prefix"), uint(24)}, {QStringLiteral("label"), QStringLiteral("eth0:custom")}}};
        profile.settings[QStringLiteral("ipv4")].insert(QStringLiteral("address-data"), QVariant::fromValue(addresses));
        NetworkAddressData routes{{{QStringLiteral("dest"), QStringLiteral("198.51.100.0")}, {QStringLiteral("prefix"), uint(24)}, {QStringLiteral("next-hop"), QStringLiteral("192.0.2.254")}}};
        profile.settings[QStringLiteral("ipv4")].insert(QStringLiteral("route-data"), QVariant::fromValue(routes));
        profile.settings[QStringLiteral("802-11-wireless-security")] = {{QStringLiteral("key-mgmt"), QStringLiteral("wpa-psk")}, {QStringLiteral("psk-flags"), uint(1)}, {QStringLiteral("pmf"), 2}};
        auto prefs = NetworkTypes::preferences(profile);
        QVERIFY(NetworkTypes::sameSettings(NetworkTypes::mergePreferences(profile, prefs), profile.settings));
        prefs.id = QStringLiteral("Renamed"); prefs.ipv4.gateway = QStringLiteral("192.0.2.1"); prefs.ipv4.dns = {QStringLiteral("1.1.1.1")};
        const auto merged = NetworkTypes::mergePreferences(profile, prefs);
        QCOMPARE(merged.value(QStringLiteral("802-11-wireless-security")), profile.settings.value(QStringLiteral("802-11-wireless-security")));
        QCOMPARE(merged.value(QStringLiteral("802-3-ethernet")), profile.settings.value(QStringLiteral("802-3-ethernet")));
        QCOMPARE(merged.value(QStringLiteral("ipv4")).value(QStringLiteral("route-data")), profile.settings.value(QStringLiteral("ipv4")).value(QStringLiteral("route-data")));
        QCOMPARE(NetworkTypes::addressData(merged.value(QStringLiteral("ipv4")).value(QStringLiteral("address-data"))).first().value(QStringLiteral("label")).toString(), QStringLiteral("eth0:custom"));
        QCOMPARE(merged.value(QStringLiteral("ipv4")).value(QStringLiteral("dns-data")).metaType(), QMetaType::fromType<QStringList>());
    }
    void modernDbusSignaturesAreTyped()
    {
        QCOMPARE(QByteArray(QDBusMetaType::typeToSignature(QMetaType::fromType<NetworkConnectionData>())), QByteArray("a{sa{sv}}"));
        QCOMPARE(QByteArray(QDBusMetaType::typeToSignature(QMetaType::fromType<NetworkAddressData>())), QByteArray("aa{sv}"));
        auto profile = fixture();
        auto prefs = NetworkTypes::preferences(profile);
        prefs.ipv4.method = QStringLiteral("manual"); prefs.ipv4.addresses = {QStringLiteral("192.0.2.4/24")};
        auto merged = NetworkTypes::mergePreferences(profile, prefs);
        const auto addresses = merged.value(QStringLiteral("ipv4")).value(QStringLiteral("address-data"));
        QCOMPARE(addresses.metaType(), QMetaType::fromType<NetworkAddressData>());
        QCOMPARE(NetworkTypes::addressData(addresses).first().value(QStringLiteral("prefix")).metaType(), QMetaType::fromType<uint>());
    }
    void snapshotComparisonIgnoresRadioNoiseButDetectsProfiles()
    {
        NetworkSnapshot original;
        original.serviceAvailable = true; original.checkpointAvailable = true;
        original.profiles = {fixture()};
        NetworkDevice device; device.path = QStringLiteral("/device/1"); device.type = 1; device.managed = true;
        original.devices = {device};
        auto current = original;
        current.connectivity = 4; current.devices[0].state = 70;
        NetworkAccessPoint ap; ap.path = QStringLiteral("/ap/1"); current.accessPoints = {ap};
        QVERIFY(NetworkTypes::sameSnapshot(original, current));
        current.profiles[0].settings[QStringLiteral("ipv4")].insert(QStringLiteral("route-metric"), qint64(99));
        QVERIFY(!NetworkTypes::sameSnapshot(original, current));
        current = original; current.profiles[0].versionId++;
        QVERIFY(!NetworkTypes::sameSnapshot(original, current));
    }
    void inactiveEditNeedsNoCheckpointButConnectionDoes()
    {
        NetworkSnapshot snapshot;
        snapshot.serviceAvailable = true; snapshot.profiles = {fixture()};
        NetworkRequest request; request.profilePath = snapshot.profiles.first().path;
        request.preferences = NetworkTypes::preferences(snapshot.profiles.first());
        QString error;
        QVERIFY(NetworkTypes::validateRequest(snapshot, request, error));
        request.action = NetworkRequest::Action::Disconnect;
        QVERIFY(!NetworkTypes::validateRequest(snapshot, request, error));
    }
    void readbackAcceptsCanonicalIpv6Text()
    {
        NetworkSnapshot before;
        before.serviceAvailable = true; before.profiles = {fixture()};
        NetworkRequest request;
        request.profilePath = before.profiles.first().path;
        request.preferences = NetworkTypes::preferences(before.profiles.first());
        request.preferences.ipv6.method = QStringLiteral("manual");
        request.preferences.ipv6.addresses = {QStringLiteral("2001:0DB8:0000:0000:0000:0000:0000:0001/64")};
        request.preferences.ipv6.dns = {QStringLiteral("2001:0DB8:0000:0000:0000:0000:0000:0053")};
        request.preferences.ipv6.gateway = QStringLiteral("2001:0DB8:0000:0000:0000:0000:0000:00FF");
        auto current = before;
        current.profiles[0].settings = NetworkTypes::mergePreferences(before.profiles[0], request.preferences);
        QCOMPARE(NetworkTypes::preferences(current.profiles[0]).ipv6.addresses.first(), QStringLiteral("2001:db8::1/64"));
        QVERIFY(NetworkTypes::matchesRequest(before, current, request));
    }
    void runtimeReapplyIsAConflictAndSavedOnlyReadbackCannotPass()
    {
        NetworkSnapshot baseline;
        baseline.serviceAvailable = true; baseline.profiles = {fixture()};
        NetworkDevice device;
        device.path = QStringLiteral("/device/1"); device.type = 1; device.state = 100;
        device.activeConnectionPath = QStringLiteral("/active/1"); device.activeProfilePath = baseline.profiles[0].path;
        device.appliedVersionId = 7; device.appliedSettings = baseline.profiles[0].settings;
        baseline.devices = {device};
        auto current = baseline;
        current.devices[0].appliedVersionId++;
        QVERIFY(!NetworkTypes::sameSnapshot(baseline, current));
        current = baseline;
        current.devices[0].appliedSettings[QStringLiteral("ipv4")].insert(QStringLiteral("route-metric"), qint64(77));
        QVERIFY(!NetworkTypes::sameSnapshot(baseline, current));
        NetworkRequest request;
        request.devicePath = device.path; request.profilePath = device.activeProfilePath;
        request.preferences = NetworkTypes::preferences(baseline.profiles[0]); request.preferences.id = QStringLiteral("New name");
        current = baseline;
        current.profiles[0].settings = NetworkTypes::mergePreferences(baseline.profiles[0], request.preferences);
        QVERIFY(!NetworkTypes::matchesRequest(baseline, current, request));
        current.devices[0].appliedSettings = current.profiles[0].settings;
        current.devices[0].appliedVersionId++;
        QVERIFY(NetworkTypes::matchesRequest(baseline, current, request));
    }
    void wifiHistoryMetadataDoesNotInvalidateAppliedPreferences()
    {
        NetworkSnapshot baseline;
        baseline.serviceAvailable = true; baseline.profiles = {fixture()};
        auto &profile = baseline.profiles[0]; profile.type = QStringLiteral("802-11-wireless");
        profile.settings[QStringLiteral("connection")].insert(QStringLiteral("type"), profile.type);
        profile.settings[QStringLiteral("802-11-wireless")] = {{QStringLiteral("ssid"), QByteArrayLiteral("Example")},
            {QStringLiteral("seen-bssids"), QStringList{QStringLiteral("02:00:00:00:00:01")}}};
        NetworkDevice device;
        device.path = QStringLiteral("/device/1"); device.type = 2; device.state = 100;
        device.activeConnectionPath = QStringLiteral("/active/1"); device.activeProfilePath = profile.path;
        device.appliedVersionId = 7; device.appliedSettings = profile.settings;
        device.appliedSettings[QStringLiteral("802-11-wireless")].remove(QStringLiteral("seen-bssids"));
        baseline.devices = {device};
        QVERIFY(NetworkTypes::sameSettings(profile.settings, device.appliedSettings));
        auto observedHistory = baseline;
        observedHistory.profiles[0].settings[QStringLiteral("802-11-wireless")].insert(QStringLiteral("seen-bssids"), QStringList{QStringLiteral("02:00:00:00:00:02")});
        QVERIFY(NetworkTypes::sameSnapshot(baseline, observedHistory));
        observedHistory.profiles[0].versionId++;
        QVERIFY(!NetworkTypes::sameSnapshot(baseline, observedHistory));
        auto securityChange = profile.settings;
        securityChange[QStringLiteral("802-11-wireless-security")].insert(QStringLiteral("key-mgmt"), QStringLiteral("wpa-eap"));
        QVERIFY(!NetworkTypes::sameSettings(profile.settings, securityChange));
        NetworkRequest request;
        request.devicePath = device.path; request.profilePath = profile.path;
        request.preferences = NetworkTypes::preferences(profile);
        request.preferences.ipv6.method = QStringLiteral("manual");
        request.preferences.ipv6.addresses = {QStringLiteral("2001:db8::1/64")};
        auto current = baseline;
        current.profiles[0].settings = NetworkTypes::mergePreferences(profile, request.preferences);
        current.devices[0].appliedSettings = current.profiles[0].settings;
        current.devices[0].appliedSettings[QStringLiteral("802-11-wireless")].remove(QStringLiteral("seen-bssids"));
        NetworkAddressData addresses{{{QStringLiteral("address"), QStringLiteral("2001:0DB8:0:0:0:0:0:1")}, {QStringLiteral("prefix"), uint(64)}}};
        current.devices[0].appliedSettings[QStringLiteral("ipv6")].insert(QStringLiteral("address-data"), QVariant::fromValue(addresses));
        QVERIFY(NetworkTypes::matchesRequest(baseline, current, request));
        current.devices[0].appliedSettings[QStringLiteral("ipv6")].insert(QStringLiteral("ignore-auto-dns"), true);
        QVERIFY(!NetworkTypes::matchesRequest(baseline, current, request));
        auto external = baseline;
        external.devices[0].appliedSettings[QStringLiteral("802-11-wireless")].insert(QStringLiteral("bssid"), QStringLiteral("02:00:00:00:00:02"));
        QVERIFY(!NetworkTypes::sameSnapshot(baseline, external));
        current.devices[0].appliedSettings = current.profiles[0].settings;
        current.devices[0].appliedSettings[QStringLiteral("802-11-wireless")].remove(QStringLiteral("seen-bssids"));
        current.profiles[0].settings[QStringLiteral("802-3-ethernet")].insert(QStringLiteral("mtu"), uint(1300));
        QVERIFY(!NetworkTypes::matchesRequest(baseline, current, request));
    }
    void activationAndEditingRejectDependencies()
    {
        NetworkSnapshot snapshot;
        snapshot.serviceAvailable = true; snapshot.checkpointAvailable = true; snapshot.profiles = {fixture()};
        NetworkDevice device; device.path = QStringLiteral("/device/1"); device.type = 1; device.managed = true;
        device.availableProfilePaths = {snapshot.profiles[0].path}; snapshot.devices = {device};
        NetworkRequest request;
        request.profilePath = snapshot.profiles[0].path; request.devicePath = device.path;
        request.preferences = NetworkTypes::preferences(snapshot.profiles[0]);
        QString error;
        for (const QString &key : {QStringLiteral("controller"), QStringLiteral("master"), QStringLiteral("port-type"), QStringLiteral("slave-type")}) {
            auto dependent = snapshot;
            dependent.profiles[0].settings[QStringLiteral("connection")].insert(key, QStringLiteral("bridge0"));
            request.action = NetworkRequest::Action::ActivateProfile;
            QVERIFY(!NetworkTypes::validateRequest(dependent, request, error)); QVERIFY(!error.isEmpty());
            request.action = NetworkRequest::Action::UpdateProfile;
            QVERIFY(!NetworkTypes::validateRequest(dependent, request, error));
        }
        snapshot.profiles[0].settings[QStringLiteral("connection")].insert(QStringLiteral("secondaries"), QStringList{QStringLiteral("vpn-uuid")});
        request.action = NetworkRequest::Action::ActivateProfile;
        QVERIFY(!NetworkTypes::validateRequest(snapshot, request, error));
        request.action = NetworkRequest::Action::UpdateProfile;
        QVERIFY(!NetworkTypes::validateRequest(snapshot, request, error));
        snapshot.profiles[0].settings[QStringLiteral("connection")].remove(QStringLiteral("secondaries"));
        snapshot.profiles[0].type = QStringLiteral("802-11-wireless"); snapshot.devices[0].type = 2;
        for (const QString &mode : {QStringLiteral("ap"), QStringLiteral("adhoc")}) {
            snapshot.profiles[0].settings[QStringLiteral("802-11-wireless")].insert(QStringLiteral("mode"), mode);
            request.action = NetworkRequest::Action::ActivateProfile;
            QVERIFY(!NetworkTypes::validateRequest(snapshot, request, error));
            request.action = NetworkRequest::Action::UpdateProfile;
            QVERIFY(!NetworkTypes::validateRequest(snapshot, request, error));
        }
    }
};
QTEST_GUILESS_MAIN(NetworkTypesTest)
#include "network_types_test.moc"
