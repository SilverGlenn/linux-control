#include "NetworkManagerBackend.h"

#include <QDBusArgument>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusServiceWatcher>
#include <QDBusVariant>
#include <QRegularExpression>
#include <QSharedPointer>
#include <QXmlStreamReader>
#include <QUuid>
#include <algorithm>

namespace {
const QString root = QStringLiteral("/org/freedesktop/NetworkManager");
const QString nm = QStringLiteral("org.freedesktop.NetworkManager");
const QString settingsPath = root + QStringLiteral("/Settings");
const QString settingsInterface = nm + QStringLiteral(".Settings");
const QString profileInterface = settingsInterface + QStringLiteral(".Connection");
const QString deviceInterface = nm + QStringLiteral(".Device");
const QString wifiInterface = deviceInterface + QStringLiteral(".Wireless");
const QString activeInterface = nm + QStringLiteral(".Connection.Active");
const QString propertiesInterface = QStringLiteral("org.freedesktop.DBus.Properties");

QVariant unwrap(const QVariant &value)
{
    if (value.metaType() == QMetaType::fromType<QDBusVariant>())
        return value.value<QDBusVariant>().variant();
    return value;
}
QVariantMap mapValue(const QVariant &value)
{
    const QVariant v = unwrap(value);
    return v.metaType() == QMetaType::fromType<QDBusArgument>()
        ? qdbus_cast<QVariantMap>(v) : v.toMap();
}
QString pathValue(const QVariant &value)
{
    const QVariant v = unwrap(value);
    return v.metaType() == QMetaType::fromType<QDBusObjectPath>()
        ? v.value<QDBusObjectPath>().path() : v.toString();
}
QStringList pathList(const QVariant &value)
{
    const QVariant v = unwrap(value);
    if (v.metaType() == QMetaType::fromType<QDBusArgument>()) {
        QStringList paths;
        for (const auto &p : qdbus_cast<QList<QDBusObjectPath>>(v)) paths << p.path();
        return paths;
    }
    if (v.canConvert<QList<QDBusObjectPath>>()) {
        QStringList paths;
        for (const auto &p : v.value<QList<QDBusObjectPath>>()) paths << p.path();
        return paths;
    }
    return v.toStringList();
}
bool numberedPath(const QString &path, const QString &kind)
{
    return QRegularExpression(QStringLiteral("^/org/freedesktop/NetworkManager/%1/[0-9]+$").arg(kind))
        .match(path).hasMatch();
}
QString stateReason(uint reason)
{
    switch (reason) {
    case 7: return QStringLiteral("NetworkManager did not receive required credentials.");
    case 8: return QStringLiteral("Wi-Fi authentication disconnected; check the password and access point.");
    case 9: return QStringLiteral("Wi-Fi authentication configuration failed.");
    case 10: return QStringLiteral("Wi-Fi authentication failed; the access point may have rejected the credentials.");
    case 11: return QStringLiteral("Wi-Fi authentication timed out.");
    case 15: return QStringLiteral("The DHCP client could not start.");
    case 16: return QStringLiteral("The DHCP client reported an error.");
    case 17: return QStringLiteral("DHCP did not obtain an address.");
    case 35: return QStringLiteral("The device firmware is missing.");
    case 39: return QStringLiteral("The device was disconnected by a user or network client.");
    case 40: return QStringLiteral("The adapter's carrier or link changed.");
    case 53: return QStringLiteral("The SSID was not found.");
    default: return reason ? QStringLiteral("NetworkManager device reason %1.").arg(reason) : QString();
    }
}
uint reasonNumber(const QVariant &v)
{
    const QVariant raw = unwrap(v);
    if (raw.metaType() == QMetaType::fromType<QDBusArgument>()) {
        const QDBusArgument argument = raw.value<QDBusArgument>();
        uint state = 0, reason = 0;
        argument.beginStructure(); argument >> state >> reason; argument.endStructure();
        return reason;
    }
    return raw.toList().value(1).toUInt();
}
QStringList ipAddresses(const QVariantMap &properties)
{
    QStringList out;
    for (const auto &a : NetworkTypes::addressData(properties.value(QStringLiteral("AddressData")))) {
        const QString address = a.value(QStringLiteral("address")).toString();
        if (!address.isEmpty()) out << address + QLatin1Char('/') + QString::number(a.value(QStringLiteral("prefix")).toUInt());
    }
    return out;
}
QSet<QString> methodNames(const QString &xml, const QString &interface)
{
    QXmlStreamReader reader(xml);
    QSet<QString> out;
    bool inside = false;
    while (!reader.atEnd()) {
        reader.readNext();
        if (reader.isStartElement() && reader.name() == QLatin1String("interface"))
            inside = reader.attributes().value(QStringLiteral("name")) == interface;
        else if (reader.isEndElement() && reader.name() == QLatin1String("interface")) inside = false;
        else if (inside && reader.isStartElement() && reader.name() == QLatin1String("method"))
            out.insert(reader.attributes().value(QStringLiteral("name")).toString());
    }
    return reader.hasError() ? QSet<QString>() : out;
}
bool permission(const NetworkSnapshot &snapshot, const QString &name)
{
    const QString value = snapshot.permissions.value(nm + QLatin1Char('.') + name);
    return value == QLatin1String("yes") || value == QLatin1String("auth");
}
bool authorized(const NetworkSnapshot &snapshot, const QString &name)
{
    return snapshot.permissions.value(nm + QLatin1Char('.') + name) == QLatin1String("yes");
}
bool profileAuthorization(const NetworkSnapshot &snapshot, const NetworkProfile &profile)
{
    const auto value = profile.settings.value(QStringLiteral("connection")).value(QStringLiteral("permissions"));
    const QStringList users = value.metaType() == QMetaType::fromType<QDBusArgument>() ? qdbus_cast<QStringList>(value) : value.toStringList();
    return authorized(snapshot, users.size() == 1 ? QStringLiteral("settings.modify.own") : QStringLiteral("settings.modify.system"));
}
bool supportedProfile(const NetworkProfile &profile)
{
    if (profile.type != QLatin1String("802-3-ethernet") && profile.type != QLatin1String("802-11-wireless")) return false;
    const auto connection = profile.settings.value(QStringLiteral("connection"));
    if (connection.value(QStringLiteral("read-only")).toBool()
        || !connection.value(QStringLiteral("controller")).toString().isEmpty()
        || !connection.value(QStringLiteral("master")).toString().isEmpty()
        || !connection.value(QStringLiteral("port-type")).toString().isEmpty()
        || !connection.value(QStringLiteral("slave-type")).toString().isEmpty()) return false;
    if (profile.type == QLatin1String("802-11-wireless")) {
        const auto security = profile.settings.value(QStringLiteral("802-11-wireless-security"));
        const QString key = security.value(QStringLiteral("key-mgmt")).toString();
        if (profile.settings.contains(QStringLiteral("802-1x")) || (!key.isEmpty() && key != QLatin1String("wpa-psk") && key != QLatin1String("sae"))
            || security.contains(QStringLiteral("wep-key0"))) return false;
    }
    // Credentials and advanced settings are preserved verbatim. Only basic
    // auto/manual IP modes can be represented accurately by the native form.
    for (const QString &family : {QStringLiteral("ipv4"), QStringLiteral("ipv6")}) {
        const QString method = profile.settings.value(family).value(QStringLiteral("method")).toString();
        if (!method.isEmpty() && method != QLatin1String("auto") && method != QLatin1String("manual")) return false;
    }
    QString error;
    return NetworkTypes::validatePreferences(NetworkTypes::preferences(profile), error);
}
void lockWrites(NetworkSnapshot &snapshot)
{
    const QString reason = QStringLiteral("A network operation has an unresolved outcome. Further writes are locked until NetworkManager restarts; current state can still be refreshed.");
    snapshot.checkpointAvailable = false;
    snapshot.reason = reason;
    for (auto &profile : snapshot.profiles) { profile.editable = false; profile.reason = reason; }
}
}

NetworkManagerBackend::NetworkManagerBackend(QObject *parent)
    : NetworkManagerBackend(QDBusConnection::systemBus(), Options(), parent) {}

