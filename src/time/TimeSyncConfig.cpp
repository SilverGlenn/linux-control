#include "TimeSyncConfig.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <cerrno>
#include <sys/stat.h>

namespace TimeSyncConfig {
namespace {

Result success() { return {true, {}, {}}; }
Result failure(const QString &message) { return {false, message, {}}; }

bool validService(TimeSync::Provider provider, const QString &service)
{
    return (provider == TimeSync::Provider::Chrony
            && (service == QLatin1String("chrony.service")
                || service == QLatin1String("chronyd.service")))
        || (provider == TimeSync::Provider::Timesyncd
            && service == QLatin1String("systemd-timesyncd.service"));
}

Result writeFile(const QString &path, const QByteArray &content)
{
    if (QFileInfo(path).isSymLink())
        return failure(QStringLiteral("Refusing to replace a symbolic link: %1").arg(path));
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly))
        return failure(QStringLiteral("Could not open %1: %2").arg(path, file.errorString()));
    if (!file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner
                             | QFileDevice::ReadGroup | QFileDevice::ReadOther)
        || file.write(content) != content.size() || !file.commit())
        return failure(QStringLiteral("Could not save %1: %2").arg(path, file.errorString()));
    return success();
}

Result chronyCommand(const QStringList &command, const CommandRunner &run)
{
    // Always address the local administrative socket, never a remote host or
    // an address/command supplied by the caller.
    return run(QStringLiteral("/usr/bin/chronyc"),
               QStringList{QStringLiteral("-h"),
                           QStringLiteral("/run/chrony/chronyd.sock")} + command);
}

} // namespace

bool trustedPath(const QString &path, bool allowMissingLeaf)
{
    if (!QDir::isAbsolutePath(path) || QDir::cleanPath(path) != path)
        return false;
    QString current = path;
    bool leaf = true;
    while (true) {
        struct stat info {};
        const QByteArray encoded = QFile::encodeName(current);
        if (::lstat(encoded.constData(), &info) != 0) {
            if (!leaf || !allowMissingLeaf || errno != ENOENT)
                return false;
        } else {
            if (S_ISLNK(info.st_mode) || info.st_uid != 0 || (info.st_mode & 0022))
                return false;
            if (leaf ? !(S_ISREG(info.st_mode) || S_ISDIR(info.st_mode))
                     : !S_ISDIR(info.st_mode))
                return false;
        }
        if (current == QLatin1String("/"))
            return true;
        current = QFileInfo(current).path();
        leaf = false;
    }
}

