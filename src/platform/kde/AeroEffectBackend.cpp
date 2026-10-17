#include "AeroEffectBackend.h"

#include <KConfig>
#include <KConfigGroup>
#include <QCryptographicHash>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusReply>
#include <QDBusServiceWatcher>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLibraryInfo>
#include <QPluginLoader>
#include <QPointer>
#include <QStandardPaths>
#include <QXmlStreamReader>
#include <functional>
#include <memory>

namespace {
constexpr auto groupName = "Effect-aeroglassblur";
constexpr auto keyName = "BlurStrength";
const QString effectId = QStringLiteral("aeroglassblur");

bool supportsEffectInterface(const QString &xmlText)
{
    if (xmlText.size() > 65536) return false;
    QXmlStreamReader xml(xmlText);
    bool correctInterface = false, reconfigure = false, loaded = false;
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement() && xml.name() == QLatin1String("interface"))
            correctInterface = xml.attributes().value(QStringLiteral("name")) == QLatin1String("org.kde.kwin.Effects");
        else if (xml.isEndElement() && xml.name() == QLatin1String("interface"))
            correctInterface = false;
        else if (correctInterface && xml.isStartElement() && xml.name() == QLatin1String("method")) {
            const auto name = xml.attributes().value(QStringLiteral("name"));
            reconfigure = reconfigure || name == QLatin1String("reconfigureEffect");
            loaded = loaded || name == QLatin1String("isEffectLoaded");
        }
    }
    return !xml.hasError() && reconfigure && loaded;
}

class KWinAeroRuntime final : public AeroEffectRuntime {
public:
    explicit KWinAeroRuntime(const QDBusConnection &connection) : m_bus(connection)
    {
        auto *watcher = new QDBusServiceWatcher(QStringLiteral("org.kde.KWin"), connection,
            QDBusServiceWatcher::WatchForOwnerChange, this);
        connect(watcher, &QDBusServiceWatcher::serviceOwnerChanged, this, [this] {
            ++m_generation; m_compatible = m_loaded = false; emit externalChange();
        });
    }
    void probe() override
    {
        const quint64 generation = m_generation;
        call(QStringLiteral("/KWin"), QStringLiteral("org.kde.KWin"), QStringLiteral("supportInformation"), {},
            [this, generation](const QDBusMessage &message) {
            const QDBusReply<QString> reply(message);
            if (generation != m_generation || !reply.isValid() || !isSupportedAeroKWinReport(reply.value())) {
                finishProbe(false, false, tr("This Aero effect integration requires KWin 6.6.6 on Wayland and its diagnostic interface."));
                return;
            }
            call(QStringLiteral("/Effects"), QStringLiteral("org.freedesktop.DBus.Introspectable"), QStringLiteral("Introspect"), {},
                [this, generation](const QDBusMessage &message) {
                const QDBusReply<QString> reply(message);
                if (generation != m_generation || !reply.isValid() || !supportsEffectInterface(reply.value())) {
                    finishProbe(false, false, tr("KWin's effect reconfiguration interface is unavailable."));
                    return;
                }
                checkLoaded([this, generation](bool success, bool loaded) {
                    if (!success || generation != m_generation)
                        finishProbe(false, false, tr("KWin's loaded Aero effect could not be checked."));
                    else
                        finishProbe(true, loaded, loaded ? QString()
                            : tr("Aero Glass Blur is not loaded. This page does not enable or disable effects."));
                });
            });
        });
    }
    void reconfigure() override
    {
        if (!m_compatible || !m_loaded) {
            emit reconfigureFinished(false, tr("The preference was saved, but a compatible loaded Aero effect is unavailable. Revert to reload it."));
            return;
        }
        const quint64 generation = m_generation;
        // Same fixed method and effect ID as the installed Aero Glass Blur KCM.
        // A reply acknowledges reconfigure(), not its resulting shader state.
        call(QStringLiteral("/Effects"), QStringLiteral("org.kde.kwin.Effects"), QStringLiteral("reconfigureEffect"), {effectId},
            [this, generation](const QDBusMessage &message) {
            const QDBusReply<void> reply(message);
            if (generation != m_generation || !reply.isValid()) {
                emit reconfigureFinished(false, tr("The blur preference was saved, but KWin did not acknowledge reconfiguring Aero Glass Blur. Revert before retrying."));
                return;
            }
            checkLoaded([this, generation](bool success, bool loaded) {
                const bool confirmed = generation == m_generation && success && loaded;
                emit reconfigureFinished(confirmed, confirmed ? QString()
                    : tr("The blur preference was saved, but Aero Glass Blur was no longer loaded after reconfiguration. Revert before retrying."));
            });
        });
    }
private:
    using Reply = std::function<void(const QDBusMessage &)>;
    void call(const QString &path, const QString &interface, const QString &method,
              const QVariantList &arguments, Reply completion)
    {
        auto message = QDBusMessage::createMethodCall(QStringLiteral("org.kde.KWin"), path, interface, method);
        message.setArguments(arguments);
        auto *pending = new QDBusPendingCallWatcher(m_bus.asyncCall(message, 2500), this);
        auto done = std::make_shared<bool>(false);
        const QPointer<QDBusPendingCallWatcher> safePending(pending);
        auto finish = [pending, safePending, done, completion] {
            if (*done || !safePending) return;
            *done = true;
            const QDBusMessage reply = pending->reply();
            pending->deleteLater(); completion(reply);
        };
        connect(pending, &QDBusPendingCallWatcher::finished, this, finish);
        if (pending->isFinished()) QTimer::singleShot(0, this, finish);
    }
    void checkLoaded(std::function<void(bool, bool)> completion)
    {
        call(QStringLiteral("/Effects"), QStringLiteral("org.kde.kwin.Effects"), QStringLiteral("isEffectLoaded"), {effectId},
            [completion](const QDBusMessage &message) {
            const QDBusReply<bool> reply(message);
            completion(reply.isValid(), reply.isValid() && reply.value());
        });
    }
    void finishProbe(bool compatible, bool loaded, const QString &reason)
    {
        m_compatible = compatible; m_loaded = loaded;
        emit probeFinished(compatible, loaded, reason);
    }
    QDBusConnection m_bus;
    quint64 m_generation = 0;
    bool m_compatible = false, m_loaded = false;
};
}