NetworkManagerBackend::NetworkManagerBackend(const QDBusConnection &bus, const Options &options, QObject *parent)
    : NetworkBackend(parent), m_bus(bus), m_options(options)
{
    NetworkTypes::registerDbusTypes();
    qDBusRegisterMetaType<QList<QDBusObjectPath>>();
    qDBusRegisterMetaType<QMap<QString, QString>>();
    qDBusRegisterMetaType<QMap<QString, uint>>();
    m_options.readTimeoutMs = std::clamp(m_options.readTimeoutMs, 50, 10000);
    m_options.snapshotTimeoutMs = std::clamp(m_options.snapshotTimeoutMs, 200, 30000);
    m_options.writeTimeoutMs = std::clamp(m_options.writeTimeoutMs, 100, 30000);
    m_options.rollbackSeconds = std::clamp(m_options.rollbackSeconds, 2u, 120u);
    m_options.pollIntervalMs = std::clamp(m_options.pollIntervalMs, 20, 1000);
    auto *watcher = new QDBusServiceWatcher(m_options.service, m_bus, QDBusServiceWatcher::WatchForOwnerChange, this);
    connect(watcher, &QDBusServiceWatcher::serviceOwnerChanged, this, &NetworkManagerBackend::ownerChanged);
    m_bus.connect(m_options.service, QString(), propertiesInterface, QStringLiteral("PropertiesChanged"),
                  this, SLOT(propertiesChanged(QString,QVariantMap,QStringList)));
    for (const QString &signal : {QStringLiteral("DeviceAdded"), QStringLiteral("DeviceRemoved"), QStringLiteral("CheckPermissions")})
        m_bus.connect(m_options.service, root, nm, signal, this, SLOT(daemonChanged()));
    for (const QString &signal : {QStringLiteral("NewConnection"), QStringLiteral("ConnectionRemoved")})
        m_bus.connect(m_options.service, settingsPath, settingsInterface, signal, this, SLOT(daemonChanged()));
    m_bus.connect(m_options.service, QString(), profileInterface, QStringLiteral("Updated"), this, SLOT(daemonChanged()));
    m_deadline.setSingleShot(true);
    connect(&m_deadline, &QTimer::timeout, this, [this] {
        if (m_busy && !m_checkpoint.isEmpty()) rollback(QStringLiteral("The confirmation time expired; NetworkManager is restoring the checkpoint."));
    });
}

void NetworkManagerBackend::call(const QString &destination, const QString &path, const QString &interface,
                                const QString &method, const QVariantList &args, bool write, const ReplyCallback &done)
{
    QDBusMessage message = QDBusMessage::createMethodCall(destination, path, interface, method);
    message.setArguments(args);
    // Request noninteractive authorization for connectivity mutations, after
    // checking that GetPermissions reports "yes". This flag is not an atomic
    // authorization guarantee: NM 1.54.3 Update2's auth_start allows interaction
    // independently. Policy races or timed-out calls therefore remain uncertain
    // and lock further writes. Checkpoint authorization precedes all mutations.
    message.setInteractiveAuthorizationAllowed(write && (method == QLatin1String("CheckpointCreate")
        || method == QLatin1String("RequestScan") || (method == QLatin1String("Update2") && m_checkpoint.isEmpty())));
    const bool mutation = write && (method == QLatin1String("ActivateConnection") || method == QLatin1String("AddAndActivateConnection2")
        || method == QLatin1String("Disconnect") || method == QLatin1String("Update2"));
    const quint64 generation = m_generation;
    if (mutation) m_mutationPending = true;
    auto *watcher = new QDBusPendingCallWatcher(m_bus.asyncCall(message, write ? m_options.writeTimeoutMs : m_options.readTimeoutMs), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, done, mutation, generation] {
        const QDBusMessage reply = watcher->reply();
        watcher->deleteLater();
        if (mutation && generation == m_generation) m_mutationPending = false;
        done(reply);
    });
}

QString NetworkManagerBackend::operationError(const QDBusMessage &reply) const
{
    // Do not surface daemon error text: some daemons/agents echo request data,
    // including a secret. Error names plus fixed explanations are sufficient.
    const QString name = reply.errorName();
    if (name.contains(QLatin1String("Cancelled"), Qt::CaseInsensitive) || name.contains(QLatin1String("Canceled"), Qt::CaseInsensitive))
        return QStringLiteral("Authorization was canceled.");
    if (name.contains(QLatin1String("PermissionDenied")) || name.contains(QLatin1String("NotAuthorized")) || name.contains(QLatin1String("AccessDenied")))
        return QStringLiteral("NetworkManager denied authorization for this operation.");
    if (name.contains(QLatin1String("NoSecrets")) || name.contains(QLatin1String("SecretsRequired")))
        return QStringLiteral("NetworkManager requires credentials; check the password or unlock the secret agent.");
    if (name.contains(QLatin1String("VersionIdMismatch")) || name.contains(QLatin1String("VersionMismatch")))
        return QStringLiteral("The saved connection changed in another application. Refresh before applying.");
    if (name.contains(QLatin1String("NoReply")) || name.contains(QLatin1String("Timeout")))
        return QStringLiteral("NetworkManager did not confirm the operation before the deadline; its outcome is uncertain.");
    if (name.contains(QLatin1String("ServiceUnknown")) || name.contains(QLatin1String("NameHasNoOwner")) || name.contains(QLatin1String("Disconnected")))
        return QStringLiteral("NetworkManager is unavailable or restarted.");
    if (name.contains(QLatin1String("UnknownMethod")) || name.contains(QLatin1String("UnknownInterface")))
        return QStringLiteral("This NetworkManager does not support the required D-Bus operation.");
    return QStringLiteral("NetworkManager rejected or could not complete the operation.");
}

void NetworkManagerBackend::getAll(const QString &owner, const QString &path, const QString &interface,
                                  const std::function<void(bool, const QVariantMap &, const QString &)> &done)
{
    call(owner, path, propertiesInterface, QStringLiteral("GetAll"), {interface}, false,
         [this, done](const QDBusMessage &reply) {
        if (reply.type() == QDBusMessage::ErrorMessage || reply.arguments().size() != 1)
            done(false, {}, operationError(reply));
        else done(true, mapValue(reply.arguments().first()), {});
    });
}

