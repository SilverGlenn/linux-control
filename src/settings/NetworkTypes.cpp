#include "NetworkTypes.h"

#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusVariant>
#include <QHostAddress>
#include <QRegularExpression>
#include <QSet>
#include <QtEndian>
#include <algorithm>

namespace {
QVariant canonical(const QVariant &value);

QVariant readArgument(const QDBusArgument &argument)
{
    switch (argument.currentType()) {
    case QDBusArgument::BasicType:
        return canonical(argument.asVariant());
    case QDBusArgument::VariantType: {
        QDBusVariant value;
        argument >> value;
        return canonical(value.variant());
    }
    case QDBusArgument::ArrayType: {
        QVariantList values;
        argument.beginArray();
        while (!argument.atEnd()) values.append(readArgument(argument));
        argument.endArray();
        return values;
    }
    case QDBusArgument::MapType: {
        QVariantList entries;
        argument.beginMap();
        while (!argument.atEnd()) {
            argument.beginMapEntry();
            const QVariant key = readArgument(argument);
            const QVariant value = readArgument(argument);
            entries.append(QVariant(QVariantList{key, value}));
            argument.endMapEntry();
        }
        argument.endMap();
        // All NetworkManager dictionaries have string keys. Preserve ordering
        // independence when comparing a daemon's demarshalled map with ours.
        QVariantMap map;
        for (const QVariant &entry : entries) {
            const auto pair = entry.toList();
            map.insert(pair[0].toString(), pair[1]);
        }
        return map;
    }
    case QDBusArgument::StructureType: {
        QVariantList values;
        argument.beginStructure();
        while (!argument.atEnd()) values.append(readArgument(argument));
        argument.endStructure();
        return values;
    }
    default: return {};
    }
}

QVariant canonical(const QVariant &value)
{
    if (value.metaType() == QMetaType::fromType<QDBusArgument>())
        return readArgument(value.value<QDBusArgument>());
    if (value.metaType() == QMetaType::fromType<QDBusVariant>())
        return canonical(value.value<QDBusVariant>().variant());
    if (value.metaType() == QMetaType::fromType<QDBusObjectPath>())
        return value.value<QDBusObjectPath>().path();
    if (value.metaType() == QMetaType::fromType<NetworkAddressData>()) {
        QVariantList result;
        for (const auto &entry : value.value<NetworkAddressData>()) result.append(canonical(entry));
        return result;
    }
    if (value.metaType() == QMetaType::fromType<QVariantMap>()) {
        QVariantMap result;
        const auto map = value.toMap();
        for (auto it = map.begin(); it != map.end(); ++it) result.insert(it.key(), canonical(it.value()));
        return result;
    }
    if (value.metaType() == QMetaType::fromType<QVariantList>() || value.metaType() == QMetaType::fromType<QStringList>()) {
        QVariantList result;
        for (const auto &entry : value.toList()) result.append(canonical(entry));
        return result;
    }
    return value;
}

bool numericAddress(const QString &text, bool ipv6, QHostAddress &address)
{
    return !text.isEmpty() && text == text.trimmed() && !text.contains('%')
        && address.setAddress(text)
        && address.protocol() == (ipv6 ? QAbstractSocket::IPv6Protocol : QAbstractSocket::IPv4Protocol);
}

bool splitCidr(const QString &text, bool ipv6, QString &address, uint &prefix)
{
    if (text.count('/') != 1) return false;
    const auto parts = text.split('/');
    static const QRegularExpression digits(QStringLiteral("^[0-9]{1,3}$"));
    if (!digits.match(parts[1]).hasMatch()) return false;
    bool ok = false;
    prefix = parts[1].toUInt(&ok);
    QHostAddress parsed;
    if (!ok || prefix > (ipv6 ? 128u : 32u) || !numericAddress(parts[0], ipv6, parsed)) return false;
    address = parsed.toString();
    return true;
}

NetworkIpConfig ipPreferences(const QVariantMap &map, bool ipv6)
{
    NetworkIpConfig config;
    config.method = map.value(QStringLiteral("method"), QStringLiteral("auto")).toString();
    config.gateway = map.value(QStringLiteral("gateway")).toString();
    config.ignoreAutoDns = map.value(QStringLiteral("ignore-auto-dns"), false).toBool();
    for (const auto &entry : NetworkTypes::addressData(map.value(QStringLiteral("address-data"))))
        config.addresses.append(entry.value(QStringLiteral("address")).toString() + '/'
                                + QString::number(entry.value(QStringLiteral("prefix")).toUInt()));
    const auto dns = canonical(map.value(QStringLiteral("dns-data"))).toList();
    for (const auto &entry : dns) config.dns.append(entry.toString());
    // Read legacy DNS arrays too; merging an unrelated field must never replace
    // an opaque legacy array with an empty modern one.
    if (!map.contains(QStringLiteral("dns-data"))) {
        const auto legacy = canonical(map.value(QStringLiteral("dns"))).toList();
        for (const auto &entry : legacy) {
            if (!ipv6) config.dns.append(QHostAddress(qFromBigEndian(entry.toUInt())).toString());
            else {
                const QByteArray bytes = entry.toByteArray();
                if (bytes.size() == 16) config.dns.append(QHostAddress(reinterpret_cast<const uchar *>(bytes.constData())).toString());
            }
        }
    }
    if (!map.contains(QStringLiteral("address-data")) && map.contains(QStringLiteral("addresses"))) {
        for (const auto &row : canonical(map.value(QStringLiteral("addresses"))).toList()) {
            const auto cells = row.toList();
            if (cells.size() < 2) continue;
            QString address;
            if (!ipv6) address = QHostAddress(qFromBigEndian(cells[0].toUInt())).toString();
            else {
                const QByteArray bytes = cells[0].toByteArray();
                if (bytes.size() != 16) continue;
                address = QHostAddress(reinterpret_cast<const uchar *>(bytes.constData())).toString();
            }
            config.addresses.append(address + '/' + QString::number(cells[1].toUInt()));
            if (cells.size() > 2 && config.gateway.isEmpty()) {
                if (!ipv6 && cells[2].toUInt()) config.gateway = QHostAddress(qFromBigEndian(cells[2].toUInt())).toString();
                else if (ipv6 && cells[2].toByteArray().size() == 16) {
                    const QHostAddress gateway(reinterpret_cast<const uchar *>(cells[2].toByteArray().constData()));
                    if (!gateway.isNull()) config.gateway = gateway.toString();
                }
            }
        }
    }
    return config;
}

void mergeIp(QVariantMap &map, const NetworkIpConfig &before, const NetworkIpConfig &after, bool ipv6)
{
    if (before.method != after.method) map.insert(QStringLiteral("method"), after.method);
    if (before.addresses != after.addresses || before.gateway != after.gateway) {
        NetworkAddressData entries;
        const auto originals = NetworkTypes::addressData(map.value(QStringLiteral("address-data")));
        for (const QString &cidr : after.addresses) {
            QString address;
            uint prefix = 0;
            if (!splitCidr(cidr, ipv6, address, prefix)) continue;
            QVariantMap entry;
            for (const auto &original : originals) {
                QHostAddress originalAddress(original.value(QStringLiteral("address")).toString());
                if (originalAddress.toString() == address && original.value(QStringLiteral("prefix")).toUInt() == prefix) {
                    entry = original;
                    break;
                }
            }
            entry.insert(QStringLiteral("address"), address);
            entry.insert(QStringLiteral("prefix"), prefix);
            entries.append(entry);
        }
        map.remove(QStringLiteral("addresses"));
        map.insert(QStringLiteral("address-data"), QVariant::fromValue(entries));
        if (after.gateway.isEmpty()) map.remove(QStringLiteral("gateway"));
        else map.insert(QStringLiteral("gateway"), QHostAddress(after.gateway).toString());
    }
    if (before.dns != after.dns) {
        QStringList dns;
        for (const auto &entry : after.dns) dns.append(QHostAddress(entry).toString());
        map.remove(QStringLiteral("dns"));
        map.insert(QStringLiteral("dns-data"), dns);
    }
    if (before.ignoreAutoDns != after.ignoreAutoDns)
        map.insert(QStringLiteral("ignore-auto-dns"), after.ignoreAutoDns);
}

bool absent(const QString &path) { return path.isEmpty() || path == QStringLiteral("/"); }

bool supportedDependencies(const NetworkProfile &profile, QString &error)
{
    const auto connection = profile.settings.value(QStringLiteral("connection"));
    for (const QString &key : {QStringLiteral("controller"), QStringLiteral("master"),
                              QStringLiteral("port-type"), QStringLiteral("slave-type")}) {
        if (!connection.value(key).toString().isEmpty()) {
            error = QStringLiteral("Controller and port profiles must be managed in an advanced network tool.");
            return false;
        }
    }
    if (!canonical(connection.value(QStringLiteral("secondaries"))).toList().isEmpty()) {
        error = QStringLiteral("Profiles with secondary connection dependencies must be managed in an advanced network tool.");
        return false;
    }
    if (profile.type == QStringLiteral("802-11-wireless")) {
        const QString mode = profile.settings.value(QStringLiteral("802-11-wireless")).value(QStringLiteral("mode")).toString();
        if (!mode.isEmpty() && mode != QStringLiteral("infrastructure")) {
            error = QStringLiteral("Hotspot and ad-hoc profiles must be managed in an advanced network tool.");
            return false;
        }
    }
    return true;
}

NetworkPreferences normalizedPreferences(const NetworkConnectionData &settings)
{
    NetworkProfile profile;
    profile.settings = settings;
    auto result = NetworkTypes::preferences(profile);
    auto normalizeIp = [](NetworkIpConfig &ip, bool ipv6) {
        for (auto &cidr : ip.addresses) {
            QString address; uint prefix = 0;
            if (splitCidr(cidr, ipv6, address, prefix)) cidr = address + '/' + QString::number(prefix);
        }
        if (!ip.gateway.isEmpty()) {
            QHostAddress address;
            if (numericAddress(ip.gateway, ipv6, address)) ip.gateway = address.toString();
        }
        for (auto &dns : ip.dns) {
            QHostAddress address;
            if (numericAddress(dns, ipv6, address)) dns = address.toString();
        }
    };
    normalizeIp(result.ipv4, false); normalizeIp(result.ipv6, true);
    return result;
}
}

