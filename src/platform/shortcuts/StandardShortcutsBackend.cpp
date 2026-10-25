#include "StandardShortcutsBackend.h"
#include "operations/SettingsFileReservations.h"
#include "operations/OperationManager.h"
#include <KConfig>
#include <KConfigGroup>
#include <KStandardShortcut>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDataStream>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QPointer>
#include <QTemporaryDir>
#include <QUuid>
#include <QtConcurrent>
#include <algorithm>
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
        error = QObject::tr("Cannot safely read standard shortcut settings: %1").arg(path); return false;
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
    struct stat metadata{};
    const bool unsafeLinks = file.exists() && (::lstat(QFile::encodeName(path).constData(), &metadata) != 0 || metadata.st_nlink != 1);
    if (unsafeLinks || fd < 0 || !dir.isWritable() || (dir.permissions() & QFileDevice::WriteOther)
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
    const QByteArray temp = QByteArray(".control-standard-shortcuts-") + QUuid::createUuid().toByteArray(QUuid::Id128);
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
        receipt->exists=true; receipt->bytes=data;
        receipt->permissions=expected.exists?expected.permissions:QFileDevice::Permissions(QFileDevice::ReadOwner|QFileDevice::WriteOwner|QFileDevice::ReadUser|QFileDevice::WriteUser);
        receipt->device=writtenImage.st_dev; receipt->inode=writtenImage.st_ino; receipt->links=writtenImage.st_nlink;
        receipt->modifiedSeconds=writtenImage.st_mtim.tv_sec; receipt->modifiedNanoseconds=writtenImage.st_mtim.tv_nsec;
    }
    if (!ok) error = QObject::tr("An atomic settings write failed or the file changed: %1").arg(path);
    return ok;
}
QString configPath(const StandardShortcutsBackendOptions &options)
{
    return options.isolated ? options.config
        : QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/kdeglobals");
}
QStringList sources(const StandardShortcutsBackendOptions &options)
{
    if (options.isolated) return options.systemConfigs;
    // KConfigPrivate::getGlobalFiles(), KF6 6.24: /etc/kde5rc, then
    // system.kdeglobals, then kdeglobals, each in increasing specificity.
    QStringList result;
    if (!qEnvironmentVariableIsSet("KDE_SKIP_KDERC")) result << QStringLiteral("/etc/kde5rc");
    auto locations = QStandardPaths::standardLocations(QStandardPaths::GenericConfigLocation);
    std::reverse(locations.begin(), locations.end());
    for (const auto &dir : locations) result << dir + QStringLiteral("/system.kdeglobals");
    const auto local = configPath(options);
    for (const auto &dir : locations) {
        const auto path = dir + QStringLiteral("/kdeglobals");
        if (path != local) result << path;
    }
    result.removeDuplicates(); return result;
}
QString categoryName(KStandardShortcut::Category category)
{
    switch (category) {
    case KStandardShortcut::Category::File: return QStringLiteral("File");
    case KStandardShortcut::Category::Edit: return QStringLiteral("Edit");
    case KStandardShortcut::Category::Navigation: return QStringLiteral("Navigation");
    case KStandardShortcut::Category::View: return QStringLiteral("View");
    case KStandardShortcut::Category::Settings: return QStringLiteral("Settings");
    case KStandardShortcut::Category::Help: return QStringLiteral("Help");
    case KStandardShortcut::Category::InvalidCategory: return {};
    }
    return {};
}
QList<ShortcutAction> catalog()
{
    QList<ShortcutAction> result;
    for (int i = KStandardShortcut::AccelNone + 1; i < KStandardShortcut::StandardShortcutCount; ++i) {
        const auto id = static_cast<KStandardShortcut::StandardShortcut>(i);
        const auto name = KStandardShortcut::name(id), category = categoryName(KStandardShortcut::category(id));
        // Native catalog validity, rather than an application-specific whitelist.
        if (name.isEmpty() || category.isEmpty() || KStandardShortcut::findByName(name) != id) continue;
        ShortcutAction action;
        action.id = {category, QStringLiteral("default"), name};
        action.componentFriendly = QCoreApplication::translate("StandardShortcutsModel", category.toUtf8().constData());
        action.contextFriendly = QObject::tr("Standard application shortcuts");
        action.friendlyName = KStandardShortcut::label(id);
        action.defaults = KStandardShortcut::hardcodedDefaultShortcut(id);
        result << action;
    }
    std::sort(result.begin(), result.end(), [](const auto &a, const auto &b) { return a.id < b.id; });
    return result;
}
bool validOptions(const StandardShortcutsBackendOptions &options, QString &error)
{
    if (!options.isolated) return true;
    if (!QDir::isAbsolutePath(options.config) || options.config.isEmpty()) {
        error = QObject::tr("Isolated standard shortcut configuration requires an absolute path."); return false;
    }
    for (const auto &source : options.systemConfigs)
        if (!QDir::isAbsolutePath(source) || source == options.config) {
            error = QObject::tr("Isolated policy sources require distinct absolute paths."); return false;
        }
    return true;
}
bool configurationRevision(const StandardShortcutsBackendOptions &options, QByteArray &revision, QString &error, bool includeUser = true)
{
    QByteArray data; QDataStream stream(&data, QIODevice::WriteOnly);
    auto paths = sources(options); if (includeUser) paths << configPath(options);
    for (const auto &path : paths) {
        FileImage image; if (!bytes(path, image, error)) return false;
        stream << path << image.exists << image.bytes << quint32(image.permissions)
               << image.device << image.inode << image.links << image.modifiedSeconds << image.modifiedNanoseconds;
    }
    revision = QCryptographicHash::hash(data, QCryptographicHash::Sha256); return true;
}
QList<QKeySequence> nativeKeys(const KConfigGroup &group, const ShortcutAction &action)
{
    if (!group.hasKey(action.id.action)) return action.defaults;
    const auto text = group.readEntry(action.id.action, QString());
    if (text == QLatin1String("none")) return {};
    auto keys = QKeySequence::listFromString(text, QKeySequence::PortableText);
    // initialize() sanitizes repeated alternatives, preserving first occurrence.
    QList<QKeySequence> result;
    for (const auto &key : keys) if (!key.isEmpty() && !result.contains(key)) result << key;
    return result;
}
bool readSnapshot(const StandardShortcutsBackendOptions &options, const QList<ShortcutAction> &entries,
                  ShortcutsSnapshot &snapshot, QString &error)
{
    if (!validOptions(options, error)) return false;
    if (options.isolated && options.beforeRead) options.beforeRead();
    QByteArray initial;
    if (!configurationRevision(options, initial, error)) return false;
    // Never use shortcut() or KSharedConfig: both cache native values. Explicit
    // absolute sources freshly parse policy and defaults, including new files.
    KConfig config(configPath(options), KConfig::NoGlobals);
    config.addConfigSources(sources(options));
    const auto group = config.group(QStringLiteral("Shortcuts"));
    snapshot = {};
    snapshot.owner = QStringLiteral("kde-standard-shortcuts:") + configPath(options);
    snapshot.interfaceSignature = QStringLiteral("KF6.KStandardShortcut/kdeglobals/Shortcuts/portable-list/v1");
    snapshot.actions = entries;
    QString writeReason;
    snapshot.writable = !config.isImmutable() && !group.isImmutable() && safeWritable(configPath(options), writeReason);
    for (auto &action : snapshot.actions) {
        action.current = nativeKeys(group, action);
        action.available = snapshot.writable && !group.isEntryImmutable(action.id.action);
        snapshot.interfaceSignature += QLatin1Char('\n') + action.id.component + QLatin1Char('/') + action.id.action;
    }
    if (!configurationRevision(options, snapshot.configRevision, error)) return false;
    if (initial != snapshot.configRevision) {
        error = QObject::tr("Standard shortcut configuration changed while it was being read. Refresh again."); return false;
    }
    snapshot.available = !snapshot.actions.isEmpty();
    if (!snapshot.available) { error = QObject::tr("The installed KDE standard shortcut catalog is unavailable."); return false; }
    snapshot.reason = snapshot.writable
        ? QObject::tr("Standard bindings are saved in KDE configuration. Applications may need to restart to adopt them.")
        : writeReason.isEmpty() ? QObject::tr("Standard application shortcuts are protected by KDE configuration policy.") : writeReason;
    snapshot.revision = ShortcutsTypes::revision(snapshot); return true;
}
bool stage(const StandardShortcutsBackendOptions &options, const FileImage &image,
           const ShortcutsSnapshot &before, const ShortcutChanges &changes, QByteArray &prepared, QString &error)
{
    QTemporaryDir directory;
    if (!directory.isValid()) { error = QObject::tr("Cannot prepare a private standard shortcut transaction."); return false; }
    const auto path = directory.filePath(QStringLiteral("kdeglobals"));
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(image.bytes) != image.bytes.size()) {
        error = QObject::tr("Cannot prepare standard shortcut settings."); return false;
    }
    file.close();
    KConfig copy(path, KConfig::NoGlobals);
    copy.addConfigSources(sources(options));
    auto group = copy.group(QStringLiteral("Shortcuts"));
    for (auto it = changes.cbegin(); it != changes.cend(); ++it) {
        const auto *action = before.find(it.key());
        if (ShortcutsTypes::sameKeys(action->current, it.value())) continue;
        // saveShortcut() compares the ordered hardcoded defaults, uses
        // deleteEntry (not revertToDefault), otherwise portable listToString.
        // Loading native default sources preserves deletion-marker semantics.
        if (it.value() == action->defaults) group.deleteEntry(it.key().action);
        else group.writeEntry(it.key().action, QKeySequence::listToString(it.value(), QKeySequence::PortableText));
    }
    if (!copy.sync()) { error = QObject::tr("Cannot stage standard shortcut settings."); return false; }
    FileImage staged;
    if (!bytes(path, staged, error)) return false;
    prepared = staged.bytes; return true;
}
bool notify(const StandardShortcutsBackendOptions &options, const QByteArrayList &keys, QString &error)
{
    if (options.isolated) return options.notify ? options.notify(keys, error) : true;
    // KConfigPrivate::notifyClients() wire format, emitted only after verified
    // atomic persistence. No registration, application launch, or invocation.
    qDBusRegisterMetaType<QByteArrayList>();
    qDBusRegisterMetaType<QHash<QString, QByteArrayList>>();
    auto message = QDBusMessage::createSignal(QStringLiteral("/kdeglobals"),
        QStringLiteral("org.kde.kconfig.notify"), QStringLiteral("ConfigChanged"));
    message.setArguments({QVariant::fromValue(QHash<QString, QByteArrayList>{{QStringLiteral("Shortcuts"), keys}})});
    if (QDBusConnection::sessionBus().send(message)) return true;
    error = QObject::tr("Standard bindings were saved, but KDE's change notification could not be sent. Restart applications to adopt them.");
    return false;
}
StandardShortcutsResult writeTransaction(const StandardShortcutsBackendOptions &options, const QList<ShortcutAction> &entries,
                                        const ShortcutsSnapshot &expected, const ShortcutChanges &requested,
                                        const std::function<bool(const QStringList &,QString &)> &retainAliases)
{
    StandardShortcutsResult result;
    QString error;
    auto fail = [&](const QString &why) { result.error = why; return result; };
    if (!readSnapshot(options, entries, result.snapshot, error)) return fail(error);
    const auto before = result.snapshot;
    if (before.revision != expected.revision || before.owner != expected.owner || before.interfaceSignature != expected.interfaceSignature)
        return fail(QObject::tr("Standard shortcuts or their configuration policy changed before Apply. Revert to reload."));
    if (!ShortcutsTypes::validate(before, requested, error)) return fail(error);
    QByteArrayList changed;
    for (auto it = requested.cbegin(); it != requested.cend(); ++it)
        if (!ShortcutsTypes::sameKeys(before.find(it.key())->current, it.value())) changed << it.key().action.toUtf8();
    if (changed.isEmpty()) { result.success = true; result.outcome = ShortcutsOutcome::Success; return result; }
    const auto path = configPath(options);
    FileImage original; QByteArray prepared, policyRevision;
    if (!configurationRevision(options, policyRevision, error, false)) return fail(error);
    if (!bytes(path, original, error) || !stage(options, original, before, requested, prepared, error)) return fail(error);
    if (options.isolated && options.beforeCommit) options.beforeCommit();
    ShortcutsSnapshot preflight;
    if (!readSnapshot(options, entries, preflight, error) || preflight.revision != before.revision) {
        if (preflight.available) result.snapshot = preflight;
        return fail(QObject::tr("Standard shortcut configuration changed while preparing Apply. No settings were written."));
    }
    FileImage committed;
    if (!atomicReplace(path, original, prepared, false, error,&committed)) return fail(error);
    if (options.isolated && options.afterReplaceBeforeRetention) options.afterReplaceBeforeRetention();
    const auto writtenKey=QStringLiteral("inode:%1:%2").arg(committed.device).arg(committed.inode);
    if (!retainAliases({writtenKey},error)) {
        result.outcome=ShortcutsOutcome::Uncertain;
        return fail(QObject::tr("Standard bindings were written, but file identities could not be retained. No notification or rollback was attempted. %1").arg(error));
    }
    // This receipt comes from the actual written descriptor before rename,
    // never a later same-byte foreign replacement during the GUI bridge.
    FileImage observed;
    if (!bytes(path, observed, error) || observed.exists!=committed.exists || observed.bytes!=committed.bytes
        || observed.device!=committed.device || observed.inode!=committed.inode
        || observed.modifiedSeconds!=committed.modifiedSeconds || observed.modifiedNanoseconds!=committed.modifiedNanoseconds) {
        result.outcome = ShortcutsOutcome::Uncertain;
        return fail(QObject::tr("Standard bindings were written, but the saved file could not be verified. Refresh before continuing."));
    }
    if (options.isolated && options.afterCommit) options.afterCommit();
    ShortcutsSnapshot after;
    bool verified = readSnapshot(options, entries, after, error);
    QByteArray afterPolicy;
    verified = verified && configurationRevision(options, afterPolicy, error, false) && afterPolicy == policyRevision;
    if (verified) {
        for (const auto &action : before.actions) {
            const auto desired = requested.contains(action.id) ? requested.value(action.id) : action.current;
            const auto *actual = after.find(action.id);
            if (!actual || !ShortcutsTypes::sameKeys(actual->current, desired) || actual->available != action.available
                || actual->defaults != action.defaults) { verified = false; break; }
        }
    }
    // Configuration/policy changes outside the user file are also verification
    // failures. Never roll back over somebody else's intervening file edit.
    FileImage current;
    bool ownFile = bytes(path, current, error) && current.exists == committed.exists && current.bytes == committed.bytes
        && current.inode == committed.inode && current.device == committed.device
        && current.modifiedSeconds == committed.modifiedSeconds && current.modifiedNanoseconds == committed.modifiedNanoseconds;
    if (!verified || !ownFile) {
        QString rollbackError;
        FileImage rollbackReceipt;
        const bool restored = ownFile && atomicReplace(path, committed, original.bytes, !original.exists, rollbackError,&rollbackReceipt)
            && retainAliases(rollbackReceipt.exists?QStringList{QStringLiteral("inode:%1:%2").arg(rollbackReceipt.device).arg(rollbackReceipt.inode)}:QStringList{},rollbackError);
        readSnapshot(options, entries, result.snapshot, error);
        result.outcome = restored ? ShortcutsOutcome::Failed : ShortcutsOutcome::Partial;
        return fail(restored ? QObject::tr("Standard shortcut verification failed. The original configuration was restored.")
                            : QObject::tr("Standard bindings changed during verification. The current file was preserved; refresh before continuing."));
    }
    result.snapshot = after;
    // Notification belongs to the GUI-owned continuation, which survives the
    // optional facade and retains the reservation until that continuation ends.
    result.notificationKeys=changed;
    result.success = true; result.outcome = ShortcutsOutcome::Success;
    return result;
}
}

