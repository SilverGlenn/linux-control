#include "PowerPolicyBackend.h"
#include "settings/PowerPolicyFields.h"
#include "operations/OperationManager.h"
#include "operations/SettingsFileReservations.h"
#include <QCoreApplication>
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrentRun>
#include <QPointer>
#include <QSet>

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
#include <atomic>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

namespace {
const QString Service = QStringLiteral("org.kde.Solid.PowerManagement");
const QString Path = QStringLiteral("/org/kde/Solid/PowerManagement");
const QString Interface = Service;
const QString Properties = QStringLiteral("org.freedesktop.DBus.Properties");
constexpr int CallTimeout = 1500;
constexpr int ReloadTimeout = 4000;
constexpr qint64 MaximumConfigSize=2*1024*1024;

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
    const int fd=::open(QFile::encodeName(path).constData(),O_RDONLY|O_NONBLOCK|O_CLOEXEC);if(fd<0)return {};
    QFile source;if(!source.open(fd,QIODevice::ReadOnly,QFileDevice::AutoCloseHandle)){::close(fd);return {};}
    struct stat metadata{};if(::fstat(fd,&metadata)!=0||!S_ISREG(metadata.st_mode)||metadata.st_size>MaximumConfigSize)return {};
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
PowerPolicyKeyState keyState(const KSharedConfig::Ptr &config, KConfig &local, KConfig *global,
                             const QString &id, PowerPolicyKey key, const QVariant &loaded,
                             const QVariant &nativeDefault, bool effectiveFallbackKnown,
                             bool editorFallbackKnown, bool supported, const QString &unavailable,
                             bool includeGlobals)
{
    const auto name = PowerPolicyFields::keyName(key), leaf = PowerPolicyFields::keyGroup(key);
    auto groupFor = [&](KConfig *source) { return id.isEmpty() ? source->group(leaf) : source->group(id).group(leaf); };
    auto group = groupFor(config.data()), localGroup = groupFor(&local), globalGroup = global?groupFor(global):KConfigGroup{};
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
        && global->accessMode() != KConfig::ReadWrite) result.resettable = false;
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
    globalStream << (includeGlobals ? QFileInfo(global->name()).absoluteFilePath() : QString{}) << result.globalPresent << (includeGlobals ? globalGroup.readEntry(name, QByteArray{}) : QByteArray{})
                 << (includeGlobals && globalGroup.isEntryImmutable(name))
                 << (includeGlobals ? entrySyntax(global->name(), id, leaf, name) : QByteArray{});
    result.globalRevision = digest(globalData);
    QByteArray data; QDataStream stream(&data, QIODevice::WriteOnly);
    stream << QFileInfo(local.name()).absoluteFilePath() << result.effectivePresent << effectiveRaw << result.localPresent << localGroup.readEntry(name, QByteArray{})
           << localGroup.isEntryImmutable(name) << entrySyntax(local.name(), id, leaf, name) << result.inheritedRevision << result.globalRevision;
    result.revision = digest(data);
    return result;
}

}

void PowerPolicyDaemon::reloadGuarded(const PowerPolicyCapabilities &,Guard guard)
{
    if(!guard){emit reloadFinished(false,false,QStringLiteral("The final saved-file guard is unavailable."));return;}
    QPointer<PowerPolicyDaemon> safe(this);auto once=std::make_shared<bool>(false);
    guard([safe,once](bool valid,const QString &error){if(*once)return;*once=true;if(!safe)return;if(!valid){emit safe->reloadFinished(false,false,error.isEmpty()?QStringLiteral("Saved power preferences changed before reload."):error);return;}safe->reload();});
}