const NetworkDevice *NetworkSnapshot::device(const QString &path) const
{ for (const auto &entry : devices) if (entry.path == path) return &entry; return nullptr; }
const NetworkProfile *NetworkSnapshot::profile(const QString &path) const
{ for (const auto &entry : profiles) if (entry.path == path) return &entry; return nullptr; }
const NetworkAccessPoint *NetworkSnapshot::accessPoint(const QString &path) const
{ for (const auto &entry : accessPoints) if (entry.path == path) return &entry; return nullptr; }

bool NetworkIpConfig::operator==(const NetworkIpConfig &other) const
{ return method == other.method && addresses == other.addresses && gateway == other.gateway && dns == other.dns && ignoreAutoDns == other.ignoreAutoDns; }
bool NetworkPreferences::operator==(const NetworkPreferences &other) const
{ return id == other.id && autoconnect == other.autoconnect && ipv4 == other.ipv4 && ipv6 == other.ipv6; }

QDBusArgument &operator<<(QDBusArgument &argument, const NetworkConnectionData &data)
{
    argument.beginMap(QMetaType::fromType<QString>(), QMetaType::fromType<QVariantMap>());
    for (auto it = data.begin(); it != data.end(); ++it) {
        argument.beginMapEntry(); argument << it.key() << it.value(); argument.endMapEntry();
    }
    argument.endMap(); return argument;
}
const QDBusArgument &operator>>(const QDBusArgument &argument, NetworkConnectionData &data)
{
    data.clear(); argument.beginMap();
    while (!argument.atEnd()) {
        QString key; QVariantMap value;
        argument.beginMapEntry(); argument >> key >> value; argument.endMapEntry(); data.insert(key, value);
    }
    argument.endMap(); return argument;
}

