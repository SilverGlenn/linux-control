#include "AeroThemeBackend.h"
#include "AeroEffectBackend.h"
#include "settings/AeroThemeContract.h"

#include <KConfig>
#include <KConfigGroup>
#include <KIconLoader>
#include <KIconTheme>
#include <KSharedDataCache>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusReply>
#include <QDBusServiceWatcher>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QStandardPaths>
#include <QtConcurrentRun>
#include <algorithm>

namespace {
class InstalledAssets final : public AeroThemeAssetProbe {
public:
    bool validate(QStringList &issues) const override { return AeroThemeContract::validateInstalledAssets(issues); }
};
class KdeAeroRuntime final : public AeroThemeRuntime {
public:
    KdeAeroRuntime() : m_effect(createAeroEffectRuntime(QDBusConnection::sessionBus()))
    {
        m_effect->setParent(this);
        connect(m_effect, &AeroEffectRuntime::probeFinished, this, [this](bool compatible, bool loaded, const QString &reason) {
            const auto bus = QDBusConnection::sessionBus();
            auto message = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.DBus"),
                QStringLiteral("/org/freedesktop/DBus"), QStringLiteral("org.freedesktop.DBus"), QStringLiteral("NameHasOwner"));
            message.setArguments({QStringLiteral("org.kde.plasmashell")});
            auto *pending = new QDBusPendingCallWatcher(bus.asyncCall(message, 1500), this);
            auto completed = std::make_shared<bool>(false);
            auto done = [this, pending, completed, compatible, loaded, reason] {
                if (*completed) return;
                *completed = true;
                const QDBusReply<bool> registered(pending->reply()); pending->deleteLater();
                m_supported = compatible && loaded && registered.isValid() && registered.value();
                emit probeFinished(m_supported, m_supported ? QString() : reason.isEmpty()
                    ? tr("A running Plasma shell and supported loaded Aero effect are required.") : reason);
            };
            connect(pending, &QDBusPendingCallWatcher::finished, this, done);
            if (pending->isFinished()) QTimer::singleShot(0, this, done);
        });
        connect(m_effect, &AeroEffectRuntime::reconfigureFinished, this, &AeroThemeRuntime::reloadFinished);
        connect(m_effect, &AeroEffectRuntime::externalChange, this, [this] { m_supported = false; emit externalChange(); });
        auto *watcher = new QDBusServiceWatcher(QStringLiteral("org.kde.plasmashell"), QDBusConnection::sessionBus(),
            QDBusServiceWatcher::WatchForOwnerChange, this);
        connect(watcher, &QDBusServiceWatcher::serviceOwnerChanged, this, [this] { m_supported = false; emit externalChange(); });
    }
    void probe() override { m_effect->probe(); }
    void reload(const QSet<QString> &changed) override
    {
        if (!m_supported) { emit reloadFinished(false, tr("Aero settings were saved, but their runtime became unavailable. Revert to inspect saved values.")); return; }
        const auto bus = QDBusConnection::sessionBus();
        bool dispatched = true;
        auto notify = [&bus, &dispatched](int type) {
            auto message = QDBusMessage::createSignal(QStringLiteral("/KGlobalSettings"),
                QStringLiteral("org.kde.KGlobalSettings"), QStringLiteral("notifyChange"));
            message.setArguments({type, 0}); dispatched = bus.send(message) && dispatched;
        };
        if (changed.contains(QStringLiteral("palette")) || changed.contains(QStringLiteral("colorScheme"))) notify(0);
        if (changed.contains(QStringLiteral("applicationStyle")) || changed.contains(QStringLiteral("kvantumTheme"))) notify(2);
        if (changed.contains(QStringLiteral("cursorTheme")) || changed.contains(QStringLiteral("cursorSize"))) notify(5);
        if (changed.contains(QStringLiteral("iconTheme"))) {
            KIconTheme::reconfigure();
            KSharedDataCache::deleteCache(QStringLiteral("icon-cache"));
            for (int i = 0; i < KIconLoader::LastGroup; ++i) KIconLoader::emitChange(KIconLoader::Group(i));
        }
        // Native SMOD decoration reload, unlike look-and-feel package application.
        const auto message = QDBusMessage::createMethodCall(QStringLiteral("org.kde.KWin"),
            QStringLiteral("/KWin"), QStringLiteral("org.kde.KWin"), QStringLiteral("reconfigure"));
        dispatched = bus.send(message) && dispatched;
        if (!dispatched) { emit reloadFinished(false, tr("Aero settings were saved, but a KDE notification could not be dispatched. Revert before retrying.")); return; }
        m_effect->reconfigure(); // bounded reply + loaded-effect check
    }
private:
    AeroEffectRuntime *m_effect;
    bool m_supported = false;
};
struct Entry { QString file, group, key; QVariant value; QString field; };
QList<Entry> entriesFor(const QVariantMap &values)
{
    QList<Entry> result;
    for (const auto &field : AeroThemeContract::fields()) {
        if (field.key != QLatin1String("palette")) {
            result.append({field.file, field.group, field.entry, values.value(field.key), field.key});
            continue;
        }
        const auto palette = values.value(field.key).toMap();
        for (auto group = palette.cbegin(); group != palette.cend(); ++group) {
            const auto map = group.value().toMap();
            for (auto item = map.cbegin(); item != map.cend(); ++item)
                result.append({field.file, group.key(), item.key(), item.value(), field.key});
        }
    }
    return result;
}
QVariant readValue(const KConfigGroup &group, const QString &key, const QVariant &type)
{
    // Identity defaults describe the approved target, never pretend a missing
    // identity marker means Aero is already selected.
    if (type.metaType().id() == QMetaType::Bool) {
        const auto raw = group.readEntry(key, type.toBool() ? QStringLiteral("true") : QStringLiteral("false"));
        if (raw == QLatin1String("true")) return true;
        if (raw == QLatin1String("false")) return false;
        return raw;
    }
    if (type.metaType().id() == QMetaType::Int) {
        bool ok; const auto raw = group.readEntry(key, QString::number(type.toInt())); const int value = raw.toInt(&ok);
        return ok ? QVariant(value) : QVariant(raw);
    }
    // SMOD's schema defaults to the Aero resource even without an explicit
    // entry. Other component identities have different native defaults.
    const bool nativeStringDefault = key == QLatin1String("CustomColor")
        || (group.name() == QLatin1String("Windeco") && key == QLatin1String("DecorationTheme"));
    return group.readEntry(key, nativeStringDefault ? type.toString() : QString());
}
bool writable(KConfig &config, const KConfigGroup &group, const QString &key)
{
    return !config.isImmutable() && !group.isImmutable() && !group.isEntryImmutable(key) && config.isConfigWritable(false);
}
}

