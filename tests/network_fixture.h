#pragma once
#include "settings/NetworkSettings.h"

class IntegrationNetworkBackend final : public NetworkBackend {
public:
    NetworkSnapshot current, baseline;
    NetworkRequest request;
    int writes = 0;
    bool holdScan = false;
    IntegrationNetworkBackend()
    {
        current.serviceAvailable = current.checkpointAvailable = true;
        current.networkingEnabled = current.wirelessEnabled = current.wirelessHardwareEnabled = true;
        current.serviceOwner = QStringLiteral(":1.100"); current.version = QStringLiteral("1.54.3");
        for (const auto &permission : {"network-control", "checkpoint-rollback", "settings.modify.system", "settings.modify.own"})
            current.permissions.insert(QStringLiteral("org.freedesktop.NetworkManager.") + QString::fromLatin1(permission), QStringLiteral("yes"));
        NetworkProfile profile;
        profile.path = QStringLiteral("/org/freedesktop/NetworkManager/Settings/1");
        profile.id = QStringLiteral("Fixture Ethernet"); profile.uuid = QStringLiteral("aa2b6971-8101-442f-8353-2caa1da204de");
        profile.type = QStringLiteral("802-3-ethernet"); profile.editable = true; profile.versionId = 1;
        profile.settings[QStringLiteral("connection")] = {{"id", profile.id}, {"uuid", profile.uuid}, {"type", profile.type}, {"autoconnect", true}};
        profile.settings[QStringLiteral("ipv4")] = {{"method", "auto"}};
        profile.settings[QStringLiteral("ipv6")] = {{"method", "auto"}};
        current.profiles << profile;
        NetworkDevice device;
        device.path = QStringLiteral("/org/freedesktop/NetworkManager/Devices/1"); device.interfaceName = QStringLiteral("eth0");
        device.type = 1; device.state = 100; device.managed = device.carrier = true;
        device.activeProfilePath = profile.path;
        device.activeConnectionPath = QStringLiteral("/org/freedesktop/NetworkManager/ActiveConnection/1");
        device.availableProfilePaths << profile.path; device.ipv4Addresses << QStringLiteral("192.0.2.10/24");
        current.devices << device;
    }
    void readCurrent() override { emit readFinished(true, current, {}); }
    void scan(const QString &) override { if (!holdScan) emit scanFinished(true, {}); }
    void finishScan() { holdScan = false; emit scanFinished(true, {}); }
    void beginChange(const NetworkSnapshot &old, const NetworkRequest &next) override
    {
        ++writes; baseline = old; request = next; request.password.clear();
    }
    void preview()
    {
        if (request.action == NetworkRequest::Action::UpdateProfile) {
            auto &profile = current.profiles.first();
            profile.settings = NetworkTypes::mergePreferences(profile, request.preferences);
            profile.id = request.preferences.id; ++profile.versionId;
        }
        emit previewReady(current, 20);
    }
    void keepChange() override { emit transactionFinished(true, false, current, {}); }
    void revertChange() override { current = baseline; emit transactionFinished(false, true, current, {}); }
};
