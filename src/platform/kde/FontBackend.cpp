#include "FontBackend.h"
#include <KConfig>
#include <KConfigGroup>
#include <KConfigGui>
#include <QCryptographicHash>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDir>
#include <QDomDocument>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QCoreApplication>
#include <QFutureWatcher>
#include <QDataStream>
#include <QtConcurrent/QtConcurrentRun>
#include <atomic>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include "operations/SettingsFileReservations.h"
#include "operations/OperationManager.h"
#include <fontconfig/fontconfig.h>
#include <memory>

namespace {
constexpr const char *fontKeys[] = {"font", "fixed", "smallestReadableFont", "toolBarFont", "menuFont", "activeFont"};
constexpr const char *renderKeys[] = {"XftAntialias", "XftHintStyle", "XftSubPixel"};
constexpr qint64 maximumConfigBytes = 1024 * 1024;
QStringList fontconfigFiles(bool *success = nullptr, QStringList *directories = nullptr)
{
    QStringList files;
    // Load current includes anew so an external edit to a parent/include is part
    // of preflight. This parses configuration; it does not build font caches.
    FcConfig *config = FcInitLoadConfig();
    if (success) *success = config != nullptr;
    if (!config) return files;
    FcStrList *list = FcConfigGetConfigFiles(config);
    if (list) {
        while (const FcChar8 *file = FcStrListNext(list))
            files << QString::fromLocal8Bit(reinterpret_cast<const char *>(file));
        FcStrListDone(list);
    }
    list = FcConfigGetConfigDirs(config);
    if (list) {
        while (const FcChar8 *file = FcStrListNext(list)) {
            const auto path = QString::fromLocal8Bit(reinterpret_cast<const char *>(file));
            files << path;
            if (directories) *directories << path;
        }
        FcStrListDone(list);
    }
    FcConfigDestroy(config);
    return files;
}
QString fontconfigPath(const QStringList &files)
{
    // KXftConfig::getConfigFile(), Plasma 6.6.6: prefer a loaded user
    // fonts.conf/.fonts.conf, then the first loaded user file, then XDG path.
    QStringList local;
    const QString home = QDir::homePath() + QLatin1Char('/');
    for (const auto &path : files)
        if (path.startsWith(home) && QFileInfo(path).isFile()) local << path;
    for (const auto &path : local)
        if (path.endsWith(QLatin1String("/fonts.conf")) || path.endsWith(QLatin1String("/.fonts.conf"))) return path;
    if (!local.isEmpty()) return local.first();
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/fontconfig/fonts.conf");
}
bool readableBytes(const QString &path, QByteArray &bytes, QString &error)
{
    bytes.clear();
    if (!QFileInfo::exists(path)) return true;
    const auto encoded = QFile::encodeName(path);
    const int descriptor = ::open(encoded.constData(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (descriptor < 0) { error = QObject::tr("Font settings cannot be read safely: %1").arg(path); return false; }
    QFile file(path);
    struct stat metadata{};
    if (!file.open(descriptor, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) {
        ::close(descriptor); error = QObject::tr("Font settings cannot be opened safely: %1").arg(path); return false;
    }
    if (::fstat(descriptor, &metadata) != 0 || !S_ISREG(metadata.st_mode) || metadata.st_size > maximumConfigBytes) {
        error = QObject::tr("Font settings cannot be read safely: %1").arg(path); return false;
    }
    bytes = file.read(maximumConfigBytes + 1);
    if (file.error() != QFileDevice::NoError || bytes.size() > maximumConfigBytes) {
        error = QObject::tr("Font settings could not be read: %1").arg(path); return false;
    }
    return true;
}
bool loadXml(const QString &path, QDomDocument &doc, QString &error)
{
    QByteArray bytes;
    if (!readableBytes(path, bytes, error)) return false;
    if (bytes.isEmpty() && !QFileInfo::exists(path)) {
        doc.appendChild(doc.createProcessingInstruction(QStringLiteral("xml"), QStringLiteral("version=\"1.0\"")));
        doc.appendChild(doc.createElement(QStringLiteral("fontconfig")));
        return true;
    }
    // No custom entities: expansion can make a small input unbounded, and the
    // backend only needs ordinary fontconfig XML, not a general XML processor.
    if (bytes.contains("<!ENTITY")) {
        error = QObject::tr("Custom XML entities in fontconfig are unsupported; no settings were changed."); return false;
    }
    auto parsed = doc.setContent(bytes, QDomDocument::ParseOption::PreserveSpacingOnlyNodes);
    if (!parsed || doc.documentElement().tagName() != QLatin1String("fontconfig")) {
        error = QObject::tr("The fontconfig file is invalid; no settings were changed: %1").arg(path); return false;
    }
    return true;
}
// Only unconditional single-edit font-target rules are editable. Pattern-target
// rendering uses a different fontconfig matching stage and is refused explicitly.
// Conditional rules,
// aliases, includes, exclusion ranges, comments and unrelated edits stay intact.
QString canonicalProperty(const QDomElement &match, QString *value = nullptr)
{
    if (match.tagName() != QLatin1String("match") || match.attributes().count() != 1
        || match.attribute(QStringLiteral("target")) != QLatin1String("font")) return {};
    auto edit = match.firstChildElement();
    if (edit.isNull() || !edit.nextSiblingElement().isNull() || edit.tagName() != QLatin1String("edit")
        || edit.attributes().count() != 2 || edit.attribute(QStringLiteral("mode")) != QLatin1String("assign")) return {};
    const QString name = edit.attribute(QStringLiteral("name"));
    if (name != QLatin1String("antialias") && name != QLatin1String("hintstyle")
        && name != QLatin1String("rgba") && name != QLatin1String("hinting")) return {};
    auto scalar = edit.firstChildElement();
    const QString tag = (name == QLatin1String("antialias") || name == QLatin1String("hinting"))
        ? QStringLiteral("bool") : QStringLiteral("const");
    if (scalar.tagName() != tag || !scalar.nextSiblingElement().isNull()
        || scalar.attributes().count() || !scalar.firstChildElement().isNull()) return {};
    if (value) *value = scalar.text().trimmed().toLower();
    return name;
}
bool renderingFromXml(const QDomDocument &doc, FontPreferences &p, QString &error)
{
    QSet<QString> properties;
    for (auto node = doc.documentElement().firstChildElement(); !node.isNull(); node = node.nextSiblingElement()) {
        QString value;
        const auto name = canonicalProperty(node, &value);
        if (!name.isEmpty()) {
            // Keeping earlier unconditional assignments while changing the last
            // one can alter intervening conditional rules; removing all of them
            // is not a safe representation of one staged override either.
            if (properties.contains(name)) {
                error = QObject::tr("This fontconfig file has duplicate unconditional rendering rules for %1. Use KDE System Settings to reconcile them before editing here.").arg(name);
                return false;
            }
            properties.insert(name);
        }
        if (node.tagName() == QLatin1String("match") && name.isEmpty()
            && node.firstChildElement(QStringLiteral("test")).isNull()) {
            for (auto edit = node.firstChildElement(QStringLiteral("edit")); !edit.isNull(); edit = edit.nextSiblingElement(QStringLiteral("edit"))) {
                const QString property = edit.attribute(QStringLiteral("name"));
                if (property == QLatin1String("antialias") || property == QLatin1String("rgba")
                    || property == QLatin1String("hintstyle") || property == QLatin1String("hinting")) {
                    error = node.attribute(QStringLiteral("target")) == QLatin1String("pattern")
                        ? QObject::tr("This fontconfig file has pattern-target rendering rules. Their matching stage cannot be represented safely by these font-target controls. Use KDE System Settings to reconcile them.")
                        : QObject::tr("This fontconfig file has unconditional rendering rules that cannot be edited safely here. Use KDE System Settings to reconcile them.");
                    return false;
                }
            }
        }
        if (name == QLatin1String("antialias")) p.antialias = value;
        if (name == QLatin1String("hintstyle")) p.hinting = value;
        if (name == QLatin1String("rgba")) p.subpixel = value;
        // Standalone hinting can disable a selected hint style. Refuse an
        // inconsistent combination rather than display a fabricated value.
    }
    QString hinting;
    bool hasHinting = false;
    for (auto node = doc.documentElement().firstChildElement(); !node.isNull(); node = node.nextSiblingElement()) {
        QString value;
        if (canonicalProperty(node, &value) == QLatin1String("hinting")) {
            hinting = value;
            hasHinting = true;
        }
    }
    if (hasHinting && ((hinting != QLatin1String("true") && hinting != QLatin1String("false"))
        || p.hinting.isEmpty() || ((hinting == QLatin1String("false")) != (p.hinting == QLatin1String("hintnone"))))) {
        error = QObject::tr("The fontconfig hinting rules do not match a supported KDE hint style. Use KDE System Settings to reconcile them.");
        return false;
    }
    if (!FontSettings::isSupported(p)) {
        error = QObject::tr("The saved fonts or rendering rules use unsupported values. No settings were changed."); return false;
    }
    return true;
}
void updateRule(QDomDocument &doc, const QString &name, const QString &value)
{
    QDomNode last;
    auto root = doc.documentElement();
    for (auto node = root.firstChildElement(); !node.isNull();) {
        const auto next = node.nextSiblingElement();
        if (canonicalProperty(node) == name) {
            if (value.isEmpty()) root.removeChild(node);
            else last = node;
        }
        node = next;
    }
    if (value.isEmpty()) return;
    auto match = doc.createElement(QStringLiteral("match"));
    match.setAttribute(QStringLiteral("target"), QStringLiteral("font"));
    auto edit = doc.createElement(QStringLiteral("edit"));
    edit.setAttribute(QStringLiteral("name"), name);
    edit.setAttribute(QStringLiteral("mode"), QStringLiteral("assign"));
    auto scalar = doc.createElement(name == QLatin1String("antialias") || name == QLatin1String("hinting")
        ? QStringLiteral("bool") : QStringLiteral("const"));
    scalar.appendChild(doc.createTextNode(value)); edit.appendChild(scalar); match.appendChild(edit);
    if (last.isNull()) {
        // Keep later conditional exclusions and per-application rules effective.
        // An explicit general choice must not silently override those rules.
        QDomNode before;
        for (auto node = root.firstChildElement(); !node.isNull(); node = node.nextSiblingElement()) {
            if (node.tagName() != QLatin1String("match") || node.firstChildElement(QStringLiteral("test")).isNull()) continue;
            for (auto child = node.firstChildElement(QStringLiteral("edit")); !child.isNull(); child = child.nextSiblingElement(QStringLiteral("edit")))
                if (child.attribute(QStringLiteral("name")) == name) { before = node; break; }
            if (!before.isNull()) break;
        }
        if (before.isNull()) root.appendChild(match); else root.insertBefore(match, before);
    } else root.replaceChild(match, last);
}
bool pathWritable(const QString &path)
{
    if (QFileInfo::exists(path)) return QFileInfo(path).isFile() && QFileInfo(path).isWritable();
    QString ancestor = QFileInfo(path).absolutePath();
    while (!QFileInfo::exists(ancestor) && ancestor != QDir::rootPath()) ancestor = QFileInfo(ancestor).absolutePath();
    return QFileInfo(ancestor).isDir() && QFileInfo(ancestor).isWritable();
}
class KdeFontReload final : public FontReloadService {
public:
    KdeFontReload(const QDBusConnection &bus, QObject *parent) : FontReloadService(parent), m_bus(bus) {}
    bool isAvailable() const override
    { return m_bus.isConnected() && QGuiApplication::platformName() == QLatin1String("wayland") && qEnvironmentVariableIsSet("KDE_FULL_SESSION"); }
    QString unavailableReason() const override
    { return tr("Font Apply requires a KDE Wayland session and its session bus. X11 resource export is not supported here."); }
    QString identity() const override { return m_bus.baseService(); }
    void requestReload() override
    {
        if (!isAvailable()) { emit finished(false, unavailableReason()); return; }
        const bool sent = m_bus.send(QDBusMessage::createSignal(QStringLiteral("/KDEPlatformTheme"),
            QStringLiteral("org.kde.KDEPlatformTheme"), QStringLiteral("refreshFonts")));
        emit finished(sent, sent ? QString() : tr("Font settings were saved, but KDE's refresh request could not be sent. Restart applications or sign out and back in."));
    }
private:
    QDBusConnection m_bus;
};
}

FontReloadService *createKdeFontReloadService(const QDBusConnection &bus, QObject *parent)
{ return new KdeFontReload(bus, parent); }

bool fontConfigurationRevision(const QStringList &paths, QByteArray &revision, QString &error)
{
    QByteArray bytes; QDataStream stream(&bytes, QIODevice::WriteOnly);
    for (const auto &path : paths) {
        stream << path;
        const auto encoded = QFile::encodeName(path);
        struct stat link{}, before{}, after{}, current{};
        if (::lstat(encoded.constData(), &link) != 0) {
            if (errno == ENOENT) { stream << false; continue; }
            error = QObject::tr("A font configuration source cannot be inspected: %1").arg(path); return false;
        }
        if (::stat(encoded.constData(), &before) != 0) {
            error = QObject::tr("A font configuration source cannot be resolved: %1").arg(path); return false;
        }
        stream << true << quint64(link.st_dev) << quint64(link.st_ino) << quint64(link.st_mode)
               << quint64(before.st_dev) << quint64(before.st_ino) << quint64(before.st_mode)
               << quint64(before.st_uid) << quint64(before.st_gid);
        if (S_ISDIR(before.st_mode)) {
            // Include directories are provenance dependencies, never reservation
            // targets. Their membership matters, but a directory is not XML.
            QDir directory(path);
            if (!directory.isReadable()) { error = QObject::tr("The fontconfig include directory cannot be read: %1").arg(path); return false; }
            stream << directory.entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot, QDir::Name);
            // Directory mtime is deliberately excluded: adding then removing a
            // member restores the same membership, while inode/policy stay guarded.
        } else {
            const int descriptor = ::open(encoded.constData(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            if (descriptor < 0) { error = QObject::tr("A font source cannot be read: %1").arg(path); return false; }
            QFile file;
            if (!file.open(descriptor, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) {
                ::close(descriptor); error = QObject::tr("A font source cannot be opened safely."); return false;
            }
            if (::fstat(descriptor, &before) != 0 || !S_ISREG(before.st_mode) || before.st_size > maximumConfigBytes) {
                error = QObject::tr("A font source is not a bounded regular file: %1").arg(path); return false;
            }
            const auto data = file.read(maximumConfigBytes + 1);
            if (file.error() != QFileDevice::NoError || data.size() > maximumConfigBytes
                || ::fstat(descriptor, &after) != 0 || ::stat(encoded.constData(), &current) != 0
                || current.st_dev != before.st_dev || current.st_ino != before.st_ino
                || after.st_size != before.st_size || after.st_mtim.tv_sec != before.st_mtim.tv_sec
                || after.st_mtim.tv_nsec != before.st_mtim.tv_nsec) {
                error = QObject::tr("A font source changed during inspection: %1").arg(path); return false;
            }
            stream << qint64(before.st_mtim.tv_sec) << qint64(before.st_mtim.tv_nsec) << data;
        }
    }
    revision = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256); return true;
}

namespace {
namespace Reservations = SettingsFileReservations;
struct FontFiles {
    QString name, userPath, xml;
    QStringList sources, targets, writes, watches, includeDirectories;
    bool isolated = false;
    KdeFontBackend::FileContext fixture;
    std::function<bool(KConfig &)> sync;
    bool simple() const { return isolated && !(fixture.fixture && fixture.fullCascade); }
};
struct FontRuntime { bool available = false; QString reason, identity; };
struct FontFileResult {
    bool success = false;
    FontState state;
    QString error;
    FontFiles files;
    QStringList keys;
    Reservations::Inspection inspection;
};
bool discoverFontFiles(FontFiles &files, QString &error)
{
    const auto userConfig = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    files.userPath = files.isolated ? files.name : userConfig + QStringLiteral("/kdeglobals");
    files.sources = {files.userPath};
    if (!files.isolated) {
        bool loaded = false;
        files.includeDirectories.clear();
        const auto discovered = fontconfigFiles(&loaded, &files.includeDirectories);
        if (!loaded) { error = QObject::tr("Fontconfig configuration discovery failed. No font settings were changed."); return false; }
        const auto active = fontconfigPath(discovered);
        if (!files.xml.isEmpty() && files.xml != active) {
            error = QObject::tr("KDE's active user fontconfig file changed. Reopen Control Panel before applying font settings."); return false;
        }
        files.xml = active; files.sources << discovered;
    }
    if (!QFileInfo(files.xml).isAbsolute() || (files.isolated && !QFileInfo(files.name).isAbsolute())) {
        error = QObject::tr("Font settings require absolute configuration paths."); return false;
    }
    files.sources << files.xml;
    files.writes = {files.userPath, files.xml};
    if (!files.simple()) {
        files.writes << userConfig + QStringLiteral("/kdeglobals");
        for (const auto &dir : QStandardPaths::standardLocations(QStandardPaths::GenericConfigLocation))
            files.sources << dir + QStringLiteral("/kdeglobals") << dir + QStringLiteral("/system.kdeglobals");
        if (!qEnvironmentVariableIsSet("KDE_SKIP_KDERC")) files.sources << QStringLiteral("/etc/kde5rc");
    }
    if (files.fixture.fixture) files.sources << files.fixture.additionalSources;
    files.sources.removeDuplicates(); files.sources.sort(); files.writes.removeDuplicates();
    files.targets = files.writes;
    for (const auto &path : files.sources)
        if (!files.includeDirectories.contains(path) && !QFileInfo(path).isDir()) files.targets << path;
    files.targets.removeDuplicates();
    files.watches.clear();
    for (const auto &path : files.sources) {
        if (QFileInfo::exists(path)) files.watches << path;
        auto parent = QFileInfo(path).absolutePath();
        while (!QFileInfo::exists(parent) && parent != QDir::rootPath()) parent = QFileInfo(parent).absolutePath();
        files.watches << parent;
    }
    files.watches.removeDuplicates();
    return true;
}
FontFileResult readFontFiles(FontFiles files, const FontRuntime &runtime)
{
    FontFileResult result;
    if (files.fixture.fixture && files.fixture.beforeRead) files.fixture.beforeRead();
    if (!discoverFontFiles(files, result.error)) return result;
    result.files = files;
    QByteArray before;
    if (!fontConfigurationRevision(files.sources, before, result.error)) return result;
    {
        KConfig config(files.name, files.simple() ? KConfig::SimpleConfig : KConfig::FullConfig);
        result.state.writable = !config.isImmutable() && config.isConfigWritable(false) && pathWritable(files.xml);
        for (int role = 0; role < 6; ++role) {
            KConfigGroup group(&config, role == 5 ? "WM" : "General");
            QFont fallback(role == 1 ? QStringLiteral("Hack") : QStringLiteral("Noto Sans"), role == 2 ? 8 : 10);
            fallback.setStyleName(QStringLiteral("Regular"));
            if (group.hasKey(fontKeys[role])) {
                QFont parsed;
                if (!parsed.fromString(group.readEntry(fontKeys[role], QString()))) {
                    result.error = QObject::tr("A saved KDE font is invalid: %1. No settings were changed.").arg(QString::fromLatin1(fontKeys[role])); return result;
                }
            }
            result.state.preferences.fonts[role] = FontSettings::canonicalFont(group.readEntry(fontKeys[role], fallback));
            result.state.writable &= !group.isImmutable() && !group.isEntryImmutable(fontKeys[role]);
        }
        KConfigGroup general(&config, "General");
        for (auto key : renderKeys) result.state.writable &= !general.isImmutable() && !general.isEntryImmutable(key);
        QDomDocument doc;
        if (!loadXml(files.xml, doc, result.error) || !renderingFromXml(doc, result.state.preferences, result.error)) return result;
        result.state.reloadAvailable = runtime.available;
        if (!result.state.writable) result.state.unavailableReason = QObject::tr("Font settings are locked by policy or their configuration files are not writable.");
        else if (!runtime.available) result.state.unavailableReason = runtime.reason;
    }
    if (!fontConfigurationRevision(files.sources, result.state.revision, result.error)) return result;
    result.success = before == result.state.revision;
    if (!result.success) result.error = QObject::tr("Font configuration sources changed during reading. Revert to retry.");
    return result;
}
FontFileResult saveFontFiles(const FontFiles &files, const FontRuntime &runtime,
    const FontState &expected, const FontPreferences &requested, const Reservations::Reservation &reservation,
    const QStringList &expectedKeys, const std::shared_ptr<std::atomic_bool> &invalidated,
    const std::function<bool(const QStringList &, QString &)> &retainAliases)
{
    struct MutationEnd { Reservations::Reservation value; ~MutationEnd() { Reservations::endMutation(value); } } mutation{reservation};
    QStringList retainedKeys = expectedKeys;
    auto work = [&]() -> FontFileResult {
        auto result = readFontFiles(files, runtime);
        if (!result.success) return result;
        result.success = false;
        const auto current = result.state;
        if (!current.canApply() || current.preferences != expected.preferences || current.revision != expected.revision
            || Reservations::currentKeys(reservation.paths, &result.error) != expectedKeys || invalidated->load()) {
            if (result.error.isEmpty()) result.error = QObject::tr("Font settings, policy, source identity or provider changed before the write. Revert to reload; no settings were changed.");
            return result;
        }
        QDomDocument doc;
        if (!loadXml(files.xml, doc, result.error)) return result;
        auto dependencies = files.sources;
        for (const auto &target : files.writes) dependencies.removeAll(target);
        QByteArray dependencyRevision;
        if (!fontConfigurationRevision(dependencies, dependencyRevision, result.error)) return result;
        auto beforeSecondSources = files.sources; beforeSecondSources.removeAll(files.xml);
        QByteArray beforeSecondRevision, xmlRevision;
        if (!fontConfigurationRevision(beforeSecondSources, beforeSecondRevision, result.error)) return result;
        if (!fontConfigurationRevision({files.xml}, xmlRevision, result.error)) return result;
        const bool renderingChanged = current.preferences.antialias != requested.antialias
            || current.preferences.hinting != requested.hinting || current.preferences.subpixel != requested.subpixel;
        if (current.preferences.antialias != requested.antialias) updateRule(doc, QStringLiteral("antialias"), requested.antialias);
        if (current.preferences.subpixel != requested.subpixel) updateRule(doc, QStringLiteral("rgba"), requested.subpixel);
        if (current.preferences.hinting != requested.hinting) {
            updateRule(doc, QStringLiteral("hintstyle"), requested.hinting);
            updateRule(doc, QStringLiteral("hinting"), requested.hinting.isEmpty() ? QString()
                : requested.hinting == QLatin1String("hintnone") ? QStringLiteral("false") : QStringLiteral("true"));
        }
        // The hook is before the first mutation, while the transaction owns
        // both files. A native KConfig lock wait is not cancelled at a deadline.
        if (files.fixture.fixture && files.fixture.beforeSync) files.fixture.beforeSync();
        QByteArray boundary;
        if (invalidated->load() || !fontConfigurationRevision(files.sources, boundary, result.error)
            || boundary != expected.revision || Reservations::currentKeys(reservation.paths) != expectedKeys) {
            if (result.error.isEmpty()) result.error = QObject::tr("Font sources or provider changed before synchronization. No write was attempted.");
            return result;
        }
        // Preserve XML-first, KConfig-second partial-save semantics.
        if (renderingChanged) {
            if (!QDir().mkpath(QFileInfo(files.xml).absolutePath())) {
                result.error = QObject::tr("The fontconfig directory could not be created. No settings were changed."); return result;
            }
            QSaveFile file(files.xml); const auto xml = doc.toByteArray(-1); struct stat written{};
            if (!file.open(QIODevice::WriteOnly) || file.write(xml) != xml.size() || !file.flush()
                || ::fstat(file.handle(), &written) != 0 || !file.commit()) {
                result.error = QObject::tr("The fontconfig rendering preferences could not be saved. KDE font preferences were not written."); return result;
            }
            // Bind this exact atomic XML replacement, before a GUI bridge or
            // KConfig lock wait can observe/adopt an unrelated same-byte inode.
            struct stat actual{};
            const auto sameWrittenXml = [&] {
                return ::stat(QFile::encodeName(files.xml).constData(), &actual) == 0
                    && actual.st_dev == written.st_dev && actual.st_ino == written.st_ino
                    && actual.st_size == written.st_size && actual.st_mtim.tv_sec == written.st_mtim.tv_sec
                    && actual.st_mtim.tv_nsec == written.st_mtim.tv_nsec;
            };
            if (!sameWrittenXml() || !fontConfigurationRevision({files.xml}, xmlRevision, result.error) || !sameWrittenXml()) {
                result.error = QObject::tr("Fontconfig was written, but its exact replacement changed before verification. KDE preferences were not written; Revert to inspect the partial save."); return result;
            }
            retainedKeys << QStringLiteral("inode:%1:%2").arg(quint64(written.st_dev)).arg(quint64(written.st_ino));
            const auto keys = Reservations::currentKeys(reservation.paths, &result.error);
            retainedKeys << keys; retainedKeys.removeDuplicates();
            if (keys.isEmpty() || !retainAliases(retainedKeys, result.error)) {
                result.error = QObject::tr("Fontconfig was written, but replacement aliases could not be retained. KDE preferences were not written; Revert to inspect the partial save. %1").arg(result.error); return result;
            }
            if (files.fixture.fixture && files.fixture.afterXml) files.fixture.afterXml();
        }
        auto secondBoundaryMatches = [&] {
            QByteArray sourcesNow, xmlNow;
            return !invalidated->load() && fontConfigurationRevision(beforeSecondSources, sourcesNow, result.error)
                && sourcesNow == beforeSecondRevision && fontConfigurationRevision({files.xml}, xmlNow, result.error)
                && xmlNow == xmlRevision;
        };
        if (!secondBoundaryMatches()) {
            result.error = renderingChanged
                ? QObject::tr("Fontconfig was saved, but a source or its XML replacement changed before KDE synchronization. KDE preferences were not written; Revert to inspect the partial save.")
                : QObject::tr("Font sources changed before KDE synchronization. No font preferences were written."); return result;
        }
        bool synced = false; QByteArray committed;
        {
            KConfig config(files.name, files.simple() ? KConfig::SimpleConfig : KConfig::FullConfig);
            bool writable = !config.isImmutable() && config.isConfigWritable(false);
            for (int role = 0; role < 6; ++role) {
                KConfigGroup group(&config, role == 5 ? "WM" : "General");
                writable &= !group.isImmutable() && !group.isEntryImmutable(fontKeys[role])
                    && group.readEntry(fontKeys[role], current.preferences.fonts[role]) == current.preferences.fonts[role];
            }
            KConfigGroup general(&config, "General");
            for (auto key : renderKeys) writable &= !general.isImmutable() && !general.isEntryImmutable(key);
            if (!writable || invalidated->load()) {
                result.error = renderingChanged
                    ? QObject::tr("Fontconfig rendering preferences were saved, but KDE font preferences changed or became locked before synchronization. Revert to read the partial save.")
                    : QObject::tr("KDE font preferences changed or became locked before the write. No font preferences were written."); return result;
            }
            for (int role = 0; role < 6; ++role)
                if (current.preferences.fonts[role] != requested.fonts[role])
                    KConfigGroup(&config, role == 5 ? "WM" : "General").writeEntry(fontKeys[role], requested.fonts[role]);
            auto mirror = [&general](const char *key, const QString &value, bool boolean) {
                if (value.isEmpty()) general.revertToDefault(key);
                else if (boolean) general.writeEntry(key, value == QLatin1String("true"));
                else general.writeEntry(key, value);
            };
            if (current.preferences.antialias != requested.antialias) mirror("XftAntialias", requested.antialias, true);
            if (current.preferences.hinting != requested.hinting) mirror("XftHintStyle", requested.hinting, false);
            if (current.preferences.subpixel != requested.subpixel) mirror("XftSubPixel", requested.subpixel, false);
            if (!secondBoundaryMatches()) {
                config.markAsClean();
                result.error = renderingChanged
                    ? QObject::tr("Fontconfig was saved, but sources or rendering mirrors changed before KDE synchronization. KDE preferences were not written; Revert to inspect the partial save.")
                    : QObject::tr("Font sources changed before KDE synchronization. No font preferences were written."); return result;
            }
            synced = files.isolated && files.sync ? files.sync(config) : config.sync();
            // Suppress KConfig's destructor retry while it is still worker-owned.
            if (!synced) config.markAsClean();
            if (synced && !fontConfigurationRevision(files.sources, committed, result.error)) committed.clear();
            const auto keys = Reservations::currentKeys(reservation.paths, &result.error);
            retainedKeys << keys; retainedKeys.removeDuplicates();
            if (keys.isEmpty() || !retainAliases(retainedKeys, result.error)) {
                result.error = QObject::tr("Font settings may have a partial save, but replacement aliases could not be retained. No refresh was requested. %1").arg(result.error); return result;
            }
            if (files.fixture.fixture && files.fixture.afterSync) files.fixture.afterSync();
        }
        if (!synced) {
            result.error = renderingChanged
                ? QObject::tr("Fontconfig rendering preferences were saved, but KDE font preferences could not be synchronized. Revert to read the partial save.")
                : QObject::tr("KDE font preferences could not be synchronized. Revert to read the saved settings."); return result;
        }
        auto saved = readFontFiles(files, runtime);
        QByteArray dependencyReadback;
        const bool dependenciesMatch = fontConfigurationRevision(dependencies, dependencyReadback, result.error)
            && dependencyReadback == dependencyRevision;
        QByteArray xmlReadback;
        const bool xmlMatch = fontConfigurationRevision({files.xml}, xmlReadback, result.error) && xmlReadback == xmlRevision;
        bool mirrorsMatch = true;
        {
            KConfig check(files.name, files.simple() ? KConfig::SimpleConfig : KConfig::FullConfig);
            KConfigGroup general(&check, "General");
            if (current.preferences.antialias != requested.antialias && !requested.antialias.isEmpty())
                mirrorsMatch &= general.hasKey("XftAntialias") && general.readEntry("XftAntialias", requested.antialias != QLatin1String("true")) == (requested.antialias == QLatin1String("true"));
            if (current.preferences.hinting != requested.hinting && !requested.hinting.isEmpty())
                mirrorsMatch &= general.readEntry("XftHintStyle", QString()) == requested.hinting;
            if (current.preferences.subpixel != requested.subpixel && !requested.subpixel.isEmpty())
                mirrorsMatch &= general.readEntry("XftSubPixel", QString()) == requested.subpixel;
        }
        if (!saved.success || saved.state.preferences != requested || saved.state.revision != committed || !mirrorsMatch || !dependenciesMatch || !xmlMatch || invalidated->load()) {
            QString detail = saved.error;
            if (detail.isEmpty()) {
                for (int role = 0; role < 6; ++role) {
                    if (saved.state.preferences.fonts[role] != requested.fonts[role]) {
                        detail = QObject::tr("Role %1 differs: saved [%2], requested [%3].").arg(QString::fromLatin1(fontKeys[role]),
                            saved.state.preferences.fonts[role].toString(), requested.fonts[role].toString()); break;
                    }
                }
            }
            if (detail.isEmpty()) detail = !xmlMatch ? QObject::tr("The committed fontconfig file changed while KDE preferences were being saved.")
                : !dependenciesMatch ? QObject::tr("Inherited font settings changed while saving.")
                : !mirrorsMatch ? QObject::tr("The saved KDE rendering mirrors differ from the request.")
                : saved.state.revision != committed ? QObject::tr("Font configuration sources changed after saving.")
                : invalidated->load() ? QObject::tr("The font refresh provider changed while saving.")
                : QObject::tr("The saved rendering preferences differ from the request.");
            result.error = QObject::tr("Font settings were written, but their saved values or sources could not be verified. No refresh was requested. Revert to read the partial save. %1").arg(detail); return result;
        }
        return saved;
    };
    auto result = work();
    retainedKeys << Reservations::currentKeys(reservation.paths); retainedKeys.removeDuplicates(); result.keys = retainedKeys;
    return result;
}

class FontFileJob final : public QObject {
public:
    std::function<void(const FontFileResult &)> readFinished;
    std::function<void(bool, const QString &, const FontFileResult &)> writeFinished;
    std::function<void(const QString &)> pending;
    FontFileJob(FontFiles files, FontReloadService *reload, bool write, FontState expected, FontPreferences requested)
        : QObject(QCoreApplication::instance()), m_files(std::move(files)), m_reload(reload), m_write(write),
          m_expected(std::move(expected)), m_requested(std::move(requested))
    {
        reload->setParent(this);
        m_runtime = {reload->isAvailable(), reload->unavailableReason(), reload->identity()};
        connect(reload, &FontReloadService::availabilityChanged, this, [this] { m_invalidated->store(true); });
        connect(reload, &FontReloadService::finished, this, [this](bool success, const QString &error) {
            if (m_phase != Remote) return;
            m_remote.stop(); verify(success, error);
        });
        m_deadline.setSingleShot(true); m_deadline.setInterval(m_files.fixture.fixture ? qMax(1, m_files.fixture.softDeadlineMs) : 20000);
        connect(&m_deadline, &QTimer::timeout, this, [this] {
            if (m_phase != Done && pending) pending(m_write
                ? tr("Font saving continues in the background. Wait for completion before retrying or closing.")
                : tr("Font settings are still being read. Wait for completion."));
        });
        m_remote.setSingleShot(true); m_remote.setInterval(3000);
        connect(&m_remote, &QTimer::timeout, this, [this] {
            if (m_phase == Remote) verify(false, tr("Font settings were saved, but the KDE refresh request timed out. A sent request may still complete. Revert to read the saved settings."));
        });
    }
    void start()
    {
        OperationManager::instance()->setActivity(this, m_write ? tr("Saving font settings") : tr("Reading font settings"), true);
        m_deadline.start(); m_phase = Files;
        const auto files = m_files; const auto runtime = m_runtime; const auto registry = Reservations::snapshot(); const bool write = m_write;
        worker([files, runtime, registry, write] {
            auto result = readFontFiles(files, runtime);
            if (write && result.success) {
                result.inspection = Reservations::inspect(result.files.targets, registry);
                if (!result.inspection.success) { result.success = false; result.error = result.inspection.error; }
            }
            return result;
        }, [this](FontFileResult result) {
            m_last = result;
            if (!m_write) { completeRead(result); return; }
            if (!result.success || !result.state.canApply() || result.state.preferences != m_expected.preferences
                || result.state.revision != m_expected.revision || !providerMatches()) {
                completeWrite(false, !result.error.isEmpty() ? result.error : !result.state.canApply() ? result.state.unavailableReason
                    : tr("Font sources or provider changed before saving. Revert to reload.")); return;
            }
            m_files = result.files;
            QString error; m_reservation = Reservations::acquire(result.inspection, error);
            if (!m_reservation.valid() || !Reservations::beginMutation(m_reservation)) {
                completeWrite(false, error.isEmpty() ? tr("Font configuration files are owned by another operation.") : error); return;
            }
            const auto files = m_files; const auto runtime = m_runtime; const auto expected = m_expected; const auto requested = m_requested;
            const auto reservation = m_reservation; const auto keys = result.inspection.keys; const auto invalidated = m_invalidated;
            auto *receiver = this;
            worker([files, runtime, expected, requested, reservation, keys, invalidated, receiver] {
                auto retain = [receiver, reservation](const QStringList &identities, QString &error) {
                    bool retained = false;
                    const bool delivered = QMetaObject::invokeMethod(receiver, [reservation, identities, &retained] {
                        retained = Reservations::refresh(reservation, identities);
                    }, Qt::BlockingQueuedConnection);
                    if (!delivered || !retained) error = QObject::tr("The font file identities could not be retained on the owning thread.");
                    return delivered && retained;
                };
                return saveFontFiles(files, runtime, expected, requested, reservation, keys, invalidated, retain);
            }, [this](FontFileResult saved) {
                m_last = saved;
                if (!saved.keys.isEmpty() && !Reservations::refresh(m_reservation, saved.keys) && saved.error.isEmpty())
                    saved.error = tr("Saved font file identities could not be retained.");
                if (!saved.success || !saved.error.isEmpty() || !providerMatches()) {
                    completeWrite(false, !saved.error.isEmpty() ? saved.error : tr("Font preferences may be saved, but the provider changed. No refresh was requested.")); return;
                }
                m_savedRevision = saved.state.revision;
                verifyBoundary();
            });
        });
    }
private:
    enum Phase { Created, Files, Remote, Done } m_phase = Created;
    template<class Work, class Complete> void worker(Work work, Complete complete)
    {
        auto *watcher = new QFutureWatcher<FontFileResult>(this);
        connect(watcher, &QFutureWatcher<FontFileResult>::finished, this, [this, watcher, complete = std::move(complete)] {
            auto result = watcher->result(); watcher->deleteLater(); if (m_phase != Done) complete(std::move(result));
        });
        watcher->setFuture(QtConcurrent::run(std::move(work)));
    }
    bool providerMatches() const { return !m_invalidated->load() && m_reload->isAvailable() && m_reload->identity() == m_runtime.identity; }
    void verifyBoundary()
    {
        const auto files = m_files; const auto runtime = m_runtime;
        worker([files, runtime] { return readFontFiles(files, runtime); }, [this](FontFileResult result) {
            m_last = result;
            if (!result.success || result.state.revision != m_savedRevision || result.state.preferences != m_requested || !providerMatches()) {
                completeWrite(false, !result.error.isEmpty() ? result.error : tr("Saved font sources or provider changed before refresh. No refresh was requested; Revert to inspect.")); return;
            }
            m_phase = Remote; m_remote.start(); m_reload->requestReload();
        });
    }
    void verify(bool success, const QString &error)
    {
        m_phase = Files; const auto files = m_files; const auto runtime = m_runtime;
        worker([files, runtime] { return readFontFiles(files, runtime); }, [this, success, error](FontFileResult result) {
            m_last = result;
            const bool verified = success && error.isEmpty() && result.success && result.state.revision == m_savedRevision
                && result.state.preferences == m_requested && providerMatches();
            completeWrite(verified, verified ? QString() : !error.isEmpty() ? error : !result.error.isEmpty() ? result.error
                : tr("Font preferences were saved, but their final sources or provider could not be verified. A sent refresh may still complete; Revert to inspect."));
        });
    }
    void completeRead(const FontFileResult &result)
    {
        m_phase = Done; m_deadline.stop(); const auto done = std::move(readFinished);
        auto current = result;
        if (!providerMatches()) {
            current.state.reloadAvailable = false;
            current.state.unavailableReason = !m_reload->isAvailable() ? m_reload->unavailableReason()
                : tr("The font refresh provider changed during reading. Refresh before applying.");
        }
        if (done) done(current);
        OperationManager::instance()->setActivity(this, {}, false); deleteLater();
    }
    void completeWrite(bool success, const QString &error)
    {
        m_phase = Done; m_deadline.stop(); m_remote.stop(); Reservations::release(m_reservation);
        const auto done = std::move(writeFinished); if (done) done(success, error, m_last);
        OperationManager::instance()->setActivity(this, {}, false); deleteLater();
    }
    FontFiles m_files;
    FontReloadService *m_reload;
    bool m_write;
    FontState m_expected;
    FontPreferences m_requested;
    FontRuntime m_runtime;
    FontFileResult m_last;
    QByteArray m_savedRevision;
    Reservations::Reservation m_reservation;
    std::shared_ptr<std::atomic_bool> m_invalidated = std::make_shared<std::atomic_bool>(false);
    QTimer m_deadline, m_remote;
};
}

KdeFontBackend::KdeFontBackend(QObject *parent)
    : KdeFontBackend(QStringLiteral("kdeglobals"), {}, createKdeFontReloadService(QDBusConnection::sessionBus()), false, parent) {}
KdeFontBackend::KdeFontBackend(const QString &config, const QString &xml, FontReloadService *reload, QObject *parent)
    : KdeFontBackend(config, xml, reload, true, parent)
{ Q_ASSERT(QFileInfo(config).isAbsolute() && QFileInfo(xml).isAbsolute()); }
KdeFontBackend::KdeFontBackend(const QString &config, const QString &xml, FontReloadService *reload, const FileContext &context, QObject *parent)
    : KdeFontBackend(config, xml, reload, parent)
{ if (m_isolated) m_context = context; }
KdeFontBackend::KdeFontBackend(const QString &config, const QString &xml, FontReloadService *reload, bool isolated, QObject *parent)
    : FontBackend(parent), m_configName(config), m_fontconfigPath(xml), m_reload(reload), m_isolated(isolated)
{
    Q_ASSERT(reload); reload->setParent(this);
    m_debounce.setSingleShot(true); m_debounce.setInterval(80);
    auto changed = [this](const QString &) { m_debounce.start(); };
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, changed);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, changed);
    connect(&m_debounce, &QTimer::timeout, this, &FontBackend::externalChange);
    connect(reload, &FontReloadService::availabilityChanged, this, &FontBackend::externalChange);
}
void KdeFontBackend::watchPaths(const QStringList &paths)
{
    const auto registered = m_watcher.files() + m_watcher.directories();
    auto added = paths;
    for (const auto &path : registered) added.removeAll(path);
    if (!added.isEmpty()) m_watcher.addPaths(added);
}
void KdeFontBackend::beginJob(bool write, const FontState &expected, const FontPreferences &requested)
{
    if (m_job) return;
    if (write && !FontSettings::isSupported(requested)) {
        emit writeFinished(false, tr("The selected fonts or rendering settings are unsupported. No settings were changed.")); return;
    }
    FontFiles files; files.name = m_configName; files.xml = m_fontconfigPath;
    files.isolated = m_isolated; files.fixture = m_context; files.sync = m_syncForTest;
    auto *job = new FontFileJob(files, m_reload, write, expected, requested); m_job = job;
    QPointer<KdeFontBackend> safe(this);
    job->pending = [safe](const QString &message) { if (safe) emit safe->operationPending(message); };
    job->readFinished = [safe](const FontFileResult &result) {
        if (!safe) return;
        safe->m_reload->setParent(safe); safe->m_job = nullptr;
        if (result.success) safe->m_fontconfigPath = result.files.xml;
        safe->watchPaths(result.files.watches);
        emit safe->readFinished(result.success, result.state, result.error);
    };
    job->writeFinished = [safe](bool success, const QString &error, const FontFileResult &result) {
        if (!safe) return;
        safe->m_reload->setParent(safe); safe->m_job = nullptr; safe->watchPaths(result.files.watches);
        emit safe->writeFinished(success, error);
    };
    job->start();
}
void KdeFontBackend::readCurrent() { beginJob(false); }
void KdeFontBackend::writePreferences(const FontState &expected, const FontPreferences &requested)
{
    auto normalized = expected; normalized.preferences = FontSettings::canonicalPreferences(expected.preferences);
    beginJob(true, normalized, FontSettings::canonicalPreferences(requested));
}