KdeAeroThemeBackend::KdeAeroThemeBackend(QObject *parent)
    : KdeAeroThemeBackend(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation),
                         new KdeAeroRuntime, std::make_unique<InstalledAssets>(), false, parent) {}
KdeAeroThemeBackend::KdeAeroThemeBackend(const QString &directory, AeroThemeRuntime *runtime,
    std::unique_ptr<AeroThemeAssetProbe> assets, QObject *parent)
    : KdeAeroThemeBackend(directory, runtime, std::move(assets), true, parent) { Q_ASSERT(QFileInfo(directory).isAbsolute()); }
KdeAeroThemeBackend::KdeAeroThemeBackend(const QString &directory, AeroThemeRuntime *runtime,
    std::unique_ptr<AeroThemeAssetProbe> assets, bool isolated, QObject *parent)
    : AeroThemeBackend(parent), m_directory(directory), m_runtime(runtime), m_assets(std::move(assets)), m_isolated(isolated)
{
    Q_ASSERT(m_runtime && m_assets); m_runtime->setParent(this);
    for (const auto &field : AeroThemeContract::fields()) {
        m_watchedFiles << m_directory + QLatin1Char('/') + field.file;
        if (!isolated) for (const auto &path : QStandardPaths::standardLocations(QStandardPaths::GenericConfigLocation))
            m_watchedFiles << path + QLatin1Char('/') + field.file;
    }
    if (!isolated) m_watchedFiles += AeroThemeContract::installedAssetPaths();
    m_watchedFiles.removeDuplicates();
    auto changed = [this] { watchFiles(); m_debounce.start(); };
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, changed);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, changed);
    m_debounce.setSingleShot(true); m_debounce.setInterval(100);
    connect(&m_debounce, &QTimer::timeout, this, &AeroThemeBackend::externalChange);
    connect(runtime, &AeroThemeRuntime::externalChange, this, &AeroThemeBackend::externalChange);
    connect(runtime, &AeroThemeRuntime::probeFinished, this, [this](bool supported, const QString &reason) {
        if (m_phase != Phase::Read && m_phase != Phase::Preflight) return;
        m_runtimeSupported = supported; m_runtimeReason = reason; m_runtimeDone = true; finishProbe();
    });
    connect(runtime, &AeroThemeRuntime::reloadFinished, this, [this](bool success, const QString &error) {
        if (m_phase != Phase::Reload) return;
        m_timeout.stop();
        AeroThemeState state; QString readError;
        const bool saved = readState(state, readError) && state.values == m_requested;
        m_phase = Phase::Idle;
        emit writeFinished(success && saved, !saved ? tr("Aero settings were saved incompletely or changed during reload. Revert to inspect saved values. %1").arg(readError)
            : success ? QString() : error.isEmpty() ? tr("Aero settings were saved, but runtime reload failed. Revert before retrying.") : error);
    });
    m_timeout.setSingleShot(true); m_timeout.setInterval(12000);
    connect(&m_timeout, &QTimer::timeout, this, [this] {
        const bool read = m_phase == Phase::Read;
        ++m_generation; m_timedOut = true; m_runtimeSupported = false;
        m_runtimeReason = tr("Runtime checks timed out; restart the application before applying again.");
        m_phase = Phase::Idle;
        const auto message = tr("Aero runtime checks timed out. Saved settings may require Revert before retrying.");
        if (read) emit readFinished(false, {}, message); else emit writeFinished(false, message);
    });
    watchFiles();
}
QString KdeAeroThemeBackend::configName(const QString &file) const
{ return m_isolated ? m_directory + QLatin1Char('/') + file : file; }
void KdeAeroThemeBackend::watchFiles()
{
    for (const auto &path : m_watchedFiles) {
        if (QFileInfo::exists(path) && !QFileInfo(path).isDir() && !m_watcher.files().contains(path)) m_watcher.addPath(path);
        QString directory = QFileInfo(path).isDir() ? path : QFileInfo(path).absolutePath();
        while (!QDir(directory).exists() && directory != QDir::rootPath()) directory = QFileInfo(directory).absolutePath();
        if (!m_watcher.directories().contains(directory)) m_watcher.addPath(directory);
    }
}
void KdeAeroThemeBackend::readCurrent()
{
    if (m_phase != Phase::Idle) return;
    if (m_timedOut) {
        AeroThemeState state; QString error; const bool read = readState(state, error);
        emit readFinished(read, state, error); return;
    }
    m_phase = Phase::Read; startProbe();
}
void KdeAeroThemeBackend::writeSettings(const QVariantMap &expected, const QVariantMap &requested)
{
    if (m_phase != Phase::Idle) return;
    if (m_timedOut) { emit writeFinished(false, tr("Runtime checks timed out; restart the application before applying again.")); return; }
    QString reason;
    if (expected.keys() != AeroThemeContract::defaults().keys() || !AeroThemeContract::validateRequested(requested, reason)) {
        emit writeFinished(false, reason.isEmpty() ? tr("A complete Aero baseline is required before applying.") : reason); return;
    }
    m_expected = expected; m_requested = requested;
    m_phase = Phase::Preflight; startProbe();
}
void KdeAeroThemeBackend::startProbe()
{
    const quint64 generation = ++m_generation;
    m_assetsDone = m_runtimeDone = false;
    m_timeout.start();
    if (m_isolated) {
        m_assetsSupported = m_assets->validate(m_assetIssues); m_assetsDone = true;
    } else {
        auto *future = new QFutureWatcher<QPair<bool, QStringList>>(this);
        connect(future, &QFutureWatcherBase::finished, this, [this, future, generation] {
            const auto result = future->result(); future->deleteLater();
            if (generation != m_generation || (m_phase != Phase::Read && m_phase != Phase::Preflight)) return;
            m_assetsSupported = result.first; m_assetIssues = result.second; m_assetsDone = true; finishProbe();
        });
        // No QObject accesses on this worker and no arbitrary paths or IDs.
        future->setFuture(QtConcurrent::run([] {
            QStringList issues; const bool supported = AeroThemeContract::validateInstalledAssets(issues);
            return qMakePair(supported, issues);
        }));
    }
    m_runtime->probe();
}
void KdeAeroThemeBackend::finishProbe()
{
    if (!m_assetsDone || !m_runtimeDone) return;
    AeroThemeState state; QString error;
    const bool read = readState(state, error);
    if (m_phase == Phase::Read) {
        m_timeout.stop(); m_phase = Phase::Idle; emit readFinished(read, state, error); return;
    }
    if (m_phase != Phase::Preflight) return;
    if (!read || state.values != m_expected || !state.canApply()) {
        m_timeout.stop(); m_phase = Phase::Idle;
        emit writeFinished(false, !error.isEmpty() ? error : state.values != m_expected
            ? tr("Aero settings changed before the write. Revert to reload the complete baseline.")
            : state.issues.join(QLatin1Char('\n'))); return;
    }
    if (!save(error)) {
        m_timeout.stop(); m_phase = Phase::Idle; emit writeFinished(false, error); return;
    }
    QSet<QString> changed;
    for (auto it = m_requested.cbegin(); it != m_requested.cend(); ++it)
        if (it.value() != m_expected.value(it.key())) changed.insert(it.key());
    m_phase = Phase::Reload; m_timeout.start(); m_runtime->reload(changed);
}