void NetworkTypes::registerDbusTypes()
{
    static const bool registered = [] {
        qDBusRegisterMetaType<NetworkConnectionData>();
        qDBusRegisterMetaType<NetworkAddressData>();
        qRegisterMetaType<NetworkSnapshot>(); qRegisterMetaType<NetworkRequest>(); return true;
    }();
    Q_UNUSED(registered);
}

NetworkAddressData NetworkTypes::addressData(const QVariant &value)
{
    if (value.metaType() == QMetaType::fromType<NetworkAddressData>()) return value.value<NetworkAddressData>();
    NetworkAddressData result;
    for (const auto &entry : canonical(value).toList()) result.append(entry.toMap());
    return result;
}

NetworkPreferences NetworkTypes::preferences(const NetworkProfile &profile)
{
    NetworkPreferences result;
    const auto connection = profile.settings.value(QStringLiteral("connection"));
    result.id = connection.value(QStringLiteral("id"), profile.id).toString();
    result.autoconnect = connection.value(QStringLiteral("autoconnect"), true).toBool();
    result.ipv4 = ipPreferences(profile.settings.value(QStringLiteral("ipv4")), false);
    result.ipv6 = ipPreferences(profile.settings.value(QStringLiteral("ipv6")), true);
    return result;
}

bool NetworkTypes::validateIp(const NetworkIpConfig &config, bool ipv6, QString &error)
{
    error.clear();
    const QString family = ipv6 ? QStringLiteral("IPv6") : QStringLiteral("IPv4");
    auto fail = [&](const QString &reason) { error = family + QStringLiteral(": ") + reason; return false; };
    if (config.method != QStringLiteral("auto") && config.method != QStringLiteral("manual"))
        return fail(QStringLiteral("Only automatic and manual addressing can be edited here."));
    if (config.addresses.size() > 64 || config.dns.size() > 16)
        return fail(QStringLiteral("Use at most 64 addresses and 16 DNS servers."));
    if (config.method == QStringLiteral("manual") && config.addresses.isEmpty())
        return fail(QStringLiteral("Manual addressing requires at least one address with a prefix."));
    QSet<QString> addresses;
    for (const auto &entry : config.addresses) {
        QString address; uint prefix = 0;
        if (!splitCidr(entry, ipv6, address, prefix)) return fail(QStringLiteral("Enter a valid address and CIDR prefix."));
        if (addresses.contains(address)) return fail(QStringLiteral("Duplicate addresses are not allowed."));
        addresses.insert(address);
        const QHostAddress parsed(address);
        if (parsed.isNull() || parsed.isMulticast() || (!ipv6 && parsed.toIPv4Address() == 0xffffffffu))
            return fail(QStringLiteral("Use a unicast address."));
    }
    if (!config.gateway.isEmpty()) {
        QHostAddress address;
        if (!numericAddress(config.gateway, ipv6, address) || address.isNull() || address.isMulticast())
            return fail(QStringLiteral("Enter a valid gateway from this address family."));
        if (config.addresses.isEmpty()) return fail(QStringLiteral("A gateway requires a configured address."));
    }
    QSet<QString> dns;
    for (const auto &entry : config.dns) {
        QHostAddress address;
        if (!numericAddress(entry, ipv6, address) || address.isNull() || address.isMulticast())
            return fail(QStringLiteral("Enter numeric DNS addresses from this address family."));
        const QString key = address.toString();
        if (dns.contains(key)) return fail(QStringLiteral("Duplicate DNS servers are not allowed."));
        dns.insert(key);
    }
    return true;
}

