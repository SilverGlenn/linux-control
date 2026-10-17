#include "PowerPolicyBackend.h"
#include "settings/PowerPolicyFields.h"

#include <KConfigGroup>
#include <KCoreConfigSkeleton>
#include <KSharedConfig>
#include <QDBusMessage>
#include <QDBusArgument>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusServiceWatcher>
#include <QDBusVariant>
#include <QDir>
#include <QCryptographicHash>
#include <QDataStream>
#include <algorithm>
#include <QFileInfo>
#include <QFile>
#include <QStandardPaths>
#include <array>
#include <memory>

namespace {
const QString Service = QStringLiteral("org.kde.Solid.PowerManagement");
const QString Path = QStringLiteral("/org/kde/Solid/PowerManagement");
const QString Interface = Service;
const QString Properties = QStringLiteral("org.freedesktop.DBus.Properties");
constexpr int CallTimeout = 1500;
constexpr int ReloadTimeout = 4000;

bool installedSchemaSupported()
{
    // PowerDevil has no public schema-version getter. Only the installed
    // version whose schema was verified is enabled; unknown versions fail closed.
    const QStringList roots = {QStringLiteral("/usr/lib"), QStringLiteral("/usr/lib64"),
        QStringLiteral("/usr/lib/x86_64-linux-gnu"), QStringLiteral("/usr/lib/aarch64-linux-gnu")};
    for (const auto &root : roots)
        if (QFileInfo(root + QStringLiteral("/libpowerdevilcore.so.2")).canonicalFilePath()
                .endsWith(QStringLiteral("/libpowerdevilcore.so.6.6.6")))
            return true;
    return false;
}
bool boolReply(const QDBusMessage &reply, bool *value)
{
    if (reply.type() == QDBusMessage::ErrorMessage || reply.arguments().size() != 1
        || reply.arguments().first().metaType().id() != QMetaType::Bool)
        return false;
    *value = reply.arguments().first().toBool();
    return true;
}
QVariant propertyReply(const QDBusMessage &reply)
{
    if (reply.type() == QDBusMessage::ErrorMessage || reply.arguments().size() != 1)
        return {};
    return qvariant_cast<QDBusVariant>(reply.arguments().first()).variant();
}

// The subset of KDE v6.6.6 PowerDevilProfileSettings.kcfg used here. Items use
// nested KConfigGroup objects, exactly as kconfig_compiler-generated settings do.
// https://invent.kde.org/plasma/powerdevil/-/blob/v6.6.6/PowerDevilProfileSettings.kcfg
class ProfileSchema {
public:
    ProfileSchema(const KSharedConfig::Ptr &config, const QString &id, const PowerPolicyCapabilities &caps)
        : skeleton(config), display(config->group(id).group(QStringLiteral("Display"))),
          suspend(config->group(id).group(QStringLiteral("SuspendAndShutdown"))), keyboard(config->group(id).group(QStringLiteral("Keyboard")))
    {
        values.id = id; const bool low = id == QLatin1String("LowBattery"), ac = id == QLatin1String("AC");
        auto boolean = [&](const KConfigGroup &group, const QString &key, bool &value, bool fallback) { add(new KCoreConfigSkeleton::ItemBool({}, key, value, fallback), group, group.name() + key); };
        auto integer = [&](const KConfigGroup &group, const QString &key, int &value, int fallback) { add(new KCoreConfigSkeleton::ItemInt({}, key, value, fallback), group, group.name() + key); };
        auto number = [&](const QString &key, uint &value, uint fallback) { add(new KCoreConfigSkeleton::ItemUInt({}, key, value, fallback), suspend, key); };
        boolean(display, QStringLiteral("TurnOffDisplayWhenIdle"), values.displayEnabled, true);
        integer(display, QStringLiteral("TurnOffDisplayIdleTimeoutSec"), values.displaySeconds, ac ? 600 : caps.mobile ? (low ? 60 : 120) : (low ? 120 : 300));
        boolean(display, QStringLiteral("DimDisplayWhenIdle"), values.dimEnabled, true);
        integer(display, QStringLiteral("DimDisplayIdleTimeoutSec"), values.dimSeconds, ac ? 300 : caps.mobile ? (low ? 30 : 60) : (low ? 60 : 120));
        boolean(display, QStringLiteral("UseProfileSpecificDisplayBrightness"), values.brightnessEnabled, low);
        integer(display, QStringLiteral("DisplayBrightness"), values.brightnessPercent, low ? 30 : 70);
        boolean(keyboard, QStringLiteral("UseProfileSpecificKeyboardBrightness"), values.keyboardBrightnessEnabled, false);
        integer(keyboard, QStringLiteral("KeyboardBrightness"), values.keyboardBrightnessPercent, 50);
        integer(display, QStringLiteral("TurnOffDisplayIdleTimeoutWhenLockedSec"), values.lockedDisplaySeconds, 60);
        boolean(display, QStringLiteral("LockBeforeTurnOffDisplay"), values.lockBeforeDisplayOff, caps.mobile);
        number(QStringLiteral("AutoSuspendAction"), values.suspendAction, caps.canSuspend && !caps.virtualMachine ? 1u : 0u);
        integer(suspend, QStringLiteral("AutoSuspendIdleTimeoutSec"), values.suspendSeconds, ac ? 900 : caps.mobile ? 300 : (low ? 300 : 600));
        number(QStringLiteral("PowerButtonAction"), values.powerButtonAction, caps.mobile ? 128u : 16u);
        number(QStringLiteral("PowerDownAction"), values.powerDownAction, 16u);
        number(QStringLiteral("LidAction"), values.lidAction, caps.virtualMachine ? 0u : caps.canSuspend ? 1u : 64u);
        boolean(suspend, QStringLiteral("InhibitLidActionWhenExternalMonitorPresent"), values.inhibitLidActionWhenExternalMonitorPresent, true);
        number(QStringLiteral("SleepMode"), values.sleepMode, 1u);
        skeleton.load();
    }
    void add(KConfigSkeletonItem *item, const KConfigGroup &group, const QString &name) { item->setGroup(group); skeleton.addItem(item, name); }
    KCoreConfigSkeleton skeleton;
    KConfigGroup display, suspend, keyboard;
    PowerPolicyProfile values;
};
class GlobalSchema {
public:
    GlobalSchema(const KSharedConfig::Ptr &config, const PowerPolicyCapabilities &caps)
        : skeleton(config), battery(config->group(QStringLiteral("BatteryManagement"))) {
        auto add = [&](KConfigSkeletonItem *item, const QString &name) { item->setGroup(battery); skeleton.addItem(item, name); };
        add(new KCoreConfigSkeleton::ItemInt({}, QStringLiteral("BatteryLowLevel"), low, 10), QStringLiteral("Low"));
        add(new KCoreConfigSkeleton::ItemInt({}, QStringLiteral("BatteryCriticalLevel"), critical, 5), QStringLiteral("Critical"));
        add(new KCoreConfigSkeleton::ItemUInt({}, QStringLiteral("BatteryCriticalAction"), action, caps.hibernateSupported ? 2u : caps.canSuspend ? 1u : 0u), QStringLiteral("Action"));
        skeleton.load();
    }
    KCoreConfigSkeleton skeleton; KConfigGroup battery; int low = 10, critical = 5; uint action = 0;
};
QList<PowerPolicyAction> actionChoices(const PowerPolicyCapabilities &caps, bool buttons, bool lid = false)
{
    QList<PowerPolicyAction> result{{0, QStringLiteral("Do nothing")}};
    if (caps.suspendActionSupported && caps.canSuspend) result << PowerPolicyAction{1, QStringLiteral("Sleep")};
    if (caps.suspendActionSupported && caps.hibernateSupported) result << PowerPolicyAction{2, QStringLiteral("Hibernate")};
    if (caps.suspendActionSupported) {
        result << PowerPolicyAction{8, QStringLiteral("Shut down")};
        if (buttons) { result << PowerPolicyAction{32, QStringLiteral("Lock screen")}; if (!lid) result << PowerPolicyAction{16, QStringLiteral("Show logout screen")}; }
    }
    if (buttons && caps.displaySupported) result << PowerPolicyAction{64, QStringLiteral("Turn off screen")};
    return result;
}
bool fallbackKnown(PowerPolicyKey key, const QString &id, const PowerPolicyCapabilities &caps, bool editor)
{
    const bool mobileKnown = editor ? caps.defaultMobileKnown : caps.mobileKnown;
    switch (key) {
    case PowerPolicyKey::DisplaySeconds: case PowerPolicyKey::DimSeconds:
        return id == QLatin1String("AC") || mobileKnown;
    case PowerPolicyKey::IdleSeconds:
        return id == QLatin1String("AC") || id == QLatin1String("LowBattery") || mobileKnown;
    case PowerPolicyKey::PowerButtonAction: case PowerPolicyKey::LockBeforeDisplayOff: return mobileKnown;
    case PowerPolicyKey::IdleAction: case PowerPolicyKey::LidAction:
        return caps.virtualMachineKnown && (caps.virtualMachine || caps.canSuspendKnown);
    case PowerPolicyKey::BatteryCriticalAction:
        return caps.hibernateKnown && (caps.hibernateSupported || caps.canSuspendKnown);
    default: return true;
    }
}
bool typedEntryKnown(const KConfigGroup &group, const QString &name, const QVariant &type)
{
    if (!group.hasKey(name)) return false;
    if (type.metaType().id() == QMetaType::Bool) return true; // KF's native truth conversion.
    auto raw = QVariant(group.readEntry(name, QByteArray{}));
    return raw.convert(type.metaType());
}
// Preserve native entry syntax in provenance as well as KF's effective value:
// masks, expansion, localization and immutable suffixes can otherwise disappear
// behind hasKey()/readEntry(). Groups and keys here are fixed native schema IDs.
QByteArray entrySyntax(const QString &path, const QString &id, const QString &leaf, const QString &name)
{
    QFile source(path); if (!source.open(QIODevice::ReadOnly)) return {};
    const auto target = (id.isEmpty() ? QString{} : QStringLiteral("[%1]").arg(id)) + QStringLiteral("[%1]").arg(leaf);
    QByteArray evidence; QDataStream stream(&evidence, QIODevice::WriteOnly);
    bool inGroup = false;
    while (!source.atEnd()) {
        const auto line = source.readLine().trimmed();
        if (line.startsWith('[')) {
            const auto header = QString::fromUtf8(line);
            inGroup = header == target || header == target + QLatin1String("[$i]") || header == target + QLatin1String("[$d]");
            if (inGroup && (header.endsWith(QLatin1String("[$i]")) || header.endsWith(QLatin1String("[$d]")))) stream << line;
        } else if (inGroup) {
            const auto equal = line.indexOf('='); const auto left = (equal < 0 ? line : line.left(equal)).trimmed();
            const auto native = name.toUtf8();
            if (left == native || left.startsWith(native + '[')) stream << line;
        }
    }
    return evidence;
}
PowerPolicyKeyState keyState(const KSharedConfig::Ptr &config, KConfig &local, KConfig &global,
                             const QString &id, PowerPolicyKey key, const QVariant &loaded,
                             const QVariant &nativeDefault, bool effectiveFallbackKnown,
                             bool editorFallbackKnown, bool supported, const QString &unavailable,
                             bool includeGlobals)
{
    const auto name = PowerPolicyFields::keyName(key), leaf = PowerPolicyFields::keyGroup(key);
    auto groupFor = [&](KConfig *source) { return id.isEmpty() ? source->group(leaf) : source->group(id).group(leaf); };
    auto group = groupFor(config.data()), localGroup = groupFor(&local), globalGroup = groupFor(&global);
    PowerPolicyKeyState result;
    result.effectivePresent = group.hasKey(name); result.localPresent = localGroup.hasKey(name);
    result.globalPresent = includeGlobals && globalGroup.hasKey(name);
    result.immutable = group.isImmutable() || group.isEntryImmutable(name);
    result.valueKnown = result.effectivePresent ? typedEntryKnown(group, name, loaded) : effectiveFallbackKnown;
    const auto effectiveRaw = group.readEntry(name, QByteArray{});
    config->setReadDefaults(true);
    result.inheritedPresent = group.hasKey(name);
    const auto inheritedRaw = group.readEntry(name, QByteArray{});
    result.defaultKnown = result.inheritedPresent ? typedEntryKnown(group, name, nativeDefault) : editorFallbackKnown;
    config->setReadDefaults(false);
    result.defaultValue = nativeDefault;
    result.writable = unavailable.isEmpty() && supported && !result.immutable && config->accessMode() == KConfig::ReadWrite;
    result.reason = unavailable;
    if (result.reason.isEmpty() && !supported) result.reason = QStringLiteral("The required native action or hardware is unavailable.");
    if (result.reason.isEmpty() && !result.writable) result.reason = QStringLiteral("This key is locked or its native configuration is not writable.");
    if (!result.defaultKnown) result.defaultReason = QStringLiteral("The native editor default inputs could not be verified.");
    // KConfigSkeleton writes only when the editor-loaded value differs from its
    // editor default. An absent key loads the same fallback even if the daemon's
    // independently computed effective default is unknown.
    result.resettable = result.writable && result.defaultKnown && result.effectivePresent && result.valueKnown && loaded != nativeDefault;
    if (result.resettable && result.globalPresent && !result.localPresent && !result.inheritedPresent
        && global.accessMode() != KConfig::ReadWrite) result.resettable = false;
    if (!result.resettable) {
        if (!result.writable) result.resetReason = result.reason;
        else if (!result.defaultKnown) result.resetReason = result.defaultReason;
        else if (!result.effectivePresent) result.resetReason = result.valueKnown ? QStringLiteral("No saved override; already uses the native default.") : QStringLiteral("No saved override; effective default unverified.");
        else if (!result.valueKnown) result.resetReason = QStringLiteral("The saved numeric value is malformed; choose an explicit value to repair it.");
        else if (loaded == nativeDefault) result.resetReason = QStringLiteral("Already equals the native editor default; KDE leaves this override unchanged.");
        else result.resetReason = QStringLiteral("The native global configuration is not writable.");
    }
    auto digest = [](const QByteArray &data) { return QCryptographicHash::hash(data, QCryptographicHash::Sha256); };
    QByteArray inherited; QDataStream inheritedStream(&inherited, QIODevice::WriteOnly);
    inheritedStream << result.inheritedPresent << inheritedRaw << result.immutable;
    QStringList inheritedSources = config->additionalConfigSources();
    if (includeGlobals) {
        const auto user = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
        for (const auto &directory : QStandardPaths::standardLocations(QStandardPaths::GenericConfigLocation)) {
            inheritedSources << directory + QStringLiteral("/system.kdeglobals");
            if (QDir::cleanPath(directory) != QDir::cleanPath(user))
                inheritedSources << directory + QStringLiteral("/powerdevilrc") << directory + QStringLiteral("/kdeglobals");
        }
        if (!qEnvironmentVariableIsSet("KDE_SKIP_KDERC")) inheritedSources << QStringLiteral("/etc/kde5rc");
    }
    inheritedSources.removeDuplicates();
    for (const auto &path : inheritedSources)
        inheritedStream << path << QFileInfo(path).canonicalFilePath() << entrySyntax(path, id, leaf, name);
    result.inheritedRevision = digest(inherited);
    QByteArray globalData; QDataStream globalStream(&globalData, QIODevice::WriteOnly);
    globalStream << (includeGlobals ? QFileInfo(global.name()).absoluteFilePath() : QString{}) << result.globalPresent << (includeGlobals ? globalGroup.readEntry(name, QByteArray{}) : QByteArray{})
                 << (includeGlobals && globalGroup.isEntryImmutable(name))
                 << (includeGlobals ? entrySyntax(global.name(), id, leaf, name) : QByteArray{});
    result.globalRevision = digest(globalData);
    QByteArray data; QDataStream stream(&data, QIODevice::WriteOnly);
    stream << QFileInfo(local.name()).absoluteFilePath() << result.effectivePresent << effectiveRaw << result.localPresent << localGroup.readEntry(name, QByteArray{})
           << localGroup.isEntryImmutable(name) << entrySyntax(local.name(), id, leaf, name) << result.inheritedRevision << result.globalRevision;
    result.revision = digest(data);
    return result;
}

}