bool KdeAeroThemeBackend::readState(AeroThemeState &state, QString &error) const
{
    for (const auto &path : m_watchedFiles) {
        if (!path.startsWith(m_directory + QLatin1Char('/')) || !QFileInfo::exists(path) || QFileInfo(path).isDir()) continue;
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) { error = tr("Aero configuration cannot be read: %1").arg(path); return false; }
    }
    const auto target = AeroThemeContract::defaults();
    for (const auto &field : AeroThemeContract::fields()) {
        KConfig config(configName(field.file), m_isolated ? KConfig::SimpleConfig : KConfig::FullConfig);
        bool canWrite = true;
        if (field.key == QLatin1String("palette")) {
            QVariantMap palette;
            const auto definitions = field.defaultValue.toMap();
            for (auto definition = definitions.cbegin(); definition != definitions.cend(); ++definition) {
                KConfigGroup group(&config, definition.key()); QVariantMap values;
                const auto entries = definition.value().toMap();
                for (auto item = entries.cbegin(); item != entries.cend(); ++item) {
                    values.insert(item.key(), readValue(group, item.key(), item.value()));
                    canWrite = writable(config, group, item.key()) && canWrite;
                }
                palette.insert(definition.key(), values);
            }
            // Capture all color/effect entries so another theme's extra groups
            // cannot remain active after restoring the approved Aero palette.
            for (const auto &name : config.groupList()) {
                if (!name.startsWith(QLatin1String("Colors:")) && !name.startsWith(QLatin1String("ColorEffects:"))) continue;
                const KConfigGroup group(&config, name);
                if (group.keyList().isEmpty() && !definitions.contains(name)) continue;
                auto values = palette.value(name).toMap();
                for (const auto &key : group.keyList()) {
                    values.insert(key, group.readEntry(key, QString()));
                    canWrite = writable(config, group, key) && canWrite;
                }
                palette.insert(name, values);
            }
            state.values.insert(field.key, palette);
        } else {
            const KConfigGroup group(&config, field.group);
            state.values.insert(field.key, readValue(group, field.entry, target.value(field.key)));
            canWrite = writable(config, group, field.entry);
        }
        if (canWrite) state.writableKeys.insert(field.key);
        else state.issues << tr("Aero setting is locked or its configuration is not writable: %1").arg(field.label);
    }
    state.assetsSupported = m_assetsSupported; state.runtimeSupported = m_runtimeSupported;
    state.issues += m_assetIssues;
    if (!m_runtimeSupported) state.issues << (m_runtimeReason.isEmpty() ? tr("The Aero runtime is unavailable.") : m_runtimeReason);
    return true;
}

