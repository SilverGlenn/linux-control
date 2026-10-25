#include "AppearanceBackend.h"

#include <KConfig>
#include <KConfigGroup>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusServiceWatcher>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QRegularExpression>
#include <QXmlStreamReader>
#include <QPointer>
#include <functional>
#include <memory>
#include <atomic>
#include <QCoreApplication>
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrentRun>
#include <QCryptographicHash>
#include <QDataStream>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include "operations/SettingsFileReservations.h"
#include "operations/OperationManager.h"

namespace {
constexpr auto groupName = "Windows";
constexpr auto keyName = "TitlebarDoubleClickCommand";

void observeCompletion(QDBusPendingCallWatcher *pending, QObject *context,
                       std::function<void()> completion)
{
    // Disconnected buses can return an already-completed call before the
    // finished signal is connected. Both delivery paths share one guard.
    auto completed = std::make_shared<bool>(false);
    const QPointer<QDBusPendingCallWatcher> safePending(pending);
    auto once = [completed, safePending, completion = std::move(completion)] {
        if (*completed || !safePending)
            return;
        *completed = true;
        completion();
    };
    QObject::connect(pending, &QDBusPendingCallWatcher::finished, context, once);
    if (pending->isFinished())
        QTimer::singleShot(0, context, once);
}

class KWinReloadService final : public AppearanceReloadService {
public:
    explicit KWinReloadService(const QDBusConnection &connection, QObject *parent = nullptr)
        : AppearanceReloadService(parent), m_bus(connection)
    {
        auto *watcher = new QDBusServiceWatcher(QStringLiteral("org.kde.KWin"),
            m_bus, QDBusServiceWatcher::WatchForOwnerChange, this);
        connect(watcher, &QDBusServiceWatcher::serviceOwnerChanged, this,
                [this](const QString &, const QString &, const QString &) { probe(); });
        QTimer::singleShot(0, this, [this] { probe(); });
    }
    bool isAvailable() const override { return m_available; }
    QString unavailableReason() const override { return m_reason; }
    QString identity() const override { return m_owner + QLatin1Char('/') + QString::number(m_generation); }
    void requestReload(const QString &action) override
    {
        if (!m_available || m_owner.isEmpty()) {
            emit finished(false, m_reason);
            return;
        }
        const auto generation = m_generation; const auto owner = m_owner;
        confirmOwner(owner, generation, [this, action, generation, owner](bool current) {
            if (!current) {
                emit finished(false, tr("Window behavior was saved, but KWin's owner changed before reload. No reload was sent; Revert to inspect.")); return;
            }
            auto message = QDBusMessage::createMethodCall(owner,
                QStringLiteral("/KWin"), QStringLiteral("org.kde.KWin"), QStringLiteral("reconfigure"));
            if (!m_bus.send(message)) {
                emit finished(false, tr("The setting was saved, but the KWin reload request could not be sent. Revert to reload the saved value.")); return;
            }
            const int expected = action == QLatin1String("Maximize") ? 5000
                : action == QLatin1String("Minimize") ? 5002 : 5018;
            verify(expected, 0, generation);
        });
    }
private:
    void confirmOwner(const QString &owner, quint64 generation, std::function<void(bool)> completion)
    {
        auto message = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.DBus"),
            QStringLiteral("/org/freedesktop/DBus"), QStringLiteral("org.freedesktop.DBus"), QStringLiteral("GetNameOwner"));
        message.setArguments({QStringLiteral("org.kde.KWin")});
        auto *pending = new QDBusPendingCallWatcher(m_bus.asyncCall(message, 1500), this);
        observeCompletion(pending, this, [this, pending, owner, generation, completion = std::move(completion)] {
            const QDBusPendingReply<QString> reply = *pending; pending->deleteLater();
            completion(generation == m_generation && m_available && m_owner == owner && !reply.isError() && reply.value() == owner);
        });
    }
    void probe()
    {
        const quint64 generation = ++m_generation;
        m_available = false;
        m_reason = tr("Checking whether KWin supports reloading window behavior…");
        m_owner.clear();
        emit availabilityChanged();
        auto owner = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.DBus"),
            QStringLiteral("/org/freedesktop/DBus"), QStringLiteral("org.freedesktop.DBus"), QStringLiteral("GetNameOwner"));
        owner.setArguments({QStringLiteral("org.kde.KWin")});
        auto *pending = new QDBusPendingCallWatcher(m_bus.asyncCall(owner, 1500), this);
        observeCompletion(pending, this, [this, pending, generation] {
            const QDBusPendingReply<QString> reply = *pending; pending->deleteLater();
            if (generation != m_generation) return;
            if (reply.isError() || !reply.value().startsWith(QLatin1Char(':'))) {
                m_reason = tr("KWin's window-behavior reload service is unavailable. Apply is disabled.");
                emit availabilityChanged(); return;
            }
            m_owner = reply.value(); probeInterface(generation);
        });
    }
    void probeInterface(quint64 generation)
    {
        auto message = QDBusMessage::createMethodCall(m_owner,
            QStringLiteral("/KWin"), QStringLiteral("org.freedesktop.DBus.Introspectable"),
            QStringLiteral("Introspect"));
        auto *pending = new QDBusPendingCallWatcher(m_bus.asyncCall(message, 2500), this);
        observeCompletion(pending, this,
                [this, pending, generation] {
            const QDBusPendingReply<QString> reply = *pending;
            pending->deleteLater();
            if (generation != m_generation)
                return;
            bool supported = false;
            if (!reply.isError()) {
                QXmlStreamReader xml(reply.value());
                bool kwinInterface = false;
                while (!xml.atEnd()) {
                    xml.readNext();
                    if (xml.isStartElement() && xml.name() == QLatin1String("interface"))
                        kwinInterface = xml.attributes().value(QStringLiteral("name")) == QLatin1String("org.kde.KWin");
                    else if (xml.isEndElement() && xml.name() == QLatin1String("interface"))
                        kwinInterface = false;
                    else if (kwinInterface && xml.isStartElement() && xml.name() == QLatin1String("method")
                             && xml.attributes().value(QStringLiteral("name")) == QLatin1String("reconfigure"))
                        supported = true;
                }
                if (xml.hasError())
                    supported = false;
            }
            if (supported)
                probeReport(generation);
            else {
                m_reason = tr("KWin's window-behavior reload service is unavailable. Apply is disabled.");
                emit availabilityChanged();
            }
        });
    }
    void probeReport(quint64 generation)
    {
        auto message = QDBusMessage::createMethodCall(m_owner,
            QStringLiteral("/KWin"), QStringLiteral("org.kde.KWin"), QStringLiteral("supportInformation"));
        auto *pending = new QDBusPendingCallWatcher(m_bus.asyncCall(message, 1500), this);
        observeCompletion(pending, this, [this, pending, generation] {
            const QDBusPendingReply<QString> reply = *pending;
            pending->deleteLater();
            if (generation != m_generation)
                return;
            int operation = 0;
            m_available = !reply.isError() && parseKWinAppearanceReport(reply.value(), operation);
            m_reason = m_available ? QString()
                : tr("Live window-behavior verification requires KWin 6.6.6's diagnostic interface. Apply is disabled.");
            emit availabilityChanged();
        });
    }
    void verify(int expected, int attempt, quint64 generation)
    {
        // reconfigure schedules a 200ms timer. Poll read-only diagnostics without
        // treating its no-reply method as an acknowledgement or blocking the UI.
        QTimer::singleShot(250, this, [this, expected, attempt, generation] {
            if (generation != m_generation) {
                emit finished(false, tr("The preference was saved, but KWin restarted before its active value could be verified. Revert to reload the saved value."));
                return;
            }
            auto message = QDBusMessage::createMethodCall(m_owner,
                QStringLiteral("/KWin"), QStringLiteral("org.kde.KWin"), QStringLiteral("supportInformation"));
            auto *pending = new QDBusPendingCallWatcher(m_bus.asyncCall(message, 1000), this);
            observeCompletion(pending, this, [this, pending, expected, attempt, generation] {
                const QDBusPendingReply<QString> reply = *pending;
                pending->deleteLater();
                int operation = 0;
                const bool valid = generation == m_generation && !reply.isError()
                    && parseKWinAppearanceReport(reply.value(), operation);
                if (valid && operation == expected) {
                    confirmOwner(m_owner, generation, [this](bool current) {
                        emit finished(current, current ? QString() : tr("The setting was saved, but KWin's owner changed before live verification completed. A sent reload may still complete; Revert to inspect."));
                    });
                } else if (generation == m_generation && attempt < 5) {
                    verify(expected, attempt + 1, generation);
                } else {
                    emit finished(false, tr("The preference was saved, but KWin's active title-bar action could not be verified. Revert to reload the saved value before trying again."));
                }
            });
        });
    }
    QDBusConnection m_bus;
    bool m_available = false;
    quint64 m_generation = 0;
    QString m_owner;
    QString m_reason = tr("Checking KWin's window-behavior reload service…");
};
}