PowerPolicyDBus::PowerPolicyDBus(QObject *parent)
    : PowerPolicyDBus(QDBusConnection::sessionBus(), QDBusConnection::systemBus(),
                     false, parent)
{
    m_schemaAuto=true;
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
                      this, SLOT(capabilitiesChanged()));
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
    connect(watcher,&QDBusServiceWatcher::serviceOwnerChanged,this,[this]{invalidateProvider(QStringLiteral("A native power provider changed during reload."));});
    auto *hardwareWatcher=new QDBusServiceWatcher(QStringLiteral("org.freedesktop.UPower"),m_system,QDBusServiceWatcher::WatchForOwnerChange,this);
    hardwareWatcher->addWatchedService(QStringLiteral("org.freedesktop.systemd1"));
    connect(hardwareWatcher,&QDBusServiceWatcher::serviceOwnerChanged,this,[this]{invalidateProvider(QStringLiteral("A native power provider changed during reload."));});
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
    const auto generation=++m_queryGeneration,epoch=m_ownerEpoch;
    if(!m_schemaAuto){queryNativeCapabilities(generation,epoch,m_schemaSupported);return;}
    auto *watcher=new QFutureWatcher<bool>(this);
    connect(watcher,&QFutureWatcher<bool>::finished,this,[this,watcher,generation,epoch]{const bool supported=watcher->result();watcher->deleteLater();if(generation==m_queryGeneration)queryNativeCapabilities(generation,epoch,supported);});
    watcher->setFuture(QtConcurrent::run([]{return installedSchemaSupported();}));
}
void PowerPolicyDBus::queryNativeCapabilities(quint64 generation,quint64 epoch,bool schemaSupported)
{
    call(m_session, QStringLiteral("org.freedesktop.DBus"), QStringLiteral("/org/freedesktop/DBus"),
         QStringLiteral("org.freedesktop.DBus"), QStringLiteral("GetNameOwner"), {Service},
         [this, generation, epoch, schemaSupported](const QDBusMessage &ownerReply) {
        if (generation != m_queryGeneration) return;
        auto caps = std::make_shared<PowerPolicyCapabilities>(); caps->schemaSupported = schemaSupported;
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
            if (epoch != m_ownerEpoch) { caps->serviceAvailable = false; caps->reason = QStringLiteral("A native power provider changed during capability discovery. Revert to inspect again."); }
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
            if (epoch == m_ownerEpoch) {m_owner=caps->owner;m_capabilityRevision=caps->revision;}
            emit capabilitiesFinished(*caps);
        };
        call(m_session, QStringLiteral("org.freedesktop.DBus"), QStringLiteral("/org/freedesktop/DBus"),
             QStringLiteral("org.freedesktop.DBus"), QStringLiteral("GetConnectionUnixProcessID"), {caps->owner},
             [this,daemonEnvironmentKnown,daemonEnvironment,done](const QDBusMessage &reply){
            if(reply.type()==QDBusMessage::ErrorMessage||reply.arguments().size()!=1||reply.arguments().first().metaType().id()!=QMetaType::UInt||!reply.arguments().first().toUInt()){done();return;}
            const auto pid=reply.arguments().first().toUInt();
            using Environment=QPair<bool,QMap<QByteArray,QByteArray>>;
            auto *watcher=new QFutureWatcher<Environment>(this);
            connect(watcher,&QFutureWatcher<Environment>::finished,this,[watcher,daemonEnvironmentKnown,daemonEnvironment,done]{const auto result=watcher->result();watcher->deleteLater();*daemonEnvironmentKnown=result.first;*daemonEnvironment=result.second;done();});
            watcher->setFuture(QtConcurrent::run([pid]{
                Environment result;const auto path=QFile::encodeName(QStringLiteral("/proc/%1/environ").arg(pid));const int fd=::open(path.constData(),O_RDONLY|O_NONBLOCK|O_CLOEXEC);if(fd<0)return result;
                QFile environment;if(!environment.open(fd,QIODevice::ReadOnly,QFileDevice::AutoCloseHandle)){::close(fd);return result;}
                struct stat metadata{};if(::fstat(fd,&metadata)!=0||!S_ISREG(metadata.st_mode))return result;
                const auto data=environment.read(1024*1024+1);if(environment.error()!=QFileDevice::NoError||data.size()>1024*1024)return result;result.first=true;
                for(const auto &entry:data.split('\0')){const auto equal=entry.indexOf('=');if(equal<0)continue;const auto name=entry.left(equal);if(name=="QT_QUICK_CONTROLS_MOBILE"||name=="KDE_KIRIGAMI_TABLET_MODE"||name=="QT_NO_XDG_DESKTOP_PORTAL")result.second.insert(name,entry.mid(equal+1));}
                return result;
            }));
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
void PowerPolicyDBus::retireCapabilitiesQuery(){++m_queryGeneration;}
void PowerPolicyDBus::reloadGuarded(const PowerPolicyCapabilities &expected,Guard guard)
{
    if(m_reloading||m_guardedReload){emit reloadFinished(false,false,QStringLiteral("Another native power reload is in progress."));return;}
    const auto attempt=++m_guardGeneration;const auto epoch=m_ownerEpoch;m_guardedReload=true;
    QPointer<PowerPolicyDBus> safe(this);auto connection=std::make_shared<QMetaObject::Connection>();
    *connection=connect(this,&PowerPolicyDaemon::capabilitiesFinished,this,[safe,connection,expected,guard=std::move(guard),attempt,epoch](const PowerPolicyCapabilities &caps){
        QObject::disconnect(*connection);if(!safe||attempt!=safe->m_guardGeneration||!safe->m_guardedReload)return;
        if(!caps.serviceAvailable||!caps.schemaSupported||caps.owner!=expected.owner||caps.revision!=expected.revision||epoch!=safe->m_ownerEpoch||!guard){safe->m_guardedReload=false;emit safe->reloadFinished(false,false,QStringLiteral("Native power owner or capabilities changed before reload."));return;}
        auto once=std::make_shared<bool>(false);
        guard([safe,expected,epoch,attempt,once](bool valid,const QString &error){
            if(*once)return;*once=true;if(!safe||attempt!=safe->m_guardGeneration||!safe->m_guardedReload)return;
            if(!valid||epoch!=safe->m_ownerEpoch||safe->m_capabilityRevision!=expected.revision){safe->m_guardedReload=false;emit safe->reloadFinished(false,false,error.isEmpty()?QStringLiteral("Saved power preferences or provider changed at the final reload boundary."):error);return;}
            safe->call(safe->m_session,QStringLiteral("org.freedesktop.DBus"),QStringLiteral("/org/freedesktop/DBus"),QStringLiteral("org.freedesktop.DBus"),QStringLiteral("GetNameOwner"),{Service},[safe,expected,epoch,attempt](const QDBusMessage &reply){
                if(!safe||attempt!=safe->m_guardGeneration||!safe->m_guardedReload)return;
                const bool valid=reply.type()==QDBusMessage::ReplyMessage&&reply.arguments().size()==1&&reply.arguments().first().metaType().id()==QMetaType::QString&&reply.arguments().first().toString()==expected.owner&&epoch==safe->m_ownerEpoch&&safe->m_capabilityRevision==expected.revision;
                safe->m_guardedReload=false;
                if(!valid){emit safe->reloadFinished(false,false,QStringLiteral("The native power owner changed after the saved-file guard; no reload was sent."));return;}
                safe->sendReload(expected.owner,epoch);
            });
        });
    });
    queryCapabilities();
}
void PowerPolicyDBus::reload(){sendReload(m_owner,m_ownerEpoch);}
void PowerPolicyDBus::sendReload(const QString &owner,quint64 epoch)
{
    if(m_reloading)return;
    if(owner.isEmpty()||owner!=m_owner||epoch!=m_ownerEpoch){emit reloadFinished(false,false,QStringLiteral("The native PowerDevil owner is unavailable or changed."));return;}
    m_reloadOwner=owner;m_reloading=true;m_reloadSignal=false;m_reloadReply=false;const auto generation=++m_generation;m_reloadTimer.start();
    call(m_session,m_reloadOwner,Path,Interface,QStringLiteral("refreshStatus"),{},[this,generation,epoch](const QDBusMessage &reply){
        if(!m_reloading||generation!=m_generation)return;
        if(epoch!=m_ownerEpoch||reply.type()!=QDBusMessage::ReplyMessage||!reply.arguments().isEmpty()){finishReload(false,false,QStringLiteral("Power policy was saved, but the native reload was not acknowledged. A sent request may still complete. %1").arg(reply.errorMessage()));return;}
        m_reloadReply=true;if(m_reloadSignal)finishReload(true,false,{});
    });
}
void PowerPolicyDBus::configurationReloaded()
{
    QPointer<PowerPolicyDBus> safe(this);
    if(m_reloading&&calledFromDBus()&&message().service()==m_reloadOwner){m_reloadSignal=true;if(m_reloadReply)finishReload(true,false,{});}
    if(safe)emit safe->changed();
}
void PowerPolicyDBus::invalidateProvider(const QString &reason)
{
    ++m_ownerEpoch;m_owner.clear();m_capabilityRevision.clear();++m_guardGeneration;
    const bool guarded=m_guardedReload;m_guardedReload=false;QPointer<PowerPolicyDBus> safe(this);
    if(m_reloading)finishReload(false,false,reason);else if(guarded)emit reloadFinished(false,false,reason);
    if(safe)emit safe->providerChanged();if(safe)emit safe->changed();
}
void PowerPolicyDBus::capabilitiesChanged(){invalidateProvider(QStringLiteral("Native power action capabilities changed during the operation."));}
void PowerPolicyDBus::portalSettingChanged(const QString &group,const QString &key,const QDBusVariant &)
{if(group==QLatin1String("org.kde.TabletMode")&&key==QLatin1String("enabled"))invalidateProvider(QStringLiteral("The native tablet-mode default inputs changed."));}
void PowerPolicyDBus::hardwarePropertiesChanged(const QString &interface,const QVariantMap &changed,const QStringList &invalidated)
{
    if(interface!=QLatin1String("org.freedesktop.UPower.Device")&&interface!=QLatin1String("org.freedesktop.UPower"))return;
    const QStringList keys=interface.endsWith(QLatin1String(".Device"))?QStringList{"Type","PowerSupply","IsPresent"}:QStringList{"LidIsPresent"};
    for(const auto &key:keys)if(changed.contains(key)||invalidated.contains(key)){invalidateProvider(QStringLiteral("Native power hardware/default capabilities changed."));return;}
    emit this->changed();
}
void PowerPolicyDBus::powerSourceChanged(const QString &){emit changed();}
void PowerPolicyDBus::cancel()
{
    if(m_guardedReload){m_guardedReload=false;++m_guardGeneration;retireCapabilitiesQuery();emit reloadFinished(false,true,QStringLiteral("The native reload was cancelled before dispatch."));return;}
    if(m_reloading)finishReload(false,true,QStringLiteral("Waiting for the PowerDevil reload was cancelled; the sent request may still complete."));
}
void PowerPolicyDBus::finishReload(bool acknowledged, bool cancelled, const QString &error)
{
    if (!m_reloading)
        return;
    m_reloading = false;
    m_reloadTimer.stop();
    emit reloadFinished(acknowledged, cancelled, error);
}

namespace {
using ConfigContext=KdePowerPolicyBackend::ConfigContext;
QStringList relevantFiles(const QString &fileName,const ConfigContext &context)
{
    QStringList files{fileName};files<<context.defaultSources;
    if(context.fullCascade){for(const auto &dir:QStandardPaths::standardLocations(QStandardPaths::GenericConfigLocation))files<<dir+"/powerdevilrc"<<dir+"/kdeglobals"<<dir+"/system.kdeglobals";if(!qEnvironmentVariableIsSet("KDE_SKIP_KDERC"))files<<"/etc/kde5rc";}
    files.removeDuplicates();return files;
}
bool readFile(const QString &fileName,const ConfigContext &context,const PowerPolicyCapabilities &caps,PowerPolicySnapshot *snapshot,QString *error)
{
    if(!QFileInfo(fileName).isAbsolute()||(context.fullCascade&&QDir::cleanPath(fileName)!=QDir::cleanPath(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)+QStringLiteral("/powerdevilrc")))){*error=QStringLiteral("Full native cascading requires the active absolute XDG powerdevilrc path.");return false;}
    if(context.fixture&&context.beforeFileRead)context.beforeFileRead();
    QMap<QString,QString> canonical;QMap<QPair<quint64,quint64>,QString> identities;
    for(const auto &path:relevantFiles(fileName,context)){
        const QFileInfo info(path);if(!info.isAbsolute()){*error=QStringLiteral("Native power configuration/default source paths must be absolute.");return false;}if(!info.exists()&&!info.isSymLink())continue;
        if(!info.isFile()||!info.isReadable()||info.size()>MaximumConfigSize){*error=QStringLiteral("A native power source is not a readable bounded regular file, or is a broken link.");return false;}
        const auto name=info.canonicalFilePath(),normalized=QDir::cleanPath(info.absoluteFilePath());struct stat metadata{};
        if(name.isEmpty()||::stat(QFile::encodeName(path).constData(),&metadata)!=0||!S_ISREG(metadata.st_mode)){*error=QStringLiteral("A native power source changed identity during inspection.");return false;}
        const auto identity=qMakePair(quint64(metadata.st_dev),quint64(metadata.st_ino));
        if((canonical.contains(name)&&canonical.value(name)!=normalized)||(identities.contains(identity)&&identities.value(identity)!=normalized)){*error=QStringLiteral("Distinct native power configuration/default paths alias the same file. No mutation is permitted.");return false;}
        canonical.insert(name,normalized);identities.insert(identity,normalized);
        const int fd=::open(QFile::encodeName(path).constData(),O_RDONLY|O_NONBLOCK|O_CLOEXEC);if(fd<0){*error=QStringLiteral("A native power source could not be opened.");return false;}
        QFile source;if(!source.open(fd,QIODevice::ReadOnly,QFileDevice::AutoCloseHandle)){::close(fd);*error=QStringLiteral("A native power source handle could not be read.");return false;}
        struct stat opened{};if(::fstat(fd,&opened)!=0||!S_ISREG(opened.st_mode)||opened.st_size>MaximumConfigSize||opened.st_dev!=metadata.st_dev||opened.st_ino!=metadata.st_ino){*error=QStringLiteral("A native power source changed identity before reading.");return false;}
    }
    *snapshot={};
    auto config = KSharedConfig::openConfig(context.fullCascade ? QStringLiteral("powerdevilrc") : fileName,
                                           context.fullCascade ? KConfig::FullConfig : context.defaultSources.isEmpty()?KConfig::SimpleConfig:KConfig::NoGlobals);
    if (!context.defaultSources.isEmpty()) config->addConfigSources(context.defaultSources);
    config->reparseConfiguration();
    KConfig local(fileName, KConfig::SimpleConfig);
    std::unique_ptr<KConfig> global;if(context.fullCascade)global=std::make_unique<KConfig>(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)+QStringLiteral("/kdeglobals"),KConfig::SimpleConfig);
    auto editorCaps = caps; editorCaps.mobile = caps.defaultMobile;
    snapshot->serviceAvailable = caps.serviceAvailable && caps.schemaSupported;
    snapshot->unavailableReason = caps.reason;
    if (!snapshot->serviceAvailable && snapshot->unavailableReason.isEmpty())
        snapshot->unavailableReason = QStringLiteral("KDE PowerDevil or its verified configuration schema is unavailable.");
    snapshot->owner = caps.owner; snapshot->capabilityRevision = caps.revision;
    for (const QString &id : {QStringLiteral("AC"), QStringLiteral("Battery"), QStringLiteral("LowBattery")}) {
        ProfileSchema schema(config, id, caps); auto p = schema.values;
        const QString hardware = id != QLatin1String("AC") && !caps.hasBattery
            ? caps.batteryKnown ? QStringLiteral("No system power-supply battery is present.") : QStringLiteral("System battery hardware could not be verified.") : QString{};
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
        policy(schema.display, {"TurnOffDisplayWhenIdle", "TurnOffDisplayIdleTimeoutSec"}, caps.displaySupported, unsupported, p.displayReason, p.displayEditable);
        policy(schema.suspend, {"AutoSuspendAction", "AutoSuspendIdleTimeoutSec"}, caps.suspendActionSupported, unsupported, p.sleepReason, p.sleepEditable);
        policy(schema.display, {"DimDisplayWhenIdle", "DimDisplayIdleTimeoutSec"}, caps.dimSupported, unsupported, p.dimReason, p.dimEditable);
        policy(schema.display, {"UseProfileSpecificDisplayBrightness", "DisplayBrightness"}, caps.brightnessSupported, unsupported, p.brightnessReason, p.brightnessEditable);
        policy(schema.keyboard, {"UseProfileSpecificKeyboardBrightness", "KeyboardBrightness"}, caps.keyboardBrightnessSupported, unsupported, p.keyboardBrightnessReason, p.keyboardBrightnessEditable);
        policy(schema.display, {"TurnOffDisplayIdleTimeoutWhenLockedSec"}, caps.displaySupported, unsupported, p.lockedDisplayReason, p.lockedDisplayEditable);
        policy(schema.display, {"LockBeforeTurnOffDisplay"}, caps.displaySupported, unsupported, p.lockBeforeDisplayOffReason, p.lockBeforeDisplayOffEditable);
        policy(schema.suspend, {"PowerButtonAction"}, caps.buttonSupported, unsupported, p.powerButtonReason, p.powerButtonEditable);
        policy(schema.suspend, {"PowerDownAction"}, caps.buttonSupported, unsupported, p.powerDownReason, p.powerDownEditable);
        policy(schema.suspend, {"LidAction"}, caps.buttonSupported && caps.lidKnown && caps.hasLid, unsupported, p.lidReason, p.lidEditable);
        policy(schema.suspend, {"InhibitLidActionWhenExternalMonitorPresent"}, caps.buttonSupported && caps.lidKnown && caps.hasLid, unsupported, p.lidExternalMonitorReason, p.lidExternalMonitorEditable);
        p.idleActions = actionChoices(caps, false); p.powerButtonActions = actionChoices(caps, true); p.lidActions = actionChoices(caps, true, true);
        if (caps.suspendActionSupported && caps.canSuspend) p.sleepModes << PowerPolicyAction{1, QStringLiteral("Standby (memory)")};
        if (caps.suspendActionSupported && caps.hybridSuspendSupported) p.sleepModes << PowerPolicyAction{2, QStringLiteral("Hybrid sleep")};
        if (caps.suspendActionSupported && caps.suspendThenHibernateSupported) p.sleepModes << PowerPolicyAction{3, QStringLiteral("Standby, then hibernate")};
        policy(schema.suspend, {QStringLiteral("SleepMode")}, !p.sleepModes.isEmpty(), unsupported, p.sleepModeReason, p.sleepModeEditable);
        ProfileSchema editor(config, id, editorCaps); editor.skeleton.setDefaults();
        for (const auto key : PowerPolicyFields::profileKeys()) {
            bool supported = false;
            switch (key) {
            case PowerPolicyKey::DisplayEnabled: case PowerPolicyKey::DisplaySeconds: case PowerPolicyKey::LockedDisplaySeconds: case PowerPolicyKey::LockBeforeDisplayOff: supported = caps.displaySupported; break;
            case PowerPolicyKey::IdleAction: case PowerPolicyKey::IdleSeconds: supported = caps.suspendActionSupported; break;
            case PowerPolicyKey::DimEnabled: case PowerPolicyKey::DimSeconds: supported = caps.dimSupported; break;
            case PowerPolicyKey::BrightnessEnabled: case PowerPolicyKey::BrightnessPercent: supported = caps.brightnessSupported; break;
            case PowerPolicyKey::KeyboardBrightnessEnabled: case PowerPolicyKey::KeyboardBrightnessPercent: supported = caps.keyboardBrightnessSupported; break;
            case PowerPolicyKey::PowerButtonAction: case PowerPolicyKey::PowerDownAction: supported = caps.buttonSupported; break;
            case PowerPolicyKey::LidAction: case PowerPolicyKey::LidExternalMonitorInhibition: supported = caps.buttonSupported && caps.lidKnown && caps.hasLid; break;
            case PowerPolicyKey::SleepMode: supported = !p.sleepModes.isEmpty(); break;
            default: break;
            }
            p.keys.insert(key, keyState(config, local, global.get(), id, key, PowerPolicyFields::value(p, key),
                PowerPolicyFields::value(editor.values, key), fallbackKnown(key, id, caps, false),
                fallbackKnown(key, id, caps, true), supported, common, context.fullCascade));
        }
        snapshot->profiles << p;
    }
    GlobalSchema globals(config, caps);
    snapshot->batteryLowLevel = globals.low; snapshot->batteryCriticalLevel = globals.critical; snapshot->batteryCriticalAction = globals.action;
    snapshot->batteryCriticalActions = actionChoices(caps, false);
    QString reason = !snapshot->serviceAvailable ? snapshot->unavailableReason : !caps.hasBattery ? QStringLiteral("A system power-supply battery could not be verified.") : QString{};
    if (reason.isEmpty() && (config->accessMode() != KConfig::ReadWrite || globals.battery.isImmutable())) reason = QStringLiteral("Battery management policy is not writable.");
    snapshot->batteryLevelsReason = reason;
    if (reason.isEmpty() && (globals.battery.isEntryImmutable("BatteryLowLevel") || globals.battery.isEntryImmutable("BatteryCriticalLevel"))) snapshot->batteryLevelsReason = QStringLiteral("Battery levels are locked by policy.");
    snapshot->batteryLevelsEditable = snapshot->batteryLevelsReason.isEmpty();
    snapshot->batteryCriticalActionReason = reason;
    if (reason.isEmpty() && (!caps.suspendActionSupported || globals.battery.isEntryImmutable("BatteryCriticalAction"))) snapshot->batteryCriticalActionReason = QStringLiteral("The native critical battery action is unavailable or locked.");
    snapshot->batteryCriticalActionEditable = snapshot->batteryCriticalActionReason.isEmpty();
    GlobalSchema editorGlobals(config, editorCaps); editorGlobals.skeleton.setDefaults();
    PowerPolicySnapshot editorValues; editorValues.batteryLowLevel = editorGlobals.low;
    editorValues.batteryCriticalLevel = editorGlobals.critical; editorValues.batteryCriticalAction = editorGlobals.action;
    const auto globalUnavailable = !snapshot->serviceAvailable ? snapshot->unavailableReason
        : !caps.hasBattery ? QStringLiteral("A system power-supply battery could not be verified.") : QString{};
    for (const auto key : PowerPolicyFields::globalKeys())
        snapshot->keys.insert(key, keyState(config, local, global.get(), {}, key, PowerPolicyFields::value(*snapshot, key),
            PowerPolicyFields::value(editorValues, key), fallbackKnown(key, {}, caps, false),
            fallbackKnown(key, {}, caps, true), key != PowerPolicyKey::BatteryCriticalAction || caps.suspendActionSupported,
            globalUnavailable, context.fullCascade));
    return true;
}

QByteArray sourceRevision(const QString &fileName,const ConfigContext &context,QString &error)
{
    QByteArray bytes;QDataStream stream(&bytes,QIODevice::WriteOnly);
    for(const auto &path:relevantFiles(fileName,context)){
        stream<<path;struct stat before{};
        if(::stat(QFile::encodeName(path).constData(),&before)!=0){
            if(errno==ENOENT&&!QFileInfo(path).isSymLink()){stream<<false;continue;}
            error=QStringLiteral("A Power Options source identity could not be inspected.");return {};
        }
        const int fd=::open(QFile::encodeName(path).constData(),O_RDONLY|O_NONBLOCK|O_CLOEXEC);if(fd<0){error=QStringLiteral("A Power Options source could not be read for the final file guard.");return {};}
        QFile source;if(!source.open(fd,QIODevice::ReadOnly,QFileDevice::AutoCloseHandle)){::close(fd);error=QStringLiteral("A Power Options source handle could not be inspected.");return {};}struct stat opened{},after{};
        if(::fstat(fd,&opened)!=0||!S_ISREG(opened.st_mode)||opened.st_size>MaximumConfigSize||opened.st_dev!=before.st_dev||opened.st_ino!=before.st_ino){error=QStringLiteral("A Power Options source changed identity while being fingerprinted.");return {};}
        const auto data=source.read(MaximumConfigSize+1);
        if(source.error()!=QFileDevice::NoError||data.size()>MaximumConfigSize||::fstat(fd,&after)!=0||after.st_size!=opened.st_size||after.st_mtim.tv_sec!=opened.st_mtim.tv_sec||after.st_mtim.tv_nsec!=opened.st_mtim.tv_nsec){error=QStringLiteral("A Power Options source changed during the final file guard.");return {};}
        struct stat link{};if(::lstat(QFile::encodeName(path).constData(),&link)!=0){error=QStringLiteral("A Power Options source path changed while being fingerprinted.");return {};}
        stream<<quint64(link.st_dev)<<quint64(link.st_ino)<<quint64(link.st_mode)<<quint64(link.st_uid)<<quint64(link.st_gid)<<QFileInfo(path).symLinkTarget();
        stream<<true<<quint64(opened.st_dev)<<quint64(opened.st_ino)<<quint64(opened.st_mode)<<quint64(opened.st_uid)<<quint64(opened.st_gid)<<qint64(opened.st_mtim.tv_sec)<<qint64(opened.st_mtim.tv_nsec)<<data;
    }
    return QCryptographicHash::hash(bytes,QCryptographicHash::Sha256);
}
namespace Reservations=SettingsFileReservations;
QStringList reservationPaths(const QString &fileName,const ConfigContext &context)
{
    QStringList paths{fileName};if(context.fullCascade)paths<<QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)+QStringLiteral("/kdeglobals");return paths;
}
struct FileResult {
    bool success=false,saved=false,cancelled=false;
    PowerPolicySnapshot snapshot;
    QString error;QByteArray receipt;QStringList keys;
    Reservations::Inspection admission;
};
FileResult readFiles(const QString &fileName,const ConfigContext &context,const PowerPolicyCapabilities &caps)
{
    FileResult result;result.success=readFile(fileName,context,caps,&result.snapshot,&result.error);
    if(result.success){result.receipt=sourceRevision(fileName,context,result.error);result.success=!result.receipt.isEmpty();}return result;
}
FileResult saveFiles(const QString &fileName,const ConfigContext &context,const PowerPolicyCapabilities &caps,const PowerPolicySnapshot &expected,const PowerPolicySnapshot &requested,const Reservations::Reservation &reservation,const QStringList &expectedKeys,const std::shared_ptr<std::atomic_bool> &cancelled,const std::shared_ptr<std::atomic_bool> &providerChanged)
{
    struct MutationEnd {Reservations::Reservation value;~MutationEnd(){Reservations::endMutation(value);}} end{reservation};
    auto result=readFiles(fileName,context,caps);
    if(!result.success)return result;
    if(result.snapshot!=expected||!result.snapshot.serviceAvailable||!PowerPolicyFields::requestValid(expected,requested,result.error)||Reservations::currentKeys(reservation.paths,&result.error)!=expectedKeys){result.success=false;if(result.error.isEmpty())result.error=QStringLiteral("Native power preferences, defaults, policy, provider or file identity changed at the write boundary.");return result;}
    result.success=false;
    if(cancelled->load()||providerChanged->load()){result.cancelled=cancelled->load();result.error=result.cancelled?QStringLiteral("Power policy saving was cancelled before native synchronization."):QStringLiteral("The native power provider changed before synchronization.");return result;}
    bool synced=false;QByteArray committedReceipt;QString receiptError;
    {
        // Only explicitly owned scalar intents are written. All native objects
        // are born, used, cleaned and destroyed on this same worker.
        KConfig config(context.fullCascade?QStringLiteral("powerdevilrc"):fileName,context.fullCascade?KConfig::FullConfig:context.defaultSources.isEmpty()?KConfig::SimpleConfig:KConfig::NoGlobals);
        if(!context.defaultSources.isEmpty())config.addConfigSources(context.defaultSources);config.reparseConfiguration();
        const auto write=[&](const QString &id,PowerPolicyKey key,PowerPolicyIntent intent,const QVariant &value,const PowerPolicyKeyState &state){
            const auto name=PowerPolicyFields::keyName(key),leaf=PowerPolicyFields::keyGroup(key);auto group=id.isEmpty()?config.group(leaf):config.group(id).group(leaf);
            if(intent==PowerPolicyIntent::RestoreDefault&&!state.inheritedPresent)group.revertToDefault(name);
            else switch(value.metaType().id()){
                case QMetaType::Bool:group.writeEntry(name,value.toBool());break;
                case QMetaType::UInt:group.writeEntry(name,value.toUInt());break;
                case QMetaType::Int:group.writeEntry(name,value.toInt());break;
                default:Q_UNREACHABLE();
            }
        };
        for(int i=0;i<requested.profiles.size();++i){const auto &profile=requested.profiles[i];for(auto it=profile.intents.cbegin();it!=profile.intents.cend();++it)write(profile.id,it.key(),it.value(),PowerPolicyFields::value(profile,it.key()),expected.profiles[i].keys.value(it.key()));}
        for(auto it=requested.intents.cbegin();it!=requested.intents.cend();++it)write({},it.key(),it.value(),PowerPolicyFields::value(requested,it.key()),expected.keys.value(it.key()));
        if(context.fixture&&context.beforeSync)context.beforeSync();
        if(cancelled->load()||providerChanged->load()){config.markAsClean();result.cancelled=cancelled->load();result.error=result.cancelled?QStringLiteral("Power policy saving was cancelled before native synchronization."):QStringLiteral("Native power capabilities changed before synchronization; reload was not requested.");return result;}
        const auto boundaryReceipt=sourceRevision(fileName,context,receiptError);
        if(boundaryReceipt.isEmpty()||boundaryReceipt!=result.receipt){config.markAsClean();result.error=receiptError.isEmpty()?QStringLiteral("Native power sources or policy changed immediately before synchronization; no write was attempted."):receiptError;return result;}
        // KConfig's native lock wait and post-lock merge are not cancellable
        // and are not compare-and-swap. No worker is killed or retried.
        synced=config.sync();result.saved=true;
        if(!synced)config.markAsClean(); // suppress destructor/reparse retries
        if(synced)committedReceipt=sourceRevision(fileName,context,receiptError);
        result.keys=Reservations::currentKeys(reservation.paths);
        if(context.fixture&&context.afterSync)context.afterSync();
    }
    result.keys<<Reservations::currentKeys(reservation.paths);result.keys.removeDuplicates();result.keys.sort();
    auto readback=readFiles(fileName,context,caps);result.snapshot=readback.snapshot;result.receipt=committedReceipt.isEmpty()?readback.receipt:committedReceipt;
    result.cancelled=cancelled->load();
    if(!synced){result.error=QStringLiteral("Native power policy saving failed; some fields or global defaults may already be saved. No reload was requested. Revert to inspect.");return result;}
    if(!readback.success){result.error=readback.error;return result;}
    if(committedReceipt.isEmpty()||readback.receipt!=committedReceipt){result.error=receiptError.isEmpty()?QStringLiteral("Native power source bytes or identities changed after synchronization; reload was not requested."):receiptError;return result;}
    if(!PowerPolicyFields::matchesRequest(expected,requested,result.snapshot,result.error))return result;
    if(result.cancelled){result.error=QStringLiteral("Cancellation was requested while native synchronization was running. The file operation has now completed and preferences may be saved; synchronization was not cancelled. No reload was requested.");return result;}
    if(providerChanged->load()){result.error=QStringLiteral("Power preferences were saved, but the provider changed during synchronization. No reload was requested; Revert to inspect.");return result;}
    result.success=true;result.error.clear();return result;
}
bool sameProvider(const PowerPolicyCapabilities &a,const PowerPolicyCapabilities &b)
{return a.serviceAvailable&&a.schemaSupported&&b.serviceAvailable&&b.schemaSupported&&a.owner==b.owner&&a.revision==b.revision;}
}

class PowerPolicyFileJob final : public QObject {
public:
    using ReadFinished=std::function<void(bool,const PowerPolicySnapshot &,const QString &)>;
    using WriteFinished=std::function<void(bool,bool,bool,const QString &)>;
    ReadFinished readFinished;WriteFinished writeFinished;std::function<void(const QString &)> pending;
    PowerPolicyFileJob(QString file,ConfigContext context,PowerPolicyDaemon *daemon,bool write,PowerPolicySnapshot expected,PowerPolicySnapshot requested)
        :QObject(QCoreApplication::instance()),m_file(std::move(file)),m_context(std::move(context)),m_daemon(daemon),m_write(write),m_expected(std::move(expected)),m_requested(std::move(requested))
    {
        daemon->setParent(this);m_deadline.setSingleShot(true);m_deadline.setInterval(m_context.fixture?qMax(1,m_context.softDeadlineMs):20000);
        connect(&m_deadline,&QTimer::timeout,this,[this]{if(m_phase!=Phase::Done&&pending)pending(QStringLiteral("Power Options is taking longer and continues in the background. Wait for completion before retrying or closing."));});
        connect(daemon,&PowerPolicyDaemon::providerChanged,this,[this]{m_providerChanged->store(true);});
        connect(daemon,&PowerPolicyDaemon::capabilitiesFinished,this,[this](const PowerPolicyCapabilities &caps){queried(caps);});
        connect(daemon,&PowerPolicyDaemon::reloadFinished,this,[this](bool acknowledged,bool cancelled,const QString &error){
            if(m_phase==Phase::Boundary){m_deferredReload=true;m_deferredAcknowledged=acknowledged;m_deferredCancelled=cancelled;m_deferredError=error;return;}
            if(m_phase==Phase::Reload)reloaded(acknowledged,cancelled,error);
        });
    }
    void start()
    {
        OperationManager::instance()->setActivity(this,m_write?QStringLiteral("Saving Power Options"):QStringLiteral("Reading Power Options"),true);
        if(m_phase==Phase::Done)return;
        m_deadline.start();m_phase=m_write?Phase::PreflightQuery:Phase::ReadQuery;m_daemon->queryCapabilities();
    }
    void cancel()
    {
        if(!m_write||m_phase==Phase::Done)return;m_cancelled->store(true);
        if(m_phase==Phase::Created||m_phase==Phase::PreflightQuery){m_daemon->retireCapabilitiesQuery();completeWrite(false,false,true,QStringLiteral("Power policy saving was cancelled before any file worker or native write started."));return;}
        if(m_phase==Phase::Reload){m_daemon->cancel();return;}
        if(pending)pending(QStringLiteral("Cancellation was requested. The operation continues until native file work finishes; a running synchronization was not cancelled. Wait for completion."));
    }
private:
    enum class Phase {Created,ReadQuery,ReadFiles,PreflightQuery,Admission,Save,ReloadQuery,Reload,Boundary,VerifyQuery,VerifyFiles,Done};
    template<class Work,class Done>void worker(Work work,Done done)
    {
        auto *watcher=new QFutureWatcher<FileResult>(this);
        connect(watcher,&QFutureWatcher<FileResult>::finished,this,[this,watcher,done=std::move(done)]{auto result=watcher->result();watcher->deleteLater();if(m_phase!=Phase::Done)done(std::move(result));});
        watcher->setFuture(QtConcurrent::run(std::move(work)));
    }
    void queried(const PowerPolicyCapabilities &caps)
    {
        if(m_phase==Phase::ReadQuery){m_phase=Phase::ReadFiles;const auto file=m_file;const auto context=m_context;worker([file,context,caps]{return readFiles(file,context,caps);},[this](FileResult result){completeRead(result.success,result.snapshot,result.error);});return;}
        if(m_phase==Phase::PreflightQuery){
            if(m_cancelled->load()){completeWrite(false,false,true,QStringLiteral("Power policy saving was cancelled before writing."));return;}
            m_caps=caps;m_phase=Phase::Admission;const auto file=m_file;const auto context=m_context;const auto inventory=Reservations::snapshot();
            worker([file,context,caps,inventory]{auto result=readFiles(file,context,caps);if(result.success){result.admission=Reservations::inspect(reservationPaths(file,context),inventory);if(!result.admission.success){result.success=false;result.error=result.admission.error;}}return result;},[this](FileResult result){
                QString error;
                if(m_cancelled->load()){completeWrite(false,false,true,QStringLiteral("Power policy saving was cancelled before writing."));return;}
                if(!result.success||result.snapshot!=m_expected||!result.snapshot.serviceAvailable||!PowerPolicyFields::requestValid(m_expected,m_requested,error)){
                    completeWrite(false,false,false,!result.error.isEmpty()?result.error:!error.isEmpty()?error:QStringLiteral("Native power preferences, defaults, policy or provider capabilities changed before saving. Revert to reload."));return;
                }
                m_reservation=Reservations::acquire(result.admission,error);if(!m_reservation.valid()){completeWrite(false,false,false,error);return;}
                if(!Reservations::beginMutation(m_reservation)){completeWrite(false,false,false,QStringLiteral("The power settings-file reservation could not enter its native transaction."));return;}
                m_phase=Phase::Save;const auto file=m_file;const auto context=m_context;const auto caps=m_caps;const auto expected=m_expected;const auto requested=m_requested;const auto reservation=m_reservation;const auto keys=result.admission.keys;const auto cancelled=m_cancelled;const auto changed=m_providerChanged;
                worker([file,context,caps,expected,requested,reservation,keys,cancelled,changed]{return saveFiles(file,context,caps,expected,requested,reservation,keys,cancelled,changed);},[this](FileResult result){
                    m_saved=result.saved;m_savedSnapshot=result.snapshot;m_savedReceipt=result.receipt;
                    if(!result.keys.isEmpty()&&!Reservations::refresh(m_reservation,result.keys)&&result.error.isEmpty())result.error=QStringLiteral("Post-save native power file identities could not be retained.");
                    if(!result.success||!result.error.isEmpty()){
                        m_writeError=result.error;
                        if(!m_saved){completeWrite(false,false,m_cancelled->load()||result.cancelled,m_writeError);return;}
                        verify();return;
                    }
                    if(m_cancelled->load()||m_providerChanged->load()){
                        m_writeError=m_cancelled->load()?QStringLiteral("Cancellation was requested after file synchronization completed. Saved preferences remain; reload was not requested."):QStringLiteral("The native power provider changed after saving; reload was not requested.");verify();return;
                    }
                    m_phase=Phase::ReloadQuery;m_daemon->queryCapabilities();
                });
            });return;
        }
        if(m_phase==Phase::ReloadQuery){
            if(m_cancelled->load()||m_providerChanged->load()||!sameProvider(m_caps,caps)){
                m_writeError=m_cancelled->load()?QStringLiteral("Cancellation was requested after saving. Reload was not requested."):QStringLiteral("Native power owner or capabilities changed after saving; reload was not requested.");verifyFiles(caps);return;
            }
            m_phase=Phase::Reload;QPointer<PowerPolicyFileJob> safe(this);
            m_daemon->reloadGuarded(caps,[safe,caps](PowerPolicyDaemon::GuardCompletion done){if(!safe){done(false,QStringLiteral("The tracked power save operation is unavailable."));return;}safe->boundary(caps,std::move(done));});return;
        }
        if(m_phase==Phase::VerifyQuery)verifyFiles(caps);
    }
    void boundary(const PowerPolicyCapabilities &caps,PowerPolicyDaemon::GuardCompletion done)
    {
        if(m_phase!=Phase::Reload||m_guardStarted){done(false,QStringLiteral("This power reload guard no longer owns the save operation."));return;}
        m_guardStarted=true;m_phase=Phase::Boundary;const auto file=m_file;const auto context=m_context;const auto saved=m_savedSnapshot;const auto receipt=m_savedReceipt;const auto paths=m_reservation.paths;
        worker([file,context,caps,saved,receipt,paths]{auto result=readFiles(file,context,caps);if(result.success&&(result.snapshot!=saved||result.receipt!=receipt)){result.success=false;result.error=QStringLiteral("Saved power preferences, policy, defaults or foreign source bytes changed before reload; no reload was sent.");}if(result.success){result.keys=Reservations::currentKeys(paths,&result.error);result.success=!result.keys.isEmpty();}return result;},[this,done=std::move(done)](FileResult result){
            m_phase=Phase::Reload;
            if(m_cancelled->load()||m_providerChanged->load()||m_deferredReload){result.success=false;result.error=m_cancelled->load()?QStringLiteral("Cancellation was requested before reload dispatch; saved preferences remain."):QStringLiteral("The native power provider changed during the final saved-file guard.");}
            if(result.success&&!Reservations::refresh(m_reservation,result.keys)){result.success=false;result.error=QStringLiteral("Native power target identities could not be retained before reload.");}
            done(result.success,result.error);
            if(m_phase==Phase::Reload&&m_deferredReload){m_deferredReload=false;reloaded(m_deferredAcknowledged,m_deferredCancelled,m_deferredError);}
        });
    }
    void reloaded(bool acknowledged,bool cancelled,const QString &error)
    {
        m_acknowledged=acknowledged;if(cancelled)m_cancelled->store(true);m_reloadError=error;verify();
    }
    void verify(){m_phase=Phase::VerifyQuery;m_daemon->queryCapabilities();}
    void verifyFiles(const PowerPolicyCapabilities &caps)
    {
        m_phase=Phase::VerifyFiles;const auto file=m_file;const auto context=m_context;
        worker([file,context,caps]{return readFiles(file,context,caps);},[this](FileResult result){
            QString verification;const bool matches=result.success&&PowerPolicyFields::matchesRequest(m_expected,m_requested,result.snapshot,verification);
            QString error=!m_writeError.isEmpty()?m_writeError:!m_reloadError.isEmpty()?m_reloadError:!result.success?result.error:!matches?verification:!m_savedReceipt.isEmpty()&&result.receipt!=m_savedReceipt?QStringLiteral("Native power source bytes or identities changed during Apply; Revert to inspect the saved state."):m_providerChanged->load()?QStringLiteral("Native power provider capabilities changed during Apply."):QString();
            if(error.isEmpty()&&m_cancelled->load())error=QStringLiteral("Power policy waiting was cancelled. Saved preferences remain and any sent native reload may still complete.");
            completeWrite(m_saved,m_acknowledged,m_cancelled->load(),error);
        });
    }
    void completeRead(bool success,const PowerPolicySnapshot &snapshot,const QString &error)
    {
        if(m_phase==Phase::Done)return;m_phase=Phase::Done;m_deadline.stop();const auto done=std::move(readFinished);if(done)done(success,snapshot,error);OperationManager::instance()->setActivity(this,{},false);deleteLater();
    }
    void completeWrite(bool saved,bool acknowledged,bool cancelled,const QString &error)
    {
        if(m_phase==Phase::Done)return;m_phase=Phase::Done;m_deadline.stop();Reservations::release(m_reservation);const auto done=std::move(writeFinished);if(done)done(saved,acknowledged,cancelled,error);OperationManager::instance()->setActivity(this,{},false);deleteLater();
    }
    QString m_file;ConfigContext m_context;PowerPolicyDaemon *m_daemon;bool m_write;
    PowerPolicySnapshot m_expected,m_requested,m_savedSnapshot;PowerPolicyCapabilities m_caps;Reservations::Reservation m_reservation;QByteArray m_savedReceipt;
    std::shared_ptr<std::atomic_bool> m_cancelled=std::make_shared<std::atomic_bool>(false),m_providerChanged=std::make_shared<std::atomic_bool>(false);
    Phase m_phase=Phase::Created;bool m_saved=false,m_acknowledged=false,m_guardStarted=false,m_deferredReload=false,m_deferredAcknowledged=false,m_deferredCancelled=false;
    QString m_writeError,m_reloadError,m_deferredError;QTimer m_deadline;
};

KdePowerPolicyBackend::KdePowerPolicyBackend(QObject *parent)
    :KdePowerPolicyBackend(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)+QStringLiteral("/powerdevilrc"),new PowerPolicyDBus,ConfigContext{true,{}},parent){}