void NetworkManagerBackend::readSnapshot(const SnapshotCallback &done)
{
    struct Read {
        NetworkSnapshot snapshot;
        QString owner;
        QString error;
        int pending = 1;
        bool finished = false;
        bool supportsActivate2 = false;
    };
    const auto state = QSharedPointer<Read>::create();
    auto complete = [this, state, done](const QString &error = QString()) {
        if (state->finished) return;
        if (!error.isEmpty() && state->error.isEmpty()) state->error = error;
        if (--state->pending) return;
        state->finished = true;
        if (!state->error.isEmpty()) { done(false, state->snapshot, state->error); return; }
        // Confirm the owner at the end as well as at the start. We never combine
        // inventory from a replacement daemon with an earlier generation.
        call(QStringLiteral("org.freedesktop.DBus"), QStringLiteral("/org/freedesktop/DBus"),
             QStringLiteral("org.freedesktop.DBus"), QStringLiteral("GetNameOwner"), {m_options.service}, false,
             [this, state, done](const QDBusMessage &reply) {
            if (reply.type() == QDBusMessage::ErrorMessage || reply.arguments().value(0).toString() != state->owner) {
                done(false, state->snapshot, QStringLiteral("NetworkManager changed while its state was being read.")); return;
            }
            auto &snapshot = state->snapshot;
            for (auto &profile : snapshot.profiles) {
                profile.editable = profile.versionId && supportedProfile(profile)
                    && (permission(snapshot, QStringLiteral("settings.modify.system")) || permission(snapshot, QStringLiteral("settings.modify.own")));
                if (!profile.editable) profile.reason = QStringLiteral("This profile is read-only, has unsupported IP/controller settings, or cannot be safely updated with this account.");
            }
            std::sort(snapshot.devices.begin(), snapshot.devices.end(), [](const auto &a, const auto &b) { return a.path < b.path; });
            std::sort(snapshot.profiles.begin(), snapshot.profiles.end(), [](const auto &a, const auto &b) { return a.path < b.path; });
            std::sort(snapshot.accessPoints.begin(), snapshot.accessPoints.end(), [](const auto &a, const auto &b) { return a.path < b.path; });
            snapshot.serviceAvailable = true;
            if (m_uncertain) lockWrites(snapshot);
            m_supportsActivate2 = state->supportsActivate2;
            m_last = snapshot;
            done(true, snapshot, {});
        });
    };
    QTimer::singleShot(m_options.snapshotTimeoutMs, this, [state, done] {
        if (state->finished) return;
        state->finished = true;
        done(false, state->snapshot, QStringLiteral("NetworkManager inventory timed out; no settings were changed."));
    });
    call(QStringLiteral("org.freedesktop.DBus"), QStringLiteral("/org/freedesktop/DBus"),
         QStringLiteral("org.freedesktop.DBus"), QStringLiteral("GetNameOwner"), {m_options.service}, false,
         [this, state, complete](const QDBusMessage &reply) {
        if (state->finished) return;
        if (reply.type() == QDBusMessage::ErrorMessage) { complete(operationError(reply)); return; }
        state->owner = reply.arguments().value(0).toString();
        if (!state->owner.startsWith(QLatin1Char(':'))) { complete(QStringLiteral("NetworkManager has no unique D-Bus owner.")); return; }
        state->snapshot.serviceOwner = state->owner;
        state->pending += 4;
        call(state->owner, root, QStringLiteral("org.freedesktop.DBus.Introspectable"), QStringLiteral("Introspect"), {}, false,
             [state, complete](const QDBusMessage &r) {
            if (state->finished) return;
            const auto methods = methodNames(r.arguments().value(0).toString(), nm);
            state->snapshot.checkpointAvailable = methods.contains(QStringLiteral("CheckpointCreate"))
                && methods.contains(QStringLiteral("CheckpointRollback")) && methods.contains(QStringLiteral("CheckpointDestroy"));
            state->supportsActivate2 = methods.contains(QStringLiteral("AddAndActivateConnection2"));
            complete(r.type() == QDBusMessage::ErrorMessage ? QStringLiteral("NetworkManager capabilities could not be inspected.") : QString());
        });
        call(state->owner, root, nm, QStringLiteral("GetPermissions"), {}, false,
             [this, state, complete](const QDBusMessage &r) {
            if (state->finished) return;
            if (r.type() == QDBusMessage::ErrorMessage) { complete(operationError(r)); return; }
            state->snapshot.permissions = qdbus_cast<QMap<QString, QString>>(r.arguments().value(0));
            complete();
        });
        getAll(state->owner, root, nm, [this, state, complete](bool ok, const QVariantMap &p, const QString &error) {
            if (state->finished) return;
            if (!ok) { complete(error); return; }
            state->snapshot.version = p.value(QStringLiteral("Version")).toString();
            state->snapshot.connectivity = p.value(QStringLiteral("Connectivity")).toUInt();
            state->snapshot.state = p.value(QStringLiteral("State")).toUInt();
            state->snapshot.connectivityCheckAvailable = p.value(QStringLiteral("ConnectivityCheckAvailable")).toBool();
            state->snapshot.connectivityCheckEnabled = p.value(QStringLiteral("ConnectivityCheckEnabled")).toBool();
            state->snapshot.wirelessEnabled = p.value(QStringLiteral("WirelessEnabled")).toBool();
            state->snapshot.wirelessHardwareEnabled = p.value(QStringLiteral("WirelessHardwareEnabled")).toBool();
            state->snapshot.networkingEnabled = p.value(QStringLiteral("NetworkingEnabled")).toBool();
            state->snapshot.checkpoints = pathList(p.value(QStringLiteral("Checkpoints")));
            const auto devices = pathList(p.value(QStringLiteral("Devices")));
            if (devices.size() > 256) { complete(QStringLiteral("NetworkManager returned too many devices.")); return; }
            for (const QString &path : devices) {
                if (!numberedPath(path, QStringLiteral("Devices"))) { complete(QStringLiteral("NetworkManager returned an invalid device path.")); return; }
                ++state->pending;
                getAll(state->owner, path, deviceInterface, [this, state, complete, path](bool deviceOk, const QVariantMap &d, const QString &deviceError) {
                    if (state->finished) return;
                    if (!deviceOk) { complete(deviceError); return; }
                    NetworkDevice device;
                    device.path = path;
                    device.interfaceName = d.value(QStringLiteral("Interface")).toString();
                    device.type = d.value(QStringLiteral("DeviceType")).toUInt();
                    device.state = d.value(QStringLiteral("State")).toUInt();
                    device.managed = d.value(QStringLiteral("Managed")).toBool();
                    device.stateReason = reasonNumber(d.value(QStringLiteral("StateReason")));
                    device.reason = stateReason(device.stateReason);
                    device.activeConnectionPath = pathValue(d.value(QStringLiteral("ActiveConnection")));
                    device.availableProfilePaths = pathList(d.value(QStringLiteral("AvailableConnections")));
                    const int index = state->snapshot.devices.size();
                    state->snapshot.devices << device;
                    if (device.managed && (device.type == 1 || device.type == 2) && device.state == 100) {
                        ++state->pending;
                        call(state->owner, path, deviceInterface, QStringLiteral("GetAppliedConnection"), {uint(0)}, false,
                             [this, state, complete, index](const QDBusMessage &appliedReply) {
                            if (state->finished) return;
                            if (appliedReply.type() == QDBusMessage::ErrorMessage || appliedReply.arguments().size() != 2) {
                                complete(operationError(appliedReply)); return;
                            }
                            state->snapshot.devices[index].appliedSettings = qdbus_cast<NetworkConnectionData>(appliedReply.arguments().value(0));
                            state->snapshot.devices[index].appliedVersionId = appliedReply.arguments().value(1).toULongLong();
                            complete();
                        });
                    }
                    if (numberedPath(device.activeConnectionPath, QStringLiteral("ActiveConnection"))) {
                        ++state->pending;
                        getAll(state->owner, device.activeConnectionPath, activeInterface,
                               [state, complete, index](bool activeOk, const QVariantMap &a, const QString &activeError) {
                            if (state->finished) return;
                            if (activeOk) state->snapshot.devices[index].activeProfilePath = pathValue(a.value(QStringLiteral("Connection")));
                            complete(activeOk ? QString() : activeError);
                        });
                    }
                    for (const QString &family : {QStringLiteral("Ip4Config"), QStringLiteral("Ip6Config")}) {
                        const QString ipPath = pathValue(d.value(family));
                        if (!numberedPath(ipPath, family == QLatin1String("Ip4Config") ? QStringLiteral("IP4Config") : QStringLiteral("IP6Config"))) continue;
                        ++state->pending;
                        getAll(state->owner, ipPath, nm + (family == QLatin1String("Ip4Config") ? QStringLiteral(".IP4Config") : QStringLiteral(".IP6Config")),
                               [state, complete, index, family](bool ipOk, const QVariantMap &ip, const QString &ipError) {
                            if (state->finished) return;
                            if (ipOk) {
                                if (family == QLatin1String("Ip4Config")) state->snapshot.devices[index].ipv4Addresses = ipAddresses(ip);
                                else state->snapshot.devices[index].ipv6Addresses = ipAddresses(ip);
                            }
                            complete(ipOk ? QString() : ipError);
                        });
                    }
                    if (device.type == 1) {
                        ++state->pending;
                        getAll(state->owner, path, deviceInterface + QStringLiteral(".Wired"),
                               [state, complete, index](bool wiredOk, const QVariantMap &w, const QString &wiredError) {
                            if (state->finished) return;
                            if (wiredOk) state->snapshot.devices[index].carrier = w.value(QStringLiteral("Carrier")).toBool();
                            complete(wiredOk ? QString() : wiredError);
                        });
                    }
                    if (device.type == 2) {
                        ++state->pending;
                        getAll(state->owner, path, wifiInterface, [this, state, complete, index, path](bool wifiOk, const QVariantMap &w, const QString &wifiError) {
                            if (state->finished) return;
                            if (!wifiOk) { complete(wifiError); return; }
                            state->snapshot.devices[index].activeAccessPointPath = pathValue(w.value(QStringLiteral("ActiveAccessPoint")));
                            const auto aps = pathList(w.value(QStringLiteral("AccessPoints")));
                            if (aps.size() > 2048) { complete(QStringLiteral("NetworkManager returned too many access points.")); return; }
                            for (const QString &apPath : aps) {
                                if (!numberedPath(apPath, QStringLiteral("AccessPoint"))) { complete(QStringLiteral("NetworkManager returned an invalid access point path.")); return; }
                                ++state->pending;
                                getAll(state->owner, apPath, nm + QStringLiteral(".AccessPoint"),
                                       [state, complete, path, apPath](bool apOk, const QVariantMap &ap, const QString &apError) {
                                    if (state->finished) return;
                                    if (!apOk) { complete(apError); return; }
                                    NetworkAccessPoint accessPoint;
                                    accessPoint.path = apPath; accessPoint.devicePath = path;
                                    accessPoint.ssid = ap.value(QStringLiteral("Ssid")).toByteArray();
                                    accessPoint.name = QString::fromUtf8(accessPoint.ssid);
                                    accessPoint.strength = ap.value(QStringLiteral("Strength")).toUInt();
                                    const uint flags = ap.value(QStringLiteral("Flags")).toUInt();
                                    const uint wpa = ap.value(QStringLiteral("WpaFlags")).toUInt();
                                    const uint rsn = ap.value(QStringLiteral("RsnFlags")).toUInt();
                                    const uint security = wpa | rsn;
                                    if (ap.value(QStringLiteral("Mode")).toUInt() != 2 || accessPoint.ssid.isEmpty() || accessPoint.ssid.size() > 32)
                                        accessPoint.reason = QStringLiteral("Hidden or non-infrastructure Wi-Fi is not supported by this form.");
                                    else if (!security && !(flags & 1u)) accessPoint.security = NetworkAccessPoint::Security::Open;
                                    else if (rsn & 0x400u) accessPoint.security = NetworkAccessPoint::Security::Sae;
                                    else if (security & 0x100u) accessPoint.security = NetworkAccessPoint::Security::WpaPsk;
                                    else accessPoint.reason = QStringLiteral("This network requires enterprise, WEP, OWE, or other unsupported credentials.");
                                    state->snapshot.accessPoints << accessPoint;
                                    complete();
                                });
                            }
                            complete();
                        });
                    }
                    complete();
                });
            }
            complete();
        });
        call(state->owner, settingsPath, settingsInterface, QStringLiteral("ListConnections"), {}, false,
             [this, state, complete](const QDBusMessage &r) {
            if (state->finished) return;
            if (r.type() == QDBusMessage::ErrorMessage) { complete(operationError(r)); return; }
            const auto profiles = pathList(r.arguments().value(0));
            if (profiles.size() > 2048) { complete(QStringLiteral("NetworkManager returned too many saved profiles.")); return; }
            for (const QString &path : profiles) {
                if (!numberedPath(path, QStringLiteral("Settings"))) { complete(QStringLiteral("NetworkManager returned an invalid saved profile path.")); return; }
                ++state->pending;
                getAll(state->owner, path, profileInterface,
                     [this, state, complete, path](bool beforeOk, const QVariantMap &before, const QString &beforeError) {
                    if (state->finished) return;
                    if (!beforeOk) { complete(beforeError); return; }
                    call(state->owner, path, profileInterface, QStringLiteral("GetSettings"), {}, false,
                     [this, state, complete, path, before](const QDBusMessage &profileReply) {
                    if (state->finished) return;
                    if (profileReply.type() == QDBusMessage::ErrorMessage) { complete(operationError(profileReply)); return; }
                    NetworkProfile profile;
                    profile.path = path;
                    profile.settings = qdbus_cast<NetworkConnectionData>(profileReply.arguments().value(0));
                    const auto c = profile.settings.value(QStringLiteral("connection"));
                    profile.id = c.value(QStringLiteral("id")).toString(); profile.uuid = c.value(QStringLiteral("uuid")).toString();
                    profile.type = c.value(QStringLiteral("type")).toString();
                    const int index = state->snapshot.profiles.size();
                    state->snapshot.profiles << profile;
                    ++state->pending;
                    getAll(state->owner, path, profileInterface,
                           [state, complete, index, before](bool profileOk, const QVariantMap &p, const QString &profileError) {
                        if (state->finished) return;
                        if (profileOk && (before.value(QStringLiteral("VersionId")) != p.value(QStringLiteral("VersionId"))
                            || before.value(QStringLiteral("Unsaved")) != p.value(QStringLiteral("Unsaved"))
                            || before.value(QStringLiteral("Flags")) != p.value(QStringLiteral("Flags")))) {
                            complete(QStringLiteral("A saved connection changed while its settings were being read. Refresh and retry.")); return;
                        }
                        if (profileOk) {
                            auto &profile = state->snapshot.profiles[index];
                            profile.versionId = p.value(QStringLiteral("VersionId")).toULongLong();
                            profile.unsaved = p.value(QStringLiteral("Unsaved")).toBool();
                            profile.flags = p.value(QStringLiteral("Flags")).toUInt();
                        }
                        complete(profileOk ? QString() : profileError);
                    });
                    complete();
                    });
                });
            }
            complete();
        });
        complete();
    });
}