AppearanceReloadService *createKWinAppearanceReloadService(const QDBusConnection &connection, QObject *parent)
{
    return new KWinReloadService(connection, parent);
}

bool parseKWinAppearanceReport(const QString &report, int &operation)
{
    if (report.size() > 65536)
        return false;
    const QStringList lines = report.split(QLatin1Char('\n'));
    if (lines.count(QStringLiteral("KWin version: 6.6.6")) != 1)
        return false;
    const int section = lines.indexOf(QStringLiteral("Options"));
    if (section < 0 || section + 1 >= lines.size() || lines[section + 1] != QLatin1String("======="))
        return false;
    bool found = false;
    const QRegularExpression field(QStringLiteral("^operationTitlebarDblClick: ([0-9]+)$"));
    for (int i = section + 2; i < lines.size() && !lines[i].isEmpty(); ++i) {
        const auto match = field.match(lines[i]);
        if (match.hasMatch()) {
            if (found)
                return false;
            bool ok = false;
            operation = match.captured(1).toInt(&ok);
            if (!ok)
                return false;
            found = true;
        }
    }
    return found;
}

KdeAppearanceBackend::KdeAppearanceBackend(QObject *parent)
    : KdeAppearanceBackend(QStringLiteral("kwinrc"),
                           createKWinAppearanceReloadService(QDBusConnection::sessionBus()), false, parent)
{
}