QByteArray supportedAeroEffectSha256()
{
    // Installed binary equals the local build from aeroshell-kwin-components
    // commit 97cc934b54aa9b237a8b814cb86f93f7d33b5ebf (KWin 6.6.6).
    // The plugin publishes no schema version; fail closed for any other build.
    return QByteArrayLiteral("4ed00683b05ee02593c1b99da4f1aa864676ece792e7f56ab92cf83f1fd34d4e");
}

AeroEffectPluginIdentity inspectAeroEffectPlugin(const QString &absolutePath)
{
    AeroEffectPluginIdentity identity;
    QFile file(absolutePath);
    if (!QFileInfo(absolutePath).isAbsolute() || !file.open(QIODevice::ReadOnly)
        || file.size() > 32 * 1024 * 1024) {
        identity.error = QStringLiteral("The installed Aero Glass Blur plugin could not be read.");
        return identity;
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file)) {
        identity.error = QStringLiteral("The installed Aero Glass Blur plugin could not be identified.");
        return identity;
    }
    identity.sha256 = hash.result().toHex();
    QPluginLoader plugin(absolutePath);
    identity.metadata = plugin.metaData(); // Does not load executable plugin code.
    return identity;
}

bool validateAeroEffectPlugin(const AeroEffectPluginIdentity &identity, QString &reason)
{
    reason.clear();
    if (!identity.error.isEmpty()) { reason = identity.error; return false; }
    const auto data = identity.metadata.value(QStringLiteral("MetaData")).toObject();
    const auto plugin = data.value(QStringLiteral("KPlugin")).toObject();
    if (identity.sha256 != supportedAeroEffectSha256()
        || identity.metadata.value(QStringLiteral("IID")).toString() != QLatin1String("org.kde.kwin.EffectPluginFactory6.6.6")
        || plugin.value(QStringLiteral("Name")).toString() != QLatin1String("Aero Glass Blur")
        || data.value(QStringLiteral("X-KDE-ConfigModule")).toString() != QLatin1String("kwin_aeroglassblur_config")) {
        reason = QStringLiteral("This Aero Glass Blur build has no verified settings schema. Apply requires the supported installed build for KWin 6.6.6.");
        return false;
    }
    return true;
}

