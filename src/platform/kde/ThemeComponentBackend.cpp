#include "ThemeComponentBackend.h"

#include <KConfig>
#include <KConfigGroup>
#include <KIconLoader>
#include <KIconTheme>
#include <KSharedDataCache>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLibrary>
#include <QSet>
#include <QStandardPaths>
#include <QStyleFactory>
#include <algorithm>

namespace {
constexpr const char *groups[] = {"Icons", "Mouse", "KDE"};
constexpr const char *keys[] = {"Theme", "cursorTheme", "widgetStyle"};
constexpr const char *defaults[] = {"breeze", "default", "Breeze"};
constexpr const char *files[] = {"kdeglobals", "kcminputrc", "kdeglobals"};

class KdeComponentReload final : public ThemeComponentReloadService {
public:
    bool isAvailable(ThemeComponent) const override
    {
        return QDBusConnection::sessionBus().isConnected()
            && (qEnvironmentVariable("KDE_SESSION_VERSION") == QLatin1String("6")
                || qEnvironmentVariable("XDG_CURRENT_DESKTOP").split(QLatin1Char(':')).contains(QStringLiteral("KDE")));
    }
    QString unavailableReason(ThemeComponent component) const override
    {
        return isAvailable(component) ? QString()
            : tr("A connected Plasma 6 session is required to request KDE theme reloads.");
    }
    bool requestReload(ThemeComponent component, QString &error) override
    {
        if (!isAvailable(component)) { error = unavailableReason(component); return false; }
        if (component == ThemeComponent::Icons) {
            // Plasma Workspace v6.6.6 kcms/icons/iconssettings.cpp uses these
            // public Frameworks APIs after saving Icons/Theme.
            KIconTheme::reconfigure();
            KSharedDataCache::deleteCache(QStringLiteral("icon-cache"));
            for (int i = 0; i < KIconLoader::LastGroup; ++i)
                KIconLoader::emitChange(KIconLoader::Group(i));
            return true;
        }
        // Release-matched protocol: plasma-workspace v6.6.6
        // kcms/kcms-common_p.h + kcms-common.cpp. Dispatch is NOT an ack.
        // This does not duplicate the KCM's X11 resource export (runRdb),
        // hence the explicit sign-out caveat in the page and apply result.
        auto message = QDBusMessage::createSignal(QStringLiteral("/KGlobalSettings"),
            QStringLiteral("org.kde.KGlobalSettings"), QStringLiteral("notifyChange"));
        message.setArguments({component == ThemeComponent::Cursors ? 5 : 2, 0});
        if (!QDBusConnection::sessionBus().send(message)) {
            error = tr("The preference was saved, but KDE's reload notification could not be sent. Revert to inspect the saved value.");
            return false;
        }
        return true;
    }
};

void sortChoices(QList<ThemeComponentChoice> &choices)
{
    std::sort(choices.begin(), choices.end(), [](const auto &a, const auto &b) {
        return QString::localeAwareCompare(a.name, b.name) < 0;
    });
}
}

KdeThemeComponentBackend::KdeThemeComponentBackend(QObject *parent)
    : KdeThemeComponentBackend(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation),
                              new KdeComponentReload, {}, false, parent)
{
}

KdeThemeComponentBackend::KdeThemeComponentBackend(const QString &directory,
    ThemeComponentReloadService *reload, const ThemeComponentChoices &choices, QObject *parent)
    : KdeThemeComponentBackend(directory, reload, choices, true, parent)
{
    Q_ASSERT(QFileInfo(directory).isAbsolute());
}

KdeThemeComponentBackend::KdeThemeComponentBackend(const QString &directory,
    ThemeComponentReloadService *reload, const ThemeComponentChoices &choices, bool isolated, QObject *parent)
    : ThemeComponentBackend(parent), m_directory(directory), m_reload(reload),
      m_choices(choices), m_isolated(isolated)
{
    Q_ASSERT(reload);
    reload->setParent(this);
    for (const char *file : {"kdeglobals", "kcminputrc"}) {
        m_watchedFiles << m_directory + QLatin1Char('/') + QLatin1String(file);
        if (!isolated)
            for (const auto &path : QStandardPaths::standardLocations(QStandardPaths::GenericConfigLocation))
                m_watchedFiles << path + QLatin1Char('/') + QLatin1String(file);
    }
    m_watchedFiles.removeDuplicates();
    auto changed = [this](const QString &) { watchFiles(); m_debounce.start(); };
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, changed);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, changed);
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(100);
    connect(&m_debounce, &QTimer::timeout, this, [this] { emit externalChange(); });
    connect(reload, &ThemeComponentReloadService::availabilityChanged, this, &ThemeComponentBackend::externalChange);
    watchFiles();
}