KdeAppearanceBackend::KdeAppearanceBackend(const QString &absoluteConfigPath,
                                         AppearanceReloadService *reload, QObject *parent)
    : KdeAppearanceBackend(absoluteConfigPath, reload, true, parent)
{
    Q_ASSERT(QFileInfo(absoluteConfigPath).isAbsolute());
}

KdeAppearanceBackend::KdeAppearanceBackend(const QString &configName,
                                         AppearanceReloadService *reload, bool isolated, QObject *parent)
    : AppearanceBackend(parent), m_configName(configName), m_reload(reload), m_isolated(isolated)
{
    Q_ASSERT(reload);
    reload->setParent(this);
    m_userPath = isolated ? configName : QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
        + QStringLiteral("/kwinrc");
    m_watchedFiles << m_userPath;
    if (!isolated) {
        // FullConfig includes global and system defaults/policy. Watch those too.
        for (const auto &dir : QStandardPaths::standardLocations(QStandardPaths::GenericConfigLocation))
            m_watchedFiles << dir + QStringLiteral("/kwinrc") << dir + QStringLiteral("/kdeglobals");
        m_watchedFiles.removeDuplicates();
    }
    auto changed = [this](const QString &) { watchFiles(); m_debounce.start(); };
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, changed);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, changed);
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(80);
    connect(&m_debounce, &QTimer::timeout, this, [this] { emit externalChange(); });
    connect(reload, &AppearanceReloadService::availabilityChanged, this, &AppearanceBackend::externalChange);
    watchFiles();
}