bool NetworkTypes::validatePreferences(const NetworkPreferences &value, QString &error)
{
    error.clear();
    if (value.id.trimmed().isEmpty() || value.id.toUtf8().size() > 255 || value.id.contains(QChar::Null)) {
        error = QStringLiteral("Enter a profile name between 1 and 255 bytes."); return false;
    }
    return validateIp(value.ipv4, false, error) && validateIp(value.ipv6, true, error);
}

bool NetworkTypes::validatePassword(NetworkAccessPoint::Security security, const QString &password, QString &error)
{
    error.clear();
    if (security == NetworkAccessPoint::Security::Unsupported) {
        error = QStringLiteral("Enterprise, WEP, and other unsupported Wi-Fi security must be configured in another network tool."); return false;
    }
    if (security == NetworkAccessPoint::Security::Open) return true;
    if (password.contains(QChar::Null)) { error = QStringLiteral("The Wi-Fi password cannot contain a null character."); return false; }
    // NetworkManager's SAE psk accepts a nonempty passphrase of any length,
    // unlike WPA-PSK's 8..63 ASCII characters or 64 hexadecimal digits.
    if (security == NetworkAccessPoint::Security::Sae) {
        if (!password.isEmpty() && password.toUtf8().size() <= 4096) return true;
        error = QStringLiteral("Enter a WPA3 password (up to 4096 bytes)."); return false;
    }
    static const QRegularExpression hex(QStringLiteral("^[0-9a-fA-F]{64}$"));
    if (hex.match(password).hasMatch()) return true;
    if (password.size() >= 8 && password.size() <= 63) {
        bool ascii = true;
        for (const auto character : password) if (character.unicode() < 32 || character.unicode() > 126) ascii = false;
        if (ascii) return true;
    }
    error = QStringLiteral("Use 8–63 ASCII characters or exactly 64 hexadecimal digits for WPA-Personal."); return false;
}