Result configure(TimeSync::Provider provider, const QString &service, bool enabled,
                 const QString &server, const Paths &paths,
                 const CommandRunner &run,
                 const std::optional<ServiceState> &previousState)
{
    if (!validService(provider, service))
        return failure(QStringLiteral("Unsupported time synchronization service."));
    if (previousState && previousState->unitFileState != QLatin1String("enabled")
        && previousState->unitFileState != QLatin1String("enabled-runtime")
        && previousState->unitFileState != QLatin1String("disabled"))
        return failure(QStringLiteral("The previous time service state is unknown."));
    if (!server.isEmpty() && !TimeSync::validServer(server))
        return failure(QStringLiteral("Enter a DNS host name or an IPv4/IPv6 address without options or spaces."));

    // Turning synchronization off preserves the configured sources.
    if (!enabled)
        return run(QStringLiteral("/usr/bin/systemctl"),
                   {QStringLiteral("disable"), QStringLiteral("--now"), service});

    const bool chrony = provider == TimeSync::Provider::Chrony;
    const QString path = chrony ? paths.chronySource : paths.timesyncdConfig;
    if (chrony) {
        QFile config(paths.chronyConfig);
        if (!config.open(QIODevice::ReadOnly)
            || TimeSync::chronySourcesDirectory(QString::fromUtf8(config.readAll())).isEmpty())
            return failure(QStringLiteral("Chrony is not configured to read /etc/chrony/sources.d. Its sources were not changed."));
    }
    if (!QFileInfo(QFileInfo(path).path()).isDir())
        return failure(QStringLiteral("The time configuration directory is unavailable: %1").arg(QFileInfo(path).path()));
    if (QFileInfo(path).isSymLink())
        return failure(QStringLiteral("Refusing to modify a symbolic link: %1").arg(path));

    const bool existed = QFileInfo::exists(path);
    QByteArray previous;
    if (existed) {
        QFile original(path);
        if (!original.open(QIODevice::ReadOnly) || original.size() > 65536)
            return failure(QStringLiteral("Could not read the existing time configuration: %1").arg(path));
        previous = original.readAll();
        if (!previous.trimmed().isEmpty()
            && TimeSync::preferredServer(QString::fromUtf8(previous)).isEmpty())
            return failure(QStringLiteral("The existing Control Panel time configuration has unrecognized contents and was left unchanged."));
    }
    const QByteArray content = server.isEmpty() ? QByteArray{}
        : chrony ? QStringLiteral("# Managed by Control Panel. Other sources remain available.\nserver %1 iburst prefer\n").arg(server).toUtf8()
                 : QStringLiteral("# Managed by Control Panel.\n[Time]\nNTP=\nNTP=%1\n").arg(server).toUtf8();
    const bool changed = server.isEmpty() ? existed : (!existed || previous != content);
    if (changed) {
        const Result saved = server.isEmpty()
            ? (QFile::remove(path) ? success() : failure(QStringLiteral("Could not restore default time sources.")))
            : writeFile(path, content);
        if (!saved.ok)
            return saved;
    }

    auto restore = [&](const Result &error) {
        Result restored = success();
        if (changed)
            restored = existed ? writeFile(path, previous)
                               : (QFile::remove(path) ? success() : failure(QStringLiteral("Could not remove the new time configuration.")));
        QStringList rollbackErrors;
        if (!restored.ok)
            rollbackErrors << restored.error;
        auto restoreCommand = [&](const QStringList &args) {
            const Result result = run(QStringLiteral("/usr/bin/systemctl"), args);
            if (!result.ok)
                rollbackErrors << result.error;
            return result.ok;
        };
        if (previousState) {
            // A failed reload must not leave a previously stopped service on,
            // nor change whether it will be started at the next boot.
            const bool serviceRestored = restoreCommand(
                {previousState->active ? QStringLiteral("start") : QStringLiteral("stop"), service});
            if (previousState->unitFileState == QLatin1String("enabled-runtime")) {
                restoreCommand({QStringLiteral("disable"), service});
                restoreCommand({QStringLiteral("enable"), QStringLiteral("--runtime"), service});
            } else {
                restoreCommand({previousState->unitFileState == QLatin1String("enabled")
                                    ? QStringLiteral("enable") : QStringLiteral("disable"), service});
            }
            if (restored.ok && changed && serviceRestored && previousState->active) {
                const Result reload = chrony
                    ? chronyCommand({QStringLiteral("reload"), QStringLiteral("sources")}, run)
                    : run(QStringLiteral("/usr/bin/systemctl"), {QStringLiteral("restart"), service});
                if (!reload.ok)
                    rollbackErrors << reload.error;
            }
        } else if (restored.ok && changed) {
            const Result reload = chrony
                ? chronyCommand({QStringLiteral("reload"), QStringLiteral("sources")}, run)
                : run(QStringLiteral("/usr/bin/systemctl"), {QStringLiteral("try-restart"), service});
            if (!reload.ok)
                rollbackErrors << reload.error;
        }
        return failure(error.error
            + (rollbackErrors.isEmpty()
                ? previousState ? QStringLiteral("\nThe previous server configuration and service state were restored.")
                                : QStringLiteral("\nThe previous server configuration was restored. Synchronization may have been enabled; check its status before retrying.")
                : QStringLiteral("\nSome changes could not be restored: %1\nCheck the server configuration and synchronization state before retrying.")
                      .arg(rollbackErrors.join(QLatin1String("; ")))));
    };

    Result result = run(QStringLiteral("/usr/bin/systemctl"),
                        {QStringLiteral("enable"), QStringLiteral("--now"), service});
    if (!result.ok)
        return restore(result);
    if (chrony)
        result = chronyCommand({QStringLiteral("reload"), QStringLiteral("sources")}, run);
    else if (changed)
        result = run(QStringLiteral("/usr/bin/systemctl"), {QStringLiteral("restart"), service});
    if (!result.ok)
        return restore(result);
    return success();
}

Result refresh(TimeSync::Provider provider, const QString &service,
               const CommandRunner &run)
{
    if (!validService(provider, service))
        return failure(QStringLiteral("Unsupported time synchronization service."));
    if (provider == TimeSync::Provider::Chrony)
        return chronyCommand({QStringLiteral("burst"), QStringLiteral("4/4")}, run);
    return run(QStringLiteral("/usr/bin/systemctl"), {QStringLiteral("restart"), service});
}

} // namespace TimeSyncConfig
