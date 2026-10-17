#include "UserFontService.h"
#include "operations/OperationManager.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QProcess>
#include <QRawFont>
#include <QStandardPaths>
#include <QSet>
#include <QTimer>
#include <QUuid>
#include <algorithm>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
constexpr qint64 maxFontBytes = 32 * 1024 * 1024;
struct Fd {
    int value = -1;
    explicit Fd(int v = -1) : value(v) {}
    ~Fd() { if (value >= 0) ::close(value); }
    Fd(const Fd &) = delete;
    Fd &operator=(const Fd &) = delete;
};
QString hash(const QByteArray &bytes) { return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()); }
// Walk from / with descriptors, refusing every symlink component, including
// XDG_DATA_HOME. No operation ever resolves an unchecked child path.
int openDirectory(const QString &path, bool create)
{
    if (!QDir::isAbsolutePath(path) || QDir::cleanPath(path) != path) return -1;
    int fd = ::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    const auto parts = path.split('/', Qt::SkipEmptyParts);
    for (const QString &part : parts) {
        const QByteArray name = QFile::encodeName(part);
        int next = ::openat(fd, name.constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (next < 0 && create && errno == ENOENT) {
            if (::mkdirat(fd, name.constData(), 0700) == 0)
                next = ::openat(fd, name.constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        }
        ::close(fd);
        fd = next;
        if (fd < 0) return -1;
    }
    struct stat st {};
    if (::fstat(fd, &st) || st.st_uid != ::getuid() || (st.st_mode & 0022)) { ::close(fd); return -1; }
    return fd;
}
bool readRegular(int dir, const QByteArray &name, QByteArray *bytes, struct stat *identity = nullptr)
{
    Fd file(::openat(dir, name.constData(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
    struct stat st {};
    if (file.value < 0 || ::fstat(file.value, &st) || !S_ISREG(st.st_mode) || st.st_uid != ::getuid()
        || st.st_nlink != 1 || st.st_size < 0 || st.st_size > maxFontBytes) return false;
    QFile stream;
    if (!stream.open(file.value, QIODevice::ReadOnly, QFileDevice::DontCloseHandle)) return false;
    *bytes = stream.read(maxFontBytes + 1);
    if (bytes->size() != st.st_size) return false;
    if (identity) *identity = st;
    return true;
}
QJsonObject registry(int dir, bool *ok)
{
    struct stat st {};
    if (::fstatat(dir, ".registry.json", &st, AT_SYMLINK_NOFOLLOW) && errno == ENOENT) { *ok = true; return {}; }
    QByteArray bytes;
    *ok = readRegular(dir, ".registry.json", &bytes);
    if (!*ok) return {};
    QJsonParseError error;
    const auto doc = QJsonDocument::fromJson(bytes, &error);
    *ok = error.error == QJsonParseError::NoError && doc.isObject();
    return doc.object();
}
QByteArray temporaryName() { return QByteArray(".pending-") + QUuid::createUuid().toByteArray(QUuid::Id128); }
bool writeFile(int dir, const QByteArray &name, const QByteArray &bytes)
{
    Fd file(::openat(dir, name.constData(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
    if (file.value < 0) return false;
    qint64 done = 0;
    while (done < bytes.size()) {
        const ssize_t n = ::write(file.value, bytes.constData() + done, bytes.size() - done);
        if (n <= 0) { ::unlinkat(dir, name.constData(), 0); return false; }
        done += n;
    }
    if (::fsync(file.value)) { ::unlinkat(dir, name.constData(), 0); return false; }
    return true;
}
bool writeRegistry(int dir, const QJsonObject &data)
{
    const auto temp = temporaryName();
    if (!writeFile(dir, temp, QJsonDocument(data).toJson(QJsonDocument::Compact))) return false;
    const bool ok = ::renameat(dir, temp.constData(), dir, ".registry.json") == 0;
    if (!ok) ::unlinkat(dir, temp.constData(), 0);
    ::fsync(dir);
    return ok;
}
QJsonObject identity(const struct stat &st, const QByteArray &data)
{
    return {{"device", QString::number(st.st_dev)}, {"inode", QString::number(st.st_ino)}, {"hash", hash(data)}};
}
bool matches(const QJsonObject &entry, const struct stat &st, const QByteArray &data)
{
    return !entry.isEmpty() && entry == identity(st, data);
}
bool managedName(const QString &name)
{
    if (name.size() != 68 || !name.endsWith(".ttf")) return false;
    for (const QChar ch : name.left(64)) if (!QStringLiteral("0123456789abcdef").contains(ch)) return false;
    return true;
}
}

UserFontService::UserFontService(const Options &options, QObject *parent) : QObject(parent), m_options(options) {}
UserFontService::~UserFontService()
{
    if (m_process) { m_process->kill(); m_process->waitForFinished(100); }
    OperationManager::instance()->setActivity(this, {}, false);
    if (m_mutation) OperationManager::instance()->releaseMutation(this);
}
UserFontService *UserFontService::instance()
{
    static QPointer<UserFontService> service;
    if (!service) {
        Options options;
        options.dataDirectory = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
        service = new UserFontService(options, QCoreApplication::instance());
    }
    return service;
}
QString UserFontService::managedDirectory() const { return m_options.dataDirectory + QStringLiteral("/fonts/controlpanel"); }
void UserFontService::reject(const QString &message) { m_result = message; emit changed(); }
void UserFontService::refresh()
{
    if (m_busy) return;
    m_busy = true;
    m_result = "Reading installed fonts…";
    OperationManager::instance()->setActivity(this, "Reading installed fonts", true);
    emit changed();
    startInventory();
}
void UserFontService::startInventory() { startProcess(Stage::Inventory); }
void UserFontService::startProcess(Stage stage)
{
    m_stage = stage;
    OperationManager::instance()->setActivity(this, stage == Stage::Cache ? "Refreshing the user font cache" : "Reading installed fonts", true);
    m_output.clear(); m_failed = false; m_processError.clear();
    m_process = new QProcess(this);
    auto *process = m_process;
    m_timer = new QTimer(process);
    m_timer->setSingleShot(true);
    connect(m_timer, &QTimer::timeout, process, [this, process] {
        m_failed = true; m_processError = "The font tool timed out."; process->kill();
    });
    const auto drain = [this, process] {
        const QByteArray bytes = process->readAllStandardOutput();
        process->readAllStandardError();
        if (bytes.size() > m_options.outputLimit - m_output.size()) {
            m_failed = true; m_processError = "The font tool returned too much output."; process->kill();
        } else m_output += bytes;
    };
    connect(process, &QProcess::readyReadStandardOutput, this, drain);
    connect(process, &QProcess::readyReadStandardError, this, [process] { process->readAllStandardError(); });
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart && m_process == process) processFinished(false, process->errorString());
    });
    connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this, process, drain](int code, QProcess::ExitStatus status) {
        if (m_process != process) return;
        drain();
        processFinished(!m_failed && code == 0 && status == QProcess::NormalExit,
                        m_processError.isEmpty() ? "The font tool failed." : m_processError);
    });
    m_timer->start(qMax(1, m_options.timeoutMs));
    if (stage == Stage::Inventory)
        process->start(m_options.listProgram, {QStringLiteral("--format=%{family[0]}\t%{file}\n")});
    else
        process->start(m_options.cacheProgram, {QStringLiteral("-f"), managedDirectory()});
}
void UserFontService::processFinished(bool success, const QString &error)
{
    if (!m_process) return;
    m_timer->stop();
    m_process->deleteLater(); m_process = nullptr; m_timer = nullptr;
    if (m_stage == Stage::Cache) {
        if (!success) m_mutationResult += " Font cache refresh failed: " + error;
        startInventory();
        return;
    }
    if (success) {
        QList<Family> families;
        QHash<QString, QString> seen;
        QSet<QString> observedPaths;
        for (const QByteArray &line : m_output.split('\n')) {
            const int tab = line.indexOf('\t');
            if (tab < 1) continue;
            const QString name = QString::fromUtf8(line.left(tab)).trimmed();
            const QString file = QString::fromUtf8(line.mid(tab + 1)).trimmed();
            if (name.isEmpty() || !QDir::isAbsolutePath(file)) continue;
            observedPaths.insert(file);
            if (!seen.contains(name) || canRemove(file)) seen.insert(name, file);
        }
        for (auto it = seen.cbegin(); it != seen.cend(); ++it) families.append({it.key(), it.value()});
        std::sort(families.begin(), families.end(), [](const Family &a, const Family &b) {
            return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
        });
        m_families = families; m_inventoryValid = true;
        m_result = m_mutation ? m_mutationResult : QStringLiteral("%1 font families").arg(families.size());
        if (m_mutation && !m_expectedPath.isEmpty()) {
            if (observedPaths.contains(m_expectedPath) == m_expectedRemoval)
                m_result += m_expectedRemoval ? " The font tool still lists this file; removal from the inventory could not be verified."
                                              : " The font tool has not detected the copied file; installation in the inventory could not be verified.";
            else m_result += " Verified by a fresh font inventory.";
        }
    } else {
        m_inventoryValid = false;
        m_result = (m_mutation ? m_mutationResult + " " : QString()) + "Could not verify installed fonts: " + error;
    }
    finish();
}
void UserFontService::finish()
{
    m_stage = Stage::Idle; m_busy = false;
    if (m_mutation) OperationManager::instance()->releaseMutation(this);
    m_mutation = false;
    OperationManager::instance()->setActivity(this, {}, false);
    emit changed();
}
bool UserFontService::beginMutation()
{
    if (m_busy) return false;
    if (!OperationManager::instance()->acquireMutation(this)) { reject("Another change is in progress. Try again when it finishes."); return false; }
    m_busy = true; m_mutation = true;
    OperationManager::instance()->setActivity(this, "Updating user fonts", true);
    m_expectedPath.clear(); m_expectedRemoval = false;
    return true;
}
void UserFontService::refreshCache(const QString &message)
{
    m_mutationResult = message;
    m_result = message + " Refreshing the font cache…";
    emit changed();
    startProcess(Stage::Cache);
}
bool UserFontService::canRemove(const QString &path) const
{
    if (QFileInfo(path).absolutePath() != managedDirectory()) return false;
    const QString name = QFileInfo(path).fileName();
    if (!managedName(name)) return false;
    Fd dir(openDirectory(managedDirectory(), false));
    if (dir.value < 0) return false;
    bool ok = false;
    const auto entries = registry(dir.value, &ok);
    QByteArray data; struct stat st {};
    return ok && readRegular(dir.value, QFile::encodeName(name), &data, &st)
        && matches(entries.value(name).toObject(), st, data);
}
void UserFontService::installFont(const QString &source)
{
    if (!beginMutation()) return;
    const auto fail = [this](const QString &message) { m_result = message; finish(); };
    // Source is opened without following a symlink, then validated from those
    // exact bytes. A suffix alone never authorizes installation.
    Fd src(::open(QFile::encodeName(source).constData(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
    struct stat st {};
    if (src.value < 0 || ::fstat(src.value, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size > maxFontBytes) {
        fail("Choose a regular font file smaller than 32 MiB; symbolic links are unavailable."); return;
    }
    QFile file;
    if (!file.open(src.value, QIODevice::ReadOnly, QFileDevice::DontCloseHandle)) {
        fail("The selected font file could not be read."); return;
    }
    const QByteArray data = file.read(maxFontBytes + 1);
    QRawFont font;
    font.loadFromData(data, 16, QFont::PreferDefaultHinting);
    if (data.size() != st.st_size || !font.isValid() || font.familyName().isEmpty()) { fail("The file does not contain a supported TrueType or OpenType font."); return; }
    Fd dir(openDirectory(managedDirectory(), true));
    if (dir.value < 0) { fail("The current-user font directory is unsafe or unavailable (symlinks and shared writable directories are refused)."); return; }
    bool ok = false;
    auto entries = registry(dir.value, &ok);
    if (!ok) { fail("The managed-font registry is unsafe or unreadable. No font was installed."); return; }
    const QString name = hash(data) + ".ttf";
    const QByteArray target = name.toLatin1(), temp = temporaryName();
    if (!writeFile(dir.value, temp, data)) { fail("Could not copy the font into the current-user font directory."); return; }
    // linkat publishes the completed file atomically and never overwrites.
    if (::linkat(dir.value, temp.constData(), dir.value, target.constData(), 0)) {
        ::unlinkat(dir.value, temp.constData(), 0);
        fail("This font is already installed here, or its destination is unavailable. No file was overwritten."); return;
    }
    ::unlinkat(dir.value, temp.constData(), 0);
    m_expectedPath = managedDirectory() + "/" + name;
    QByteArray copied;
    if (!readRegular(dir.value, target, &copied, &st) || copied != data) { refreshCache("The copied font could not be verified. No removal is authorized for it."); return; }
    entries.insert(name, identity(st, data));
    if (!writeRegistry(dir.value, entries)) { refreshCache("The font file was copied, but its management record could not be saved. Removal is unavailable."); return; }
    refreshCache("Font copied for the current user.");
}
void UserFontService::removeFont(const QString &path)
{
    if (!beginMutation()) return;
    const auto fail = [this](const QString &message) { m_result = message; finish(); };
    if (QFileInfo(path).absolutePath() != managedDirectory() || !managedName(QFileInfo(path).fileName())) {
        fail("Only fonts installed by this Control Panel can be removed. System and other user fonts are unavailable for removal."); return;
    }
    Fd dir(openDirectory(managedDirectory(), false));
    bool ok = false;
    auto entries = dir.value < 0 ? QJsonObject() : registry(dir.value, &ok);
    const QString name = QFileInfo(path).fileName();
    const auto target = QFile::encodeName(name);
    QByteArray data; struct stat st {};
    if (!ok || !readRegular(dir.value, target, &data, &st) || !matches(entries.value(name).toObject(), st, data)) {
        fail("The font changed or is not managed by this Control Panel. No file was removed."); return;
    }
    // Move to an unpredictable private name, then recheck the actual object
    // before unlinking. A racing replacement is preserved, never deleted.
    const auto quarantine = temporaryName();
    if (::renameat(dir.value, target.constData(), dir.value, quarantine.constData())) { fail("Could not prepare the font for removal."); return; }
    QByteArray moved; struct stat movedStat {};
    if (!readRegular(dir.value, quarantine, &moved, &movedStat) || !matches(entries.value(name).toObject(), movedStat, moved)) {
        if (::linkat(dir.value, quarantine.constData(), dir.value, target.constData(), 0) == 0) ::unlinkat(dir.value, quarantine.constData(), 0);
        fail("The font changed during removal. The changed file was preserved in the managed directory."); return;
    }
    if (::unlinkat(dir.value, quarantine.constData(), 0)) { fail("Could not remove the prepared font file."); return; }
    m_expectedPath = managedDirectory() + "/" + name; m_expectedRemoval = true;
    entries.remove(name);
    if (!writeRegistry(dir.value, entries)) { refreshCache("Font file removed; its stale management record could not be cleared."); return; }
    refreshCache("Current-user font file removed.");
}
