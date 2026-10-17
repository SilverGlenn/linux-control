#include "CustomShortcutsBackend.h"
#include <KConfig>
#include <KConfigGroup>
#include <KDesktopFile>
#include <KService>
#include <KServiceAction>
#include <KGlobalAccel>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDataStream>
#include <QDBusArgument>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusVariant>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QUuid>
#include <QXmlStreamReader>
#include <QtConcurrent>
#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <kglobalaccel_version.h>

namespace {
const QString service = QStringLiteral("org.kde.kglobalaccel");
const QString rootPath = QStringLiteral("/kglobalaccel");
const QString rootInterface = QStringLiteral("org.kde.KGlobalAccel");
const QString componentInterface = QStringLiteral("org.kde.kglobalaccel.Component");
constexpr qint64 maxBytes = 8 * 1024 * 1024;
struct FileImage {
    QByteArray bytes; bool exists = false; QFileDevice::Permissions permissions{};
    QByteArray catalogIdentity;
    bool usable = true;
    quint64 device = 0, inode = 0, links = 0;
    qint64 modifiedSeconds = 0, modifiedNanoseconds = 0;
};
bool bytes(const QString &path, FileImage &out, QString &error)
{
    out = {}; const QFileInfo info(path); out.exists = info.exists() || info.isSymLink(); out.permissions = info.permissions();
    if (!out.exists) return true;
    QFile file(path);
    if (!info.isFile() || !file.open(QIODevice::ReadOnly) || file.size() > maxBytes) {
        error = QObject::tr("Cannot safely read custom shortcut settings: %1").arg(path); return false;
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
// Optional catalog entries can be dangling links, unreadable, or unsupported
// files. Reserve their IDs and fingerprint both the directory entry and target
// observation without making the whole application manager unavailable.
FileImage catalogImage(const QString &path)
{
    FileImage image; QString ignored;
    image.usable = bytes(path, image, ignored);
    QByteArray identity; QDataStream stream(&identity, QIODevice::WriteOnly);
    auto appendMetadata = [&](bool present, const struct stat &metadata, int failure) {
        stream << present << failure;
        if (present) stream << quint64(metadata.st_dev) << quint64(metadata.st_ino) << quint64(metadata.st_nlink)
            << quint32(metadata.st_mode) << quint64(metadata.st_uid) << quint64(metadata.st_gid) << qint64(metadata.st_size)
            << qint64(metadata.st_mtim.tv_sec) << qint64(metadata.st_mtim.tv_nsec)
            << qint64(metadata.st_ctim.tv_sec) << qint64(metadata.st_ctim.tv_nsec);
    };
    struct stat entry{}, target{};
    const auto encoded = QFile::encodeName(path);
    const bool entryPresent = ::lstat(encoded.constData(), &entry) == 0; const int entryFailure = entryPresent ? 0 : errno;
    appendMetadata(entryPresent, entry, entryFailure);
    stream << QFileInfo(path).symLinkTarget() << QFileInfo(path).canonicalFilePath();
    const bool targetPresent = ::stat(encoded.constData(), &target) == 0; const int targetFailure = targetPresent ? 0 : errno;
    appendMetadata(targetPresent, target, targetFailure);
    if (!image.usable) {
        image.bytes.clear(); image.exists = entryPresent;
        if (entryPresent) {
            image.device = entry.st_dev; image.inode = entry.st_ino; image.links = entry.st_nlink;
            image.modifiedSeconds = entry.st_mtim.tv_sec; image.modifiedNanoseconds = entry.st_mtim.tv_nsec;
        }
    }
    stream << image.usable;
    image.catalogIdentity = QCryptographicHash::hash(identity, QCryptographicHash::Sha256);
    return image;
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
    struct stat metadata{}, directoryMetadata{};
    const bool unsafeLinks = file.exists() && (::lstat(QFile::encodeName(path).constData(), &metadata) != 0 || metadata.st_nlink != 1 || metadata.st_uid != ::geteuid());
    const bool unsafeDirectory = fd < 0 || ::fstat(fd, &directoryMetadata) != 0 || directoryMetadata.st_uid != ::geteuid();
    if (unsafeLinks || unsafeDirectory || !dir.isWritable() || (dir.permissions() & QFileDevice::WriteOther)
        || file.isSymLink() || (file.exists() && (!file.isFile() || !file.isWritable()
            || (file.permissions() & QFileDevice::WriteOther)))) {
        if (fd >= 0) ::close(fd);
        reason = QObject::tr("A settings file or its directory is unavailable, linked, writable by another user, not owned by this user, or protected: %1").arg(path); return false;
    }
    ::close(fd); return true;
}
bool atomicReplace(const QString &path, const FileImage &expected, const QByteArray &data, bool remove, QString &error)
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
    const QByteArray temp = QByteArray(".control-custom-shortcuts-") + QUuid::createUuid().toByteArray(QUuid::Id128);
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
    if (!ok) error = QObject::tr("An atomic settings write failed or the file changed: %1").arg(path);
    return ok;
}
class DBusTransport final : public CustomShortcutsTransport {
public:
    explicit DBusTransport(const QDBusConnection &bus) : m_bus(bus) {}
    CustomShortcutsReply call(const QString &destination, const QString &path, const QString &interface,
                             const QString &method, const QVariantList &arguments) override
    {
        auto message = QDBusMessage::createMethodCall(destination, path, interface, method); message.setArguments(arguments);
        const auto response = m_bus.call(message, QDBus::Block, 2000);
        CustomShortcutsReply reply;
        reply.success = response.type() == QDBusMessage::ReplyMessage;
        const bool mutation = method == QLatin1String("doRegister") || method == QLatin1String("unregister") || method == QLatin1String("cleanUp");
        reply.uncertain = !reply.success && mutation;
        if (!reply.success) { reply.error = response.errorName() + QStringLiteral(": ") + response.errorMessage(); return reply; }
        const int arity = method == QLatin1String("doRegister") ? 0 : 1;
        if (response.arguments().size() != arity) { reply.success = false; reply.uncertain = mutation; reply.error = QStringLiteral("Unexpected native reply arity."); return reply; }
        if (!arity) return reply;
        const auto value = response.arguments().first();
        if (method == QLatin1String("Introspect") || method == QLatin1String("GetNameOwner") || method == QLatin1String("supportInformation")) {
            reply.success = value.metaType() == QMetaType::fromType<QString>(); reply.value = value;
        } else if (method == QLatin1String("Get")) {
            reply.success = value.metaType() == QMetaType::fromType<QDBusVariant>();
            if (reply.success) {
                reply.value = qvariant_cast<QDBusVariant>(value).variant();
                reply.success = reply.value.metaType() == QMetaType::fromType<QString>();
            }
        } else if (method == QLatin1String("GetConnectionUnixProcessID") || method == QLatin1String("GetConnectionUnixUser")) {
            reply.success = value.metaType() == QMetaType::fromType<uint>(); reply.value = value;
        } else if (method == QLatin1String("getComponent")) {
            reply.success = value.metaType() == QMetaType::fromType<QDBusObjectPath>(); reply.value = qvariant_cast<QDBusObjectPath>(value).path();
        } else if (method == QLatin1String("unregister") || method == QLatin1String("cleanUp")) {
            reply.success = value.metaType() == QMetaType::fromType<bool>(); reply.value = value;
        } else if (method == QLatin1String("getShortcutContexts")) {
            reply.success = value.metaType() == QMetaType::fromType<QStringList>(); reply.value = value;
        } else if (value.metaType() == QMetaType::fromType<QDBusArgument>()) {
            const auto arg = qvariant_cast<QDBusArgument>(value); const auto signature = arg.currentSignature();
            if (method == QLatin1String("allComponents") && signature == QLatin1String("ao")) {
                QStringList paths; arg.beginArray();
                while (!arg.atEnd() && paths.size() < 4096) { QDBusObjectPath item; arg >> item; paths << item.path(); }
                reply.success = arg.atEnd(); arg.endArray(); reply.value = paths;
            } else if (method == QLatin1String("allShortcutInfos") && signature == QLatin1String("a(ssssssaiai)")) {
                QList<ShortcutAction> actions; arg.beginArray();
                while (!arg.atEnd() && actions.size() < 20000) {
                    ShortcutAction action; arg.beginStructure();
                    arg >> action.id.action >> action.friendlyName >> action.id.component >> action.componentFriendly >> action.id.context >> action.contextFriendly;
                    for (int which = 0; which < 2; ++which) {
                        arg.beginArray(); int count = 0;
                        while (!arg.atEnd() && count < 4096) { int key; arg >> key; (which ? action.defaults : action.current) << QKeySequence(key); ++count; }
                        if (!arg.atEnd()) reply.success = false; arg.endArray();
                    }
                    arg.endStructure(); actions << action;
                }
                reply.success = reply.success && arg.atEnd(); arg.endArray(); reply.value = QVariant::fromValue(actions);
            } else if ((method == QLatin1String("shortcutKeys") || method == QLatin1String("defaultShortcutKeys")) && signature == QLatin1String("a(ai)")) {
                QList<QKeySequence> keys; arg.beginArray();
                while (!arg.atEnd() && keys.size() < 4096) {
                    int chords[4] = {}; int count = 0; arg.beginStructure(); arg.beginArray();
                    while (!arg.atEnd() && count < 4) { arg >> chords[count]; ++count; }
                    if (count != 4 || !arg.atEnd()) reply.success = false; arg.endArray(); arg.endStructure();
                    keys << QKeySequence(chords[0], chords[1], chords[2], chords[3]);
                }
                reply.success = reply.success && arg.atEnd(); arg.endArray(); reply.value = QVariant::fromValue(keys);
            } else reply.success = false;
        } else reply.success = false;
        if (!reply.success) { reply.error = QStringLiteral("Unexpected or oversized native reply signature."); reply.uncertain = mutation; }
        return reply;
    }
private:
    QDBusConnection m_bus;
};
bool hasMethods(const QString &source, const QString &interface, const QSet<QString> &required)
{
    QXmlStreamReader xml(source); bool inside = false, method = false; QString name; QStringList input, output; QSet<QString> found;
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement() && xml.name() == QLatin1String("interface")) inside = xml.attributes().value(QStringLiteral("name")) == interface;
        if (inside && xml.isStartElement() && xml.name() == QLatin1String("method")) {
            method = true; name = xml.attributes().value(QStringLiteral("name")).toString(); input.clear(); output.clear();
        }
        if (inside && xml.isStartElement() && xml.name() == QLatin1String("property")) {
            const auto signature = QStringLiteral("@") + xml.attributes().value(QStringLiteral("name")).toString()
                + QLatin1Char(':') + xml.attributes().value(QStringLiteral("type")).toString()
                + QLatin1Char(':') + xml.attributes().value(QStringLiteral("access")).toString();
            if (required.contains(signature)) found.insert(signature);
        }
        if (inside && method && xml.isStartElement() && xml.name() == QLatin1String("arg")) {
            const bool out = xml.attributes().value(QStringLiteral("direction")) == QLatin1String("out");
            (out ? output : input) << xml.attributes().value(QStringLiteral("type")).toString();
        }
        if (xml.isEndElement() && xml.name() == QLatin1String("method") && method) {
            const auto signature = name + QLatin1Char('(') + input.join(QLatin1Char(',')) + QStringLiteral(")->") + output.join(QLatin1Char(','));
            if (required.contains(signature)) found.insert(signature); method = false;
        }
        if (xml.isEndElement() && xml.name() == QLatin1String("interface")) inside = false;
    }
    return !xml.hasError() && found == required;
}
const QSet<QString> rootMethods{QStringLiteral("allComponents()->ao"), QStringLiteral("getComponent(s)->o"), QStringLiteral("doRegister(as)->"),
    QStringLiteral("unregister(s,s)->b"), QStringLiteral("shortcutKeys(as)->a(ai)"), QStringLiteral("defaultShortcutKeys(as)->a(ai)")};