bool NetworkTypes::validateRequest(const NetworkSnapshot &snapshot, const NetworkRequest &request, QString &error)
{
    error.clear();
    auto fail = [&](const QString &reason) { error = reason; return false; };
    if (!snapshot.serviceAvailable) return fail(QStringLiteral("NetworkManager is unavailable."));
    const auto *device = snapshot.device(request.devicePath);
    const auto *profile = snapshot.profile(request.profilePath);
    if (request.action == NetworkRequest::Action::UpdateProfile) {
        if (!profile || !profile->editable) return fail(profile && !profile->reason.isEmpty() ? profile->reason : QStringLiteral("This saved profile cannot be edited here."));
        if (!supportedDependencies(*profile, error)) return false;
        const bool active = std::any_of(snapshot.devices.begin(), snapshot.devices.end(), [&](const NetworkDevice &entry) { return entry.activeProfilePath == profile->path; });
        if (active && !snapshot.checkpointAvailable) return fail(QStringLiteral("NetworkManager rollback checkpoints are unavailable. Active changes are disabled."));
        return validatePreferences(request.preferences, error);
    }
    if (!snapshot.checkpointAvailable) return fail(QStringLiteral("NetworkManager rollback checkpoints are unavailable. Connection changes are disabled."));
    if (!device || !device->managed || (device->type != 1 && device->type != 2))
        return fail(QStringLiteral("Select a managed Ethernet or Wi-Fi adapter."));
    if (request.action == NetworkRequest::Action::Disconnect) {
        if (absent(device->activeConnectionPath)) return fail(QStringLiteral("This adapter is already disconnected."));
        return true;
    }
    if (request.action == NetworkRequest::Action::ActivateProfile) {
        if (!profile) return fail(QStringLiteral("The saved profile is no longer available."));
        if (!supportedDependencies(*profile, error)) return false;
        const bool wifi = profile->type == QStringLiteral("802-11-wireless");
        if ((wifi && device->type != 2) || (!wifi && (profile->type != QStringLiteral("802-3-ethernet") || device->type != 1)))
            return fail(QStringLiteral("This profile does not match the selected adapter."));
        if (!device->availableProfilePaths.contains(profile->path)) return fail(QStringLiteral("This saved profile is unavailable on the selected adapter."));
        if (wifi) {
            const QString key = profile->settings.value(QStringLiteral("802-11-wireless-security")).value(QStringLiteral("key-mgmt")).toString();
            if ((!key.isEmpty() && key != QStringLiteral("wpa-psk") && key != QStringLiteral("sae")) || profile->settings.contains(QStringLiteral("802-1x")))
                return fail(QStringLiteral("This Wi-Fi security type is not supported here."));
        }
        return true;
    }
    const auto *ap = snapshot.accessPoint(request.accessPointPath);
    if (!ap || ap->devicePath != device->path || device->type != 2 || ap->ssid.isEmpty() || ap->ssid.size() > 32)
        return fail(QStringLiteral("Select a visible Wi-Fi network on this adapter."));
    return validatePassword(ap->security, request.password, error);
}

NetworkConnectionData NetworkTypes::mergePreferences(const NetworkProfile &profile, const NetworkPreferences &value)
{
    auto result = profile.settings;
    const auto before = preferences(profile);
    if (before.id != value.id) result[QStringLiteral("connection")].insert(QStringLiteral("id"), value.id);
    if (before.autoconnect != value.autoconnect) result[QStringLiteral("connection")].insert(QStringLiteral("autoconnect"), value.autoconnect);
    if (before.ipv4 != value.ipv4) mergeIp(result[QStringLiteral("ipv4")], before.ipv4, value.ipv4, false);
    if (before.ipv6 != value.ipv6) mergeIp(result[QStringLiteral("ipv6")], before.ipv6, value.ipv6, true);
    return result;
}