void NetworkManagerBackend::readCurrent()
{
    if (m_readBusy) return;
    m_readBusy = true;
    readSnapshot([this](bool ok, const NetworkSnapshot &snapshot, const QString &error) {
        m_readBusy = false;
        emit readFinished(ok, snapshot, error);
    });
}

void NetworkManagerBackend::scan(const QString &devicePath)
{
    if (m_scanBusy || m_busy || m_uncertain) { emit scanFinished(false, QStringLiteral("A network operation is already pending.")); return; }
    m_scanBusy = true;
    readSnapshot([this, devicePath](bool ok, const NetworkSnapshot &snapshot, const QString &error) {
        const auto *device = snapshot.device(devicePath);
        if (!ok || !device || !numberedPath(devicePath, QStringLiteral("Devices")) || !device->managed || device->type != 2
            || !permission(snapshot, QStringLiteral("wifi.scan"))) {
            m_scanBusy = false;
            emit scanFinished(false, ok ? QStringLiteral("Wi-Fi scanning is unavailable for this device or account.") : error); return;
        }
        call(snapshot.serviceOwner, devicePath, wifiInterface, QStringLiteral("RequestScan"), {QVariantMap()}, true,
             [this](const QDBusMessage &reply) {
            m_scanBusy = false;
            const bool ok = reply.type() != QDBusMessage::ErrorMessage;
            emit scanFinished(ok, ok ? QString() : operationError(reply));
            if (ok) emit externalChange();
        });
    });
}

bool NetworkManagerBackend::sameOwnership(const NetworkSnapshot &snapshot) const
{
    return snapshot.serviceAvailable && !m_owner.isEmpty() && snapshot.serviceOwner == m_owner;
}

bool NetworkManagerBackend::checkpointLive(const NetworkSnapshot &snapshot) const
{
    return !m_checkpoint.isEmpty() && snapshot.checkpoints.contains(m_checkpoint)
        && m_elapsed.isValid() && m_elapsed.elapsed() < qint64(m_options.rollbackSeconds) * 1000;
}

bool NetworkManagerBackend::expectedProfiles(const NetworkSnapshot &snapshot) const
{
    if (!sameOwnership(snapshot)) return false;
    if (snapshot.devices.size() != m_baseline.devices.size()) return false;
    for (const auto &original : m_baseline.devices) {
        const auto *current = snapshot.device(original.path);
        if (!current || current->interfaceName != original.interfaceName || current->type != original.type || current->managed != original.managed) return false;
        if (original.path != m_request.devicePath && (current->state != original.state || current->activeConnectionPath != original.activeConnectionPath
            || current->activeProfilePath != original.activeProfilePath || current->appliedVersionId != original.appliedVersionId
            || !NetworkTypes::sameSettings(current->appliedSettings, original.appliedSettings))) return false;
    }
    for (const auto &original : m_baseline.profiles) {
        const auto *current = snapshot.profile(original.path);
        if (!current || current->uuid != original.uuid) return false;
        if (m_request.action == NetworkRequest::Action::UpdateProfile && original.path == m_request.profilePath) {
            if (!NetworkTypes::sameSettings(m_expectedSettings, current->settings)) return false;
            if (m_profileVersion && current->versionId != m_profileVersion) return false;
        } else if (current->versionId != original.versionId || !NetworkTypes::sameSettings(original.settings, current->settings)) return false;
    }
    // New profiles belonging to other applications are never deleted. A new
    // profile is therefore not a reason to use the global DELETE_NEW flag.
    return true;
}

bool NetworkManagerBackend::restoredState(const NetworkSnapshot &snapshot) const
{
    if (!sameOwnership(snapshot)) return false;
    for (const auto &before : m_baseline.profiles) {
        const auto *after = snapshot.profile(before.path);
        if (!after || after->uuid != before.uuid || after->unsaved != before.unsaved
            || !NetworkTypes::sameSettings(before.settings, after->settings)) return false;
    }
    if (!m_newProfile.isEmpty() && snapshot.profile(m_newProfile)) return false;
    for (const auto &before : m_baseline.devices) {
        const auto *after = snapshot.device(before.path);
        if (!after || before.interfaceName != after->interfaceName || before.type != after->type || before.managed != after->managed) return false;
        const bool wasActive = before.state == 100 && numberedPath(before.activeProfilePath, QStringLiteral("Settings"));
        if (wasActive) {
            if (after->state != 100 || after->activeProfilePath != before.activeProfilePath) return false;
            if (!NetworkTypes::sameSettings(after->appliedSettings, before.appliedSettings)) return false;
        } else if (before.activeConnectionPath == QLatin1String("/") || before.activeConnectionPath.isEmpty()) {
            if (after->activeConnectionPath != QLatin1String("/") && !after->activeConnectionPath.isEmpty()) return false;
            if (after->state != before.state) return false;
        } else return false;
    }
    return true;
}