PowerPolicyDBus::PowerPolicyDBus(QObject *parent)
    : PowerPolicyDBus(QDBusConnection::sessionBus(), QDBusConnection::systemBus(),
                     installedSchemaSupported(), parent)
{
}
PowerPolicyDBus::PowerPolicyDBus(const QDBusConnection &session, const QDBusConnection &system,
                               bool schemaSupported, QObject *parent)
    : PowerPolicyDaemon(parent), m_session(session), m_system(system), m_schemaSupported(schemaSupported)
{
    // Subscribe before any reload request. The method reply is also required:
    // an unrelated signal alone never constitutes a successful reload.
    m_session.connect(Service, Path, Interface, QStringLiteral("configurationReloaded"),
                      this, SLOT(configurationReloaded()));
    m_session.connect(Service, Path, Interface, QStringLiteral("supportedActionsChanged"),
                      this, SIGNAL(changed()));
    m_session.connect(Service, Path, Interface, QStringLiteral("profileChanged"),
                      this, SLOT(powerSourceChanged(QString)));
    m_session.connect(QStringLiteral("org.freedesktop.portal.Desktop"),
                      QStringLiteral("/org/freedesktop/portal/desktop"),
                      QStringLiteral("org.freedesktop.portal.Settings"), QStringLiteral("SettingChanged"),
                      this, SLOT(portalSettingChanged(QString,QString,QDBusVariant)));
    m_system.connect(QStringLiteral("org.freedesktop.UPower"),
                     QStringLiteral("/org/freedesktop/UPower/devices/DisplayDevice"), Properties,
                     QStringLiteral("PropertiesChanged"), this,
                     SLOT(hardwarePropertiesChanged(QString,QVariantMap,QStringList)));
    m_system.connect(QStringLiteral("org.freedesktop.UPower"), QStringLiteral("/org/freedesktop/UPower"), Properties,
        QStringLiteral("PropertiesChanged"), this, SLOT(hardwarePropertiesChanged(QString,QVariantMap,QStringList)));
    auto *watcher = new QDBusServiceWatcher(Service, m_session,
        QDBusServiceWatcher::WatchForOwnerChange, this);
    watcher->addWatchedService(QStringLiteral("org.freedesktop.PowerManagement"));
    watcher->addWatchedService(QStringLiteral("org.freedesktop.portal.Desktop"));
    connect(watcher, &QDBusServiceWatcher::serviceOwnerChanged, this,
            [this](const QString &, const QString &, const QString &) { ++m_ownerEpoch; m_owner.clear(); if (m_reloading) finishReload(false, false, QStringLiteral("A native power provider changed during reload.")); emit changed(); });
    auto *hardwareWatcher = new QDBusServiceWatcher(QStringLiteral("org.freedesktop.UPower"), m_system,
        QDBusServiceWatcher::WatchForOwnerChange, this);
    hardwareWatcher->addWatchedService(QStringLiteral("org.freedesktop.systemd1"));
    connect(hardwareWatcher, &QDBusServiceWatcher::serviceOwnerChanged, this,
            [this](const QString &, const QString &, const QString &) { ++m_ownerEpoch; m_owner.clear(); if (m_reloading) finishReload(false, false, QStringLiteral("A native power provider changed during reload.")); emit changed(); });
    m_reloadTimer.setSingleShot(true);
    m_reloadTimer.setInterval(ReloadTimeout);
    connect(&m_reloadTimer, &QTimer::timeout, this, [this] {
        finishReload(false, false, QStringLiteral("PowerDevil did not acknowledge the configuration reload before the timeout."));
    });
}
void PowerPolicyDBus::call(const QDBusConnection &bus, const QString &service,
                           const QString &path, const QString &interface,
                           const QString &method, const QVariantList &args, Reply completion)
{
    if (!bus.isConnected()) {
        QTimer::singleShot(0, this, [completion] {
            completion(QDBusMessage::createError(QStringLiteral("org.freedesktop.DBus.Error.Disconnected"),
                QStringLiteral("The D-Bus connection is unavailable.")));
        });
        return;
    }
    auto message = QDBusMessage::createMethodCall(service, path, interface, method);
    message.setArguments(args);
    auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(message, CallTimeout), this);
    auto completed = std::make_shared<bool>(false);
    auto finish = [watcher, completion, completed] {
        if (*completed)
            return;
        *completed = true;
        const auto reply = watcher->reply();
        watcher->deleteLater();
        completion(reply);
    };
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [finish](QDBusPendingCallWatcher *) { finish(); });
    // Disconnected-bus errors can be complete before the signal connection.
    // Deliver them asynchronously, exactly once, just like ordinary replies.
    if (watcher->isFinished())
        QTimer::singleShot(0, watcher, finish);
}
void PowerPolicyDBus::queryCapabilities()
{
    const auto generation = ++m_queryGeneration, epoch = m_ownerEpoch;
    call(m_session, QStringLiteral("org.freedesktop.DBus"), QStringLiteral("/org/freedesktop/DBus"),
         QStringLiteral("org.freedesktop.DBus"), QStringLiteral("GetNameOwner"), {Service},
         [this, generation, epoch](const QDBusMessage &ownerReply) {
        if (generation != m_queryGeneration) return;
        auto caps = std::make_shared<PowerPolicyCapabilities>(); caps->schemaSupported = m_schemaSupported;
        if (ownerReply.type() == QDBusMessage::ErrorMessage || ownerReply.arguments().size() != 1
            || ownerReply.arguments().first().metaType().id() != QMetaType::QString
            || !ownerReply.arguments().first().toString().startsWith(QLatin1Char(':')) || epoch != m_ownerEpoch) {
            caps->reason = QStringLiteral("The PowerDevil native service owner could not be verified.");
            emit capabilitiesFinished(*caps); return;
        }
        caps->owner = ownerReply.arguments().first().toString();
        auto remaining = std::make_shared<int>(16);
        auto daemonEnvironmentKnown = std::make_shared<bool>(false);
        auto daemonEnvironment = std::make_shared<QMap<QByteArray, QByteArray>>();
        const auto resolveMobile = [](const QMap<QByteArray, QByteArray> &environment, bool portalKnown, bool portalValue, bool &known, bool &value) {
            const auto mobile = QByteArray("QT_QUICK_CONTROLS_MOBILE"), tablet = QByteArray("KDE_KIRIGAMI_TABLET_MODE");
            auto enabled = [](const QByteArray &v) { return v == "1" || v == "true"; };
            if (environment.contains(mobile) || environment.contains(tablet)) {
                known = true; value = enabled(environment.value(mobile)) || enabled(environment.value(tablet));
            } else if (environment.contains("QT_NO_XDG_DESKTOP_PORTAL")) { known = true; value = false; }
            else { known = portalKnown; value = portalKnown && portalValue; }
        };
        QMap<QByteArray, QByteArray> clientEnvironment;
        for (const auto *name : {"QT_QUICK_CONTROLS_MOBILE", "KDE_KIRIGAMI_TABLET_MODE", "QT_NO_XDG_DESKTOP_PORTAL"})
            if (qEnvironmentVariableIsSet(name)) clientEnvironment.insert(name, qgetenv(name));
        // Native editor defaults use this client process, while effective
        // daemon defaults use the pinned daemon's process-local Kirigami inputs.
        auto done = [this, caps, remaining, generation, epoch, daemonEnvironmentKnown, daemonEnvironment, clientEnvironment, resolveMobile] {
            if (--*remaining || generation != m_queryGeneration) return;
            const bool portalKnown = caps->mobileKnown, portalValue = caps->mobile;
            resolveMobile(clientEnvironment, portalKnown, portalValue, caps->defaultMobileKnown, caps->defaultMobile);
            if (*daemonEnvironmentKnown) resolveMobile(*daemonEnvironment, portalKnown, portalValue, caps->mobileKnown, caps->mobile);
            else { caps->mobileKnown = false; caps->mobile = false; }
            caps->sleepSupported = caps->suspendActionSupported && caps->canSuspend;
            if (epoch != m_ownerEpoch) { caps->serviceAvailable = false; caps->reason = QStringLiteral("A native power provider changed during capability discovery. Refresh to retry."); }
            else if (!caps->schemaSupported) caps->reason = QStringLiteral("The installed PowerDevil schema is unsupported (requires verified 6.6.6).");
            else if (!caps->serviceAvailable) caps->reason = QStringLiteral("The PowerDevil native interface could not be verified.");
            QByteArray identity; QDataStream stream(&identity, QIODevice::WriteOnly);
            stream << caps->owner << epoch << caps->schemaSupported << caps->serviceAvailable
                   << caps->displaySupported << caps->suspendActionSupported << caps->canSuspendKnown << caps->canSuspend
                   << caps->hibernateKnown << caps->hibernateSupported << caps->hybridSuspendSupported << caps->suspendThenHibernateSupported
                   << caps->dimSupported << caps->brightnessSupported << caps->keyboardBrightnessSupported << caps->buttonSupported
                   << caps->lidKnown << caps->hasLid << caps->batteryKnown << caps->hasBattery
                   << caps->mobileKnown << caps->mobile << caps->defaultMobileKnown << caps->defaultMobile << caps->virtualMachineKnown << caps->virtualMachine;
            caps->revision = QString::fromLatin1(QCryptographicHash::hash(identity, QCryptographicHash::Sha256).toHex());
            if (epoch == m_ownerEpoch) m_owner = caps->owner;
            emit capabilitiesFinished(*caps);
        };
        call(m_session, QStringLiteral("org.freedesktop.DBus"), QStringLiteral("/org/freedesktop/DBus"),
             QStringLiteral("org.freedesktop.DBus"), QStringLiteral("GetConnectionUnixProcessID"), {caps->owner},
             [daemonEnvironmentKnown, daemonEnvironment, done](const QDBusMessage &reply) {
            if (reply.type() != QDBusMessage::ErrorMessage && reply.arguments().size() == 1
                && reply.arguments().first().metaType().id() == QMetaType::UInt && reply.arguments().first().toUInt()) {
                QFile environment(QStringLiteral("/proc/%1/environ").arg(reply.arguments().first().toUInt()));
                if (environment.open(QIODevice::ReadOnly)) {
                    const auto data = environment.read(1024 * 1024 + 1);
                    if (environment.error() == QFileDevice::NoError && data.size() <= 1024 * 1024) {
                        *daemonEnvironmentKnown = true;
                        // Inspect only the three audited names. Never publish
                        // environment values or include them in fingerprints.
                        for (const auto &entry : data.split('\0')) {
                            const auto name = entry.left(entry.indexOf('='));
                            if (name == "QT_QUICK_CONTROLS_MOBILE" || name == "KDE_KIRIGAMI_TABLET_MODE" || name == "QT_NO_XDG_DESKTOP_PORTAL") {
                                daemonEnvironment->insert(name, entry.mid(entry.indexOf('=') + 1));
                            }
                        }
                    }
                }
            }
            done();
        });
        call(m_session, caps->owner, Path, Interface, QStringLiteral("currentProfile"), {}, [caps, done](const auto &reply) {
            caps->serviceAvailable = reply.type() != QDBusMessage::ErrorMessage && reply.arguments().size() == 1
                && reply.arguments().first().metaType().id() == QMetaType::QString
                && QStringList{QStringLiteral("AC"), QStringLiteral("Battery"), QStringLiteral("LowBattery")}.contains(reply.arguments().first().toString()); done();
        });
        auto action = [this, caps, done](const QString &id, bool PowerPolicyCapabilities::*field) {
            call(m_session, caps->owner, Path, Interface, QStringLiteral("isActionSupported"), {id}, [caps, done, field](const auto &reply) { boolReply(reply, &(caps.get()->*field)); done(); });
        };
        action(QStringLiteral("DPMSControl"), &PowerPolicyCapabilities::displaySupported);
        action(QStringLiteral("SuspendSession"), &PowerPolicyCapabilities::suspendActionSupported);
        action(QStringLiteral("DimDisplay"), &PowerPolicyCapabilities::dimSupported);
        action(QStringLiteral("ScreenBrightnessControl"), &PowerPolicyCapabilities::brightnessSupported);
        action(QStringLiteral("KeyboardBrightnessControl"), &PowerPolicyCapabilities::keyboardBrightnessSupported);
        action(QStringLiteral("HandleButtonEvents"), &PowerPolicyCapabilities::buttonSupported);
        auto power = [this, caps, done](const QString &method, bool PowerPolicyCapabilities::*field, bool PowerPolicyCapabilities::*known = nullptr) {
            // The audited freedesktop facade belongs to the same pinned native daemon.
            call(m_session, caps->owner, QStringLiteral("/org/freedesktop/PowerManagement"), QStringLiteral("org.freedesktop.PowerManagement"), method, {},
                 [caps, done, field, known](const auto &reply) { const bool valid = boolReply(reply, &(caps.get()->*field)); if (known) caps.get()->*known = valid; done(); });
        };
        power(QStringLiteral("CanSuspend"), &PowerPolicyCapabilities::canSuspend, &PowerPolicyCapabilities::canSuspendKnown);
        power(QStringLiteral("CanHibernate"), &PowerPolicyCapabilities::hibernateSupported, &PowerPolicyCapabilities::hibernateKnown);
        power(QStringLiteral("CanHybridSuspend"), &PowerPolicyCapabilities::hybridSuspendSupported);
        power(QStringLiteral("CanSuspendThenHibernate"), &PowerPolicyCapabilities::suspendThenHibernateSupported);
        call(m_session, caps->owner, Path, Interface, QStringLiteral("isLidPresent"), {}, [caps, done](const auto &reply) { caps->lidKnown = boolReply(reply, &caps->hasLid); done(); });
        call(m_system, QStringLiteral("org.freedesktop.UPower"), QStringLiteral("/org/freedesktop/UPower/devices/DisplayDevice"), Properties,
             QStringLiteral("GetAll"), {QStringLiteral("org.freedesktop.UPower.Device")}, [caps, done](const auto &reply) {
            if (reply.type() != QDBusMessage::ErrorMessage && reply.arguments().size() == 1) {
                const auto map = qdbus_cast<QVariantMap>(reply.arguments().first());
                caps->batteryKnown = map.value(QStringLiteral("Type")).metaType().id() == QMetaType::UInt
                    && map.value(QStringLiteral("PowerSupply")).metaType().id() == QMetaType::Bool && map.value(QStringLiteral("IsPresent")).metaType().id() == QMetaType::Bool;
                const auto type = map.value(QStringLiteral("Type")).toUInt();
                caps->hasBattery = caps->batteryKnown && (type == 2 || type == 3) && map.value(QStringLiteral("PowerSupply")).toBool() && map.value(QStringLiteral("IsPresent")).toBool();
            } done();
        });
        call(m_session, QStringLiteral("org.freedesktop.portal.Desktop"), QStringLiteral("/org/freedesktop/portal/desktop"), QStringLiteral("org.freedesktop.portal.Settings"),
             QStringLiteral("ReadAll"), {QStringList{QStringLiteral("org.kde.TabletMode")}}, [caps, done](const auto &reply) {
            if (reply.type() != QDBusMessage::ErrorMessage && reply.arguments().size() == 1) {
                const auto groups = qdbus_cast<QMap<QString, QVariantMap>>(reply.arguments().first());
                const auto value = groups.value(QStringLiteral("org.kde.TabletMode")).value(QStringLiteral("enabled"));
                caps->mobileKnown = value.metaType().id() == QMetaType::Bool; caps->mobile = caps->mobileKnown && value.toBool();
            } done();
        });
        call(m_system, QStringLiteral("org.freedesktop.systemd1"), QStringLiteral("/org/freedesktop/systemd1"), Properties,
             QStringLiteral("Get"), {QStringLiteral("org.freedesktop.systemd1.Manager"), QStringLiteral("Virtualization")}, [caps, done](const auto &reply) {
            const auto value = propertyReply(reply); caps->virtualMachineKnown = value.metaType().id() == QMetaType::QString;
            caps->virtualMachine = caps->virtualMachineKnown && !value.toString().isEmpty(); done();
        });
    });
}
void PowerPolicyDBus::reload()
{
    if (m_reloading)
        return;
    if (m_owner.isEmpty()) { emit reloadFinished(false, false, QStringLiteral("The native PowerDevil owner is unavailable.")); return; }
    m_reloadOwner = m_owner;
    m_reloading = true;
    m_reloadSignal = false;
    m_reloadReply = false;
    const auto generation = ++m_generation;
    m_reloadTimer.start();
    call(m_session, m_reloadOwner, Path, Interface, QStringLiteral("refreshStatus"), {},
        [this, generation](const QDBusMessage &reply) {
            if (!m_reloading || generation != m_generation)
                return;
            if (reply.type() == QDBusMessage::ErrorMessage || !reply.arguments().isEmpty()) {
                finishReload(false, false, QStringLiteral("Power policy was saved, but PowerDevil reload failed: %1").arg(reply.errorMessage()));
                return;
            }
            m_reloadReply = true;
            if (m_reloadSignal)
                finishReload(true, false, {});
        });
}
void PowerPolicyDBus::configurationReloaded()
{
    if (m_reloading && calledFromDBus() && message().service() == m_reloadOwner) {
        m_reloadSignal = true;
        if (m_reloadReply)
            finishReload(true, false, {});
    }
    emit changed();
}
void PowerPolicyDBus::portalSettingChanged(const QString &group, const QString &key, const QDBusVariant &)
{
    if (group == QLatin1String("org.kde.TabletMode") && key == QLatin1String("enabled"))
        emit changed();
}
void PowerPolicyDBus::hardwarePropertiesChanged(const QString &interface, const QVariantMap &, const QStringList &)
{
    if (interface == QLatin1String("org.freedesktop.UPower.Device") || interface == QLatin1String("org.freedesktop.UPower"))
        emit changed();
}
void PowerPolicyDBus::powerSourceChanged(const QString &)
{
    emit changed();
}
void PowerPolicyDBus::cancel()
{
    if (m_reloading)
        finishReload(false, true, QStringLiteral("Waiting for the PowerDevil reload was cancelled; the request may still complete."));
}
void PowerPolicyDBus::finishReload(bool acknowledged, bool cancelled, const QString &error)
{
    if (!m_reloading)
        return;
    m_reloading = false;
    m_reloadTimer.stop();
    emit reloadFinished(acknowledged, cancelled, error);
}