bool isSupportedAeroKWinReport(const QString &report)
{
    if (report.size() > 65536) return false;
    const auto lines = report.split(QLatin1Char('\n'));
    return lines.count(QStringLiteral("KWin version: 6.6.6")) == 1
        && lines.count(QStringLiteral("Operation Mode: Wayland")) == 1;
}

AeroEffectRuntime *createAeroEffectRuntime(const QDBusConnection &connection)
{
    return new KWinAeroRuntime(connection);
}

KdeAeroEffectBackend::KdeAeroEffectBackend(QObject *parent)
    : KdeAeroEffectBackend(QStringLiteral("kwinrc"), createAeroEffectRuntime(QDBusConnection::sessionBus()), {}, false, parent)
{
}

KdeAeroEffectBackend::KdeAeroEffectBackend(const QString &file, AeroEffectRuntime *runtime,
    const AeroEffectPluginIdentity &identity, QObject *parent)
    : KdeAeroEffectBackend(file, runtime, identity, true, parent)
{
    Q_ASSERT(QFileInfo(file).isAbsolute());
}

KdeAeroEffectBackend::KdeAeroEffectBackend(const QString &configName, AeroEffectRuntime *runtime,
    const AeroEffectPluginIdentity &identity, bool isolated, QObject *parent)
    : AeroEffectBackend(parent), m_configName(configName), m_runtime(runtime), m_identity(identity), m_isolated(isolated)
{
    Q_ASSERT(runtime); runtime->setParent(this);
    m_userFile = isolated ? configName : QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
        + QStringLiteral("/kwinrc");
    m_pluginPath = QLibraryInfo::path(QLibraryInfo::PluginsPath) + QStringLiteral("/kwin/effects/plugins/aeroglassblur.so");
    m_watchedFiles << m_userFile;
    if (!isolated) {
        for (const auto &path : QStandardPaths::standardLocations(QStandardPaths::GenericConfigLocation))
            m_watchedFiles << path + QStringLiteral("/kwinrc") << path + QStringLiteral("/kdeglobals");
        m_watchedFiles << m_pluginPath;
    }
    m_watchedFiles.removeDuplicates();
    auto changed = [this] { watchFiles(); m_debounce.start(); };
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, changed);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, changed);
    m_debounce.setSingleShot(true); m_debounce.setInterval(100);
    connect(&m_debounce, &QTimer::timeout, this, &AeroEffectBackend::externalChange);
    connect(runtime, &AeroEffectRuntime::externalChange, this, &AeroEffectBackend::externalChange);
    connect(runtime, &AeroEffectRuntime::probeFinished, this, &KdeAeroEffectBackend::onProbe);
    connect(runtime, &AeroEffectRuntime::reconfigureFinished, this, [this](bool success, const QString &error) {
        if (m_phase != Phase::Reconfigure) return;
        m_phase = Phase::Idle; emit writeFinished(success, error);
    });
    watchFiles();
}

void KdeAeroEffectBackend::watchFiles()
{
    for (const auto &path : m_watchedFiles) {
        if (QFileInfo::exists(path) && !m_watcher.files().contains(path)) m_watcher.addPath(path);
        QString directory = QFileInfo(path).absolutePath();
        while (!QDir(directory).exists() && directory != QDir::rootPath())
            directory = QFileInfo(directory).absolutePath();
        if (!m_watcher.directories().contains(directory)) m_watcher.addPath(directory);
    }
}