const QSet<QString> componentMethods{QStringLiteral("getShortcutContexts()->as"), QStringLiteral("allShortcutInfos(s)->a(ssssssaiai)"), QStringLiteral("cleanUp()->b"), QStringLiteral("@uniqueName:s:read")};
CustomShortcutsBackendOptions liveOptions()
{
    CustomShortcutsBackendOptions o;
    o.config = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/kglobalshortcutsrc");
    o.dataHome = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    o.dataDirs = QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation);
    o.dataDirs.removeAll(o.dataHome); o.dataDirs.prepend(o.dataHome); return o;
}
bool validOptions(const CustomShortcutsBackendOptions &options, QString &error)
{
    if (!QDir::isAbsolutePath(options.config) || !QDir::isAbsolutePath(options.dataHome)) { error = QObject::tr("Custom shortcut paths must be absolute."); return false; }
    if (options.isolated) for (const auto &dir : options.dataDirs)
        if (!QDir::isAbsolutePath(dir)) { error = QObject::tr("Isolated application sources require absolute paths."); return false; }
    return true;
}
// KWin 6.6.6 embeds KGlobalAccelD on Wayland. The standalone daemon has no
// version RPC; the embedded route also checks the pinned KWin version endpoint.
bool auditedRuntime(const std::shared_ptr<CustomShortcutsTransport> &transport, const QString &owner, QString &signature, QString &error)
{
    if (QString::fromLatin1(KGLOBALACCEL_VERSION_STRING) != QLatin1String("6.24.0")) {
        error = QObject::tr("Native definition changes require the audited KGlobalAccel 6.24.0 SDK."); return false;
    }
    QProcess packages;
    packages.start(QStringLiteral("/usr/bin/dpkg-query"), {QStringLiteral("-W"), QStringLiteral("-f=${Package} ${Version}\n"),
        QStringLiteral("plasma-desktop"), QStringLiteral("kglobalacceld"), QStringLiteral("libkglobalacceld0"),
        QStringLiteral("libkf6globalaccel6"), QStringLiteral("kwin-wayland")});
    if (!packages.waitForFinished(2000)) { packages.kill(); packages.waitForFinished(200); error = QObject::tr("The installed KDE versions could not be verified within the time budget."); return false; }
    const auto output = QString::fromUtf8(packages.readAllStandardOutput());
    QMap<QString, QString> versions;
    for (const auto &line : output.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        const auto parts = line.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (parts.size() != 2) continue;
        auto version = parts[1]; if (version.contains(QLatin1Char(':'))) version = version.section(QLatin1Char(':'), 1);
        versions.insert(parts[0], version.section(QLatin1Char('-'), 0, 0));
    }
    if (packages.exitStatus() != QProcess::NormalExit || packages.exitCode() != 0
        || versions.value(QStringLiteral("plasma-desktop")) != QLatin1String("6.6.6")
        || versions.value(QStringLiteral("kglobalacceld")) != QLatin1String("6.6.5")
        || versions.value(QStringLiteral("libkglobalacceld0")) != QLatin1String("6.6.5")
        || versions.value(QStringLiteral("libkf6globalaccel6")) != QLatin1String("6.24.0")) {
        error = QObject::tr("Native definition changes are audited for Plasma Desktop 6.6.6, kglobalacceld 6.6.5, and KGlobalAccel 6.24.0. This installation is read-only."); return false;
    }
    const auto pid = transport->call(QStringLiteral("org.freedesktop.DBus"), QStringLiteral("/org/freedesktop/DBus"), QStringLiteral("org.freedesktop.DBus"), QStringLiteral("GetConnectionUnixProcessID"), {owner});
    const auto uid = transport->call(QStringLiteral("org.freedesktop.DBus"), QStringLiteral("/org/freedesktop/DBus"), QStringLiteral("org.freedesktop.DBus"), QStringLiteral("GetConnectionUnixUser"), {owner});
    if (!pid.success || !uid.success || !pid.value.toUInt() || uid.value.toUInt() != ::geteuid()) {
        error = QObject::tr("The shortcut service process identity cannot be verified."); return false;
    }
    const QFileInfo executable(QStringLiteral("/proc/%1/exe").arg(pid.value.toUInt()));
    auto target = executable.symLinkTarget();
    QString package = QStringLiteral("kglobalacceld"), runtime = QStringLiteral("standalone");
    const auto kwinOwner = transport->call(QStringLiteral("org.freedesktop.DBus"), QStringLiteral("/org/freedesktop/DBus"), QStringLiteral("org.freedesktop.DBus"), QStringLiteral("GetNameOwner"), {QStringLiteral("org.kde.KWin")});
    if (kwinOwner.success && kwinOwner.value.toString() == owner) {
        runtime = QStringLiteral("kwin-wayland6.6.6"); package = QStringLiteral("kwin-wayland");
        const auto kwinPid = transport->call(QStringLiteral("org.freedesktop.DBus"), QStringLiteral("/org/freedesktop/DBus"), QStringLiteral("org.freedesktop.DBus"), QStringLiteral("GetConnectionUnixProcessID"), {kwinOwner.value.toString()});
        const auto xml = transport->call(owner, QStringLiteral("/KWin"), QStringLiteral("org.freedesktop.DBus.Introspectable"), QStringLiteral("Introspect"), {});
        if (versions.value(QStringLiteral("kwin-wayland")) != QLatin1String("6.6.6") || !kwinPid.success || kwinPid.value.toUInt() != pid.value.toUInt()
            || !xml.success || !hasMethods(xml.value.toString(), QStringLiteral("org.kde.KWin"), {QStringLiteral("supportInformation()->s")})) {
            error = QObject::tr("The embedded shortcut service does not match the audited KWin owner, process, package, or version interface."); return false;
        }
        const auto information = transport->call(owner, QStringLiteral("/KWin"), QStringLiteral("org.kde.KWin"), QStringLiteral("supportInformation"), {});
        const auto version = QRegularExpression(QStringLiteral("^KWin version: ([^\\s]+)\\s*$"), QRegularExpression::MultilineOption).match(information.value.toString());
        const auto wayland = QRegularExpression(QStringLiteral("^Operation Mode: Wayland\\s*$"), QRegularExpression::MultilineOption).match(information.value.toString());
        if (!information.success || !version.hasMatch() || version.captured(1) != QLatin1String("6.6.6") || !wayland.hasMatch()) {
            error = QObject::tr("Embedded native definition changes require the audited KWin 6.6.6 Wayland runtime."); return false;
        }
        // CAP_SYS_NICE makes /proc/PID/exe unreadable to this same-user client
        // on the audited target. Keep the owner/version checks above and verify
        // readable kernel comm/cmdline evidence plus the protected package path.
        if (target.isEmpty()) {
            QFile comm(QStringLiteral("/proc/%1/comm").arg(pid.value.toUInt()));
            QFile command(QStringLiteral("/proc/%1/cmdline").arg(pid.value.toUInt()));
            if (!comm.open(QIODevice::ReadOnly) || !command.open(QIODevice::ReadOnly)
                || comm.read(4096).trimmed() != QByteArray("kwin_wayland")
                || command.read(32768).split('\0').value(0) != QByteArray("/usr/bin/kwin_wayland")) {
                error = QObject::tr("The embedded shortcut process evidence cannot be verified."); return false;
            }
            target = QStringLiteral("/usr/bin/kwin_wayland");
            runtime += QStringLiteral("/proc-exe-protected");
        }
    }
    struct stat metadata{};
    const auto expectedExecutable = package == QLatin1String("kwin-wayland") ? QStringLiteral("kwin_wayland") : QStringLiteral("kglobalacceld");
    if (QFileInfo(target).fileName() != expectedExecutable || target.contains(QStringLiteral(" (deleted)"))
        || ::stat(QFile::encodeName(target).constData(), &metadata) != 0 || !S_ISREG(metadata.st_mode) || metadata.st_uid != 0 || (metadata.st_mode & 0022)) {
        error = QObject::tr("The pinned shortcut service does not match a protected audited native executable."); return false;
    }
    QProcess ownership;
    ownership.start(QStringLiteral("/usr/bin/dpkg-query"), {QStringLiteral("-S"), target});
    if (!ownership.waitForFinished(2000)) { ownership.kill(); ownership.waitForFinished(200); error = QObject::tr("The shortcut daemon package ownership check timed out."); return false; }
    if (ownership.exitCode() || ownership.exitStatus() != QProcess::NormalExit
        || !QString::fromUtf8(ownership.readAllStandardOutput()).split(QLatin1Char('\n')).contains(package + QStringLiteral(": ") + target)) {
        error = QObject::tr("The shortcut service executable is not owned by the audited native package."); return false;
    }
    QProcess libraryFiles;
    libraryFiles.start(QStringLiteral("/usr/bin/dpkg-query"), {QStringLiteral("-L"), QStringLiteral("libkglobalacceld0")});
    if (!libraryFiles.waitForFinished(2000)) { libraryFiles.kill(); libraryFiles.waitForFinished(200); error = QObject::tr("The native shortcut library identity check timed out."); return false; }
    QString library;
    for (const auto &path : QString::fromUtf8(libraryFiles.readAllStandardOutput()).split(QLatin1Char('\n')))
        if (QFileInfo(path).fileName() == QLatin1String("libKGlobalAccelD.so.6.6.5")) { library = path; break; }
    struct stat libraryMetadata{};
    if (libraryFiles.exitCode() || libraryFiles.exitStatus() != QProcess::NormalExit || library.isEmpty()
        || ::stat(QFile::encodeName(library).constData(), &libraryMetadata) != 0 || !S_ISREG(libraryMetadata.st_mode)
        || libraryMetadata.st_uid != 0 || (libraryMetadata.st_mode & 0022)) {
        error = QObject::tr("The installed KGlobalAccelD 6.6.5 library is unavailable or not protected."); return false;
    }
    signature += QStringLiteral("/sdk6.24.0/plasma6.6.6/%1/pid%2/dev%3/ino%4/library%5:%6")
        .arg(runtime).arg(pid.value.toUInt()).arg(metadata.st_dev).arg(metadata.st_ino).arg(libraryMetadata.st_dev).arg(libraryMetadata.st_ino);
    return true;
}
QList<QKeySequence> nativeKeys(const QString &text)
{
    QList<QKeySequence> result;
    if (text.isEmpty() || text == QLatin1String("none")) return result;
    for (const auto &part : text.split(QLatin1Char('\t'))) { const auto key = QKeySequence::fromString(part, QKeySequence::PortableText); if (!key.isEmpty()) result << key; }
    return result;
}
QString commandPath(const CustomShortcutsBackendOptions &options, const QString &component)
{ return options.dataHome + QStringLiteral("/applications/") + component; }
bool inventory(const CustomShortcutsBackendOptions &options, CustomShortcutsSnapshot &snapshot, QString &error)
{
    if (options.isolated && options.beforeScan) options.beforeScan();
    QByteArray data; QDataStream stream(&data, QIODevice::WriteOnly); QSet<QString> seen;
    QStringList dirs = options.dataDirs; dirs.removeAll(options.dataHome); dirs.prepend(options.dataHome);
    for (const auto &dir : dirs) {
        const auto root = dir + QStringLiteral("/applications"); stream << root << QFileInfo::exists(root);
        QStringList files; QDirIterator iterator(root, {QStringLiteral("*.desktop")}, QDir::Files | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
        while (iterator.hasNext()) {
            files << iterator.next();
            if (files.size() > 20000) { error = QObject::tr("The installed application catalog is too large to validate safely."); return false; }
        }
        std::sort(files.begin(), files.end());
        if (files.size() > 20000) { error = QObject::tr("The installed application catalog is too large to validate safely."); return false; }
        for (const auto &path : files) {
            const auto image = catalogImage(path);
            stream << path << image.bytes << quint32(image.permissions) << image.device << image.inode << image.links << image.modifiedSeconds << image.modifiedNanoseconds
                << image.catalogIdentity << image.usable;
            QString id = QDir(root).relativeFilePath(path); id.replace(QLatin1Char('/'), QLatin1Char('-'));
            if (seen.contains(id)) continue; seen.insert(id);
            if (!CustomShortcutsTypes::validComponent(id)) continue;
            if (!image.usable) continue;
            KDesktopFile desktop(path); const auto group = desktop.desktopGroup();
            if (group.readEntry("Hidden", false) || group.readEntry("Type", QString()) != QLatin1String("Application")) continue;
            // Per-file KService parsing never queries/invalidates/builds KSycoca.
            KService serviceFile(path);
            if (!serviceFile.isValid() || !serviceFile.isApplication() || serviceFile.exec().isEmpty()) continue;
            const auto shortcutType = group.readEntry("X-KDE-GlobalShortcutType", QString());
            if (!shortcutType.isEmpty() && shortcutType != QLatin1String("Application")) continue;
            CustomShortcutApplication app; app.id = id; app.name = serviceFile.name(); app.icon = serviceFile.icon(); app.path = path; app.exec = serviceFile.exec();
            app.command = group.readEntry("X-KDE-GlobalAccel-CommandShortcut", false);
            app.identity = QCryptographicHash::hash(path.toUtf8() + '\0' + image.bytes + '\0' + QFileInfo(path).canonicalFilePath().toUtf8(), QCryptographicHash::Sha256);
            auto addDefault = [&](const QString &action, const QString &label, const QString &encoded) {
                ShortcutAction entry; entry.id = {id, QStringLiteral("default"), action}; entry.componentFriendly = app.name; entry.friendlyName = label;
                entry.defaults = nativeKeys(encoded); entry.current = entry.defaults; app.defaults << entry;
            };
            for (const auto &action : serviceFile.actions()) addDefault(action.name(), action.text(), action.property<QStringList>(QStringLiteral("X-KDE-Shortcuts")).join(QLatin1Char('\t')));
            addDefault(QStringLiteral("_launch"), app.name, serviceFile.property<QStringList>(QStringLiteral("X-KDE-Shortcuts")).join(QLatin1Char('\t')));
            snapshot.applications << app;
        }
    }
    snapshot.inventoryRevision = QCryptographicHash::hash(data, QCryptographicHash::Sha256); return true;
}
using DiskEntries = QMap<QStringList, QMap<QString, QString>>;
DiskEntries diskEntries(const QString &path)
{
    KConfig config(path, KConfig::SimpleConfig); DiskEntries result;
    std::function<void(const KConfigGroup &, const QStringList &)> visit = [&](const auto &group, const auto &parts) {
        if (!group.keyList().isEmpty()) result.insert(parts, group.entryMap());
        for (const auto &child : group.groupList()) visit(group.group(child), parts + QStringList{child});
    };
    for (const auto &name : config.groupList()) visit(config.group(name), {name});
    return result;
}
bool configRevision(const QString &path, QByteArray &revision, QString &error)
{
    FileImage image; if (!bytes(path, image, error)) return false;
    QByteArray data; QDataStream stream(&data, QIODevice::WriteOnly);
    stream << path << image.exists << image.bytes << quint32(image.permissions) << image.device << image.inode << image.links << image.modifiedSeconds << image.modifiedNanoseconds;
    revision = QCryptographicHash::hash(data, QCryptographicHash::Sha256); return true;
}
struct NativeRead {
    CustomShortcutsSnapshot snapshot;
    QMap<QString, QString> paths;
    QMap<QString, QStringList> contexts;
};
CustomShortcutsReply nativeCall(const std::shared_ptr<CustomShortcutsTransport> &transport, const QString &owner, const QString &path,
                               const QString &interface, const QString &method, const QVariantList &arguments = {})
{ return transport->call(owner, path, interface, method, arguments); }
bool readNative(const std::shared_ptr<CustomShortcutsTransport> &transport, const CustomShortcutsBackendOptions &options,
                NativeRead &result, QString &error)
{
    if (!validOptions(options, error)) return false;
    QElapsedTimer budget; budget.start();
    auto call = [&](const QString &destination, const QString &path, const QString &interface, const QString &method, const QVariantList &args = {}) {
        if (budget.elapsed() > options.readBudgetMs) return CustomShortcutsReply{false, false, {}, QObject::tr("Reading native custom shortcuts exceeded its time budget.")};
        return nativeCall(transport, destination, path, interface, method, args);
    };
    result = {}; auto &snapshot = result.snapshot;
    const auto owner = call(QStringLiteral("org.freedesktop.DBus"), QStringLiteral("/org/freedesktop/DBus"), QStringLiteral("org.freedesktop.DBus"), QStringLiteral("GetNameOwner"), {service});
    if (!owner.success || owner.value.toString().isEmpty()) { error = owner.error; return false; }
    snapshot.owner = owner.value.toString();
    const auto xml = call(snapshot.owner, rootPath, QStringLiteral("org.freedesktop.DBus.Introspectable"), QStringLiteral("Introspect"));
    if (!xml.success || !hasMethods(xml.value.toString(), rootInterface, rootMethods)) { error = QObject::tr("The shortcut service does not expose the audited native creation/removal interface."); return false; }
    snapshot.interfaceSignature = QStringLiteral("KGlobalAccel/native-desktop-component/v6.6.5/v2");
    QByteArray initial; if (!configRevision(options.config, initial, error) || !inventory(options, snapshot, error)) return false;
    const auto components = call(snapshot.owner, rootPath, rootInterface, QStringLiteral("allComponents"));
    if (!components.success) { error = components.error; return false; }
    QMap<ShortcutActionId, bool> seen;
    for (const auto &path : components.value.toStringList()) {
        const auto componentXml = call(snapshot.owner, path, QStringLiteral("org.freedesktop.DBus.Introspectable"), QStringLiteral("Introspect"));
        if (!componentXml.success || !hasMethods(componentXml.value.toString(), componentInterface, componentMethods)) { error = QObject::tr("A shortcut component exposes an unsupported native interface."); return false; }
        const auto contexts = call(snapshot.owner, path, componentInterface, QStringLiteral("getShortcutContexts"));
        if (!contexts.success || contexts.value.toStringList().size() > 4096) { error = contexts.error; return false; }
        const auto identity = call(snapshot.owner, path, QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("Get"), {componentInterface, QStringLiteral("uniqueName")});
        const auto component = identity.value.toString();
        if (!identity.success || component.isEmpty() || result.paths.contains(component)) { error = QObject::tr("The native shortcut component identity is unavailable or duplicated."); return false; }
        auto sortedContexts = contexts.value.toStringList(); std::sort(sortedContexts.begin(), sortedContexts.end());
        if (std::adjacent_find(sortedContexts.cbegin(), sortedContexts.cend()) != sortedContexts.cend()) { error = QObject::tr("The native shortcut context inventory contains duplicates."); return false; }
        for (const auto &context : contexts.value.toStringList()) {
            const auto infos = call(snapshot.owner, path, componentInterface, QStringLiteral("allShortcutInfos"), {context});
            if (!infos.success) { error = infos.error; return false; }
            for (auto action : qvariant_cast<QList<ShortcutAction>>(infos.value)) {
                if (action.id.component != component || action.id.context != context || seen.contains(action.id)) {
                    error = QObject::tr("The native shortcut registry returned conflicting action identities."); return false;
                }
                seen.insert(action.id, true);
                const auto current = call(snapshot.owner, rootPath, rootInterface, QStringLiteral("shortcutKeys"), {action.id.nativeId()});
                const auto defaults = call(snapshot.owner, rootPath, rootInterface, QStringLiteral("defaultShortcutKeys"), {action.id.nativeId()});
                if (!current.success || !defaults.success) { error = !current.success ? current.error : defaults.error; return false; }
                action.current = qvariant_cast<QList<QKeySequence>>(current.value); action.defaults = qvariant_cast<QList<QKeySequence>>(defaults.value);
                snapshot.actions << action;
                if (snapshot.actions.size() > 20000) { error = QObject::tr("The native shortcut action registry is too large."); return false; }
            }
        }
        if (!component.isEmpty()) {
            result.paths.insert(component, path);
            result.contexts.insert(component, sortedContexts);
            snapshot.contexts.insert(component, sortedContexts);
        }
    }
    KConfig config(options.config, KConfig::SimpleConfig); QString configReason;
    const bool versionSupported = options.isolated || auditedRuntime(transport, snapshot.owner, snapshot.interfaceSignature, configReason);
    snapshot.writable = versionSupported && !config.isImmutable() && safeWritable(options.config, configReason);
    if (config.isImmutable()) configReason = QObject::tr("Native shortcut configuration is protected by policy.");
    const auto serviceGroup = config.group(QStringLiteral("services"));
    if (serviceGroup.isImmutable()) { snapshot.writable = false; configReason = QObject::tr("Native application shortcut configuration is protected by policy."); }
    QSet<QString> componentsSeen;
    for (const auto &app : snapshot.applications) {
        if (!app.command && !result.paths.contains(app.id)) continue;
        CustomShortcutEntry entry; entry.component = app.id; entry.name = app.name; entry.exec = app.exec; entry.path = app.path; entry.identity = app.identity;
        entry.command = app.command; entry.registered = result.paths.contains(app.id);
        const auto policy = serviceGroup.group(app.id);
        bool mutableRegistry = snapshot.writable && !serviceGroup.isImmutable() && !policy.isImmutable();
        for (const auto &context : result.contexts.value(app.id)) if (policy.group(context).isImmutable()) mutableRegistry = false;
        for (const auto &action : snapshot.actions) if (action.id.component == app.id) {
            const auto group = action.id.context == QLatin1String("default") ? policy : policy.group(action.id.context);
            if (group.isImmutable() || group.isEntryImmutable(action.id.action)) mutableRegistry = false;
        }
        entry.removable = mutableRegistry;
        if (entry.command) {
            QString writableReason; const bool local = app.path == commandPath(options, app.id);
            struct stat metadata{}; const bool owned = ::lstat(QFile::encodeName(app.path).constData(), &metadata) == 0 && metadata.st_uid == ::geteuid();
            KDesktopFile desktop(app.path); const auto group = desktop.desktopGroup();
            entry.editable = local && owned && safeWritable(app.path, writableReason) && !desktop.isImmutable() && !group.isImmutable()
                && !group.isEntryImmutable("Name") && !group.isEntryImmutable("Exec");
            bool shadowed = false;
            for (const auto &dir : options.dataDirs) if (dir != options.dataHome && QFileInfo::exists(dir + QStringLiteral("/applications/") + app.id)) shadowed = true;
            entry.removable = entry.removable && entry.editable && !shadowed;
            if (!entry.editable) entry.reason = QObject::tr("The command definition is outside the writable user applications directory, linked, not owned by this user, or protected by policy.");
            else if (shadowed) entry.reason = QObject::tr("Removing this command would reveal another installed desktop file with the same identity.");
        }
        if (!mutableRegistry) entry.reason = QObject::tr("This shortcut component is protected by native configuration policy.");
        snapshot.entries << entry; componentsSeen.insert(app.id);
    }
    // Registered service components whose desktop source is missing remain
    // visible but read-only, rather than deleting or guessing their ownership.
    for (auto it = result.paths.cbegin(); it != result.paths.cend(); ++it) if (it.key().endsWith(QLatin1String(".desktop")) && !componentsSeen.contains(it.key())) {
        CustomShortcutEntry entry; entry.component = it.key(); entry.name = it.key(); entry.registered = true;
        for (const auto &action : snapshot.actions) if (action.id.component == it.key()) { entry.name = action.componentFriendly; break; }
        entry.reason = QObject::tr("The installed desktop definition cannot be resolved safely."); snapshot.entries << entry; componentsSeen.insert(entry.component);
    }
    CustomShortcutsSnapshot verifiedInventory;
    if (!inventory(options, verifiedInventory, error) || verifiedInventory.inventoryRevision != snapshot.inventoryRevision) { error = QObject::tr("The application catalog changed during the read. Refresh again."); return false; }
    if (!configRevision(options.config, snapshot.configRevision, error) || snapshot.configRevision != initial) { error = QObject::tr("Native shortcut configuration changed during the read. Refresh again."); return false; }
    const auto endOwner = call(QStringLiteral("org.freedesktop.DBus"), QStringLiteral("/org/freedesktop/DBus"), QStringLiteral("org.freedesktop.DBus"), QStringLiteral("GetNameOwner"), {service});
    if (!endOwner.success || endOwner.value.toString() != snapshot.owner) { error = QObject::tr("The shortcut service owner changed during the read."); return false; }
    std::sort(snapshot.entries.begin(), snapshot.entries.end(), [](const auto &a, const auto &b) { return a.component < b.component; });
    std::sort(snapshot.applications.begin(), snapshot.applications.end(), [](const auto &a, const auto &b) { return a.name.localeAwareCompare(b.name) < 0; });
    snapshot.available = true;
    snapshot.reason = snapshot.writable ? QObject::tr("Definitions are staged until Apply. Assign keys in Global Shortcuts after registration. Edited commands may require a shortcut service or session restart.") : configReason;
    snapshot.revision = CustomShortcutsTypes::revision(snapshot); return true;
}
KConfigGroup actionGroup(KConfig &config, const ShortcutActionId &id)
{
    KConfigGroup group = id.component.endsWith(QLatin1String(".desktop")) ? config.group("services").group(id.component) : config.group(id.component);
    return id.context == QLatin1String("default") ? group : group.group(id.context);
}
bool savedMatches(const CustomShortcutsBackendOptions &options, const CustomShortcutsSnapshot &snapshot, QString &error)
{
    KConfig config(options.config, KConfig::SimpleConfig);
    for (const auto &action : snapshot.actions) {
        const auto group = actionGroup(config, action.id); const auto entry = group.readEntry(action.id.action, QStringList{});
        const bool mismatch = action.id.component.endsWith(QLatin1String(".desktop"))
            ? !ShortcutsTypes::sameKeys(group.hasKey(action.id.action) ? nativeKeys(group.readEntry(action.id.action, QString())) : action.defaults, action.current)
            : entry.size() == 3 && (!ShortcutsTypes::sameKeys(nativeKeys(entry[0]), action.current) || !ShortcutsTypes::sameKeys(nativeKeys(entry[1]), action.defaults));
        if (mismatch) { error = QObject::tr("Saved shortcut bindings differ from the daemon registry. Wait for native persistence or restart the shortcut service before applying."); return false; }
    }
    return true;
}
QByteArray registryRevision(const CustomShortcutsSnapshot &snapshot)
{
    ShortcutsSnapshot state; state.owner = snapshot.owner; state.interfaceSignature = snapshot.interfaceSignature; state.actions = snapshot.actions;
    QByteArray bytes; QDataStream stream(&bytes, QIODevice::WriteOnly); stream << ShortcutsTypes::revision(state) << snapshot.contexts;
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
}
bool sameUntouched(const CustomShortcutsSnapshot &before, const CustomShortcutsSnapshot &after, const QSet<QString> &touched)
{
    auto filtered = [&](const auto &source) { auto copy = source; copy.actions.erase(std::remove_if(copy.actions.begin(), copy.actions.end(), [&](const auto &a) { return touched.contains(a.id.component); }), copy.actions.end()); for (const auto &id : touched) copy.contexts.remove(id); return registryRevision(copy); };
    return filtered(before) == filtered(after);
}
bool sameUntouchedRegistry(const NativeRead &before, const NativeRead &after, const QSet<QString> &touched)
{
    if (!sameUntouched(before.snapshot, after.snapshot, touched)) return false;
    auto previousPaths = before.paths, nextPaths = after.paths;
    for (const auto &component : touched) { previousPaths.remove(component); nextPaths.remove(component); }
    return previousPaths == nextPaths;
}
bool allDesktopFiles(const CustomShortcutsBackendOptions &options, QMap<QString, FileImage> &images, QString &error)
{
    images.clear(); QStringList dirs = options.dataDirs; dirs.removeAll(options.dataHome); dirs.prepend(options.dataHome);
    for (const auto &dir : dirs) {
        QDirIterator iterator(dir + QStringLiteral("/applications"), {QStringLiteral("*.desktop")}, QDir::Files | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
        while (iterator.hasNext()) {
            const auto path = iterator.next(); images.insert(path, catalogImage(path));
            if (images.size() > 20000) { error = QObject::tr("The installed desktop-file catalog is too large to validate safely."); return false; }
        }
    }
    return true;
}
bool sameImage(const FileImage &a, const FileImage &b)
{
    return a.exists == b.exists && a.bytes == b.bytes && a.permissions == b.permissions && a.device == b.device && a.inode == b.inode
        && a.links == b.links && a.modifiedSeconds == b.modifiedSeconds && a.modifiedNanoseconds == b.modifiedNanoseconds
        && a.catalogIdentity == b.catalogIdentity && a.usable == b.usable;
}
bool filesMatch(const CustomShortcutsBackendOptions &options, const QMap<QString, FileImage> &expected, QString &error)
{
    QMap<QString, FileImage> actual;
    if (!allDesktopFiles(options, actual, error) || actual.keys() != expected.keys()) { error = QObject::tr("The installed desktop-file catalog changed during Apply."); return false; }
    for (auto it = expected.cbegin(); it != expected.cend(); ++it) if (!sameImage(it.value(), actual.value(it.key()))) {
        error = QObject::tr("An application or command desktop file changed during Apply: %1").arg(it.key()); return false;
    }
    return true;
}
bool diskGuard(const CustomShortcutsBackendOptions &options, const DiskEntries &baseline, const CustomShortcutsSnapshot &snapshot,
               const QSet<QString> &removed, QString &error)
{
    auto before = baseline, after = diskEntries(options.config);
    for (auto it = after.cbegin(); it != after.cend(); ++it) {
        const auto &path = it.key();
        if (path.size() < 2 || path[0] != QLatin1String("services") || !removed.contains(path[1])) continue;
        for (auto entry = it.value().cbegin(); entry != it.value().cend(); ++entry)
            if (!before.value(path).contains(entry.key()) || before.value(path).value(entry.key()) != entry.value()) {
                error = QObject::tr("A native component's saved state changed externally during removal. Further mutations were stopped."); return false;
            }
    }
    auto prune = [&](DiskEntries &entries) {
        for (auto it = entries.begin(); it != entries.end();) {
            const auto &path = it.key();
            if (path.size() >= 2 && path[0] == QLatin1String("services") && removed.contains(path[1])) it = entries.erase(it);
            else ++it;
        }
    };
    prune(before); prune(after);
    // Native persistence may materialize friendly names for existing ordinary
    // components. No other unrelated semantic edit is accepted.
    for (const auto &action : snapshot.actions) if (!action.id.component.endsWith(QLatin1String(".desktop"))) {
        QStringList path{action.id.component}; if (action.id.context != QLatin1String("default")) path << action.id.context;
        const auto label = action.id.context == QLatin1String("default") ? action.componentFriendly : action.contextFriendly;
        if (!before.value(path).contains(QStringLiteral("_k_friendly_name")) && after.value(path).value(QStringLiteral("_k_friendly_name")) == label) {
            after[path].remove(QStringLiteral("_k_friendly_name")); if (after[path].isEmpty()) after.remove(path);
        }
    }
    if (before == after) return true;
    error = QObject::tr("Unrelated native shortcut configuration changed during Apply. Further mutations were stopped."); return false;
}
bool ensureApplicationsDirectory(const CustomShortcutsBackendOptions &options, QString &error)
{
    const auto path = options.dataHome + QStringLiteral("/applications");
    if (QFileInfo::exists(path)) return safeWritable(path + QStringLiteral("/.control-path-check"), error);
    const int fd = directoryFd(options.dataHome);
    if (fd < 0 || (QFileInfo(options.dataHome).permissions() & QFileDevice::WriteOther) || !QFileInfo(options.dataHome).isWritable()) {
        if (fd >= 0) ::close(fd); error = QObject::tr("The user application directory cannot be created safely."); return false;
    }
    const bool ok = ::mkdirat(fd, "applications", 0700) == 0 || errno == EEXIST;
    ::close(fd);
    if (!ok) { error = QObject::tr("The user application directory could not be created."); return false; }
    return safeWritable(path + QStringLiteral("/.control-path-check"), error);
}
bool prepareDesktop(const FileImage &original, const CustomShortcutCommand &command, bool creating, QByteArray &prepared, QString &error)
{
    QTemporaryDir directory; if (!directory.isValid()) { error = QObject::tr("Cannot prepare a private command definition."); return false; }
    const auto path = directory.filePath(QStringLiteral("command.desktop")); QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(original.bytes) != original.bytes.size()) { error = QObject::tr("Cannot stage a command definition."); return false; }
    file.close();
    KDesktopFile desktop(path); auto group = desktop.desktopGroup();
    if (creating) {
        group.writeEntry("Type", "Application"); group.writeEntry("NoDisplay", true); group.writeEntry("StartupNotify", false);
        group.writeEntry("X-KDE-GlobalAccel-CommandShortcut", true);
    }
    group.writeEntry("Name", command.name); group.writeEntry("Exec", command.exec);
    if (!desktop.sync()) { error = QObject::tr("The private command definition could not be written."); return false; }
    FileImage image; if (!bytes(path, image, error)) return false; prepared = image.bytes; return true;
}
struct DefinitionWrite { QString component, path; FileImage original, committed; QByteArray prepared; bool remove = false, done = false; };
CustomShortcutsResult writeNative(const std::shared_ptr<CustomShortcutsTransport> &transport, const CustomShortcutsBackendOptions &options,
                                 const CustomShortcutsSnapshot &expected, const CustomShortcutChanges &requested, const QString &lockedOwner)
{
    CustomShortcutsResult result; QString error; NativeRead current; QElapsedTimer budget; budget.start();
    QList<DefinitionWrite> writes; bool nativeMutated = false; QSet<QString> removed;
    auto fail = [&](const QString &why, bool uncertain = false) {
        bool rolledBack = !nativeMutated;
        if (!nativeMutated) for (auto it = writes.rbegin(); it != writes.rend(); ++it) if (it->done) {
            QString rollbackError; FileImage now;
            if (!bytes(it->path, now, rollbackError) || !sameImage(now, it->committed)
                || !atomicReplace(it->path, it->committed, it->original.bytes, !it->original.exists, rollbackError)) rolledBack = false;
        }
        result.outcome = uncertain ? ShortcutsOutcome::Uncertain : nativeMutated || !rolledBack ? ShortcutsOutcome::Partial : ShortcutsOutcome::Failed;
        NativeRead actual; QString ignored;
        if (readNative(transport, options, actual, ignored)) result.snapshot = actual.snapshot;
        result.error = why; return result;
    };
    if (!readNative(transport, options, current, error)) return fail(error);
    result.snapshot = current.snapshot;
    if (!lockedOwner.isEmpty() && lockedOwner == current.snapshot.owner) return fail(QObject::tr("A native mutation is uncertain. Further changes require a new shortcut service owner."));
    if (current.snapshot.revision != expected.revision || current.snapshot.owner != expected.owner || current.snapshot.interfaceSignature != expected.interfaceSignature)
        return fail(QObject::tr("Applications, commands, native shortcuts, or policy changed before Apply. Revert to reload."));
    if (!CustomShortcutsTypes::validate(current.snapshot, requested, error) || !savedMatches(options, current.snapshot, error)) return fail(error);
    if (requested.isEmpty()) { result.success = true; result.outcome = ShortcutsOutcome::Success; return result; }
    const auto original = current.snapshot; const auto baseline = diskEntries(options.config);
    // doRegister creates a new service component with loadFromService(), not
    // loadSettings(). It would ignore saved overrides for an unregistered ID.
    // Reject that baseline before touching definitions or the native registry.
    KConfig saved(options.config, KConfig::SimpleConfig);
    QStringList prospectiveComponents = requested.addApplications;
    for (const auto &command : requested.addCommands) prospectiveComponents << command.component;
    QList<ShortcutAction> prospectiveActions = original.actions;
    for (const auto &component : prospectiveComponents) {
        const auto group = saved.group("services").group(component);
        if (!group.keyList().isEmpty() || !group.groupList().isEmpty())
            return fail(QObject::tr("This unregistered component has saved native shortcut overrides. Native registration would ignore them; resolve the saved state before registering."));
        const auto *application = original.application(component);
        if (!application) continue; // New command definitions declare no keys.
        for (const auto &action : application->defaults) {
            for (const auto &key : action.defaults) for (const auto &other : prospectiveActions) for (const auto &occupied : other.current)
                if (ShortcutsTypes::conflicts(key, occupied))
                    return fail(QObject::tr("An application's declared native default shortcut is already occupied. Resolve the binding conflict before registering this component."));
            prospectiveActions << action;
        }
    }
    QMap<QString, FileImage> expectedFiles;
    if (!allDesktopFiles(options, expectedFiles, error)) return fail(error);
    auto prepare = [&](CustomShortcutCommand command, bool create, bool remove) {
        DefinitionWrite write; write.component = command.component; write.path = create ? commandPath(options, command.component) : original.find(command.component)->path;
        if (!bytes(write.path, write.original, error)) return false;
        if (create && write.original.exists) { error = QObject::tr("The new command desktop path already exists."); return false; }
        if (!create && !safeWritable(write.path, error)) return false;
        QString normalized;
        if (!remove && (!CustomShortcutsTypes::normalizeCommand(command.name, command.exec, normalized, error))) return false;
        command.exec = normalized;
        if (!remove && !prepareDesktop(write.original, command, create, write.prepared, error)) return false;
        write.remove = remove; writes << write; return true;
    };
    for (const auto &command : requested.addCommands) {
        // A shadowed/hidden file can reserve an ID without entering the picker.
        for (const auto &path : expectedFiles.keys()) {
            QString root; for (const auto &dir : options.dataDirs) if (path.startsWith(dir + QStringLiteral("/applications/"))) { root = dir + QStringLiteral("/applications/"); break; }
            if (!root.isEmpty()) { auto id = path.mid(root.size()); id.replace(QLatin1Char('/'), QLatin1Char('-')); if (id == command.component) return fail(QObject::tr("The native command identity is reserved by an existing desktop file.")); }
        }
        if (!prepare(command, true, false)) return fail(error);
    }
    for (const auto &command : requested.editCommands) if (!prepare(command, false, false)) return fail(error);
    for (const auto &component : requested.removeComponents) if (original.find(component)->command)
        if (!prepare({component, {}, {}}, false, true)) return fail(error);
    NativeRead preflight;
    if (!readNative(transport, options, preflight, error) || preflight.snapshot.revision != original.revision)
        return fail(QObject::tr("Native shortcut state changed while preparing Apply. Nothing was changed."));
    current = preflight;
    auto guard = [&]() {
        if (budget.elapsed() > options.writeBudgetMs) { error = QObject::tr("The native application shortcut update exceeded its time budget."); return false; }
        if (!safeWritable(options.config, error) || !filesMatch(options, expectedFiles, error) || !diskGuard(options, baseline, original, removed, error)) return false;
        KConfig policy(options.config, KConfig::SimpleConfig);
        QStringList registryTargets = requested.addApplications + requested.removeComponents;
        for (const auto &command : requested.addCommands) registryTargets << command.component;
        for (const auto &component : registryTargets) {
            const auto group = policy.group("services").group(component);
            if (group.isImmutable()) { error = QObject::tr("Native application shortcut policy changed during Apply."); return false; }
            for (const auto &context : current.snapshot.contexts.value(component))
                if (context != QLatin1String("default") && group.group(context).isImmutable()) {
                    error = QObject::tr("A native shortcut context is protected by policy."); return false;
                }
            for (const auto &action : original.actions) if (action.id.component == component) {
                const auto context = action.id.context == QLatin1String("default") ? group : group.group(action.id.context);
                if (context.isImmutable() || context.isEntryImmutable(action.id.action)) { error = QObject::tr("A native shortcut action is protected by policy."); return false; }
            }
        }
        NativeRead fresh;
        if (!readNative(transport, options, fresh, error) || !fresh.snapshot.writable) {
            if (error.isEmpty()) error = QObject::tr("The native shortcut owner or registry changed during Apply."); return false;
        }
        if (registryRevision(fresh.snapshot) != registryRevision(current.snapshot) || fresh.paths != current.paths) {
            QSet<QString> pruned;
            for (const auto &component : removed) if (current.paths.contains(component) && !fresh.paths.contains(component)) {
                const bool hadDefault = std::any_of(current.snapshot.actions.cbegin(), current.snapshot.actions.cend(), [&](const auto &action) {
                    return action.id.component == component && action.id.context == QLatin1String("default");
                });
                if (!hadDefault) pruned.insert(component);
            }
            if (pruned.isEmpty() || !sameUntouchedRegistry(current, fresh, pruned)) {
                error = QObject::tr("The native shortcut owner or registry changed during Apply."); return false;
            }
        }
        current = fresh; return true;
    };
    int commitIndex = 0;
    for (auto &write : writes) {
        if (options.isolated && options.beforeCommit) options.beforeCommit(commitIndex, write.path);
        if (!guard()) return fail(error);
        if (!write.original.exists && !ensureApplicationsDirectory(options, error)) return fail(error);
        if (!atomicReplace(write.path, write.original, write.prepared, write.remove, error)) return fail(error);
        write.done = true;
        if (!bytes(write.path, write.committed, error)) return fail(QObject::tr("A command definition was written but its identity could not be verified."));
        if (write.remove) expectedFiles.remove(write.path); else expectedFiles.insert(write.path, catalogImage(write.path));
        if (options.isolated && options.afterCommit) options.afterCommit(commitIndex, write.path);
        ++commitIndex;
    }
    if (!guard()) return fail(error);
    QStringList additions = requested.addApplications;
    for (const auto &command : requested.addCommands) additions << command.component;
    for (const auto &component : additions) {
        if (!guard()) return fail(error);
        const auto *app = current.snapshot.application(component);
        if (!app) return fail(QObject::tr("The prepared desktop definition cannot be parsed as a native application."));
        const auto source = *app;
        // The audited KCM registers a dummy action only to parse the desktop
        // service, then unregisters that action without registering any keys.
        nativeMutated = true;
        const auto registered = nativeCall(transport, original.owner, rootPath, rootInterface, QStringLiteral("doRegister"), {QStringList{component, QString(), source.name, QString()}});
        if (!registered.success) return fail(registered.error, registered.uncertain);
        const auto unregister = nativeCall(transport, original.owner, rootPath, rootInterface, QStringLiteral("unregister"), {component, QString()});
        if (!unregister.success || !unregister.value.toBool()) return fail(unregister.error.isEmpty() ? QObject::tr("The native dummy action was not unregistered.") : unregister.error, unregister.uncertain);
        QElapsedTimer registration; registration.start(); NativeRead after;
        bool accepted = false;
        do {
            if (readNative(transport, options, after, error)) {
                if (after.snapshot.owner != original.owner || !sameUntouchedRegistry(current, after, {component})) break;
                const auto *entry = after.snapshot.find(component);
                if (entry && entry->registered) {
                    QList<ShortcutAction> target;
                    for (const auto &action : after.snapshot.actions) if (action.id.component == component) target << action;
                    accepted = target.size() == source.defaults.size();
                    for (const auto &expectedAction : source.defaults) {
                        auto found = std::find_if(target.cbegin(), target.cend(), [&](const auto &a) { return a.id == expectedAction.id; });
                        if (found == target.cend() || !ShortcutsTypes::sameKeys(found->defaults, expectedAction.defaults) || !ShortcutsTypes::sameKeys(found->current, expectedAction.defaults)) accepted = false;
                    }
                    if (accepted) break;
                }
            }
            QThread::msleep(50);
        } while (registration.elapsed() < options.persistenceBudgetMs && budget.elapsed() < options.writeBudgetMs);
        if (!accepted) return fail(error.isEmpty() ? QObject::tr("The application component was not accepted with its complete native actions and defaults. Refresh to inspect partial registration.") : error);
        current = after;
        if (!filesMatch(options, expectedFiles, error) || !diskGuard(options, baseline, original, removed, error)) return fail(error);
    }
    for (const auto &component : requested.removeComponents) {
        if (!guard()) return fail(error);
        auto componentActions = [&](const CustomShortcutsSnapshot &snapshot) {
            ShortcutsSnapshot state;
            for (const auto &action : snapshot.actions) if (action.id.component == component) state.actions << action;
            return state;
        };
        auto acceptRemovalReadback = [&](const NativeRead &before, const NativeRead &after, const QString &removedAction) {
            if (after.snapshot.owner != original.owner || !sameUntouchedRegistry(before, after, {component})) return false;
            if (!after.paths.contains(component)) return true; // Native persistence pruned the whole intended component.
            if (after.paths.value(component) != before.paths.value(component) || after.contexts.value(component) != before.contexts.value(component)) return false;
            auto expected = componentActions(before.snapshot);
            expected.actions.erase(std::remove_if(expected.actions.begin(), expected.actions.end(), [&](const auto &action) { return action.id.action == removedAction; }), expected.actions.end());
            return ShortcutsTypes::revision(expected) == ShortcutsTypes::revision(componentActions(after.snapshot));
        };
        if (current.paths.contains(component)) {
            removed.insert(component);
            const auto initialActions = componentActions(current.snapshot).actions;
            if (current.contexts.value(component) == QStringList{QStringLiteral("default")} && !initialActions.isEmpty()) {
                // Keep the exact native KCM path for ordinary single-context services.
                nativeMutated = true;
                const auto cleanup = nativeCall(transport, original.owner, current.paths.value(component), componentInterface, QStringLiteral("cleanUp"));
                if (!cleanup.success || !cleanup.value.toBool()) return fail(cleanup.error.isEmpty() ? QObject::tr("The native component rejected cleanup.") : cleanup.error, cleanup.uncertain);
                NativeRead after;
                if (!readNative(transport, options, after, error) || after.paths.contains(component)
                    || !sameUntouchedRegistry(current, after, {component})) return fail(error.isEmpty() ? QObject::tr("Native component cleanup was not fully verified.") : error);
                current = after;
            } else {
                bool scheduledPrune = false;
                while (current.paths.contains(component)) {
                    if (!guard()) return fail(error);
                    if (!current.paths.contains(component)) break;
                    const auto actions = componentActions(current.snapshot).actions;
                    if (actions.isEmpty()) {
                        // Empty contexts have no unregister target and cleanUp returns false.
                        // An empty default dummy schedules native writeSettings, which prunes
                        // the actionless component and every saved context/group together.
                        nativeMutated = true;
                        const auto registered = nativeCall(transport, original.owner, rootPath, rootInterface, QStringLiteral("doRegister"),
                            {QStringList{component + QStringLiteral("|default"), QString(), original.find(component)->name, QString()}});
                        if (!registered.success) return fail(registered.error, registered.uncertain);
                        NativeRead withDummy;
                        if (!readNative(transport, options, withDummy, error) || !sameUntouchedRegistry(current, withDummy, {component})
                            || withDummy.contexts.value(component) != current.contexts.value(component)) return fail(error.isEmpty() ? QObject::tr("The native cleanup dummy could not be verified.") : error);
                        const auto dummyActions = componentActions(withDummy.snapshot).actions;
                        if (dummyActions.size() != 1 || !dummyActions.first().id.action.isEmpty() || dummyActions.first().id.context != QLatin1String("default")
                            || !ShortcutsTypes::sameKeys(dummyActions.first().current, {}) || !ShortcutsTypes::sameKeys(dummyActions.first().defaults, {}))
                            return fail(QObject::tr("Native cleanup registered unexpected actions or keys."));
                        current = withDummy;
                    }
                    auto targets = componentActions(current.snapshot).actions;
                    QSet<QString> defaultNames;
                    for (const auto &action : targets) if (action.id.context == QLatin1String("default")) defaultNames.insert(action.id.action);
                    std::sort(targets.begin(), targets.end(), [&](const auto &a, const auto &b) {
                        const bool aDefault = defaultNames.contains(a.id.action), bDefault = defaultNames.contains(b.id.action);
                        if (aDefault != bDefault) return !aDefault; // Remove nondefault-only names before native default-empty pruning.
                        return a.id < b.id;
                    });
                    const auto target = targets.first(); const auto before = current;
                    if (!guard()) return fail(error);
                    if (!current.paths.contains(component)) break;
                    nativeMutated = true;
                    const auto unregistered = nativeCall(transport, original.owner, rootPath, rootInterface, QStringLiteral("unregister"),
                        {component + QLatin1Char('|') + target.id.context, target.id.action});
                    if (!unregistered.success) return fail(unregistered.error, unregistered.uncertain);
                    NativeRead after;
                    if (!readNative(transport, options, after, error) || !acceptRemovalReadback(before, after, target.id.action))
                        return fail(error.isEmpty() ? QObject::tr("Whole-component unregister readback changed an unexpected action, context, or service.") : error);
                    if (!unregistered.value.toBool() && after.paths.contains(component)) return fail(QObject::tr("The native action rejected unregister."));
                    current = after; scheduledPrune = true;
                    if (componentActions(current.snapshot).actions.isEmpty()) break;
                }
                if (scheduledPrune && current.paths.contains(component)) {
                    QElapsedTimer pruning; pruning.start();
                    do {
                        if (budget.elapsed() > options.writeBudgetMs || !filesMatch(options, expectedFiles, error)
                            || !diskGuard(options, baseline, original, removed, error)) return fail(error.isEmpty() ? QObject::tr("Native component pruning exceeded its time budget.") : error);
                        NativeRead after;
                        if (!readNative(transport, options, after, error) || !sameUntouchedRegistry(current, after, {component})
                            || !componentActions(after.snapshot).actions.isEmpty()
                            || (after.paths.contains(component) && after.contexts.value(component) != current.contexts.value(component)))
                            return fail(error.isEmpty() ? QObject::tr("The native component changed while waiting for complete removal.") : error);
                        current = after;
                        if (!current.paths.contains(component)) break;
                        QThread::msleep(50);
                    } while (pruning.elapsed() < options.persistenceBudgetMs && budget.elapsed() < options.writeBudgetMs);
                    if (current.paths.contains(component)) return fail(QObject::tr("Native unregister was accepted, but the complete component was not durably pruned."));
                }
            }
        }
        if (current.paths.contains(component) || current.snapshot.find(component)) return fail(QObject::tr("Whole-component removal was not fully verified."));
    }
    QElapsedTimer persistence; persistence.start();
    do {
        if (!guard()) return fail(error);
        bool removedFromDisk = true;
        const auto disk = diskEntries(options.config);
        for (auto it = disk.cbegin(); it != disk.cend(); ++it) if (it.key().size() >= 2 && it.key()[0] == QLatin1String("services") && removed.contains(it.key()[1])) removedFromDisk = false;
        if (removedFromDisk) break;
        if (persistence.elapsed() >= options.persistenceBudgetMs) return fail(QObject::tr("Component cleanup was accepted, but native persistence was not verified."));
        QThread::msleep(50);
    } while (budget.elapsed() < options.writeBudgetMs);
    for (const auto &command : requested.addCommands + requested.editCommands.values()) {
        const auto *entry = current.snapshot.find(command.component); QString normalized;
        if (!CustomShortcutsTypes::normalizeCommand(command.name, command.exec, normalized, error) || !entry || entry->name != command.name || entry->exec != normalized || !entry->command)
            return fail(QObject::tr("The saved command definition did not match independent native readback."));
    }
    if (budget.elapsed() > options.writeBudgetMs) return fail(QObject::tr("Native persistence exceeded its time budget."));
    if (!savedMatches(options, current.snapshot, error)) return fail(error);
    result.snapshot = current.snapshot; result.success = true; result.outcome = ShortcutsOutcome::Success; return result;
}
}
std::shared_ptr<CustomShortcutsTransport> createCustomShortcutsDBusTransport(const QDBusConnection &bus)
{ return std::make_shared<DBusTransport>(bus); }
KdeCustomShortcutsBackend::KdeCustomShortcutsBackend(QObject *parent)
    : KdeCustomShortcutsBackend(createCustomShortcutsDBusTransport(QDBusConnection::sessionBus()), liveOptions(), parent) {}