QString KdeThemeComponentBackend::configName(int component) const
{
    return m_isolated ? m_directory + QLatin1Char('/') + QLatin1String(files[component])
                      : QLatin1String(files[component]);
}

ThemeComponentChoices KdeThemeComponentBackend::installedChoices() const
{
    if (m_isolated)
        return m_choices;
    ThemeComponentChoices choices;
    for (const auto &id : KIconTheme::list()) {
        KIconTheme theme(id);
        if (theme.isValid() && !theme.isHidden())
            choices[0].append({id, theme.name().isEmpty() ? id : theme.name()});
    }
    // Use the same authoritative Xcursor search path as v6.6.6's cursor KCM.
    // Resolve its read-only public function without introducing an X11 header
    // dependency in this Wayland-capable application. No guessed search order.
    QLibrary xcursor(QStringLiteral("Xcursor"), 1);
    using LibraryPath = const char *(*)();
    const auto libraryPath = reinterpret_cast<LibraryPath>(xcursor.resolve("XcursorLibraryPath"));
    const QStringList cursorPaths = libraryPath
        ? QString::fromLocal8Bit(libraryPath()).split(QLatin1Char(':'), Qt::SkipEmptyParts) : QStringList();
    QSet<QString> seen;
    for (QString path : cursorPaths) {
        if (path.startsWith(QStringLiteral("~/")))
            path.replace(0, 1, QDir::homePath());
        const QDir root(path);
        for (const auto &id : root.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            if (seen.contains(id))
                continue;
            // Mark the first occurrence even when invalid: lower-priority
            // duplicates must not claim to describe Xcursor's chosen directory.
            seen.insert(id);
            const QDir theme(root.filePath(id));
            if (!theme.exists(QStringLiteral("cursors")) || !theme.exists(QStringLiteral("index.theme")))
                continue;
            KConfig index(theme.filePath(QStringLiteral("index.theme")), KConfig::SimpleConfig);
            const KConfigGroup group(&index, "Icon Theme");
            if (!group.readEntry("Hidden", false))
                choices[1].append({id, group.readEntry("Name", id)});
        }
    }
    for (const auto &style : QStyleFactory::keys())
        choices[2].append({style, style});
    for (auto &list : choices)
        sortChoices(list);
    return choices;
}

void KdeThemeComponentBackend::watchFiles()
{
    for (const auto &path : m_watchedFiles) {
        if (QFileInfo::exists(path) && !m_watcher.files().contains(path))
            m_watcher.addPath(path);
        QString directory = QFileInfo(path).absolutePath();
        while (!QDir(directory).exists() && directory != QDir::rootPath())
            directory = QFileInfo(directory).absolutePath();
        if (!m_watcher.directories().contains(directory))
            m_watcher.addPath(directory);
    }
}

bool KdeThemeComponentBackend::readState(ThemeComponentState &state, QString &error) const
{
    for (const auto &path : m_watchedFiles) {
        if (QFileInfo::exists(path)) {
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly)) {
                error = tr("Personalization configuration cannot be read: %1").arg(path);
                return false;
            }
        }
    }
    const auto choices = installedChoices();
    for (int i = 0; i < themeComponentCount; ++i) {
        KConfig config(configName(i), m_isolated ? KConfig::SimpleConfig : KConfig::FullConfig);
        KConfigGroup group(&config, groups[i]);
        auto &field = state.fields[i];
        field.value = group.readEntry(keys[i], QString::fromLatin1(defaults[i]));
        field.choices = choices[i];
        // Saved values are observable even when unsupported; only the exact
        // approved identity may be offered as a new selection.
        auto &approved = field.choices;
        approved.erase(std::remove_if(approved.begin(), approved.end(), [i](const auto &choice) {
            return !ThemeComponentSettings::isApprovedChoice(static_cast<ThemeComponent>(i), choice.id);
        }), approved.end());
        field.writable = !config.isImmutable() && !group.isImmutable() && !group.isEntryImmutable(keys[i])
            && config.isConfigWritable(false) && m_isolated;
        field.reloadAvailable = m_reload->isAvailable(static_cast<ThemeComponent>(i));
        if (!m_isolated)
            field.unavailableReason = tr("Use Personalization to apply the validated Aero theme.");
        else if (!field.writable)
            field.unavailableReason = tr("This component is locked by policy or its configuration is not writable.");
        else if (!field.reloadAvailable)
            field.unavailableReason = m_reload->unavailableReason(static_cast<ThemeComponent>(i));
        else if (field.choices.isEmpty()) {
            field.reloadAvailable = false;
            field.unavailableReason = tr("No installed choices could be found for this component.");
        }
    }
    return true;
}