bool NetworkTypes::sameSettings(const NetworkConnectionData &left, const NetworkConnectionData &right)
{
    if (left.keys() != right.keys()) return false;
    for (auto it = left.begin(); it != left.end(); ++it) {
        auto a = it.value(), b = right.value(it.key());
        if (it.key() == QStringLiteral("connection")) { a.remove(QStringLiteral("timestamp")); b.remove(QStringLiteral("timestamp")); }
        // NM 1.54.3 get_settings_auth_cb injects private timestamp/BSSID
        // history into GetSettings. These are observations, not configuration.
        if (it.key() == QStringLiteral("802-11-wireless")) { a.remove(QStringLiteral("seen-bssids")); b.remove(QStringLiteral("seen-bssids")); }
        if (canonical(a) != canonical(b)) return false;
    }
    return true;
}

bool NetworkTypes::sameSnapshot(const NetworkSnapshot &left, const NetworkSnapshot &right)
{
    if (!left.isValid() || !right.isValid() || left.serviceOwner != right.serviceOwner
        || left.checkpointAvailable != right.checkpointAvailable || left.devices.size() != right.devices.size()
        || left.profiles.size() != right.profiles.size()) return false;
    for (const auto &device : left.devices) {
        const auto *other = right.device(device.path);
        if (!other || device.interfaceName != other->interfaceName || device.type != other->type
            || device.managed != other->managed || device.activeConnectionPath != other->activeConnectionPath
            || device.activeProfilePath != other->activeProfilePath) return false;
        if (!absent(device.activeProfilePath)
            && (device.appliedVersionId != other->appliedVersionId
                || !sameSettings(device.appliedSettings, other->appliedSettings))) return false;
    }
    for (const auto &profile : left.profiles) {
        const auto *other = right.profile(profile.path);
        if (!other || profile.uuid != other->uuid || profile.versionId != other->versionId
            || !sameSettings(profile.settings, other->settings)) return false;
    }
    return true;
}

bool NetworkTypes::matchesRequest(const NetworkSnapshot &baseline, const NetworkSnapshot &current, const NetworkRequest &request)
{
    if (!current.isValid() || current.serviceOwner != baseline.serviceOwner) return false;
    const auto *device = current.device(request.devicePath);
    if (request.action == NetworkRequest::Action::Disconnect)
        return device && absent(device->activeConnectionPath) && (device->state == 30 || device->state == 20);
    if (request.action == NetworkRequest::Action::ActivateProfile)
        return device && device->state == 100 && device->activeProfilePath == request.profilePath;
    if (request.action == NetworkRequest::Action::ConnectWifi) {
        const auto *ap = baseline.accessPoint(request.accessPointPath);
        const auto *profile = device ? current.profile(device->activeProfilePath) : nullptr;
        if (!ap || !device || device->state != 100 || !profile) return false;
        const auto ssid = profile->settings.value(QStringLiteral("802-11-wireless")).value(QStringLiteral("ssid")).toByteArray();
        return ssid == ap->ssid && (absent(device->activeAccessPointPath) || device->activeAccessPointPath == ap->path);
    }
    const auto *original = baseline.profile(request.profilePath);
    const auto *updated = current.profile(request.profilePath);
    // mergePreferences canonicalizes numeric addresses. Compare its complete
    // expected result so equivalent expanded/compact IPv6 input is accepted.
    if (!original || !updated) return false;
    const auto expected = mergePreferences(*original, request.preferences);
    if (!sameSettings(expected, updated->settings)) return false;
    if (!request.devicePath.isEmpty()) {
        if (!device || device->state != 100 || device->activeProfilePath != request.profilePath) return false;
        // GetSettings injects history such as wireless.seen-bssids, while
        // GetAppliedConnection exposes the actual applied profile. Verify the
        // supported requested fields here; sameSnapshot still checks the full
        // applied settings and revision to detect external runtime changes.
        if (device->appliedVersionId
            && normalizedPreferences(expected) != normalizedPreferences(device->appliedSettings)) return false;
    }
    return true;
}

QString NetworkTypes::securityName(NetworkAccessPoint::Security security)
{
    switch (security) {
    case NetworkAccessPoint::Security::Open: return QStringLiteral("Open");
    case NetworkAccessPoint::Security::WpaPsk: return QStringLiteral("WPA-Personal");
    case NetworkAccessPoint::Security::Sae: return QStringLiteral("WPA3-Personal");
    default: return QStringLiteral("Unsupported security");
    }
}