void KdeAppearanceBackend::watchFiles()
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

namespace {
namespace Reservations = SettingsFileReservations;
struct AppearanceFiles {
    QString name, userPath;
    QStringList sources, targets;
    bool isolated = false;
    KdeAppearanceBackend::FileContext fixture;
};
struct ReloadView { bool available = false; QString reason, identity; };
struct AppearanceFileResult {
    bool success = false, saved = false;
    AppearanceState state;
    QString error;
    QByteArray receipt;
    QStringList keys;
    Reservations::Inspection inspection;
};
QByteArray appearanceReceipt(const QStringList &paths, QString &error)
{
    QByteArray bytes; QDataStream stream(&bytes, QIODevice::WriteOnly);
    for (const auto &path : paths) {
        stream << path; const auto encoded = QFile::encodeName(path);
        struct stat link{}, before{}, after{}, current{};
        if (::lstat(encoded.constData(), &link) != 0) {
            if (errno == ENOENT) { stream << false; continue; }
            error = QObject::tr("A window-behavior source cannot be inspected."); return {};
        }
        const int fd = ::open(encoded.constData(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) { error = QObject::tr("A window-behavior source cannot be read."); return {}; }
        QFile file;
        if (!file.open(fd, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) {
            ::close(fd); error = QObject::tr("A window-behavior source cannot be opened safely."); return {};
        }
        constexpr qint64 limit = 2 * 1024 * 1024;
        if (::fstat(fd, &before) != 0 || !S_ISREG(before.st_mode) || before.st_size > limit) {
            error = QObject::tr("A window-behavior source is not a bounded regular file."); return {};
        }
        const auto data = file.read(limit + 1);
        if (file.error() != QFileDevice::NoError || data.size() > limit || ::fstat(fd, &after) != 0
            || ::stat(encoded.constData(), &current) != 0 || current.st_dev != before.st_dev || current.st_ino != before.st_ino
            || after.st_size != before.st_size || after.st_mtim.tv_sec != before.st_mtim.tv_sec
            || after.st_mtim.tv_nsec != before.st_mtim.tv_nsec) {
            error = QObject::tr("A window-behavior source changed during inspection."); return {};
        }
        stream << true << quint64(link.st_dev) << quint64(link.st_ino) << quint64(link.st_mode)
               << quint64(before.st_dev) << quint64(before.st_ino) << quint64(before.st_mode)
               << quint64(before.st_uid) << quint64(before.st_gid)
               << qint64(before.st_mtim.tv_sec) << qint64(before.st_mtim.tv_nsec) << data;
    }
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
}
AppearanceFileResult readAppearanceFiles(const AppearanceFiles &files, const ReloadView &runtime)
{
    AppearanceFileResult result;
    if (files.fixture.fixture && files.fixture.beforeRead) files.fixture.beforeRead();
    const auto before = appearanceReceipt(files.sources, result.error);
    if (before.isEmpty()) return result;
    {
        KConfig config(files.name, files.isolated ? KConfig::SimpleConfig : KConfig::FullConfig);
        KConfigGroup group(&config, groupName);
        result.state.doubleClickAction = group.readEntry(keyName, QStringLiteral("Maximize"));
        result.state.writable = !config.isImmutable() && !group.isImmutable() && !group.isEntryImmutable(keyName)
            && config.isConfigWritable(false);
        result.state.reloadAvailable = runtime.available;
        if (!result.state.writable)
            result.state.unavailableReason = QObject::tr("This window-behavior preference is locked by policy or its configuration is not writable.");
        else if (!runtime.available) result.state.unavailableReason = runtime.reason;
    }
    result.receipt = appearanceReceipt(files.sources, result.error);
    result.success = !result.receipt.isEmpty() && before == result.receipt;
    if (!result.success && result.error.isEmpty()) result.error = QObject::tr("Window-behavior sources changed during reading. Revert to retry.");
    return result;
}
AppearanceFileResult saveAppearanceFiles(const AppearanceFiles &files, const ReloadView &runtime,
    const QString &expected, const QString &requested, const QByteArray &expectedReceipt,
    const Reservations::Reservation &reservation, const QStringList &expectedKeys,
    const std::shared_ptr<std::atomic_bool> &invalidated)
{
    struct MutationEnd { Reservations::Reservation value; ~MutationEnd() { Reservations::endMutation(value); } } end{reservation};
    auto result = readAppearanceFiles(files, runtime);
    if (!result.success) return result;
    result.success = false;
    if (result.receipt != expectedReceipt || result.state.doubleClickAction != expected || !result.state.canApply()
        || Reservations::currentKeys(reservation.paths, &result.error) != expectedKeys || invalidated->load()) {
        if (result.error.isEmpty()) result.error = QObject::tr("Window behavior, policy, file ownership or provider changed before saving. Revert to reload.");
        return result;
    }
    bool synced = false; QByteArray committed;
    {
        KConfig config(files.name, files.isolated ? KConfig::SimpleConfig : KConfig::FullConfig);
        KConfigGroup group(&config, groupName);
        if (config.isImmutable() || group.isImmutable() || group.isEntryImmutable(keyName)
            || group.readEntry(keyName, QStringLiteral("Maximize")) != expected) {
            result.error = QObject::tr("The title-bar preference or policy changed before saving."); return result;
        }
        group.writeEntry(keyName, requested);
        if (files.fixture.fixture && files.fixture.beforeSync) files.fixture.beforeSync();
        const auto boundary = appearanceReceipt(files.sources, result.error);
        if (invalidated->load() || boundary.isEmpty() || boundary != expectedReceipt) {
            config.markAsClean();
            if (result.error.isEmpty()) result.error = QObject::tr("Window-behavior sources or provider changed before synchronization. No write was attempted.");
            return result;
        }
        synced = config.sync(); result.saved = true;
        if (!synced) config.markAsClean();
        if (synced) committed = appearanceReceipt(files.sources, result.error);
        result.keys = Reservations::currentKeys(reservation.paths);
        if (files.fixture.fixture && files.fixture.afterSync) files.fixture.afterSync();
    }
    result.keys << Reservations::currentKeys(reservation.paths); result.keys.removeDuplicates();
    auto actual = readAppearanceFiles(files, runtime); result.state = actual.state; result.receipt = committed;
    if (!synced) result.error = QObject::tr("Window behavior could not be fully saved. Preferences may have changed; no reload was requested. Revert to inspect.");
    else if (!actual.success) result.error = actual.error;
    else if (committed.isEmpty() || actual.receipt != committed || actual.state.doubleClickAction != requested)
        result.error = QObject::tr("Saved window-behavior sources changed or did not match the request. No reload was requested; Revert to inspect.");
    else if (invalidated->load()) result.error = QObject::tr("Window behavior was saved, but its provider changed. No reload was requested; Revert to inspect.");
    else result.success = true;
    return result;
}
class AppearanceFileJob final : public QObject {
public:
    std::function<void(bool, const AppearanceState &, const QString &)> readFinished;
    std::function<void(bool, const QString &)> writeFinished;
    std::function<void(const QString &)> pending;
    AppearanceFileJob(AppearanceFiles files, AppearanceReloadService *service, bool write, QString expected, QString requested)
        : QObject(QCoreApplication::instance()), m_files(std::move(files)), m_service(service), m_write(write),
          m_expected(std::move(expected)), m_requested(std::move(requested))
    {
        service->setParent(this);
        m_view = {service->isAvailable(), service->unavailableReason(), service->identity()};
        connect(service, &AppearanceReloadService::availabilityChanged, this, [this] { m_invalidated->store(true); });
        connect(service, &AppearanceReloadService::finished, this, [this](bool success, const QString &error) {
            if (m_phase != Remote) return;
            m_remoteTimer.stop(); verify(success, error);
        });
        m_deadline.setSingleShot(true); m_deadline.setInterval(m_files.fixture.fixture ? qMax(1, m_files.fixture.softDeadlineMs) : 20000);
        connect(&m_deadline, &QTimer::timeout, this, [this] {
            if (m_phase != Done && pending) pending(tr("Window behavior saving continues in the background. Wait for completion before retrying or closing."));
        });
        m_remoteTimer.setSingleShot(true); m_remoteTimer.setInterval(10000);
        connect(&m_remoteTimer, &QTimer::timeout, this, [this] {
            if (m_phase == Remote) verify(false, tr("The preference was saved, but live verification timed out. A sent reload may still complete. Revert to inspect."));
        });
    }
    void start()
    {
        OperationManager::instance()->setActivity(this, m_write ? tr("Saving window behavior") : tr("Reading window behavior"), true);
        m_deadline.start(); m_phase = Files;
        const auto files = m_files; const auto view = m_view; const auto registry = Reservations::snapshot(); const bool write = m_write;
        worker([files, view, registry, write] {
            auto result = readAppearanceFiles(files, view);
            if (write && result.success) {
                result.inspection = Reservations::inspect(files.targets, registry);
                if (!result.inspection.success) { result.success = false; result.error = result.inspection.error; }
            }
            return result;
        }, [this](AppearanceFileResult result) {
            if (!m_write) { completeRead(result); return; }
            if (!result.success || result.state.doubleClickAction != m_expected || !result.state.canApply() || !providerMatches()) {
                completeWrite(false, !result.error.isEmpty() ? result.error : !result.state.canApply() ? result.state.unavailableReason
                    : tr("Window behavior or provider changed before saving. Revert to reload.")); return;
            }
            QString error; m_reservation = Reservations::acquire(result.inspection, error);
            if (!m_reservation.valid() || !Reservations::beginMutation(m_reservation)) {
                completeWrite(false, error.isEmpty() ? tr("Window-behavior files are owned by another operation.") : error); return;
            }
            const auto files = m_files; const auto view = m_view; const auto expected = m_expected; const auto requested = m_requested;
            const auto receipt = result.receipt; const auto keys = result.inspection.keys; const auto reservation = m_reservation; const auto invalidated = m_invalidated;
            worker([files, view, expected, requested, receipt, keys, reservation, invalidated] {
                return saveAppearanceFiles(files, view, expected, requested, receipt, reservation, keys, invalidated);
            }, [this](AppearanceFileResult saved) {
                m_savedReceipt = saved.receipt;
                if (!saved.keys.isEmpty() && !Reservations::refresh(m_reservation, saved.keys) && saved.error.isEmpty())
                    saved.error = tr("Saved window-behavior file identities could not be retained.");
                if (!saved.success || !saved.error.isEmpty() || !providerMatches()) {
                    completeWrite(false, !saved.error.isEmpty() ? saved.error : tr("Window behavior may be saved, but its provider changed. No reload was requested.")); return;
                }
                boundary();
            });
        });
    }
private:
    enum Phase { Created, Files, Remote, Done } m_phase = Created;
    template<class Work, class Complete> void worker(Work work, Complete complete)
    {
        auto *watcher = new QFutureWatcher<AppearanceFileResult>(this);
        connect(watcher, &QFutureWatcher<AppearanceFileResult>::finished, this, [this, watcher, complete = std::move(complete)] {
            auto result = watcher->result(); watcher->deleteLater(); if (m_phase != Done) complete(std::move(result));
        });
        watcher->setFuture(QtConcurrent::run(std::move(work)));
    }
    bool providerMatches() const { return !m_invalidated->load() && m_service->isAvailable() && m_service->identity() == m_view.identity; }
    void boundary()
    {
        const auto files = m_files; const auto view = m_view;
        worker([files, view] { return readAppearanceFiles(files, view); }, [this](AppearanceFileResult result) {
            if (!result.success || result.receipt != m_savedReceipt || result.state.doubleClickAction != m_requested || !providerMatches()) {
                completeWrite(false, result.error.isEmpty() ? tr("Saved window behavior or provider changed before reload. No reload was requested; Revert to inspect.") : result.error); return;
            }
            m_phase = Remote; m_remoteTimer.start(); m_service->requestReload(m_requested);
        });
    }
    void verify(bool remoteSuccess, const QString &remoteError)
    {
        m_phase = Files; const auto files = m_files; const auto view = m_view;
        worker([files, view] { return readAppearanceFiles(files, view); }, [this, remoteSuccess, remoteError](AppearanceFileResult result) {
            const bool success = remoteSuccess && remoteError.isEmpty() && result.success && result.receipt == m_savedReceipt
                && result.state.doubleClickAction == m_requested && providerMatches();
            completeWrite(success, success ? QString() : !remoteError.isEmpty() ? remoteError : !result.error.isEmpty() ? result.error
                : tr("Window behavior was saved, but its final files or live provider could not be verified. A sent reload may still complete; Revert to inspect."));
        });
    }
    void completeRead(const AppearanceFileResult &result)
    {
        m_phase = Done; m_deadline.stop(); const auto done = std::move(readFinished);
        if (done) done(result.success, result.state, result.error);
        OperationManager::instance()->setActivity(this, {}, false); deleteLater();
    }
    void completeWrite(bool success, const QString &error)
    {
        m_phase = Done; m_deadline.stop(); m_remoteTimer.stop(); Reservations::release(m_reservation);
        const auto done = std::move(writeFinished); if (done) done(success, error);
        OperationManager::instance()->setActivity(this, {}, false); deleteLater();
    }
    AppearanceFiles m_files; AppearanceReloadService *m_service; bool m_write;
    QString m_expected, m_requested; ReloadView m_view; QByteArray m_savedReceipt;
    Reservations::Reservation m_reservation;
    std::shared_ptr<std::atomic_bool> m_invalidated = std::make_shared<std::atomic_bool>(false);
    QTimer m_deadline, m_remoteTimer;
};
}

KdeAppearanceBackend::KdeAppearanceBackend(const QString &path, AppearanceReloadService *service,
    const FileContext &context, QObject *parent) : KdeAppearanceBackend(path, service, parent)
{ if (m_isolated) m_context = context; }

void KdeAppearanceBackend::beginJob(bool write, const QString &expected, const QString &requested)
{
    if (m_job) return;
    if (write && !AppearanceSettings::isSupportedAction(requested)) {
        emit writeFinished(false, tr("This title-bar action is not supported by Control Panel.")); return;
    }
    AppearanceFiles files{m_configName, m_userPath, m_watchedFiles, {m_userPath}, m_isolated, m_context};
    if (!m_isolated) {
        files.targets << QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/kdeglobals");
        for (const auto &dir : QStandardPaths::standardLocations(QStandardPaths::GenericConfigLocation))
            files.sources << dir + QStringLiteral("/system.kdeglobals");
        if (!qEnvironmentVariableIsSet("KDE_SKIP_KDERC")) files.sources << QStringLiteral("/etc/kde5rc");
    }
    files.sources.removeDuplicates();
    files.targets << files.sources; files.targets.removeDuplicates();
    auto *job = new AppearanceFileJob(files, m_reload, write, expected, requested); m_job = job;
    QPointer<KdeAppearanceBackend> safe(this);
    job->pending = [safe](const QString &message) { if (safe) emit safe->operationPending(message); };
    job->readFinished = [safe](bool success, const AppearanceState &state, const QString &error) {
        if (!safe) return; safe->m_reload->setParent(safe); safe->m_job = nullptr; safe->watchFiles(); emit safe->readFinished(success, state, error);
    };
    job->writeFinished = [safe](bool success, const QString &error) {
        if (!safe) return; safe->m_reload->setParent(safe); safe->m_job = nullptr; safe->watchFiles(); emit safe->writeFinished(success, error);
    };
    job->start();
}
void KdeAppearanceBackend::readCurrent() { beginJob(false); }
void KdeAppearanceBackend::writeAction(const QString &expected, const QString &requested) { beginJob(true, expected, requested); }