void NetworkManagerBackend::beginChange(const NetworkSnapshot &baseline, const NetworkRequest &request)
{
    if (m_busy || m_uncertain || m_scanBusy) {
        emit transactionFinished(false, false, m_last, QStringLiteral("A network operation has an unresolved outcome. Further writes are locked until NetworkManager restarts; current state can still be refreshed.")); return;
    }
    QString error;
    if (!NetworkTypes::validateRequest(baseline, request, error)
        || (!request.devicePath.isEmpty() && !numberedPath(request.devicePath, QStringLiteral("Devices")))
        || (!request.profilePath.isEmpty() && !numberedPath(request.profilePath, QStringLiteral("Settings")))
        || (!request.accessPointPath.isEmpty() && !numberedPath(request.accessPointPath, QStringLiteral("AccessPoint")))) {
        emit transactionFinished(false, false, m_last, error.isEmpty() ? QStringLiteral("The network request contains an invalid object identity.") : error); return;
    }
    m_busy = true;
    m_confirming = m_rollingBack = false;
    m_baseline = baseline;
    m_request = request;
    m_checkpoint.clear(); m_newProfile.clear(); m_activeTarget.clear(); m_newUuid.clear(); m_failure.clear();
    m_profileVersion = 0;
    m_checkpointKnownAbsent = true;
    const quint64 generation = ++m_generation;
    readSnapshot([this, generation](bool ok, const NetworkSnapshot &current, const QString &readError) {
        if (generation != m_generation || !m_busy) return;
        QString validationError;
        if (!ok || !NetworkTypes::sameSnapshot(m_baseline, current)
            || !NetworkTypes::validateRequest(current, m_request, validationError)) {
            finish(false, ok && NetworkTypes::sameSnapshot(m_baseline, current), current,
                   !ok ? readError : validationError.isEmpty() ? QStringLiteral("The network configuration changed. Reload before applying.") : validationError); return;
        }
        if (m_request.action == NetworkRequest::Action::ConnectWifi) {
            const auto *original = m_baseline.accessPoint(m_request.accessPointPath);
            const auto *now = current.accessPoint(m_request.accessPointPath);
            if (!original || !now || original->ssid != now->ssid || original->security != now->security || original->devicePath != now->devicePath) {
                finish(false, true, current, QStringLiteral("The selected Wi-Fi network changed. Refresh the network list.")); return;
            }
        }
        m_baseline = current;
        m_owner = current.serviceOwner;
        const bool edit = m_request.action == NetworkRequest::Action::UpdateProfile;
        if (edit) {
            const auto *profile = current.profile(m_request.profilePath);
            m_expectedSettings = NetworkTypes::mergePreferences(*profile, m_request.preferences);
            m_profileVersion = profile->versionId;
            // An inactive saved preference edit changes no live network state.
            // It is committed with CAS and readback like other ordinary settings.
            QString activeDevice;
            for (const auto &device : current.devices) if (device.activeProfilePath == profile->path) {
                if (!activeDevice.isEmpty()) {
                    finish(false, true, current, QStringLiteral("This profile is active on multiple adapters; edit it in an advanced network tool.")); return;
                }
                activeDevice = device.path;
            }
            if (activeDevice.isEmpty()) { persist(true); return; }
            m_request.devicePath = activeDevice;
        }
        if (!current.checkpointAvailable || !permission(current, QStringLiteral("checkpoint-rollback"))
            || !authorized(current, QStringLiteral("network-control"))) {
            finish(false, true, current, QStringLiteral("NetworkManager recovery or network-control permission is unavailable. No connectivity changes were made.")); return;
        }
        const auto *device = current.device(m_request.devicePath);
        if (!device || !device->managed || (device->type != 1 && device->type != 2)
            || (device->state != 30 && device->state != 100)) {
            finish(false, true, current, QStringLiteral("The adapter is unavailable or is already changing state. Wait and refresh.")); return;
        }
        if (m_request.action == NetworkRequest::Action::ConnectWifi
            && (!m_supportsActivate2 || !authorized(current, QStringLiteral("settings.modify.system")))) {
            finish(false, true, current, QStringLiteral("This account cannot create a Wi-Fi profile.")); return;
        }
        if (edit && !profileAuthorization(current, *current.profile(m_request.profilePath))) {
            finish(false, true, current, QStringLiteral("Active profile editing requires authorization before the recovery timer starts. Refresh after authorization is available.")); return;
        }
        createCheckpoint();
    });
}

void NetworkManagerBackend::createCheckpoint()
{
    const quint64 generation = m_generation;
    // Start our conservative budget before dispatch, including authorization
    // and reply-delivery time. The daemon creates its own full timed checkpoint
    // after authorization; we never overestimate its remaining lifetime.
    m_elapsed.start();
    QList<QDBusObjectPath> devices;
    // Only Ethernet/Wi-Fi adapters can be mutated by this backend. Including
    // loopback/P2P/software pseudo-devices can reject the entire checkpoint:
    // NM 1.54.3 nm-checkpoint-manager.c rejects explicitly listed unrealized
    // devices. Protect all supported adapters without touching those devices.
    for (const auto &device : m_baseline.devices) if (device.managed && (device.type == 1 || device.type == 2)) devices << QDBusObjectPath(device.path);
    if (devices.isEmpty()) { finish(false, true, m_baseline, QStringLiteral("No managed adapters are available for recovery.")); return; }
    call(m_owner, root, nm, QStringLiteral("CheckpointCreate"),
         {QVariant::fromValue(devices), m_options.rollbackSeconds, uint(0)}, true,
         [this, generation](const QDBusMessage &reply) {
        if (generation != m_generation || !m_busy) return;
        if (reply.type() == QDBusMessage::ErrorMessage) {
            if (reply.errorName().contains(QLatin1String("NoReply")) || reply.errorName().contains(QLatin1String("Timeout")))
                uncertain(operationError(reply));
            else {
                const QString failure = operationError(reply);
                readSnapshot([this, generation, failure](bool ok, const NetworkSnapshot &current, const QString &error) {
                    if (generation != m_generation || !m_busy) return;
                    finish(false, ok && NetworkTypes::sameSnapshot(m_baseline, current), current, ok ? failure : error);
                });
            }
            return;
        }
        m_checkpoint = pathValue(reply.arguments().value(0));
        if (!numberedPath(m_checkpoint, QStringLiteral("Checkpoint"))) {
            uncertain(QStringLiteral("NetworkManager did not return a valid recovery checkpoint.")); return;
        }
        m_checkpointKnownAbsent = false;
        m_deadline.start(std::max(1, int(qint64(m_options.rollbackSeconds) * 1000 - m_elapsed.elapsed())));
        readSnapshot([this, generation](bool ok, const NetworkSnapshot &current, const QString &error) {
            if (generation != m_generation || !m_busy) return;
            if (!ok || !sameOwnership(current)) { uncertain(ok ? QStringLiteral("NetworkManager restarted before applying the change.") : error); return; }
            if (!checkpointLive(current)) { uncertain(QStringLiteral("The recovery checkpoint expired before applying. No connectivity change was dispatched.")); return; }
            if (!NetworkTypes::sameSnapshot(m_baseline, current)) {
                // The checkpoint belongs to an earlier configuration. Do not
                // issue a stale destructive rollback against the newer state.
                abandonOwnedCheckpoint(current, QStringLiteral("The network configuration changed after recovery was prepared. Recovery was canceled to preserve the newer configuration.")); return;
            }
            if (!authorized(current, QStringLiteral("network-control"))
                || (m_request.action == NetworkRequest::Action::UpdateProfile && !profileAuthorization(current, *current.profile(m_request.profilePath)))
                || (m_request.action == NetworkRequest::Action::ConnectWifi && !authorized(current, QStringLiteral("settings.modify.system")))) {
                rollback(QStringLiteral("Authorization changed before applying. No connectivity mutation was dispatched.")); return;
            }
            mutate();
        });
    });
}

