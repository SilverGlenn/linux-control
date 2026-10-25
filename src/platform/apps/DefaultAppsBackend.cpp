#include "DefaultAppsBackend.h"
#include "operations/SettingsFileReservations.h"
#include "operations/OperationManager.h"
#include <KConfig>
#include <KConfigGroup>
#include <KDesktopFile>
#include <KService>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QMimeDatabase>
#include <QSet>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUuid>
#include <QPointer>
#include <QtConcurrent>
#include <algorithm>
#include <functional>
#include <memory>
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
constexpr qint64 maxBytes = 8 * 1024 * 1024;
struct FileImage {
    QByteArray bytes; bool exists = false; QFileDevice::Permissions permissions{};
    quint64 device = 0, inode = 0, links = 0;
    qint64 modifiedSeconds = 0, modifiedNanoseconds = 0;
};
bool bytes(const QString &path, FileImage &out, QString &error)
{
    out = {}; const QFileInfo info(path); out.exists = info.exists() || info.isSymLink(); out.permissions = info.permissions();
    if (!out.exists) return true;
    QFile file(path);
    if (!info.isFile() || !file.open(QIODevice::ReadOnly) || file.size() > maxBytes) {
        error = QObject::tr("Cannot safely read default-program settings: %1").arg(path); return false;
    }
    struct stat metadata{};
    if (::fstat(file.handle(), &metadata) != 0 || !S_ISREG(metadata.st_mode)) {
        error = QObject::tr("Cannot validate the settings file identity: %1").arg(path); return false;
    }
    out.device = metadata.st_dev; out.inode = metadata.st_ino; out.links = metadata.st_nlink;
    out.modifiedSeconds = metadata.st_mtim.tv_sec; out.modifiedNanoseconds = metadata.st_mtim.tv_nsec;
    out.bytes = file.readAll();
    if (file.error() != QFileDevice::NoError) { error = QObject::tr("Reading settings failed: %1").arg(path); return false; }
    return true;
}
// Walk each directory with O_NOFOLLOW, then keep its descriptor through rename.
// An attacker cannot redirect an atomic replacement through a swapped symlink.
int directoryFd(const QString &path)
{
    if (!QDir::isAbsolutePath(path)) return -1;
    int fd = ::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    for (const auto &part : QDir::cleanPath(path).split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
        const auto encoded = QFile::encodeName(part);
        const int next = ::openat(fd, encoded.constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (fd >= 0) ::close(fd);
        fd = next; if (fd < 0) return -1;
    }
    return fd;
}
bool safeWritable(const QString &path, QString &reason)
{
    const QFileInfo file(path); const auto dir = QFileInfo(file.absolutePath());
    const int fd = directoryFd(file.absolutePath());
    if (fd < 0 || !dir.isWritable() || (dir.permissions() & QFileDevice::WriteOther)
        || file.isSymLink() || (file.exists() && (!file.isFile() || !file.isWritable()
            || (file.permissions() & QFileDevice::WriteOther)))) {
        if (fd >= 0) ::close(fd);
        reason = QObject::tr("A settings file or its directory is unavailable, linked, world-writable, or protected: %1").arg(path); return false;
    }
    ::close(fd); return true;
}
bool atomicReplace(const QString &path, const FileImage &expected, const QByteArray &data, bool remove, QString &error, FileImage *receipt = nullptr)
{
    if (!safeWritable(path, error)) return false;
    FileImage current;
    if (!bytes(path, current, error) || current.exists != expected.exists || current.bytes != expected.bytes
        || (expected.exists && (current.permissions != expected.permissions || current.device != expected.device
            || current.inode != expected.inode || current.modifiedSeconds != expected.modifiedSeconds
            || current.modifiedNanoseconds != expected.modifiedNanoseconds))) {
        error = QObject::tr("Settings changed during Apply: %1").arg(path); return false;
    }
    const QFileInfo info(path); const int fd = directoryFd(info.absolutePath());
    if (fd < 0) { error = QObject::tr("The settings directory changed during Apply."); return false; }
    const auto name = QFile::encodeName(info.fileName());
    struct stat st{};
    const bool exists = ::fstatat(fd, name.constData(), &st, AT_SYMLINK_NOFOLLOW) == 0;
    if (exists != expected.exists || (exists && (!S_ISREG(st.st_mode) || st.st_nlink != 1 || quint64(st.st_dev) != expected.device
        || quint64(st.st_ino) != expected.inode || st.st_mtim.tv_sec != expected.modifiedSeconds
        || st.st_mtim.tv_nsec != expected.modifiedNanoseconds))) {
        ::close(fd); error = QObject::tr("The settings file identity changed during Apply."); return false;
    }
    if (remove) {
        const bool ok = !exists || ::unlinkat(fd, name.constData(), 0) == 0;
        if (ok) ::fsync(fd);
        ::close(fd); if (!ok) error = QObject::tr("Could not restore the original settings file."); return ok;
    }
    const QByteArray temp = QByteArray(".control-default-apps-") + QUuid::createUuid().toByteArray(QUuid::Id128);
    const int output = ::openat(fd, temp.constData(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    bool ok = output >= 0;
    qsizetype written = 0;
    while (ok && written < data.size()) {
        const auto count = ::write(output, data.constData() + written, data.size() - written);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) ok = false; else written += count;
    }
    if (ok && exists) ok = ::fchmod(output, st.st_mode & 0777) == 0;
    if (ok) ok = ::fsync(output) == 0;
    struct stat writtenImage{};
    if (ok) ok = ::fstat(output,&writtenImage)==0;
    if (output >= 0) ::close(output);
    // Recheck the name after preparing bytes. renameat never follows this name.
    struct stat after{};
    const bool stillExists = ::fstatat(fd, name.constData(), &after, AT_SYMLINK_NOFOLLOW) == 0;
    if (stillExists != exists || (exists && (st.st_dev != after.st_dev || st.st_ino != after.st_ino
        || st.st_size != after.st_size || st.st_mtim.tv_sec != after.st_mtim.tv_sec || st.st_mtim.tv_nsec != after.st_mtim.tv_nsec))) ok = false;
    if (ok) ok = ::renameat(fd, temp.constData(), fd, name.constData()) == 0;
    if (!ok) ::unlinkat(fd, temp.constData(), 0);
    if (ok) ::fsync(fd);
    ::close(fd);
    if (ok && receipt) {
        receipt->exists=true; receipt->bytes=data; receipt->permissions=expected.exists?expected.permissions:QFileDevice::Permissions(QFileDevice::ReadOwner|QFileDevice::WriteOwner|QFileDevice::ReadUser|QFileDevice::WriteUser);
        receipt->device=writtenImage.st_dev; receipt->inode=writtenImage.st_ino; receipt->links=writtenImage.st_nlink;
        receipt->modifiedSeconds=writtenImage.st_mtim.tv_sec; receipt->modifiedNanoseconds=writtenImage.st_mtim.tv_nsec;
    }
    if (!ok) error = QObject::tr("An atomic settings write failed or the file changed: %1").arg(path);
    return ok;
}
DefaultAppsPaths livePaths()
{
    DefaultAppsPaths p;
    p.configHome = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    p.configDirs = QStandardPaths::standardLocations(QStandardPaths::GenericConfigLocation);
    p.configDirs.removeAll(p.configHome);
    p.dataDirs = QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation);
    p.desktops.clear();
    for (auto desktop : qEnvironmentVariable("XDG_CURRENT_DESKTOP", "KDE").split(QLatin1Char(':'), Qt::SkipEmptyParts)) {
        desktop = desktop.toLower();
        if (!desktop.contains(QLatin1Char('/')) && !desktop.contains(QLatin1Char('.'))) p.desktops << desktop;
    }
    return p;
}
bool immutable(const QString &path, const QString &group, const QString &key)
{
    KConfig config(path, KConfig::SimpleConfig);
    return config.group(group).isEntryImmutable(key);
}
bool layeredImmutable(const DefaultAppsPaths &paths, const QString &name, const QString &group, const QString &key)
{
    if (immutable(paths.configHome + QLatin1Char('/') + name, group, key)) return true;
    for (const auto &dir : paths.configDirs) if (immutable(dir + QLatin1Char('/') + name, group, key)) return true;
    return false;
}
QString emailProfile(const DefaultAppsPaths &paths)
{
    const QString name = paths.configHome + QStringLiteral("/emaildefaults");
    KConfig config(paths.isolated ? name : QStringLiteral("emaildefaults"), paths.isolated ? KConfig::SimpleConfig : KConfig::NoGlobals);
    const QString fallback = QCoreApplication::translate("KEMailSettings", "Default");
    const QString value = config.group(QStringLiteral("Defaults")).readEntry("Profile", fallback);
    return config.hasGroup(QStringLiteral("PROFILE_") + value) ? value : fallback;
}
bool desktopInventory(const DefaultAppsPaths &paths, QList<DefaultApplication> &apps, QByteArray &revision, QString &error)
{
    QCryptographicHash hash(QCryptographicHash::Sha256); QSet<QString> seen;
    for (const auto &data : paths.dataDirs) {
        const QString root = data + QStringLiteral("/applications");
        hash.addData(root.toUtf8());
        QStringList files;
        QDirIterator it(root, {QStringLiteral("*.desktop")}, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) files << it.next();
        std::sort(files.begin(), files.end());
        if (files.size() > 20000) { error = QObject::tr("The desktop-entry inventory is too large to validate safely."); return false; }
        for (const auto &file : files) {
            QString id = QDir(root).relativeFilePath(file); id.replace(QLatin1Char('/'), QLatin1Char('-'));
            FileImage image;
            if (!bytes(file, image, error)) return false;
            hash.addData(file.toUtf8()); hash.addData(image.bytes); hash.addData(QFileInfo(file).canonicalFilePath().toUtf8());
            if (seen.contains(id)) continue;
            seen.insert(id); // Hidden entries shadow entries in lower-precedence directories.
            KDesktopFile desktop(file);
            const auto group = desktop.desktopGroup();
            if (group.readEntry("Hidden", false) || group.readEntry("Type", QString()) != QLatin1String("Application")) continue;
            const QString tryExec = group.readEntry("TryExec", QString());
            if (!tryExec.isEmpty() && QStandardPaths::findExecutable(tryExec).isEmpty()) continue;
            KService service(file);
            if (!service.isValid() || !service.isApplication() || service.exec().isEmpty()) continue;
            DefaultApplication app;
            app.id = id; app.name = service.name(); app.icon = service.icon(); app.exec = service.exec(); app.path = file;
            app.categories = service.categories(); app.mimeTypes = service.mimeTypes(); app.terminal = service.terminal(); app.showInDesktop = service.showInCurrentDesktop();
            for (const auto &scheme : service.schemeHandlers()) app.mimeTypes << QStringLiteral("x-scheme-handler/") + scheme;
            app.mimeTypes.removeDuplicates();
            app.identity = QCryptographicHash::hash(file.toUtf8() + '\0' + image.bytes + '\0' + QFileInfo(file).canonicalFilePath().toUtf8(), QCryptographicHash::Sha256);
            apps << app;
        }
    }
    std::sort(apps.begin(), apps.end(), [](const auto &a, const auto &b) { return a.name.localeAwareCompare(b.name) < 0; });
    revision = hash.result(); return true;
}
// Parse a private copy. KConfig preserves all unrelated keys and groups. No
// KConfig instance used here can sync into the live settings directory.
bool editCopy(const FileImage &image, const std::function<void(KConfig &)> &edit, QByteArray &result, QString &error)
{
    QTemporaryDir dir;
    if (!dir.isValid()) { error = QObject::tr("Cannot prepare a private settings transaction."); return false; }
    const QString path = dir.filePath(QStringLiteral("settings")); QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(image.bytes) != image.bytes.size()) { error = QObject::tr("Cannot stage settings."); return false; }
    file.close();
    KConfig config(path, KConfig::SimpleConfig); edit(config);
    if (!config.sync()) { error = QObject::tr("Cannot stage the new default programs."); return false; }
    FileImage staged; if (!bytes(path, staged, error)) return false;
    result = staged.bytes; return true;
}
QStringList associationPaths(const DefaultAppsPaths &paths)
{
    QStringList result, configs{paths.configHome}; configs << paths.configDirs;
    auto append = [&paths, &result](const QString &dir) {
        for (const auto &desktop : paths.desktops) result << dir + QLatin1Char('/') + desktop + QStringLiteral("-mimeapps.list");
        result << dir + QStringLiteral("/mimeapps.list");
    };
    for (const auto &config : configs) append(config);
    for (const auto &data : paths.dataDirs) append(data + QStringLiteral("/applications"));
    result.removeDuplicates(); return result;
}
QStringList revisionPaths(const DefaultAppsPaths &paths)
{
    auto result = associationPaths(paths); QStringList configs{paths.configHome}; configs << paths.configDirs;
    for (const auto &dir : configs) for (const auto &name : {"kdeglobals", "emaildefaults", "filetypesrc"}) result << dir + QLatin1Char('/') + QLatin1String(name);
    return result;
}
bool readSnapshot(const DefaultAppsPaths &paths, DefaultAppsSnapshot &state, QString &error)
{
    state = {};
    if (paths.isolated && paths.beforeScan) paths.beforeScan();
    if (!QDir::isAbsolutePath(paths.configHome)) { error = QObject::tr("The settings directory must be an absolute path."); return false; }
    for (const auto &path : paths.configDirs + paths.dataDirs)
        if (!QDir::isAbsolutePath(path)) { error = QObject::tr("The desktop and settings paths must be absolute."); return false; }
    for (const auto &desktop : paths.desktops)
        if (desktop.isEmpty() || desktop.contains(QLatin1Char('/')) || desktop.contains(QLatin1Char('.'))) { error = QObject::tr("Invalid desktop identity."); return false; }
    QByteArray appRevision;
    if (!desktopInventory(paths, state.applications, appRevision, error)) return false;
    QCryptographicHash hash(QCryptographicHash::Sha256); hash.addData(appRevision);
    const auto associationFiles = associationPaths(paths);
    QSet<QString> types;
    for (const auto &mime : QMimeDatabase().allMimeTypes()) types.insert(mime.name());
    for (const auto &app : state.applications) for (const auto &mime : app.mimeTypes) if (DefaultAppsTypes::validMime(mime)) types.insert(DefaultAppsTypes::canonicalMime(mime));
    for (const auto &path : revisionPaths(paths)) {
        FileImage image; if (!bytes(path, image, error)) return false;
        hash.addData(path.toUtf8()); hash.addData(image.exists ? "present" : "absent"); hash.addData(image.bytes);
        hash.addData(QByteArray::number(int(image.permissions))); hash.addData(QFileInfo(path).symLinkTarget().toUtf8());
        hash.addData(QByteArray::number(image.device)); hash.addData(QByteArray::number(image.inode));
        hash.addData(QByteArray::number(image.links)); hash.addData(QByteArray::number(image.modifiedSeconds)); hash.addData(QByteArray::number(image.modifiedNanoseconds));
        hash.addData(QByteArray::number(int(QFileInfo(QFileInfo(path).absolutePath()).permissions())));
        if (associationFiles.contains(path)) {
            KConfig config(path, KConfig::SimpleConfig);
            for (const auto &key : config.group(QStringLiteral("Default Applications")).keyList()) if (DefaultAppsTypes::validMime(key)) types.insert(DefaultAppsTypes::canonicalMime(key));
        }
    }
    // Read each file once. Parsing thousands of MIME names against every file
    // separately would stall the UI and needlessly amplify I/O.
    struct Associations { QString path; QMap<QString, QStringList> defaults, added, removed; };
    QList<Associations> associations;
    for (const auto &path : associationFiles) {
        KConfig config(path, KConfig::SimpleConfig); Associations item; item.path = path;
        auto load = [&config](const QString &name, QMap<QString, QStringList> &entries) {
            const auto group = config.group(name);
            for (const auto &key : group.keyList()) if (DefaultAppsTypes::validMime(key)) entries[key] = group.readXdgListEntry(key);
        };
        load(QStringLiteral("Default Applications"), item.defaults);
        if (path.endsWith(QLatin1String("/mimeapps.list"))) {
            load(QStringLiteral("Added Associations"), item.added); load(QStringLiteral("Removed Associations"), item.removed);
        }
        associations << item;
    }
    // KService 6.24: files are processed from low to high precedence;
    // defaults add offers at base+25, after additions and removals. A default
    // itself associates an installed app, including historical forced choices.
    // No ranking is invented for applications with no saved preference.
    QMap<QString, QMap<QString, int>> offers;
    int base = 1000;
    for (auto item = associations.crbegin(); item != associations.crend(); ++item, base += 50) {
        auto add = [&](const QMap<QString, QStringList> &entries, int preference) {
            for (auto entry = entries.cbegin(); entry != entries.cend(); ++entry) {
                const QString mime = DefaultAppsTypes::canonicalMime(entry.key()); int pref = preference;
                for (const auto &id : entry.value()) {
                    if (state.application(id)) { offers[mime][id] = qMax(offers[mime].value(id, -1), pref); --pref; }
                }
            }
        };
        add(item->added, base);
        for (auto entry = item->removed.cbegin(); entry != item->removed.cend(); ++entry)
            for (const auto &id : entry.value()) offers[DefaultAppsTypes::canonicalMime(entry.key())].remove(id);
        add(item->defaults, base + 25);
    }
    for (auto offer = offers.cbegin(); offer != offers.cend(); ++offer) {
        int highest = -1; QString selected; bool ambiguous = false;
        for (auto app = offer.value().cbegin(); app != offer.value().cend(); ++app)
            if (app.value() > highest) { highest = app.value(); selected = app.key(); ambiguous = false; }
            else if (app.value() == highest) ambiguous = true;
        if (!selected.isEmpty() && !ambiguous) state.preferences.mimeDefaults[offer.key()] = selected;
    }
    KConfig general(paths.isolated ? paths.configHome + QStringLiteral("/kdeglobals") : QStringLiteral("kdeglobals"),
                    paths.isolated ? KConfig::SimpleConfig : KConfig::NoGlobals);
    state.preferences.terminalService = general.group(QStringLiteral("General")).readEntry("TerminalService", QStringLiteral("org.kde.konsole.desktop"));
    state.mimeTypes = types.values(); std::sort(state.mimeTypes.begin(), state.mimeTypes.end());
    state.revision = hash.result();
    QString mimeReason, embedReason, terminalReason, mailReason;
    safeWritable(paths.configHome + QStringLiteral("/mimeapps.list"), mimeReason);
    safeWritable(paths.configHome + QStringLiteral("/filetypesrc"), embedReason);
    safeWritable(paths.configHome + QStringLiteral("/kdeglobals"), terminalReason);
    safeWritable(paths.configHome + QStringLiteral("/emaildefaults"), mailReason);
    state.mimeWriteReason = !mimeReason.isEmpty() ? mimeReason : embedReason;
    const QString profileGroup = QStringLiteral("PROFILE_") + emailProfile(paths);
    // Policy parsing is bounded by existing keys, rather than reparsing a file
    // for every name in the shared MIME database.
    QStringList configs{paths.configHome}; configs << paths.configDirs;
    for (const auto &dir : configs) {
        for (const auto &name : QStringList{QStringLiteral("mimeapps.list"), QStringLiteral("filetypesrc")}) {
            KConfig config(dir + QLatin1Char('/') + name, KConfig::SimpleConfig);
            const QStringList groups = name == QLatin1String("mimeapps.list")
                ? QStringList{QStringLiteral("Default Applications"), QStringLiteral("Added Associations"), QStringLiteral("Removed Associations")}
                : QStringList{QStringLiteral("EmbedSettings")};
            for (const auto &groupName : groups) {
                const auto group = config.group(groupName);
                if (group.isImmutable()) { state.mimeWriteReason = QObject::tr("MIME associations are protected by KDE configuration policy."); continue; }
                for (const auto &key : group.keyList()) if (group.isEntryImmutable(key)) {
                    const QString mime = name == QLatin1String("filetypesrc") && key.startsWith(QLatin1String("embed-")) ? key.mid(6) : key;
                    if (DefaultAppsTypes::validMime(mime)) state.unavailableMimeTypes[DefaultAppsTypes::canonicalMime(mime)] = QObject::tr("This association is protected by KDE configuration policy.");
                }
            }
        }
        for (const auto &desktop : paths.desktops) {
            const QString path = dir + QLatin1Char('/') + desktop + QStringLiteral("-mimeapps.list"); KConfig config(path, KConfig::SimpleConfig);
            const auto group = config.group(QStringLiteral("Default Applications"));
            if (group.isImmutable()) state.mimeWriteReason = QObject::tr("Desktop-specific associations are protected by KDE configuration policy.");
            for (const auto &key : group.keyList()) if (group.isEntryImmutable(key)) state.unavailableMimeTypes[DefaultAppsTypes::canonicalMime(key)] = QObject::tr("This desktop-specific default is protected by KDE configuration policy.");
            if (dir == paths.configHome && QFileInfo::exists(path)) {
                QString reason; if (!safeWritable(path, reason))
                    for (const auto &key : group.keyList()) state.unavailableMimeTypes[DefaultAppsTypes::canonicalMime(key)] = reason;
            }
        }
    }
    if (layeredImmutable(paths, QStringLiteral("kdeglobals"), QStringLiteral("General"), QStringLiteral("TerminalApplication"))
        || layeredImmutable(paths, QStringLiteral("kdeglobals"), QStringLiteral("General"), QStringLiteral("TerminalService")))
        terminalReason = QObject::tr("The terminal selection is protected by KDE configuration policy.");
    if (layeredImmutable(paths, QStringLiteral("emaildefaults"), QStringLiteral("Defaults"), QStringLiteral("Profile"))
        || layeredImmutable(paths, QStringLiteral("emaildefaults"), profileGroup, QStringLiteral("EmailClient"))
        || layeredImmutable(paths, QStringLiteral("emaildefaults"), profileGroup, QStringLiteral("TerminalClient")))
        mailReason = QObject::tr("The e-mail selection is protected by KDE configuration policy.");
    if (!mailReason.isEmpty()) state.unavailableMimeTypes[QStringLiteral("x-scheme-handler/mailto")] = mailReason;
    for (const auto &r : DefaultAppsTypes::roles()) {
        if (r.role == DefaultAppRole::Terminal) {
            if (!terminalReason.isEmpty()) state.unavailableRoles[r.role] = terminalReason;
        } else if (!state.canChangeMime(DefaultAppsTypes::canonicalMime(r.anchorMime))) {
            state.unavailableRoles[r.role] = !state.mimeWriteReason.isEmpty() ? state.mimeWriteReason : state.unavailableMimeTypes.value(DefaultAppsTypes::canonicalMime(r.anchorMime));
        }
    }
    state.writable = terminalReason.isEmpty() || state.mimeWriteReason.isEmpty();
    if (!state.writable) state.reason = state.mimeWriteReason + QLatin1Char('\n') + terminalReason;
    return true;
}
DefaultAppsJobResult writeTransaction(const DefaultAppsPaths &paths, const DefaultAppsSnapshot &expected, const DefaultAppsPreferences &requested,
                                      const std::function<bool(const QStringList &,QString &)> &retainAliases)
{
        DefaultAppsSnapshot before; QString error;
        auto failure = [&before](const QString &why) { return DefaultAppsJobResult{false, DefaultAppsOutcome::Failed, before, why}; };
        if (!readSnapshot(paths, before, error)) { return failure(error); }
        if (!before.writable || before.revision != expected.revision || before.preferences != expected.preferences) {
            return failure(before.writable ? QObject::tr("Default programs or installed desktop entries changed before Apply. Revert to reload.") : before.reason);
        }
        QMap<QString, QString> changes;
        for (auto it = requested.mimeDefaults.cbegin(); it != requested.mimeDefaults.cend(); ++it) {
            if (it.value() == before.preferences.mimeDefaults.value(it.key())) continue;
            const auto *app = before.application(it.value());
            if (!before.canChangeMime(it.key()) || !before.mimeTypes.contains(it.key()) || !DefaultAppsTypes::validMime(it.key()) || !app || !DefaultAppsTypes::supports(*app, it.key())) {
                return failure(QObject::tr("An association is invalid or its selected installed application no longer supports the type."));
            }
            const auto *old = expected.application(it.value());
            if (!old || old->identity != app->identity) { return failure(QObject::tr("A selected desktop entry changed before Apply.")); }
            changes[it.key()] = it.value();
        }
        for (auto it = before.preferences.mimeDefaults.cbegin(); it != before.preferences.mimeDefaults.cend(); ++it)
            if (!requested.mimeDefaults.contains(it.key())) { return failure(QObject::tr("Removing saved defaults is unsupported; choose an installed application.")); }
        const bool terminalChanged = requested.terminalService != before.preferences.terminalService;
        const auto *terminal = before.application(requested.terminalService);
        if (terminalChanged && (!terminal || !DefaultAppsTypes::eligible(*terminal, *DefaultAppsTypes::role(DefaultAppRole::Terminal))
            || !expected.application(terminal->id) || expected.application(terminal->id)->identity != terminal->identity)) {
            return failure(QObject::tr("The selected terminal desktop entry is unavailable or invalid."));
        }
        const QString profile = emailProfile(paths), emailGroup = QStringLiteral("PROFILE_") + profile;
        const bool emailChanged = changes.contains(QStringLiteral("x-scheme-handler/mailto"));
        auto locked = [&paths](const QString &file, const QString &group, const QString &key) { return layeredImmutable(paths, file, group, key); };
        for (auto it = changes.cbegin(); it != changes.cend(); ++it) {
            if (locked(QStringLiteral("mimeapps.list"), QStringLiteral("Default Applications"), it.key())
                || locked(QStringLiteral("mimeapps.list"), QStringLiteral("Added Associations"), it.key())
                || locked(QStringLiteral("mimeapps.list"), QStringLiteral("Removed Associations"), it.key())
                || locked(QStringLiteral("filetypesrc"), QStringLiteral("EmbedSettings"), QStringLiteral("embed-") + it.key())) {
                return failure(QObject::tr("A MIME association is protected by KDE configuration policy."));
            }
            for (const auto &desktop : paths.desktops)
                if (locked(desktop + QStringLiteral("-mimeapps.list"), QStringLiteral("Default Applications"), it.key())) {
                    return failure(QObject::tr("A desktop-specific MIME default is protected by KDE configuration policy."));
                }
        }
        if (terminalChanged && (locked(QStringLiteral("kdeglobals"), QStringLiteral("General"), QStringLiteral("TerminalApplication"))
            || locked(QStringLiteral("kdeglobals"), QStringLiteral("General"), QStringLiteral("TerminalService")))) {
            return failure(QObject::tr("The terminal selection is protected by KDE configuration policy."));
        }
        if (emailChanged && (locked(QStringLiteral("emaildefaults"), QStringLiteral("Defaults"), QStringLiteral("Profile"))
            || locked(QStringLiteral("emaildefaults"), emailGroup, QStringLiteral("EmailClient"))
            || locked(QStringLiteral("emaildefaults"), emailGroup, QStringLiteral("TerminalClient")))) {
            return failure(QObject::tr("The e-mail selection is protected by KDE configuration policy."));
        }
        QMap<QString, FileImage> originals; QMap<QString, QByteArray> prepared;
        auto stage = [&](const QString &name, const std::function<void(KConfig &)> &edit) {
            const QString path = paths.configHome + QLatin1Char('/') + name; FileImage image; QByteArray result;
            if (!safeWritable(path, error) || !bytes(path, image, error) || !editCopy(image, edit, result, error)) return false;
            originals[path] = image; prepared[path] = result; return true;
        };
        if (!changes.isEmpty()) {
            if (!stage(QStringLiteral("mimeapps.list"), [&](KConfig &config) {
                for (auto it = changes.cbegin(); it != changes.cend(); ++it) {
                    auto defaults = config.group(QStringLiteral("Default Applications"));
                    for (const auto &key : defaults.keyList()) if (key != it.key() && DefaultAppsTypes::canonicalMime(key) == it.key()) defaults.deleteEntry(key);
                    defaults.writeXdgListEntry(it.key(), {it.value()});
                    auto merged = [&](const QString &groupName) {
                        auto group = config.group(groupName); QStringList values = group.readXdgListEntry(it.key());
                        for (const auto &key : group.keyList()) if (key != it.key() && DefaultAppsTypes::canonicalMime(key) == it.key()) {
                            values << group.readXdgListEntry(key); group.deleteEntry(key);
                        }
                        values.removeDuplicates(); return values;
                    };
                    auto added = config.group(QStringLiteral("Added Associations")); auto list = merged(QStringLiteral("Added Associations"));
                    list.removeAll(it.value()); list.prepend(it.value()); added.writeXdgListEntry(it.key(), list);
                    auto removed = config.group(QStringLiteral("Removed Associations")); auto removals = merged(QStringLiteral("Removed Associations"));
                    const bool removedChoice = removals.removeAll(it.value()) > 0;
                    if (removedChoice || removed.hasKey(it.key()) || !removals.isEmpty()) removed.writeXdgListEntry(it.key(), removals);
                }
            }) || !stage(QStringLiteral("filetypesrc"), [&](KConfig &config) {
                for (auto it = changes.cbegin(); it != changes.cend(); ++it) config.group(QStringLiteral("EmbedSettings")).writeEntry(QStringLiteral("embed-") + it.key(), false);
            })) { return failure(error); }
            // Existing higher-priority desktop defaults must agree with the
            // generic override; edit only the affected keys, never the rest.
            for (const auto &desktop : paths.desktops) {
                const QString name = desktop + QStringLiteral("-mimeapps.list"); const QString path = paths.configHome + QLatin1Char('/') + name;
                if (!QFileInfo::exists(path)) continue;
                KConfig config(path, KConfig::SimpleConfig); bool touched = false;
                for (auto it = changes.cbegin(); it != changes.cend(); ++it) for (const auto &key : config.group(QStringLiteral("Default Applications")).keyList())
                    touched |= DefaultAppsTypes::canonicalMime(key) == it.key();
                if (touched && !stage(name, [&](KConfig &copy) {
                    auto group = copy.group(QStringLiteral("Default Applications"));
                    for (auto it = changes.cbegin(); it != changes.cend(); ++it) {
                        bool present = false;
                        for (const auto &key : group.keyList()) if (DefaultAppsTypes::canonicalMime(key) == it.key()) {
                            present = true; if (key != it.key()) group.deleteEntry(key);
                        }
                        if (present) group.writeXdgListEntry(it.key(), {it.value()});
                    }
                })) { return failure(error); }
            }
        }
        if (terminalChanged && !stage(QStringLiteral("kdeglobals"), [&](KConfig &config) {
            auto group = config.group(QStringLiteral("General")); group.writeEntry("TerminalService", terminal->id); group.writeEntry("TerminalApplication", terminal->exec);
        })) { return failure(error); }
        if (emailChanged && !stage(QStringLiteral("emaildefaults"), [&](KConfig &config) {
            config.group(QStringLiteral("Defaults")).writeEntry("Profile", profile);
            auto group = config.group(emailGroup); const auto *app = before.application(changes.value(QStringLiteral("x-scheme-handler/mailto")));
            const bool kmail = app->id == QLatin1String("org.kde.kmail2.desktop");
            group.writePathEntry("EmailClient", kmail ? QString() : app->id); group.writeEntry("TerminalClient", !kmail && app->terminal);
        })) { return failure(error); }
        // The complete multi-file plan is prepared before the first live write.
        DefaultAppsSnapshot preflight;
        if (!readSnapshot(paths, preflight, error) || preflight.revision != before.revision) { return failure(QObject::tr("Settings changed while preparing Apply. No settings were written.")); }
        QStringList committed; QMap<QString,FileImage> receipts;
        int commitIndex = 0;
        for (auto it = prepared.cbegin(); it != prepared.cend(); ++it) {
            if (paths.isolated && paths.beforeCommit) paths.beforeCommit(commitIndex++, it.key());
            FileImage receipt;
            if (!atomicReplace(it.key(), originals.value(it.key()), it.value(), false, error,&receipt)) {
                bool restored = true;
                for (auto prior = committed.crbegin(); prior != committed.crend(); ++prior) {
                    FileImage expectedNew; QString rollbackError;
                    const auto owned=receipts.value(*prior);
                    FileImage rollbackReceipt;
                    if (!bytes(*prior, expectedNew, rollbackError) || expectedNew.bytes != prepared.value(*prior)
                        || expectedNew.device!=owned.device || expectedNew.inode!=owned.inode
                        || expectedNew.modifiedSeconds!=owned.modifiedSeconds || expectedNew.modifiedNanoseconds!=owned.modifiedNanoseconds
                        || !atomicReplace(*prior, expectedNew, originals.value(*prior).bytes, !originals.value(*prior).exists, rollbackError,&rollbackReceipt)) restored = false;
                    else if (!retainAliases(rollbackReceipt.exists?QStringList{QStringLiteral("inode:%1:%2").arg(rollbackReceipt.device).arg(rollbackReceipt.inode)}:QStringList{},rollbackError)) { restored = false; break; }
                }
                DefaultAppsSnapshot after; QString readError; readSnapshot(paths, after, readError);
                return {false, restored ? DefaultAppsOutcome::Failed : DefaultAppsOutcome::Partial, after,
                    restored ? QObject::tr("Apply failed; completed writes were rolled back. %1").arg(error)
                             : QObject::tr("Apply failed and some files could not be rolled back. Revert to inspect saved settings. %1").arg(error)};
            }
            committed << it.key();
            receipts[it.key()]=receipt;
            if (!retainAliases({QStringLiteral("inode:%1:%2").arg(receipt.device).arg(receipt.inode)},error)) {
                DefaultAppsSnapshot after; QString readError; readSnapshot(paths, after, readError);
                return {false, DefaultAppsOutcome::Uncertain, after,
                    QObject::tr("Default programs may be saved partially, but file identities could not be retained. No further writes or rollback were attempted. %1").arg(error)};
            }
        }
        DefaultAppsSnapshot after; QString readError;
        const bool readOk = readSnapshot(paths, after, readError);
        bool verified = readOk && after.preferences == requested;
        for (auto it = prepared.cbegin(); verified && it != prepared.cend(); ++it) {
            FileImage image; verified = bytes(it.key(), image, readError) && image.bytes == it.value();
        }
        return {verified, verified ? DefaultAppsOutcome::Success : DefaultAppsOutcome::Uncertain, after,
            verified ? QString() : QObject::tr("Settings were written, but their saved result could not be verified. Revert to reload. %1").arg(readError)};
}
}
KdeDefaultAppsBackend::KdeDefaultAppsBackend(QObject *parent) : KdeDefaultAppsBackend(livePaths(), parent) {}
KdeDefaultAppsBackend::KdeDefaultAppsBackend(const DefaultAppsPaths &paths, QObject *parent)
    : DefaultAppsBackend(parent), m_paths(paths)
{
    m_debounce.setSingleShot(true); m_debounce.setInterval(180);
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, [this] { m_debounce.start(); });
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { m_debounce.start(); });
    connect(&m_debounce, &QTimer::timeout, this, [this] { watch(); emit externalChange(); });
    // Directory watches do not cover newly-created nested application dirs.
    // Bounded periodic snapshots also detect policy and desktop identity changes.
    m_poll.setInterval(8000);
    connect(&m_poll, &QTimer::timeout, this, [this] {
        if (!m_jobActive && m_queue.isEmpty()) enqueueRead(0, true);
    });
    if (!m_paths.isolated) m_poll.start();
    watch();
}
void KdeDefaultAppsBackend::watch()
{
    if (!m_watcher.files().isEmpty()) m_watcher.removePaths(m_watcher.files());
    if (!m_watcher.directories().isEmpty()) m_watcher.removePaths(m_watcher.directories());
    QSet<QString> paths;
    for (const auto &path : revisionPaths(m_paths)) {
        if (QFileInfo::exists(path)) paths.insert(path);
        if (QFileInfo::exists(QFileInfo(path).absolutePath())) paths.insert(QFileInfo(path).absolutePath());
    }
    for (const auto &dir : m_paths.dataDirs) {
        const QString root = dir + QStringLiteral("/applications");
        if (QFileInfo::exists(root)) paths.insert(root);
    }
    if (!paths.isEmpty()) m_watcher.addPaths(paths.values());
}
void KdeDefaultAppsBackend::enqueueRead(quint64 token, bool poll)
{
    if (poll && (m_jobActive || !m_queue.isEmpty())) return;
    if (!poll && !m_queue.isEmpty() && !m_queue.back().write) {
        m_queue.back().poll = false; m_queue.back().tokens << token;
    } else {
        DefaultAppsJob job; job.poll = poll; if (!poll) job.tokens << token; m_queue.enqueue(job);
    }
    startNext();
}
void KdeDefaultAppsBackend::readCurrent(quint64 token) { enqueueRead(token); }
void KdeDefaultAppsBackend::writeChanges(quint64 token, const DefaultAppsSnapshot &expected, const DefaultAppsPreferences &requested)
{
    DefaultAppsJob job; job.tokens << token; job.write = true; job.expected = expected; job.requested = requested;
    m_queue.enqueue(job); startNext();
}
void KdeDefaultAppsBackend::cancel()
{
    const auto queued=m_queue; m_queue.clear();
    QPointer<KdeDefaultAppsBackend> guard(this);
    for (const auto &job:queued) {
        for (const auto token:job.tokens) {
            const auto error=tr("Queued default-program work was canceled before it started. No changes were written by that job.");
            if (job.write) emit writeFinished(token,DefaultAppsOutcome::Failed,job.expected,error);
            else emit readFinished(token,false,{},error);
            if (!guard) return;
        }
    }
    if (m_jobActive) emit operationPending(tr("Default-program work is already running and continues until its actual completion. It has not been canceled."));
}
void KdeDefaultAppsBackend::startNext()
{
    if (m_jobActive || m_queue.isEmpty()) return;
    m_jobActive=true; m_active=m_queue.dequeue();
    const auto paths=m_paths; const auto job=m_active;
    auto *work=new QObject(QCoreApplication::instance());
    auto lease=std::make_shared<SettingsFileReservations::Reservation>();
    QPointer<KdeDefaultAppsBackend> facade(this);
    OperationManager::instance()->setActivity(work,job.write?tr("Saving Default Programs"):tr("Reading Default Programs"),true);
    auto *pendingDeadline=new QTimer(work); pendingDeadline->setSingleShot(true);
    pendingDeadline->setInterval(paths.isolated?qMax(1,paths.softDeadlineMs):10000);
    connect(pendingDeadline,&QTimer::timeout,work,[facade] {
        if (facade) emit facade->operationPending(QObject::tr("Default-program work is taking longer than expected. It continues in the background; wait for its actual completion."));
    });
    pendingDeadline->start();
    auto finish=[facade,work,lease,job,pendingDeadline](const DefaultAppsJobResult &result) {
        pendingDeadline->stop();
        SettingsFileReservations::endMutation(*lease); SettingsFileReservations::release(*lease);
        OperationManager::instance()->setActivity(work,{},false);
        if (facade) {
            const bool changed=!result.success || (!facade->m_revision.isEmpty() && result.snapshot.revision!=facade->m_revision);
            if (!result.snapshot.revision.isEmpty()) facade->m_revision=result.snapshot.revision;
            facade->m_jobActive=false; facade->watch();
            if (job.write) emit facade->writeFinished(job.tokens.first(),result.outcome,result.snapshot,result.error);
            else if (job.poll) { if (changed) emit facade->externalChange(); }
            else for (const auto token:job.tokens) {
                emit facade->readFinished(token,result.success,result.snapshot,result.error);
                if (!facade) break;
            }
            if (facade) facade->startNext();
        }
        work->deleteLater();
    };
    auto run=[paths,job,work,lease,finish] {
        auto *future=new QFutureWatcher<DefaultAppsJobResult>(work);
        connect(future,&QFutureWatcher<DefaultAppsJobResult>::finished,work,[future,finish] {
            const auto result=future->result(); future->deleteLater(); finish(result);
        });
        // The app-owned work object is retained through worker completion. The
        // bridge sends immutable identity keys to the GUI registry only, never
        // a configuration object or a reference to the optional facade.
        future->setFuture(QtConcurrent::run([paths,job,work,lease] {
            auto retain=[work,lease](const QStringList &writtenKeys,QString &error) {
                auto keys=SettingsFileReservations::currentKeys(lease->paths,&error);
                if (keys.isEmpty()) return false;
                keys<<writtenKeys; keys.removeDuplicates();
                bool retained=false;
                const bool delivered=QMetaObject::invokeMethod(work,[lease,keys,&retained] {
                    retained=SettingsFileReservations::refresh(*lease,keys);
                },Qt::BlockingQueuedConnection);
                if (!delivered || !retained) error=QObject::tr("The settings-file identities could not be retained on the owning thread.");
                return delivered && retained;
            };
            if (job.write) return writeTransaction(paths,job.expected,job.requested,retain);
            DefaultAppsJobResult result; result.success=readSnapshot(paths,result.snapshot,result.error); return result;
        }));
    };
    if (!job.write) { run(); return; }
    const auto inventory=SettingsFileReservations::snapshot(); const auto targets=revisionPaths(paths);
    auto *admission=new QFutureWatcher<SettingsFileReservations::Inspection>(work);
    connect(admission,&QFutureWatcher<SettingsFileReservations::Inspection>::finished,work,[admission,lease,run,finish,job] {
        const auto inspected=admission->result(); admission->deleteLater(); QString error;
        *lease=SettingsFileReservations::acquire(inspected,error);
        if (!lease->valid() || !SettingsFileReservations::beginMutation(*lease)) {
            DefaultAppsJobResult result; result.snapshot=job.expected;
            result.error=error.isEmpty()?QObject::tr("The default-program file reservation could not be acquired."):error; finish(result); return;
        }
        run();
    });
    admission->setFuture(QtConcurrent::run([targets,inventory] { return SettingsFileReservations::inspect(targets,inventory); }));
}