KdeCustomShortcutsBackend::KdeCustomShortcutsBackend(std::shared_ptr<CustomShortcutsTransport> transport, const CustomShortcutsBackendOptions &options, QObject *parent)
    : CustomShortcutsBackend(parent), m_transport(std::move(transport)), m_options(options)
{
    Q_ASSERT(m_transport); qRegisterMetaType<CustomShortcutsSnapshot>(); qRegisterMetaType<ShortcutsOutcome>();
    m_debounce.setSingleShot(true); m_debounce.setInterval(150);
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, [this] { m_debounce.start(); });
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { m_debounce.start(); });
    connect(&m_debounce, &QTimer::timeout, this, [this] { watchFiles(); emit externalChange(); });
    connect(&m_future, &QFutureWatcher<CustomShortcutsResult>::finished, this, [this] {
        auto result = m_future.result(); const auto job = m_active; m_activeJob = false;
        if (!result.snapshot.owner.isEmpty() && result.snapshot.owner != m_lockedOwner) m_lockedOwner.clear();
        if (job.write && result.outcome == ShortcutsOutcome::Uncertain) m_lockedOwner = job.expected.owner;
        if (!result.snapshot.owner.isEmpty() && result.snapshot.owner == m_lockedOwner) {
            result.snapshot.writable = false;
            result.snapshot.reason = tr("A native mutation is uncertain. Further changes require a new shortcut service owner.");
            for (auto &entry : result.snapshot.entries) entry.editable = entry.removable = false;
            result.snapshot.revision = CustomShortcutsTypes::revision(result.snapshot);
        }
        watchFiles(); QPointer<KdeCustomShortcutsBackend> guard(this);
        if (job.write) emit writeFinished(job.tokens.first(), result.outcome, result.snapshot, result.error);
        else for (const auto token : job.tokens) { emit readFinished(token, result.success, result.snapshot, result.error); if (!guard) return; }
        if (guard) startNext();
    });
    watchFiles();
}
void KdeCustomShortcutsBackend::watchFiles()
{
    QStringList paths{m_options.config};
    for (const auto &dir : m_options.dataDirs) paths << dir + QStringLiteral("/applications");
    paths << m_options.dataHome + QStringLiteral("/applications");
    for (const auto &path : paths) {
        QFileInfo info(path);
        if (info.exists() && info.isFile() && !m_watcher.files().contains(path)) m_watcher.addPath(path);
        QString directory = info.isDir() ? path : info.absolutePath();
        while (!QFileInfo::exists(directory) && directory != QDir::rootPath()) directory = QFileInfo(directory).absolutePath();
        if (!m_watcher.directories().contains(directory)) m_watcher.addPath(directory);
    }
}
void KdeCustomShortcutsBackend::readCurrent(quint64 token)
{
    if (!m_queue.isEmpty() && !m_queue.last().write) m_queue.last().tokens << token;
    else { CustomShortcutsJob job; job.tokens << token; m_queue.enqueue(job); }
    startNext();
}
void KdeCustomShortcutsBackend::writeChanges(quint64 token, const CustomShortcutsSnapshot &expected, const CustomShortcutChanges &changes)
{ CustomShortcutsJob job; job.tokens << token; job.write = true; job.expected = expected; job.changes = changes; m_queue.enqueue(job); startNext(); }
void KdeCustomShortcutsBackend::startNext()
{
    if (m_activeJob || m_queue.isEmpty()) return;
    m_activeJob = true; m_active = m_queue.dequeue();
    const auto transport = m_transport; const auto options = m_options; const auto job = m_active; const auto lockedOwner = m_lockedOwner;
    m_future.setFuture(QtConcurrent::run([transport, options, job, lockedOwner] {
        if (job.write) return writeNative(transport, options, job.expected, job.changes, lockedOwner);
        CustomShortcutsResult result; NativeRead native;
        result.success = readNative(transport, options, native, result.error); result.snapshot = native.snapshot; return result;
    }));
}