void NetworkManagerBackend::mutate()
{
    const quint64 generation = m_generation;
    // Require enough lifetime for a bounded mutating call. An auth prompt can
    // still outlive a client timeout, which is handled as unresolved below.
    if (m_elapsed.elapsed() + m_options.writeTimeoutMs >= qint64(m_options.rollbackSeconds) * 1000) {
        rollback(QStringLiteral("Too little recovery time remains to start the network change.")); return;
    }
    auto replyDone = [this, generation](const QDBusMessage &reply) {
        if (generation != m_generation || !m_busy || m_rollingBack) return;
        if (reply.type() == QDBusMessage::ErrorMessage) {
            if (reply.errorName().contains(QLatin1String("NoReply")) || reply.errorName().contains(QLatin1String("Timeout"))) uncertain(operationError(reply));
            else rollback(operationError(reply));
            return;
        }
        if (m_request.action != NetworkRequest::Action::Disconnect) {
            m_activeTarget = pathValue(reply.arguments().value(m_request.action == NetworkRequest::Action::ConnectWifi ? 1 : 0));
            if (!numberedPath(m_activeTarget, QStringLiteral("ActiveConnection"))) {
                uncertain(QStringLiteral("NetworkManager did not identify the active connection.")); return;
            }
        }
        if (m_request.action == NetworkRequest::Action::ConnectWifi) {
            m_newProfile = pathValue(reply.arguments().value(0));
            if (!numberedPath(m_newProfile, QStringLiteral("Settings"))) {
                uncertain(QStringLiteral("NetworkManager did not identify the temporary Wi-Fi profile.")); return;
            }
        }
        pollOutcome();
    };
    if (m_request.action == NetworkRequest::Action::UpdateProfile) {
        call(m_owner, m_request.profilePath, profileInterface, QStringLiteral("Update2"),
             {QVariant::fromValue(m_expectedSettings), uint(2 | 0x40), QVariantMap{{QStringLiteral("version-id"), QVariant::fromValue(qulonglong(m_profileVersion))}}}, true,
             [this, generation](const QDBusMessage &reply) {
            if (generation != m_generation || !m_busy || m_rollingBack) return;
            if (reply.type() == QDBusMessage::ErrorMessage) {
                if (reply.errorName().contains(QLatin1String("NoReply")) || reply.errorName().contains(QLatin1String("Timeout"))) uncertain(operationError(reply));
                else rollback(operationError(reply));
                return;
            }
            m_profileVersion = 0; // Read the daemon's new revision before activation.
            readSnapshot([this, generation](bool ok, const NetworkSnapshot &current, const QString &error) {
                if (generation != m_generation || !m_busy || m_rollingBack) return;
                const auto *profile = current.profile(m_request.profilePath);
                if (!ok || !expectedProfiles(current) || !profile || !profile->unsaved || !checkpointLive(current)) {
                    uncertain(ok ? QStringLiteral("The in-memory profile or recovery checkpoint could not be verified. NetworkManager owns recovery.") : error); return;
                }
                m_profileVersion = profile->versionId;
                // Update2 does not change the device's applied connection. Only
                // activate after its settings and revision have been read back.
                if (m_elapsed.elapsed() + m_options.writeTimeoutMs >= qint64(m_options.rollbackSeconds) * 1000) {
                    rollback(QStringLiteral("Recovery time expired before profile activation.")); return;
                }
                call(m_owner, root, nm, QStringLiteral("ActivateConnection"),
                     {QVariant::fromValue(QDBusObjectPath(m_request.profilePath)), QVariant::fromValue(QDBusObjectPath(m_request.devicePath)), QVariant::fromValue(QDBusObjectPath(QStringLiteral("/")))}, true,
                     [this, generation](const QDBusMessage &activation) {
                    if (generation != m_generation || !m_busy || m_rollingBack) return;
                    if (activation.type() == QDBusMessage::ErrorMessage) {
                        if (activation.errorName().contains(QLatin1String("NoReply")) || activation.errorName().contains(QLatin1String("Timeout"))) uncertain(operationError(activation));
                        else rollback(operationError(activation));
                        return;
                    }
                    m_activeTarget = pathValue(activation.arguments().value(0));
                    if (!numberedPath(m_activeTarget, QStringLiteral("ActiveConnection"))) { uncertain(QStringLiteral("The activated profile could not be identified.")); return; }
                    pollOutcome();
                });
            });
        });
    } else if (m_request.action == NetworkRequest::Action::Disconnect) {
        call(m_owner, m_request.devicePath, deviceInterface, QStringLiteral("Disconnect"), {}, true, replyDone);
    } else if (m_request.action == NetworkRequest::Action::ActivateProfile) {
        call(m_owner, root, nm, QStringLiteral("ActivateConnection"),
             {QVariant::fromValue(QDBusObjectPath(m_request.profilePath)), QVariant::fromValue(QDBusObjectPath(m_request.devicePath)), QVariant::fromValue(QDBusObjectPath(QStringLiteral("/")))}, true, replyDone);
    } else {
        const auto *ap = m_baseline.accessPoint(m_request.accessPointPath);
        if (!ap) { rollback(QStringLiteral("The selected Wi-Fi network disappeared.")); return; }
        NetworkConnectionData data;
        m_newUuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
        data[QStringLiteral("connection")] = {{QStringLiteral("id"), ap->name}, {QStringLiteral("uuid"), m_newUuid},
            {QStringLiteral("type"), QStringLiteral("802-11-wireless")}, {QStringLiteral("autoconnect"), true}};
        data[QStringLiteral("802-11-wireless")] = {{QStringLiteral("ssid"), ap->ssid}, {QStringLiteral("mode"), QStringLiteral("infrastructure")}};
        data[QStringLiteral("ipv4")] = {{QStringLiteral("method"), QStringLiteral("auto")}};
        data[QStringLiteral("ipv6")] = {{QStringLiteral("method"), QStringLiteral("auto")}};
        if (ap->security != NetworkAccessPoint::Security::Open) {
            data[QStringLiteral("802-11-wireless-security")] = {
                {QStringLiteral("key-mgmt"), ap->security == NetworkAccessPoint::Security::Sae ? QStringLiteral("sae") : QStringLiteral("wpa-psk")},
                {QStringLiteral("psk"), m_request.password}, {QStringLiteral("psk-flags"), uint(0)}};
        }
        // The only secret copy is the short-lived D-Bus request. No GetSecrets,
        // command-line arguments, files, logs, snapshots, or retained requests.
        m_request.password.clear();
        call(m_owner, root, nm, QStringLiteral("AddAndActivateConnection2"),
             {QVariant::fromValue(data), QVariant::fromValue(QDBusObjectPath(m_request.devicePath)),
              QVariant::fromValue(QDBusObjectPath(m_request.accessPointPath)), QVariantMap{{QStringLiteral("persist"), QStringLiteral("volatile")}}}, true, replyDone);
        data.clear();
    }
}