void KdeThemeComponentBackend::readCurrent()
{
    ThemeComponentState state;
    QString error;
    const bool success = readState(state, error);
    emit readFinished(success, state, error);
}

void KdeThemeComponentBackend::writeComponents(const QStringList &expected, const QStringList &requested)
{
    // Installed names are insufficient evidence of approved asset versions.
    // Production writes now belong exclusively to the shared Aero transaction.
    if (!m_isolated) {
        emit writeFinished(false, tr("Use Personalization to apply the validated Aero theme."));
        return;
    }
    QString error;
    ThemeComponentState state;
    if (expected.size() != themeComponentCount || requested.size() != themeComponentCount) {
        emit writeFinished(false, tr("Invalid personalization selection."));
        return;
    }
    if (!readState(state, error) || state.values() != expected) {
        emit writeFinished(false, error.isEmpty() ? tr("Personalization changed before the write. Revert to reload it.") : error);
        return;
    }
    // Validate EVERY changed field before the first write, so a locked sibling
    // or a theme removed since staging cannot cause an avoidable partial save.
    for (int i = 0; i < themeComponentCount; ++i) {
        if (requested[i] == expected[i])
            continue;
        if (!ThemeComponentSettings::isApprovedChoice(static_cast<ThemeComponent>(i), requested[i])) {
            emit writeFinished(false, tr("Only approved Aero theme components can be selected."));
            return;
        }
        if (!state.fields[i].canApply() || !ThemeComponentSettings::containsChoice(state.fields[i], requested[i])) {
            emit writeFinished(false, state.fields[i].unavailableReason.isEmpty()
                ? tr("A selected component is no longer installed.") : state.fields[i].unavailableReason);
            return;
        }
    }
    QStringList baseline = expected;
    for (int i = 0; i < themeComponentCount; ++i) {
        if (requested[i] == expected[i])
            continue;
        if (!readState(state, error) || state.values() != baseline || !state.fields[i].canApply()
            || !ThemeComponentSettings::containsChoice(state.fields[i], requested[i])) {
            emit writeFinished(false, error.isEmpty() ? tr("A component changed or became unavailable during Apply. Some preferences may already be saved; Revert to reload them.") : error);
            return;
        }
        KConfig config(configName(i), m_isolated ? KConfig::SimpleConfig : KConfig::FullConfig);
        KConfigGroup group(&config, groups[i]);
        if (group.readEntry(keys[i], QString::fromLatin1(defaults[i])) != baseline[i]
            || config.isImmutable() || group.isImmutable() || group.isEntryImmutable(keys[i])
            || !config.isConfigWritable(false)) {
            emit writeFinished(false, tr("A personalization preference changed or was locked before saving. Revert to reload it."));
            return;
        }
        group.writeEntry(keys[i], requested[i]);
        if (!config.sync()) {
            config.markAsClean();
            emit writeFinished(false, tr("A personalization preference could not be saved. Some preferences may already be saved; Revert to reload them."));
            return;
        }
        baseline[i] = requested[i];
        watchFiles();
        if (!m_reload->requestReload(static_cast<ThemeComponent>(i), error)) {
            emit writeFinished(false, error.isEmpty() ? tr("A preference was saved, but its KDE reload notification failed. Revert to reload the saved values.") : error);
            return;
        }
    }
    emit writeFinished(true, QString());
}