KdePowerPolicyBackend::KdePowerPolicyBackend(const QString &file,PowerPolicyDaemon *daemon,QObject *parent)
    :KdePowerPolicyBackend(file,daemon,ConfigContext{},parent){}
KdePowerPolicyBackend::KdePowerPolicyBackend(const QString &file,PowerPolicyDaemon *daemon,const ConfigContext &context,QObject *parent)
    :PowerPolicyBackend(parent),m_configFile(file),m_daemon(daemon),m_context(context)
{
    Q_ASSERT(daemon);daemon->setParent(this);connect(daemon,&PowerPolicyDaemon::changed,this,&PowerPolicyBackend::externalChange);
    m_debounce.setSingleShot(true);m_debounce.setInterval(100);connect(&m_debounce,&QTimer::timeout,this,&PowerPolicyBackend::externalChange);
    auto changed=[this](const QString &){watchConfig();m_debounce.start();};connect(&m_watcher,&QFileSystemWatcher::fileChanged,this,changed);connect(&m_watcher,&QFileSystemWatcher::directoryChanged,this,changed);watchConfig();
}
void KdePowerPolicyBackend::watchConfig()
{
    for(const auto &file:relevantFiles(m_configFile,m_context)){
        auto dir=QFileInfo(file).absolutePath();while(!QDir(dir).exists()&&dir!=QLatin1String("/"))dir=QFileInfo(dir).absolutePath();
        if(QDir(dir).exists()&&!m_watcher.directories().contains(dir))m_watcher.addPath(dir);if(QFileInfo::exists(file)&&!m_watcher.files().contains(file))m_watcher.addPath(file);
    }
}
void KdePowerPolicyBackend::beginJob(bool write,const PowerPolicySnapshot &expected,const PowerPolicySnapshot &requested)
{
    if(m_job)return;
    auto *job=new PowerPolicyFileJob(m_configFile,m_context,m_daemon,write,expected,requested);m_job=job;QPointer<KdePowerPolicyBackend> safe(this);
    job->pending=[safe](const QString &message){if(safe)emit safe->operationPending(message);};
    job->readFinished=[safe](bool success,const PowerPolicySnapshot &snapshot,const QString &error){if(!safe)return;safe->m_daemon->setParent(safe);safe->m_job=nullptr;safe->watchConfig();emit safe->readFinished(success,snapshot,error);};
    job->writeFinished=[safe](bool saved,bool acknowledged,bool cancelled,const QString &error){if(!safe)return;safe->m_daemon->setParent(safe);safe->m_job=nullptr;safe->watchConfig();emit safe->writeFinished(saved,acknowledged,cancelled,error);};job->start();
}
void KdePowerPolicyBackend::readCurrent(){beginJob(false);}
void KdePowerPolicyBackend::writePolicy(const PowerPolicySnapshot &expected,const PowerPolicySnapshot &requested)
{
    if(m_job)return;QString error;if(!PowerPolicyFields::hasIntents(requested)||!PowerPolicyFields::requestValid(expected,requested,error)){emit writeFinished(false,false,false,error.isEmpty()?QStringLiteral("No explicit native power policy operation was requested."):error);return;}beginJob(true,expected,requested);
}
void KdePowerPolicyBackend::cancel(){if(m_job)m_job->cancel();}