KdeStandardShortcutsBackend::KdeStandardShortcutsBackend(QObject *parent)
    : KdeStandardShortcutsBackend(StandardShortcutsBackendOptions{}, parent) {}
KdeStandardShortcutsBackend::KdeStandardShortcutsBackend(const StandardShortcutsBackendOptions &options, QObject *parent)
    : ShortcutsBackend(parent), m_options(options), m_catalog(catalog())
{
    qRegisterMetaType<ShortcutsSnapshot>(); qRegisterMetaType<ShortcutsOutcome>();
    m_debounce.setSingleShot(true); m_debounce.setInterval(150);
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, [this] { m_debounce.start(); });
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { m_debounce.start(); });
    connect(&m_debounce, &QTimer::timeout, this, [this] { watchFiles(); emit externalChange(); });
    watchFiles();
}
void KdeStandardShortcutsBackend::watchFiles()
{
    auto paths = sources(m_options); paths << configPath(m_options);
    for (const auto &path : paths) {
        if (QFileInfo::exists(path) && !m_watcher.files().contains(path)) m_watcher.addPath(path);
        auto parent = QFileInfo(path).absolutePath();
        while (!QFileInfo::exists(parent) && parent != QDir::rootPath()) parent = QFileInfo(parent).absolutePath();
        if (!m_watcher.directories().contains(parent)) m_watcher.addPath(parent);
    }
}
void KdeStandardShortcutsBackend::readCurrent(quint64 token)
{
    if (!m_queue.isEmpty() && !m_queue.last().write) m_queue.last().tokens << token;
    else { StandardShortcutsJob job; job.tokens << token; m_queue.enqueue(job); }
    startNext();
}
void KdeStandardShortcutsBackend::writeChanges(quint64 token, const ShortcutsSnapshot &expected, const ShortcutChanges &requested)
{
    StandardShortcutsJob job; job.tokens << token; job.write = true; job.expected = expected; job.requested = requested;
    m_queue.enqueue(job); startNext();
}
void KdeStandardShortcutsBackend::cancel()
{
    const auto queued=m_queue; m_queue.clear(); QPointer<KdeStandardShortcutsBackend> guard(this);
    for (const auto &job:queued) for (const auto token:job.tokens) {
        const auto error=tr("Queued standard-shortcut work was canceled before it started. No changes were written by that job.");
        if (job.write) emit writeFinished(token,ShortcutsOutcome::Failed,job.expected,error);
        else emit readFinished(token,false,{},error);
        if (!guard) return;
    }
    if (m_jobActive) emit operationPending(tr("Standard-shortcut work is already running and continues until its actual completion. It has not been canceled."));
}
void KdeStandardShortcutsBackend::startNext()
{
    if (m_jobActive || m_queue.isEmpty()) return;
    m_jobActive=true; m_active=m_queue.dequeue();
    const auto options=m_options; const auto entries=m_catalog; const auto job=m_active;
    auto *work=new QObject(QCoreApplication::instance());
    auto lease=std::make_shared<SettingsFileReservations::Reservation>();
    QPointer<KdeStandardShortcutsBackend> facade(this);
    OperationManager::instance()->setActivity(work,job.write?tr("Saving Application Shortcuts"):tr("Reading Application Shortcuts"),true);
    auto *pendingDeadline=new QTimer(work); pendingDeadline->setSingleShot(true);
    pendingDeadline->setInterval(options.isolated?qMax(1,options.softDeadlineMs):10000);
    connect(pendingDeadline,&QTimer::timeout,work,[facade] {
        if (facade) emit facade->operationPending(QObject::tr("Standard-shortcut work is taking longer than expected. It continues in the background; wait for its actual completion."));
    });
    pendingDeadline->start();
    auto finish=[facade,work,lease,job,options,pendingDeadline](StandardShortcutsResult result) {
        pendingDeadline->stop();
        // Keep the app-owned activity and old/new inode aliases through the
        // actual native notification, even when the page/session is destroyed.
        if (job.write && result.success && !result.notificationKeys.isEmpty()) {
            QString error;
            if (!notify(options,result.notificationKeys,error)) {
                result.success=false; result.outcome=ShortcutsOutcome::Partial;
                result.error=error.isEmpty()?QObject::tr("Standard bindings were saved, but KDE's change notification failed. Applications may need to restart."):error;
            }
        }
        SettingsFileReservations::endMutation(*lease); SettingsFileReservations::release(*lease);
        OperationManager::instance()->setActivity(work,{},false);
        if (facade) {
            if (result.snapshot.available) facade->m_revision=result.snapshot.revision;
            facade->m_jobActive=false; facade->watchFiles();
            if (job.write) emit facade->writeFinished(job.tokens.first(),result.outcome,result.snapshot,result.error);
            else for (const auto token:job.tokens) {
                emit facade->readFinished(token,result.success,result.snapshot,result.error);
                if (!facade) break;
            }
            if (facade) facade->startNext();
        }
        work->deleteLater();
    };
    auto run=[options,entries,job,work,lease,finish] {
        auto *future=new QFutureWatcher<StandardShortcutsResult>(work);
        connect(future,&QFutureWatcher<StandardShortcutsResult>::finished,work,[future,finish] {
            const auto result=future->result(); future->deleteLater(); finish(result);
        });
        future->setFuture(QtConcurrent::run([options,entries,job,work,lease] {
            auto retain=[work,lease](const QStringList &writtenKeys,QString &error) {
                auto keys=SettingsFileReservations::currentKeys(lease->paths,&error);
                if (keys.isEmpty()) return false;
                keys<<writtenKeys; keys.removeDuplicates();
                bool retained=false;
                const bool delivered=QMetaObject::invokeMethod(work,[lease,keys,&retained] {
                    retained=SettingsFileReservations::refresh(*lease,keys);
                },Qt::BlockingQueuedConnection);
                if (!delivered || !retained) error=QObject::tr("The standard-shortcut file identities could not be retained on the owning thread.");
                return delivered && retained;
            };
            if (job.write) return writeTransaction(options,entries,job.expected,job.requested,retain);
            StandardShortcutsResult result; result.success=readSnapshot(options,entries,result.snapshot,result.error); return result;
        }));
    };
    if (!job.write) { run(); return; }
    const auto inventory=SettingsFileReservations::snapshot(); auto targets=sources(options); targets<<configPath(options); targets.removeDuplicates();
    auto *admission=new QFutureWatcher<SettingsFileReservations::Inspection>(work);
    connect(admission,&QFutureWatcher<SettingsFileReservations::Inspection>::finished,work,[admission,lease,run,finish,job] {
        const auto inspected=admission->result(); admission->deleteLater(); QString error;
        *lease=SettingsFileReservations::acquire(inspected,error);
        if (!lease->valid() || !SettingsFileReservations::beginMutation(*lease)) {
            StandardShortcutsResult result; result.snapshot=job.expected;
            result.error=error.isEmpty()?QObject::tr("The standard-shortcut file reservation could not be acquired."):error; finish(result); return;
        }
        run();
    });
    admission->setFuture(QtConcurrent::run([targets,inventory] { return SettingsFileReservations::inspect(targets,inventory); }));
}