bool KdeAeroEffectBackend::readState(AeroEffectState &state, QString &error) const
{
    for (const auto &path : m_watchedFiles) {
        if (path == m_pluginPath || !QFileInfo::exists(path)) continue;
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) { error = tr("Aero effect configuration cannot be read: %1").arg(path); return false; }
    }
    KConfig config(m_configName, m_isolated ? KConfig::SimpleConfig : KConfig::FullConfig);
    KConfigGroup group(&config, groupName);
    bool valid = false;
    state.blurStrength = group.readEntry(keyName, QStringLiteral("3")).toInt(&valid);
    if (!valid) { error = tr("The saved Aero blur strength is not a valid integer. No preference was changed."); return false; }
    state.writable = !config.isImmutable() && !group.isImmutable() && !group.isEntryImmutable(keyName)
        && config.isConfigWritable(false) && m_isolated;
    QString pluginReason;
    state.pluginSupported = validateAeroEffectPlugin(m_isolated ? m_identity : inspectAeroEffectPlugin(m_pluginPath), pluginReason);
    state.runtimeSupported = m_runtimeSupported; state.effectLoaded = m_loaded;
    if (!m_isolated) state.unavailableReason = tr("Use Personalization to edit the validated Aero theme.");
    else if (!state.writable) state.unavailableReason = tr("Aero blur strength is locked by policy or its configuration is not writable.");
    else if (!state.pluginSupported) state.unavailableReason = pluginReason;
    else if (!state.runtimeSupported || !state.effectLoaded) state.unavailableReason = m_runtimeReason;
    return true;
}

void KdeAeroEffectBackend::readCurrent()
{
    if (m_phase != Phase::Idle) return;
    m_phase = Phase::Read; m_runtime->probe();
}

void KdeAeroEffectBackend::writeStrength(int expected, int requested)
{
    if (m_phase != Phase::Idle) return;
    // The shared transaction validates the complete approved theme contract
    // before changing its blur preference. This legacy adapter cannot do so.
    if (!m_isolated) {
        emit writeFinished(false, tr("Use Personalization to edit the validated Aero theme."));
        return;
    }
    if (!AeroEffectSettings::supportedStrength(requested)) {
        emit writeFinished(false, tr("Choose an Aero blur strength from 2 through 15.")); return;
    }
    m_expected = expected; m_requested = requested;
    m_phase = Phase::Prewrite; m_runtime->probe();
}

void KdeAeroEffectBackend::onProbe(bool compatible, bool loaded, const QString &reason)
{
    if (m_phase != Phase::Read && m_phase != Phase::Prewrite) return;
    m_runtimeSupported = compatible; m_loaded = loaded; m_runtimeReason = reason;
    AeroEffectState state; QString error;
    const bool read = readState(state, error);
    if (m_phase == Phase::Read) {
        m_phase = Phase::Idle; emit readFinished(read, state, error); return;
    }
    m_phase = Phase::Idle;
    if (!read || state.blurStrength != m_expected || !state.canApply()) {
        if (error.isEmpty()) error = state.blurStrength != m_expected
            ? tr("Aero blur strength changed before the write. Revert to reload it.") : state.unavailableReason;
        emit writeFinished(false, error); return;
    }
    KConfig config(m_configName, m_isolated ? KConfig::SimpleConfig : KConfig::FullConfig);
    KConfigGroup group(&config, groupName);
    bool valid = false;
    const int current = group.readEntry(keyName, QStringLiteral("3")).toInt(&valid);
    if (!valid || current != m_expected || config.isImmutable() || group.isImmutable()
        || group.isEntryImmutable(keyName) || !config.isConfigWritable(false)) {
        emit writeFinished(false, tr("Aero blur strength changed or was locked immediately before saving. Revert to reload it.")); return;
    }
    group.writeEntry(keyName, m_requested);
    if (!config.sync()) {
        config.markAsClean(); emit writeFinished(false, tr("Aero blur strength could not be saved.")); return;
    }
    watchFiles(); m_phase = Phase::Reconfigure; m_runtime->reconfigure();
}