void NetworkManagerBackend::pollOutcome(bool restoring)
{
    const quint64 generation = m_generation;
    readSnapshot([this, generation, restoring](bool ok, const NetworkSnapshot &current, const QString &error) {
        if (generation != m_generation || !m_busy || m_rollingBack != restoring) return;
        if (!ok && restoring && m_elapsed.elapsed() <= qint64(m_options.rollbackSeconds) * 1000 + m_options.snapshotTimeoutMs) {
            // A daemon rollback can replace active objects and increment a
            // profile revision halfway through an inventory read. Retry reads
            // until a coherent restored snapshot is available; issue no writes.
            QTimer::singleShot(m_options.pollIntervalMs, this, [this, generation] { if (generation == m_generation && m_busy) pollOutcome(true); });
            return;
        }
        if (!ok || !sameOwnership(current)) { uncertain(ok ? QStringLiteral("NetworkManager restarted while the operation was pending.") : error); return; }
        if (restoring) {
            if (restoredState(current) && !current.checkpoints.contains(m_checkpoint)) {
                finish(false, true, current, m_failure); return;
            }
            if (m_elapsed.elapsed() > qint64(m_options.rollbackSeconds) * 1000 + m_options.snapshotTimeoutMs) {
                uncertain(QStringLiteral("NetworkManager recovery completed without a verified restoration. Reload the current state.")); return;
            }
            QTimer::singleShot(m_options.pollIntervalMs, this, [this, generation] { if (generation == m_generation && m_busy) pollOutcome(true); });
            return;
        }
        if (!checkpointLive(current)) {
            // The daemon timeout may already have rolled back and removed the
            // checkpoint. No new mutation is dispatched on this path.
            m_failure = QStringLiteral("The recovery checkpoint expired before the operation could be confirmed.");
            m_rollingBack = true; m_confirming = false;
            pollOutcome(true); return;
        }
        if (!expectedProfiles(current)) {
            abandonOwnedCheckpoint(current, QStringLiteral("A saved connection or another adapter changed while applying. Recovery was canceled to preserve the newer configuration.")); return;
        }
        const auto *device = current.device(m_request.devicePath);
        if (!device || !device->managed) { uncertain(QStringLiteral("The adapter was removed or became unmanaged during the operation.")); return; }
        if (device->state == 120 || device->state == 20) {
            rollback(device->reason.isEmpty() ? QStringLiteral("NetworkManager could not activate the requested network.") : device->reason); return;
        }
        bool matches = false;
        if (m_request.action == NetworkRequest::Action::Disconnect) {
            matches = device->state == 30 && (device->activeConnectionPath.isEmpty() || device->activeConnectionPath == QLatin1String("/"));
        } else {
            const QString profilePath = m_request.action == NetworkRequest::Action::ConnectWifi ? m_newProfile : m_request.profilePath;
            matches = device->state == 100 && device->activeProfilePath == profilePath && device->activeConnectionPath == m_activeTarget;
            if (matches && m_request.action == NetworkRequest::Action::ConnectWifi) {
                const auto *profile = current.profile(m_newProfile);
                const auto *ap = m_baseline.accessPoint(m_request.accessPointPath);
                matches = profile && ap && profile->uuid == m_newUuid && profile->unsaved && (profile->flags & 4u)
                    && profile->settings.value(QStringLiteral("802-11-wireless")).value(QStringLiteral("ssid")).toByteArray() == ap->ssid;
            }
        }
        if (matches) {
            if (m_request.action == NetworkRequest::Action::UpdateProfile) {
                NetworkProfile applied, expected;
                applied.settings = device->appliedSettings;
                expected.settings = m_expectedSettings;
                if (!device->appliedVersionId || NetworkTypes::preferences(applied) != NetworkTypes::preferences(expected)) {
                    rollback(QStringLiteral("The saved preferences did not match the device's applied configuration.")); return;
                }
            }
            m_preview = current;
            if (const auto *profile = current.profile(m_request.action == NetworkRequest::Action::ConnectWifi ? m_newProfile : m_request.profilePath))
                m_profileVersion = profile->versionId;
            m_confirming = true;
            // The UI confirmation deadline ends before the daemon deadline, so
            // every offered Keep has a bounded commit/readback budget left.
            const qint64 usableMs = qint64(m_options.rollbackSeconds) * 1000 - m_elapsed.elapsed()
                - m_options.writeTimeoutMs - m_options.readTimeoutMs;
            const int seconds = std::max(0, int(usableMs / 1000));
            if (!seconds) { m_confirming = false; rollback(QStringLiteral("No safe confirmation time remains. Apply again when NetworkManager responds promptly.")); return; }
            emit previewReady(current, seconds);
            return;
        }
        QTimer::singleShot(m_options.pollIntervalMs, this, [this, generation] { if (generation == m_generation && m_busy) pollOutcome(); });
    });
}

void NetworkManagerBackend::keepChange()
{
    if (!m_busy || !m_confirming || m_uncertain || m_rollingBack) return;
    m_confirming = false;
    const quint64 generation = m_generation;
    readSnapshot([this, generation](bool ok, const NetworkSnapshot &current, const QString &error) {
        if (generation != m_generation || !m_busy || m_rollingBack) return;
        if (!ok || !sameOwnership(current) || !checkpointLive(current)) {
            if (ok && sameOwnership(current) && !current.checkpoints.contains(m_checkpoint)) {
                m_failure = QStringLiteral("The recovery deadline expired before Keep."); m_rollingBack = true; pollOutcome(true);
            } else uncertain(ok ? QStringLiteral("Recovery ownership could not be verified before Keep.") : error);
            return;
        }
        const auto *device = current.device(m_request.devicePath);
        const auto *previewDevice = m_preview.device(m_request.devicePath);
        if (!expectedProfiles(current) || !device || !previewDevice
            || device->activeConnectionPath != previewDevice->activeConnectionPath || device->activeProfilePath != previewDevice->activeProfilePath || device->state != previewDevice->state) {
            abandonOwnedCheckpoint(current, QStringLiteral("The network changed after preview. Recovery was canceled to preserve the newer configuration; inspect the current state.")); return;
        }
        if (device->appliedVersionId != previewDevice->appliedVersionId || !NetworkTypes::sameSettings(device->appliedSettings, previewDevice->appliedSettings)) {
            abandonOwnedCheckpoint(current, QStringLiteral("The adapter's applied configuration changed after preview. Recovery was canceled to preserve the newer configuration.")); return;
        }
        if (m_elapsed.elapsed() + m_options.writeTimeoutMs + m_options.readTimeoutMs >= qint64(m_options.rollbackSeconds) * 1000) {
            rollback(QStringLiteral("Too little recovery time remains to save this change. Apply again and confirm sooner.")); return;
        }
        if (m_request.action == NetworkRequest::Action::UpdateProfile || m_request.action == NetworkRequest::Action::ConnectWifi) persist(false);
        else destroyCheckpoint(current);
    });
}

void NetworkManagerBackend::persist(bool inactive)
{
    const quint64 generation = m_generation;
    const QString path = m_request.action == NetworkRequest::Action::ConnectWifi ? m_newProfile : m_request.profilePath;
    const NetworkConnectionData data = inactive ? m_expectedSettings : NetworkConnectionData();
    // Empty settings on Keep preserve the daemon's existing secret material.
    call(m_owner, path, profileInterface, QStringLiteral("Update2"),
         {QVariant::fromValue(data), uint(inactive ? (1 | 0x20 | 0x40) : 1), QVariantMap{{QStringLiteral("version-id"), QVariant::fromValue(qulonglong(m_profileVersion))}}}, true,
         [this, generation, path, inactive](const QDBusMessage &reply) {
        if (generation != m_generation || !m_busy || m_rollingBack) return;
        if (reply.type() == QDBusMessage::ErrorMessage) {
            if (reply.errorName().contains(QLatin1String("NoReply")) || reply.errorName().contains(QLatin1String("Timeout"))) uncertain(operationError(reply));
            else if (inactive) {
                const QString failure = operationError(reply);
                readSnapshot([this, generation, failure](bool ok, const NetworkSnapshot &current, const QString &error) {
                    if (generation != m_generation || !m_busy) return;
                    finish(false, ok && NetworkTypes::sameSnapshot(m_baseline, current), current, ok ? failure : error);
                });
            }
            else rollback(operationError(reply));
            return;
        }
        // Saving legitimately increments VersionId; compare the full settings
        // and storage flags before adopting the newly read revision.
        m_profileVersion = 0;
        readSnapshot([this, generation, path, inactive](bool ok, const NetworkSnapshot &current, const QString &error) {
            if (generation != m_generation || !m_busy || m_rollingBack) return;
            const auto *profile = current.profile(path);
            const auto *preview = m_preview.profile(path);
            const auto &expected = inactive || m_request.action == NetworkRequest::Action::UpdateProfile ? m_expectedSettings
                : preview ? preview->settings : m_expectedSettings;
            if (!ok || !sameOwnership(current) || !profile || !profile->versionId || profile->unsaved || (profile->flags & 4u)
                || !NetworkTypes::sameSettings(profile->settings, expected)) {
                uncertain(ok ? QStringLiteral("The saved profile could not be verified. Settings may have been committed; inspect the current network state.") : error); return;
            }
            m_profileVersion = profile->versionId;
            if (!expectedProfiles(current)) { abandonOwnedCheckpoint(current, QStringLiteral("Another saved profile or adapter changed during commit. Recovery was canceled; inspect the final state.")); return; }
            if (inactive) {
                bool unchangedDevices = true;
                for (const auto &before : m_baseline.devices) {
                    const auto *now = current.device(before.path);
                    if (!now || before.activeConnectionPath != now->activeConnectionPath || before.activeProfilePath != now->activeProfilePath)
                        unchangedDevices = false;
                }
                if (!unchangedDevices) { uncertain(QStringLiteral("The saved profile was updated, but another network change occurred during commit.")); return; }
                finish(true, false, current, {}); return;
            }
            if (!checkpointLive(current)) {
                uncertain(QStringLiteral("The profile was saved but recovery expired before Keep completed. Inspect both the saved and active configuration.")); return;
            }
            const auto *device = current.device(m_request.devicePath);
            if (!device || device->state != 100 || device->activeConnectionPath != m_activeTarget || device->activeProfilePath != path) {
                uncertain(QStringLiteral("The profile was saved, but the active connection changed before confirmation.")); return;
            }
            destroyCheckpoint(current);
        });
    });
}

