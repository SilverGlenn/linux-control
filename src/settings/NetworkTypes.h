#pragma once

#include <QByteArray>
#include <QDBusArgument>
#include <QList>
#include <QMap>
#include <QStringList>
#include <QVariantMap>

using NetworkConnectionData = QMap<QString, QVariantMap>;
using NetworkAddressData = QList<QVariantMap>;

struct NetworkDevice {
    QString path;
    QString interfaceName;
    uint type = 0; // NetworkManager: 1 = Ethernet, 2 = Wi-Fi.
    uint state = 0;
    uint stateReason = 0;
    bool managed = false;
    bool carrier = false;
    QString activeConnectionPath;
    QString activeProfilePath;
    QString activeAccessPointPath;
    QStringList availableProfilePaths;
    QStringList ipv4Addresses;
    QStringList ipv6Addresses;
    NetworkConnectionData appliedSettings;
    quint64 appliedVersionId = 0;
    QString reason;
};

struct NetworkAccessPoint {
    enum class Security { Open, WpaPsk, Sae, Unsupported };
    QString path;
    QString devicePath;
    QByteArray ssid;
    QString name;
    uint strength = 0;
    Security security = Security::Unsupported;
    QString reason;
};

struct NetworkProfile {
    QString path;
    QString id;
    QString uuid;
    QString type;
    NetworkConnectionData settings; // Complete original, including opaque D-Bus values.
    bool editable = false;
    quint64 versionId = 0;
    bool unsaved = false;
    uint flags = 0;
    QString reason;
};

struct NetworkSnapshot {
    QString version;
    QString serviceOwner;
    QMap<QString, QString> permissions;
    bool serviceAvailable = false;
    bool checkpointAvailable = false;
    uint connectivity = 0;
    uint state = 0;
    bool connectivityCheckAvailable = false;
    bool connectivityCheckEnabled = false;
    bool wirelessEnabled = false;
    bool wirelessHardwareEnabled = false;
    bool networkingEnabled = false;
    QStringList checkpoints;
    QList<NetworkDevice> devices;
    QList<NetworkAccessPoint> accessPoints;
    QList<NetworkProfile> profiles;
    QString reason;
    bool isValid() const { return serviceAvailable; }
    const NetworkDevice *device(const QString &path) const;
    const NetworkProfile *profile(const QString &path) const;
    const NetworkAccessPoint *accessPoint(const QString &path) const;
};

struct NetworkIpConfig {
    QString method = QStringLiteral("auto");
    QStringList addresses; // Numeric addresses with CIDR prefix.
    QString gateway;
    QStringList dns; // Numeric DNS addresses from the same address family.
    bool ignoreAutoDns = false;
    bool operator==(const NetworkIpConfig &other) const;
    bool operator!=(const NetworkIpConfig &other) const { return !(*this == other); }
};

struct NetworkPreferences {
    QString id;
    bool autoconnect = true;
    NetworkIpConfig ipv4;
    NetworkIpConfig ipv6;
    bool operator==(const NetworkPreferences &other) const;
    bool operator!=(const NetworkPreferences &other) const { return !(*this == other); }
};

struct NetworkRequest {
    enum class Action { ActivateProfile, ConnectWifi, Disconnect, UpdateProfile };
    Action action = Action::UpdateProfile;
    QString devicePath;
    QString profilePath;
    QString accessPointPath;
    NetworkPreferences preferences;
    QString password; // Transient; never persist or include in diagnostic text.
};

namespace NetworkTypes {
void registerDbusTypes();
NetworkAddressData addressData(const QVariant &value);
NetworkPreferences preferences(const NetworkProfile &profile);
bool validateIp(const NetworkIpConfig &config, bool ipv6, QString &error);
bool validatePreferences(const NetworkPreferences &preferences, QString &error);
bool validatePassword(NetworkAccessPoint::Security security, const QString &password, QString &error);
bool validateRequest(const NetworkSnapshot &snapshot, const NetworkRequest &request, QString &error);
NetworkConnectionData mergePreferences(const NetworkProfile &profile, const NetworkPreferences &preferences);
bool sameSettings(const NetworkConnectionData &left, const NetworkConnectionData &right);
bool sameSnapshot(const NetworkSnapshot &left, const NetworkSnapshot &right);
bool matchesRequest(const NetworkSnapshot &baseline, const NetworkSnapshot &current, const NetworkRequest &request);
QString securityName(NetworkAccessPoint::Security security);
}

QDBusArgument &operator<<(QDBusArgument &argument, const NetworkConnectionData &data);
const QDBusArgument &operator>>(const QDBusArgument &argument, NetworkConnectionData &data);
Q_DECLARE_METATYPE(NetworkConnectionData)
Q_DECLARE_METATYPE(NetworkAddressData)
Q_DECLARE_METATYPE(NetworkSnapshot)
Q_DECLARE_METATYPE(NetworkRequest)