bool KdeAeroThemeBackend::save(QString &error)
{
    // Validate all keys, policy and actual saved values before the first write.
    // Each subsequent entry gets a fresh read + policy check immediately before
    // saving; an unavoidable mid-transaction failure reports partial save.
    auto expected = entriesFor(m_expected);
    auto requested = entriesFor(m_requested);
    // Union requested + baseline palette entries. Invalid QVariant is a
    // deletion sentinel, reachable only for extra Colors:/ColorEffects: keys.
    for (const auto &before : expected) {
        const auto found = std::find_if(requested.cbegin(), requested.cend(), [&](const Entry &entry) {
            return before.file == entry.file && before.group == entry.group && before.key == entry.key;
        });
        if (found != requested.cend()) continue;
        if (before.field != QLatin1String("palette") || (!before.group.startsWith(QLatin1String("Colors:"))
            && !before.group.startsWith(QLatin1String("ColorEffects:")))) {
            error = tr("The saved Aero baseline contains an unsupported entry."); return false;
        }
        requested.append({before.file, before.group, before.key, {}, before.field});
    }
    auto order = [](const Entry &a, const Entry &b) {
        if (a.file != b.file) return a.file < b.file;
        if (a.group != b.group) return a.group < b.group;
        return a.key < b.key;
    };
    std::sort(expected.begin(), expected.end(), order);
    std::sort(requested.begin(), requested.end(), order);
    if (expected.size() != requested.size()) {
        error = tr("The complete saved Aero palette baseline is required."); return false;
    }
    for (int i = 0; i < requested.size(); ++i) {
        const auto &before = expected.at(i); const auto &after = requested.at(i);
        if (before.file != after.file || before.group != after.group || before.key != after.key) {
            error = tr("The saved Aero baseline has an invalid palette shape."); return false;
        }
        KConfig config(configName(after.file), m_isolated ? KConfig::SimpleConfig : KConfig::FullConfig);
        KConfigGroup group(&config, after.group);
        if (!writable(config, group, after.key) || readValue(group, after.key, after.value) != before.value) {
            error = tr("Aero configuration changed or became locked before saving. Revert to reload it."); return false;
        }
    }
    bool savedAny = false;
    for (int i = 0; i < requested.size(); ++i) {
        const auto &before = expected.at(i); const auto &after = requested.at(i);
        if (before.value == after.value) continue;
        KConfig config(configName(after.file), m_isolated ? KConfig::SimpleConfig : KConfig::FullConfig);
        KConfigGroup group(&config, after.group);
        if (!writable(config, group, after.key) || readValue(group, after.key, after.value) != before.value) {
            error = savedAny ? tr("Some Aero settings were saved before another setting changed or became locked. No rollback was attempted; Revert to inspect saved values.")
                : tr("Aero configuration changed or became locked before saving. Revert to reload it.");
            return false;
        }
        const auto flags = m_isolated ? KConfig::Normal : KConfig::Notify;
        if (after.value.isValid()) group.writeEntry(after.key, after.value, flags);
        else group.deleteEntry(after.key, flags);
        if (!config.sync()) {
            config.markAsClean(); // Never retry a failed write from the destructor.
            error = tr("Aero settings may have been saved partially because configuration synchronization failed. No rollback was attempted; Revert to inspect saved values."); return false;
        }
        savedAny = true;
        KConfig verify(configName(after.file), m_isolated ? KConfig::SimpleConfig : KConfig::FullConfig);
        const KConfigGroup saved(&verify, after.group);
        if (after.value.isValid() ? readValue(saved, after.key, after.value) != after.value : saved.hasKey(after.key)) {
            error = tr("Aero settings were saved partially, but a saved value could not be verified. No rollback was attempted; Revert to inspect saved values."); return false;
        }
    }
    AeroThemeState saved; QString readError;
    if (!readState(saved, readError) || saved.values != m_requested) {
        error = tr("Aero settings were saved partially or changed during saving. No rollback was attempted; Revert to inspect saved values. %1").arg(readError); return false;
    }
    return true;
}
