#include "TimeSync.h"
#include "TimeSyncConfig.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QLockFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTextStream>
#include <unistd.h>

namespace {

TimeSyncConfig::Result runCommand(const QString &program, const QStringList &args)
{
    if (!TimeSyncConfig::trustedPath(program))
        return {false, QStringLiteral("Untrusted system command: %1").arg(program), {}};
    QProcess process;
    QProcessEnvironment env;
    env.insert(QStringLiteral("PATH"), QStringLiteral("/usr/sbin:/usr/bin:/sbin:/bin"));
    env.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
    process.setProcessEnvironment(env);
    process.start(program, args);
    if (!process.waitForStarted(3000))
        return {false, QStringLiteral("Could not start %1: %2").arg(program, process.errorString()), {}};
    if (!process.waitForFinished(15000)) {
        process.kill();
        process.waitForFinished(1000);
        return {false, QStringLiteral("%1 timed out. Check the time service status before retrying.").arg(program), {}};
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        const QString error = QString::fromUtf8(process.readAllStandardError()).trimmed();
        return {false, error.isEmpty() ? QStringLiteral("%1 failed (exit %2).").arg(program).arg(process.exitCode()) : error, {}};
    }
    return {true, {}, process.readAllStandardOutput()};
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTextStream err(stderr);
    const QStringList args = app.arguments();
    const bool configure = args.size() == 5 && args[1] == QLatin1String("--configure");
    const bool refresh = args.size() == 3 && args[1] == QLatin1String("--refresh");
    if ((!configure && !refresh)
        || (args[2] != QLatin1String("chrony") && args[2] != QLatin1String("timesyncd"))
        || (configure && args[3] != QLatin1String("on") && args[3] != QLatin1String("off"))
        || (configure && !args[4].isEmpty() && !TimeSync::validServer(args[4]))) {
        err << "Usage: controlpanel-time-helper --configure chrony|timesyncd on|off SERVER\n"
               "       controlpanel-time-helper --refresh chrony|timesyncd\n"
               "SERVER must be a single host name/IP address, or empty for system defaults.\n";
        return 2;
    }
    if (::geteuid() != 0) {
        err << "Administrative authorization is required.\n";
        return 1;
    }
    // readStatus also launches fixed read-only tools. Do not inherit an
    // untrusted PATH or locale from a caller running this helper through sudo.
    qputenv("PATH", "/usr/sbin:/usr/bin:/sbin:/bin");
    qputenv("LC_ALL", "C");
    QLockFile lock(QStringLiteral("/run/lock/controlpanel-time.lock"));
    if (!lock.tryLock(0)) {
        err << "Another time-setting operation is in progress.\n";
        return 1;
    }
    const auto requested = args[2] == QLatin1String("chrony")
        ? TimeSync::Provider::Chrony : TimeSync::Provider::Timesyncd;
    const TimeSync::Status status = TimeSync::readStatus();
    if (status.provider != requested || !status.enabled.has_value()) {
        err << "The time provider changed or could not be verified. Reopen Internet Time and retry.\n";
        return 1;
    }

    TimeSyncConfig::Result result;
    if (configure) {
        TimeSyncConfig::Paths paths;
        std::optional<TimeSyncConfig::ServiceState> previousState;
        if (args[3] == QLatin1String("on")) {
            const auto state = runCommand(QStringLiteral("/usr/bin/systemctl"),
                {QStringLiteral("show"), status.service,
                 QStringLiteral("--property=UnitFileState"), QStringLiteral("--value")});
            const QString unitState = QString::fromUtf8(state.output).trimmed();
            if (!state.ok || (unitState != QLatin1String("enabled")
                              && unitState != QLatin1String("enabled-runtime")
                              && unitState != QLatin1String("disabled"))) {
                err << "The time service enablement state could not be safely preserved. No settings were changed.\n";
                return 1;
            }
            previousState = TimeSyncConfig::ServiceState{*status.enabled, unitState};
            const QString path = requested == TimeSync::Provider::Chrony
                ? paths.chronySource : paths.timesyncdConfig;
            const QString parent = QFileInfo(path).path();
            // Only create the known timesyncd drop-in directory. Chrony's
            // sourcedir must already be configured by its administrator.
            if (requested == TimeSync::Provider::Timesyncd && !QFileInfo::exists(parent)) {
                if (!TimeSyncConfig::trustedPath(QFileInfo(parent).path())
                    || !QDir().mkdir(parent)) {
                    err << "Could not create the time configuration directory.\n";
                    return 1;
                }
            }
            if (!TimeSyncConfig::trustedPath(path, true)
                || (requested == TimeSync::Provider::Chrony
                    && !TimeSyncConfig::trustedPath(paths.chronyConfig))) {
                err << "The time configuration path is not a trusted system path.\n";
                return 1;
            }
        }
        result = TimeSyncConfig::configure(requested, status.service,
                                          args[3] == QLatin1String("on"), args[4], paths,
                                          runCommand, previousState);
    } else {
        if (!*status.enabled) {
            err << "Enable time synchronization before requesting an update.\n";
            return 1;
        }
        result = TimeSyncConfig::refresh(requested, status.service, runCommand);
    }
    if (!result.ok) {
        err << result.error << '\n';
        return 1;
    }
    return 0;
}