void NetworkManagerBackend::destroyCheckpoint(const NetworkSnapshot &verified)
{
    if (!sameOwnership(verified) || !checkpointLive(verified)) { uncertain(QStringLiteral("Recovery ownership expired before confirmation.")); return; }
    const quint64 generation = m_generation;
    call(m_owner, root, nm, QStringLiteral("CheckpointDestroy"), {QVariant::fromValue(QDBusObjectPath(m_checkpoint))}, true,
         [this, generation](const QDBusMessage &reply) {
        if (generation != m_generation || !m_busy || m_rollingBack) return;
        if (reply.type() == QDBusMessage::ErrorMessage) { uncertain(operationError(reply)); return; }
        m_deadline.stop();
        readSnapshot([this, generation](bool ok, const NetworkSnapshot &current, const QString &error) {
            if (generation != m_generation || !m_busy) return;
            const auto *device = current.device(m_request.devicePath);
            const auto *preview = m_preview.device(m_request.devicePath);
            if (!ok || !sameOwnership(current) || current.checkpoints.contains(m_checkpoint) || !expectedProfiles(current)
                || !device || !preview || device->state != preview->state || device->activeConnectionPath != preview->activeConnectionPath || device->activeProfilePath != preview->activeProfilePath) {
                uncertain(ok ? QStringLiteral("Keep was acknowledged, but the final network state could not be verified.") : error); return;
            }
            finish(true, false, current, {});
        });
    });
}

void NetworkManagerBackend::revertChange()
{
    if (!m_busy || m_rollingBack || m_uncertain) return;
    rollback(QStringLiteral("The network change was reverted."));
}

void NetworkManagerBackend::rollback(const QString &reason)
{
    if (!m_busy || m_rollingBack || m_uncertain) return;
    if (m_mutationPending) {
        uncertain(QStringLiteral("A network mutation is still awaiting its D-Bus reply. A compensating write was refused; NetworkManager retains timed recovery.")); return;
    }
    m_confirming = false;
    m_rollingBack = true;
    m_failure = reason;
    m_request.password.clear();
    m_deadline.stop();
    const quint64 generation = m_generation;
    readSnapshot([this, generation](bool ok, const NetworkSnapshot &current, const QString &error) {
        if (generation != m_generation || !m_busy || !m_rollingBack) return;
        if (!ok) {
            // The daemon's timer may have started rollback during this read.
            // Wait for its stable result rather than treating the read race as
            // a failed restoration or dispatching a write on stale inventory.
            pollOutcome(true); return;
        }
        if (!sameOwnership(current)) { uncertain(QStringLiteral("NetworkManager recovery ownership changed.")); return; }
        if (m_checkpoint.isEmpty()) { uncertain(QStringLiteral("No owned recovery checkpoint is available.")); return; }
        if (!current.checkpoints.contains(m_checkpoint)) { pollOutcome(true); return; }
        // Only roll back a checkpoint when every baseline profile is still
        // either unchanged or exactly our expected update. Never overwrite
        // another application's subsequent saved-profile changes.
        bool profilesOwned = true;
        for (const auto &before : m_baseline.profiles) {
            const auto *now = current.profile(before.path);
            const bool ourEdit = m_request.action == NetworkRequest::Action::UpdateProfile && before.path == m_request.profilePath;
            const bool unchanged = now && now->versionId == before.versionId && NetworkTypes::sameSettings(now->settings, before.settings);
            const bool ownedEdit = now && ourEdit && m_profileVersion && now->versionId == m_profileVersion
                && NetworkTypes::sameSettings(now->settings, m_expectedSettings);
            if (!now || now->uuid != before.uuid || (!unchanged && !ownedEdit)) profilesOwned = false;
        }
        if (!profilesOwned) { abandonOwnedCheckpoint(current, QStringLiteral("Saved profiles changed externally. Recovery was canceled to preserve the newer profile revision.")); return; }
        if (const auto *previewDevice = m_preview.device(m_request.devicePath)) {
            const auto *device = current.device(m_request.devicePath);
            if (device && device->state == previewDevice->state && device->activeConnectionPath == previewDevice->activeConnectionPath
                && (device->appliedVersionId != previewDevice->appliedVersionId || !NetworkTypes::sameSettings(device->appliedSettings, previewDevice->appliedSettings))) {
                abandonOwnedCheckpoint(current, QStringLiteral("The adapter was reconfigured externally after preview. Recovery was canceled to preserve the newer configuration.")); return;
            }
        }
        call(m_owner, root, nm, QStringLiteral("CheckpointRollback"), {QVariant::fromValue(QDBusObjectPath(m_checkpoint))}, true,
             [this, generation](const QDBusMessage &reply) {
            if (generation != m_generation || !m_busy || !m_rollingBack) return;
            if (reply.type() == QDBusMessage::ErrorMessage) { uncertain(operationError(reply)); return; }
            const auto results = qdbus_cast<QMap<QString, uint>>(reply.arguments().value(0));
            for (auto it = results.begin(); it != results.end(); ++it) if (it.value()) {
                uncertain(QStringLiteral("NetworkManager reported that a checkpoint device could not be restored.")); return;
            }
            pollOutcome(true);
        });
    });
}

void NetworkManagerBackend::abandonOwnedCheckpoint(const NetworkSnapshot &current, const QString &reason)
{
    // A known external edit must not subsequently be overwritten by the same
    // checkpoint's daemon timer. Cancel only our checkpoint, after verifying
    // its owner/presence and that no mutation can still complete later.
    if (m_mutationPending || !sameOwnership(current) || m_checkpoint.isEmpty() || !current.checkpoints.contains(m_checkpoint)) {
        uncertain(QStringLiteral("The external change cannot be safely isolated from pending recovery. Inspect the current configuration.")); return;
    }
    m_deadline.stop();
    m_confirming = false;
    const quint64 generation = m_generation;
    call(m_owner, root, nm, QStringLiteral("CheckpointDestroy"), {QVariant::fromValue(QDBusObjectPath(m_checkpoint))}, true,
         [this, generation, current, reason](const QDBusMessage &reply) {
        if (generation != m_generation || !m_busy) return;
        if (reply.type() == QDBusMessage::ErrorMessage) { uncertain(operationError(reply)); return; }
        readSnapshot([this, generation, current, reason](bool ok, const NetworkSnapshot &after, const QString &error) {
            if (generation != m_generation || !m_busy) return;
            if (!ok || !sameOwnership(after) || after.checkpoints.contains(m_checkpoint) || !NetworkTypes::sameSnapshot(current, after)) {
                uncertain(ok ? QStringLiteral("Recovery cancellation was acknowledged, but the newer configuration could not be verified.") : error); return;
            }
            finish(false, false, after, reason);
        });
    });
}

void NetworkManagerBackend::finish(bool kept, bool restored, const NetworkSnapshot &snapshot, const QString &error)
{
    m_deadline.stop();
    m_busy = m_confirming = m_rollingBack = m_mutationPending = false;
    m_request.password.clear(); m_request = NetworkRequest();
    m_expectedSettings.clear();
    m_checkpoint.clear(); m_newProfile.clear(); m_newUuid.clear(); m_activeTarget.clear();
    ++m_generation;
    emit transactionFinished(kept, restored, snapshot, error);
}

void NetworkManagerBackend::uncertain(const QString &error)
{
    // A D-Bus timeout does not cancel the daemon's method. Do not destroy its
    // checkpoint, dispatch compensating writes, or accept another transaction.
    // The daemon retains its timed recovery even if this process exits.
    m_uncertain = true;
    lockWrites(m_last);
    finish(false, false, m_last, error);
}

void NetworkManagerBackend::ownerChanged(const QString &name, const QString &, const QString &newOwner)
{
    if (name != m_options.service) return;
    if (m_busy && newOwner != m_owner) {
        uncertain(QStringLiteral("NetworkManager restarted during the change. Its final state must be read again."));
    }
    // A replacement daemon cannot complete an old daemon's pending methods.
    // An owner loss alone is insufficient; wait for an actual replacement.
    if (!newOwner.isEmpty() && newOwner != m_owner) m_uncertain = false;
    emit externalChange();
}

void NetworkManagerBackend::daemonChanged()
{
    if (!m_busy) emit externalChange();
}

void NetworkManagerBackend::propertiesChanged(const QString &, const QVariantMap &, const QStringList &)
{
    daemonChanged();
}