KdePowerPolicyBackend::KdePowerPolicyBackend(QObject *parent)
    : KdePowerPolicyBackend(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
        + QStringLiteral("/powerdevilrc"), new PowerPolicyDBus, parent)
{
    m_cascade = true;
    watchConfig();
}
KdePowerPolicyBackend::KdePowerPolicyBackend(const QString &file, PowerPolicyDaemon *daemon, QObject *parent)
    : PowerPolicyBackend(parent), m_configFile(file), m_daemon(daemon)
{
    Q_ASSERT(daemon);
    daemon->setParent(this);
    connect(daemon, &PowerPolicyDaemon::capabilitiesFinished, this,
            [this](const PowerPolicyCapabilities &caps) {
        if (!m_reading)
            return;
        m_reading = false;
        m_capabilities = caps;
        if (m_writePreflight) { m_writePreflight = false; savePolicy(m_expected, m_requested); return; }
        PowerPolicySnapshot snapshot;
        QString error;
        const bool ok = readFile(&snapshot, &error);
        emit readFinished(ok, snapshot, error);
    });
    connect(daemon, &PowerPolicyDaemon::reloadFinished, this,
            [this](bool acknowledged, bool cancelled, const QString &error) {
        if (!m_writing)
            return;
        m_writing = false;
        emit writeFinished(m_saved, acknowledged, cancelled, error);
    });
    connect(daemon, &PowerPolicyDaemon::changed, this, &PowerPolicyBackend::externalChange);
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(100);
    connect(&m_debounce, &QTimer::timeout, this, &PowerPolicyBackend::externalChange);
    auto changed = [this](const QString &) { watchConfig(); m_debounce.start(); };
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, changed);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, changed);
    watchConfig();
}
KdePowerPolicyBackend::KdePowerPolicyBackend(const QString &file, PowerPolicyDaemon *daemon,
                                             const ConfigContext &context, QObject *parent)
    : KdePowerPolicyBackend(file, daemon, parent)
{
    m_cascade = context.fullCascade;
    m_defaultSources = context.defaultSources;
    watchConfig();
}
void KdePowerPolicyBackend::watchConfig()
{
    QStringList files{m_configFile};
    files << m_defaultSources;
    if (m_cascade) {
        for (const auto &dir : QStandardPaths::standardLocations(QStandardPaths::GenericConfigLocation))
            files << dir + QStringLiteral("/powerdevilrc") << dir + QStringLiteral("/kdeglobals") << dir + QStringLiteral("/system.kdeglobals");
        if (!qEnvironmentVariableIsSet("KDE_SKIP_KDERC")) files << QStringLiteral("/etc/kde5rc");
    }
    for (const auto &file : files) {
        QString dir = QFileInfo(file).absolutePath();
        // Watch an existing ancestor too, so creating a missing config directory
        // or atomically replacing the file can restore the file subscription.
        while (!QDir(dir).exists() && dir != QStringLiteral("/"))
            dir = QFileInfo(dir).absolutePath();
        if (QDir(dir).exists() && !m_watcher.directories().contains(dir))
            m_watcher.addPath(dir);
        if (QFileInfo::exists(file) && !m_watcher.files().contains(file))
            m_watcher.addPath(file);
    }
}
void KdePowerPolicyBackend::readCurrent()
{
    if (m_reading || m_writing)
        return;
    m_reading = true;
    m_daemon->queryCapabilities();
}
bool KdePowerPolicyBackend::readFile(PowerPolicySnapshot *snapshot, QString *error)
{
    if (m_cascade && QDir::cleanPath(QFileInfo(m_configFile).absoluteFilePath())
        != QDir::cleanPath(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/powerdevilrc"))) {
        *error = QStringLiteral("Full native cascading requires the active XDG powerdevilrc path."); return false;
    }
    if (QFileInfo::exists(m_configFile) && !QFileInfo(m_configFile).isReadable()) {
        *error = QStringLiteral("The PowerDevil configuration file is not readable.");
        return false;
    }
    QStringList provenanceFiles{m_configFile}; provenanceFiles << m_defaultSources;
    if (m_cascade)
        for (const auto &directory : QStandardPaths::standardLocations(QStandardPaths::GenericConfigLocation))
            provenanceFiles << directory + QStringLiteral("/powerdevilrc") << directory + QStringLiteral("/kdeglobals") << directory + QStringLiteral("/system.kdeglobals");
    if (m_cascade && !qEnvironmentVariableIsSet("KDE_SKIP_KDERC")) provenanceFiles << QStringLiteral("/etc/kde5rc");
    for (const auto &path : provenanceFiles) {
        if (!QFileInfo::exists(path)) continue;
        QFile source(path);
        if (!source.open(QIODevice::ReadOnly)) {
            *error = QStringLiteral("A native power configuration source is unreadable; its effective values and policy cannot be verified.");
            return false;
        }
    }
    auto config = KSharedConfig::openConfig(m_cascade ? QStringLiteral("powerdevilrc") : m_configFile,
                                           m_cascade ? KConfig::FullConfig : KConfig::SimpleConfig);
    if (!m_defaultSources.isEmpty()) config->addConfigSources(m_defaultSources);
    config->reparseConfiguration();
    KConfig local(m_configFile, KConfig::SimpleConfig);
    KConfig global(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/kdeglobals"), KConfig::SimpleConfig);
    auto editorCaps = m_capabilities; editorCaps.mobile = m_capabilities.defaultMobile;
    snapshot->serviceAvailable = m_capabilities.serviceAvailable && m_capabilities.schemaSupported;
    snapshot->unavailableReason = m_capabilities.reason;
    if (!snapshot->serviceAvailable && snapshot->unavailableReason.isEmpty())
        snapshot->unavailableReason = QStringLiteral("KDE PowerDevil or its verified configuration schema is unavailable.");
    snapshot->owner = m_capabilities.owner; snapshot->capabilityRevision = m_capabilities.revision;
    for (const QString &id : {QStringLiteral("AC"), QStringLiteral("Battery"), QStringLiteral("LowBattery")}) {
        ProfileSchema schema(config, id, m_capabilities); auto p = schema.values;
        const QString hardware = id != QLatin1String("AC") && !m_capabilities.hasBattery
            ? m_capabilities.batteryKnown ? QStringLiteral("No system power-supply battery is present.") : QStringLiteral("System battery hardware could not be verified.") : QString{};
        const QString common = !snapshot->serviceAvailable ? snapshot->unavailableReason : hardware;
        auto policy = [&](const KConfigGroup &group, const QStringList &keys, bool supported, const QString &unsupported, QString &reason, bool &editable, const QString &defaultReason = QString{}) {
            reason = common;
            if (reason.isEmpty() && !supported) reason = unsupported;
            if (reason.isEmpty()) reason = defaultReason;
            if (reason.isEmpty() && (config->accessMode() != KConfig::ReadWrite || group.isImmutable()
                || std::any_of(keys.cbegin(), keys.cend(), [&](const auto &key) { return group.isEntryImmutable(key); }))) reason = QStringLiteral("This policy is locked or the native configuration is not writable.");
            editable = reason.isEmpty();
        };
        const auto unsupported = QStringLiteral("PowerDevil does not advertise the required native action or hardware.");
        policy(schema.display, {"TurnOffDisplayWhenIdle", "TurnOffDisplayIdleTimeoutSec"}, m_capabilities.displaySupported, unsupported, p.displayReason, p.displayEditable);
        policy(schema.suspend, {"AutoSuspendAction", "AutoSuspendIdleTimeoutSec"}, m_capabilities.suspendActionSupported, unsupported, p.sleepReason, p.sleepEditable);
        policy(schema.display, {"DimDisplayWhenIdle", "DimDisplayIdleTimeoutSec"}, m_capabilities.dimSupported, unsupported, p.dimReason, p.dimEditable);
        policy(schema.display, {"UseProfileSpecificDisplayBrightness", "DisplayBrightness"}, m_capabilities.brightnessSupported, unsupported, p.brightnessReason, p.brightnessEditable);
        policy(schema.keyboard, {"UseProfileSpecificKeyboardBrightness", "KeyboardBrightness"}, m_capabilities.keyboardBrightnessSupported, unsupported, p.keyboardBrightnessReason, p.keyboardBrightnessEditable);
        policy(schema.display, {"TurnOffDisplayIdleTimeoutWhenLockedSec"}, m_capabilities.displaySupported, unsupported, p.lockedDisplayReason, p.lockedDisplayEditable);
        policy(schema.display, {"LockBeforeTurnOffDisplay"}, m_capabilities.displaySupported, unsupported, p.lockBeforeDisplayOffReason, p.lockBeforeDisplayOffEditable);
        policy(schema.suspend, {"PowerButtonAction"}, m_capabilities.buttonSupported, unsupported, p.powerButtonReason, p.powerButtonEditable);
        policy(schema.suspend, {"PowerDownAction"}, m_capabilities.buttonSupported, unsupported, p.powerDownReason, p.powerDownEditable);
        policy(schema.suspend, {"LidAction"}, m_capabilities.buttonSupported && m_capabilities.lidKnown && m_capabilities.hasLid, unsupported, p.lidReason, p.lidEditable);
        policy(schema.suspend, {"InhibitLidActionWhenExternalMonitorPresent"}, m_capabilities.buttonSupported && m_capabilities.lidKnown && m_capabilities.hasLid, unsupported, p.lidExternalMonitorReason, p.lidExternalMonitorEditable);
        p.idleActions = actionChoices(m_capabilities, false); p.powerButtonActions = actionChoices(m_capabilities, true); p.lidActions = actionChoices(m_capabilities, true, true);
        if (m_capabilities.suspendActionSupported && m_capabilities.canSuspend) p.sleepModes << PowerPolicyAction{1, QStringLiteral("Standby (memory)")};
        if (m_capabilities.suspendActionSupported && m_capabilities.hybridSuspendSupported) p.sleepModes << PowerPolicyAction{2, QStringLiteral("Hybrid sleep")};
        if (m_capabilities.suspendActionSupported && m_capabilities.suspendThenHibernateSupported) p.sleepModes << PowerPolicyAction{3, QStringLiteral("Standby, then hibernate")};
        policy(schema.suspend, {QStringLiteral("SleepMode")}, !p.sleepModes.isEmpty(), unsupported, p.sleepModeReason, p.sleepModeEditable);
        ProfileSchema editor(config, id, editorCaps); editor.skeleton.setDefaults();
        for (const auto key : PowerPolicyFields::profileKeys()) {
            bool supported = false;
            switch (key) {
            case PowerPolicyKey::DisplayEnabled: case PowerPolicyKey::DisplaySeconds: case PowerPolicyKey::LockedDisplaySeconds: case PowerPolicyKey::LockBeforeDisplayOff: supported = m_capabilities.displaySupported; break;
            case PowerPolicyKey::IdleAction: case PowerPolicyKey::IdleSeconds: supported = m_capabilities.suspendActionSupported; break;
            case PowerPolicyKey::DimEnabled: case PowerPolicyKey::DimSeconds: supported = m_capabilities.dimSupported; break;
            case PowerPolicyKey::BrightnessEnabled: case PowerPolicyKey::BrightnessPercent: supported = m_capabilities.brightnessSupported; break;
            case PowerPolicyKey::KeyboardBrightnessEnabled: case PowerPolicyKey::KeyboardBrightnessPercent: supported = m_capabilities.keyboardBrightnessSupported; break;
            case PowerPolicyKey::PowerButtonAction: case PowerPolicyKey::PowerDownAction: supported = m_capabilities.buttonSupported; break;
            case PowerPolicyKey::LidAction: case PowerPolicyKey::LidExternalMonitorInhibition: supported = m_capabilities.buttonSupported && m_capabilities.lidKnown && m_capabilities.hasLid; break;
            case PowerPolicyKey::SleepMode: supported = !p.sleepModes.isEmpty(); break;
            default: break;
            }
            p.keys.insert(key, keyState(config, local, global, id, key, PowerPolicyFields::value(p, key),
                PowerPolicyFields::value(editor.values, key), fallbackKnown(key, id, m_capabilities, false),
                fallbackKnown(key, id, m_capabilities, true), supported, common, m_cascade));
        }
        snapshot->profiles << p;
    }
    GlobalSchema globals(config, m_capabilities);
    snapshot->batteryLowLevel = globals.low; snapshot->batteryCriticalLevel = globals.critical; snapshot->batteryCriticalAction = globals.action;
    snapshot->batteryCriticalActions = actionChoices(m_capabilities, false);
    QString reason = !snapshot->serviceAvailable ? snapshot->unavailableReason : !m_capabilities.hasBattery ? QStringLiteral("A system power-supply battery could not be verified.") : QString{};
    if (reason.isEmpty() && (config->accessMode() != KConfig::ReadWrite || globals.battery.isImmutable())) reason = QStringLiteral("Battery management policy is not writable.");
    snapshot->batteryLevelsReason = reason;
    if (reason.isEmpty() && (globals.battery.isEntryImmutable("BatteryLowLevel") || globals.battery.isEntryImmutable("BatteryCriticalLevel"))) snapshot->batteryLevelsReason = QStringLiteral("Battery levels are locked by policy.");
    snapshot->batteryLevelsEditable = snapshot->batteryLevelsReason.isEmpty();
    snapshot->batteryCriticalActionReason = reason;
    if (reason.isEmpty() && (!m_capabilities.suspendActionSupported || globals.battery.isEntryImmutable("BatteryCriticalAction"))) snapshot->batteryCriticalActionReason = QStringLiteral("The native critical battery action is unavailable or locked.");
    snapshot->batteryCriticalActionEditable = snapshot->batteryCriticalActionReason.isEmpty();
    GlobalSchema editorGlobals(config, editorCaps); editorGlobals.skeleton.setDefaults();
    PowerPolicySnapshot editorValues; editorValues.batteryLowLevel = editorGlobals.low;
    editorValues.batteryCriticalLevel = editorGlobals.critical; editorValues.batteryCriticalAction = editorGlobals.action;
    const auto globalUnavailable = !snapshot->serviceAvailable ? snapshot->unavailableReason
        : !m_capabilities.hasBattery ? QStringLiteral("A system power-supply battery could not be verified.") : QString{};
    for (const auto key : PowerPolicyFields::globalKeys())
        snapshot->keys.insert(key, keyState(config, local, global, {}, key, PowerPolicyFields::value(*snapshot, key),
            PowerPolicyFields::value(editorValues, key), fallbackKnown(key, {}, m_capabilities, false),
            fallbackKnown(key, {}, m_capabilities, true), key != PowerPolicyKey::BatteryCriticalAction || m_capabilities.suspendActionSupported,
            globalUnavailable, m_cascade));
    return true;
}
void KdePowerPolicyBackend::writePolicy(const PowerPolicySnapshot &expected, const PowerPolicySnapshot &requested)
{
    if (m_reading || m_writing) return;
    m_expected = expected; m_requested = requested; m_reading = m_writePreflight = true;
    m_daemon->queryCapabilities();
}
void KdePowerPolicyBackend::savePolicy(const PowerPolicySnapshot &expected, const PowerPolicySnapshot &requested)
{
    auto fail = [&](const QString &error) { emit writeFinished(false, false, false, error); };
    PowerPolicySnapshot current; QString error;
    if (!readFile(&current, &error) || current != expected || !current.serviceAvailable || requested.profiles.size() != current.profiles.size()) {
        fail(error.isEmpty() ? QStringLiteral("Native power settings, policy or provider capabilities changed before saving. Revert to reload.") : error); return;
    }
    if (!PowerPolicyFields::requestValid(expected, requested, error) || !PowerPolicyFields::hasIntents(requested)) {
        fail(error.isEmpty() ? QStringLiteral("No explicit native power policy operation was requested.") : error); return;
    }
    auto config = KSharedConfig::openConfig(m_cascade ? QStringLiteral("powerdevilrc") : m_configFile,
                                           m_cascade ? KConfig::FullConfig : KConfig::SimpleConfig);
    if (!m_defaultSources.isEmpty()) config->addConfigSources(m_defaultSources);
    config->reparseConfiguration();
    // All operations are validated before the first KConfig mutation. Explicit
    // Set bypasses skeleton's equality/default elision, so choosing a value
    // whose effective default is unknown still establishes a local override.
    auto write = [&](const QString &id, PowerPolicyKey key, PowerPolicyIntent intent, const QVariant &value,
                     const PowerPolicyKeyState &state) {
        const auto name = PowerPolicyFields::keyName(key), leaf = PowerPolicyFields::keyGroup(key);
        auto group = id.isEmpty() ? config->group(leaf) : config->group(id).group(leaf);
        if (intent == PowerPolicyIntent::RestoreDefault && !state.inheritedPresent) {
            // Exact KConfigSkeletonGenericItem native-default branch. This may
            // remove a user-kdeglobals value or create a local deletion mask
            // when a local value shadows globals; deleteEntry is not equivalent.
            group.revertToDefault(name);
        } else {
            switch (value.metaType().id()) {
            case QMetaType::Bool: group.writeEntry(name, value.toBool()); break;
            case QMetaType::UInt: group.writeEntry(name, value.toUInt()); break;
            case QMetaType::Int: group.writeEntry(name, value.toInt()); break;
            default: Q_UNREACHABLE();
            }
        }
    };
    for (int i = 0; i < requested.profiles.size(); ++i) {
        const auto &profile = requested.profiles[i];
        for (auto it = profile.intents.cbegin(); it != profile.intents.cend(); ++it)
            write(profile.id, it.key(), it.value(), PowerPolicyFields::value(profile, it.key()), expected.profiles[i].keys.value(it.key()));
    }
    for (auto it = requested.intents.cbegin(); it != requested.intents.cend(); ++it)
        write({}, it.key(), it.value(), PowerPolicyFields::value(requested, it.key()), expected.keys.value(it.key()));
    if (!config->sync()) {
        // KF objects normally retry dirty entries from destructors/reparse.
        // A failed explicit transaction must never cause a later silent write.
        config->markAsClean();
        emit writeFinished(true, false, false, QStringLiteral("Native power policy save failed; some fields or global defaults may already be saved. Revert to inspect.")); return;
    }
    PowerPolicySnapshot saved;
    if (!readFile(&saved, &error) || !PowerPolicyFields::matchesRequest(expected, requested, saved, error)) {
        emit writeFinished(true, false, false, error.isEmpty() ? QStringLiteral("Native power policy readback did not verify the requested operations; reload was not requested.") : error); return;
    }
    watchConfig(); m_saved = m_writing = true; m_daemon->reload();
}
void KdePowerPolicyBackend::cancel()
{
    if (m_writePreflight) {
        m_writePreflight = m_reading = false;
        emit writeFinished(false, false, true, QStringLiteral("Power policy save was cancelled before writing."));
        return;
    }
    if (m_writing)
        m_daemon->cancel();
}
