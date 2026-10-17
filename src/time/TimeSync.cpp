#include "TimeSync.h"

#include <QFile>
#include <QHostAddress>
#include <QMap>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSet>

namespace TimeSync {
namespace {

struct CommandResult {
    bool ok = false;
    QString output;
};

CommandResult run(const QString &program, const QStringList &arguments)
{
    QProcess process;
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
    environment.insert(QStringLiteral("LANG"), QStringLiteral("C"));
    process.setProcessEnvironment(environment);
    process.start(program, arguments, QIODevice::ReadOnly);
    if (!process.waitForStarted(1000))
        return {};
    if (!process.waitForFinished(2000)) {
        process.kill();
        process.waitForFinished(1000);
        return {};
    }
    return {process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0,
            QString::fromUtf8(process.readAllStandardOutput())};
}

QMap<QString, QString> properties(const QString &text)
{
    QMap<QString, QString> result;
    for (const QString &line : text.split(QLatin1Char('\n'))) {
        const qsizetype equals = line.indexOf(QLatin1Char('='));
        if (equals > 0)
            result.insert(line.left(equals), line.mid(equals + 1).trimmed());
    }
    return result;
}

std::optional<bool> booleanProperty(const QMap<QString, QString> &values,
                                    const QString &key)
{
    const QString value = values.value(key);
    if (value == QStringLiteral("yes"))
        return true;
    if (value == QStringLiteral("no"))
        return false;
    return std::nullopt;
}

QString readFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 1024 * 1024)
        return {};
    return QString::fromUtf8(file.readAll());
}

QStringList meaningfulLines(const QString &text)
{
    QStringList result;
    for (QString line : text.split(QLatin1Char('\n'))) {
        line = line.trimmed();
        if (line.isEmpty() || QStringLiteral("#!;%").contains(line.front()))
            continue;
        result.append(line);
    }
    return result;
}

void addDetail(Status &status, const QString &detail)
{
    if (!status.detail.isEmpty())
        status.detail += QLatin1Char(' ');
    status.detail += detail;
}

} // namespace

QString providerName(Provider provider)
{
    switch (provider) {
    case Provider::Chrony: return QStringLiteral("Chrony");
    case Provider::Timesyncd: return QStringLiteral("systemd-timesyncd");
    case Provider::Unknown: return QStringLiteral("Unknown");
    }
    return QStringLiteral("Unknown");
}

bool validServer(const QString &server)
{
    if (server.isEmpty() || server.size() > 253)
        return false;
    for (const QChar c : server) {
        if (c.unicode() > 127 || !(c.isLetterOrNumber() || c == QLatin1Char('-')
                                || c == QLatin1Char('.') || c == QLatin1Char(':')))
            return false;
    }
    if (server.contains(QLatin1Char(':'))) {
        QHostAddress address;
        return address.setAddress(server) && address.protocol() == QAbstractSocket::IPv6Protocol;
    }
    static const QRegularExpression numericDotted(QStringLiteral("^[0-9.]+$"));
    if (numericDotted.match(server).hasMatch()) {
        // QHostAddress also accepts abbreviated IPv4 forms; accept only four octets.
        const QStringList octets = server.split(QLatin1Char('.'));
        if (octets.size() != 4)
            return false;
        for (const QString &octet : octets) {
            bool ok = false;
            const int value = octet.toInt(&ok);
            if (!ok || octet.size() > 3 || (octet.size() > 1 && octet.front() == QLatin1Char('0'))
                || value < 0 || value > 255)
                return false;
        }
        return true;
    }
    static const QRegularExpression labelPattern(
        QStringLiteral("^[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?$"));
    for (const QString &label : server.split(QLatin1Char('.'))) {
        if (!labelPattern.match(label).hasMatch())
            return false;
    }
    return true;
}

QString chronySourcesDirectory(const QString &configText)
{
    const QString directory = QStringLiteral("/etc/chrony/sources.d");
    for (QString line : meaningfulLines(configText)) {
        // Chrony ignores a comment suffix beginning with one of these characters.
        const qsizetype comment = line.indexOf(QRegularExpression(QStringLiteral("[#!;%]")));
        if (comment >= 0)
            line.truncate(comment);
        const QStringList words = line.split(QRegularExpression(QStringLiteral("\\s+")),
                                             Qt::SkipEmptyParts);
        if (words.size() >= 2 && words.first() == QStringLiteral("sourcedir")
            && words.mid(1).contains(directory))
            return directory;
    }
    return {};
}

QString selectedChronySource(const QString &csv)
{
    QString selected;
    for (const QString &line : csv.split(QLatin1Char('\n'))) {
        const QStringList fields = line.trimmed().split(QLatin1Char(','));
        if (fields.size() < 3 || fields.at(1) != QStringLiteral("*"))
            continue;
        const QString mode = fields.first();
        const QString source = fields.at(2);
        QString candidate;
        if ((mode == QStringLiteral("^") || mode == QStringLiteral("=")) && validServer(source))
            candidate = source;
        else if (mode == QStringLiteral("#") && !source.isEmpty()
                 && !source.contains(QRegularExpression(QStringLiteral("[\\s\\x00-\\x1f]"))))
            candidate = QStringLiteral("Reference clock (%1)").arg(source);
        if (!candidate.isEmpty()) {
            // Multiple selected rows are an inconsistent probe, not a server choice.
            if (!selected.isEmpty())
                return {};
            selected = candidate;
        }
    }
    return selected;
}

QString preferredServer(const QString &configText)
{
    const QStringList lines = meaningfulLines(configText);
    if (lines.size() == 1) {
        const QStringList words = lines.first().split(QRegularExpression(QStringLiteral("\\s+")),
                                                      Qt::SkipEmptyParts);
        if (words.size() == 4 && words.at(0) == QStringLiteral("server")
            && validServer(words.at(1)) && words.at(2) == QStringLiteral("iburst")
            && words.at(3) == QStringLiteral("prefer"))
            return words.at(1);
    }
    const bool timesyncd = lines.size() == 3 && lines.at(0) == QStringLiteral("[Time]")
        && lines.at(1) == QStringLiteral("NTP=")
        && lines.at(2).startsWith(QStringLiteral("NTP="));
    if (timesyncd) {
        const QString server = lines.at(2).mid(4);
        if (validServer(server))
            return server;
    }
    return {};
}

ServiceProbe serviceProbeFromOutput(Provider provider, const QString &requestedService,
                                    const QString &output, bool commandOk)
{
    const auto values = properties(output);
    const QString id = values.value(QStringLiteral("Id"));
    const QString load = values.value(QStringLiteral("LoadState"));
    const QString active = values.value(QStringLiteral("ActiveState"));
    ServiceProbe probe;
    probe.provider = provider;
    probe.service = id.isEmpty() ? requestedService : id;
    // systemctl can return a nonzero exit status for a missing unit while still
    // providing explicit, complete properties. Do not confuse that with a bus error.
    const bool explicitAbsence = id == requestedService
        && load == QStringLiteral("not-found") && active == QStringLiteral("inactive");
    probe.querySucceeded = (commandOk || explicitAbsence) && !id.isEmpty()
        && !load.isEmpty() && !active.isEmpty();
    probe.installed = load == QStringLiteral("loaded");
    probe.active = active == QStringLiteral("active") || active == QStringLiteral("activating")
                   || active == QStringLiteral("reloading");
    return probe;
}

Status statusFromProbes(const QList<ServiceProbe> &services,
                       const QString &timedateOutput, bool timedateOk)
{
    Status status;
    if (timedateOk) {
        const auto values = properties(timedateOutput);
        status.enabled = booleanProperty(values, QStringLiteral("NTP"));
        status.synchronized = booleanProperty(values, QStringLiteral("NTPSynchronized"));
    } else {
        addDetail(status, QStringLiteral("Time synchronization status is unavailable."));
    }

    QList<ServiceProbe> active;
    QList<ServiceProbe> installed;
    QSet<QString> seen;
    bool failed = false;
    for (const ServiceProbe &service : services) {
        if (!service.querySucceeded) {
            failed = true;
            continue;
        }
        if (!service.installed || seen.contains(service.service))
            continue;
        seen.insert(service.service);
        if (service.active)
            active.append(service);
        if (service.provider != Provider::Unknown)
            installed.append(service);
    }
    if (failed)
        addDetail(status, QStringLiteral("Some time service information could not be read."));

    if (active.size() > 1) {
        addDetail(status, QStringLiteral("Multiple time synchronization services are active."));
        return status;
    }
    if (active.size() == 1 && active.first().provider == Provider::Unknown) {
        addDetail(status, QStringLiteral("The active time synchronization service is not supported."));
        return status;
    }
    if (active.size() == 1) {
        status.provider = active.first().provider;
        status.service = active.first().service;
        if (status.enabled == std::optional<bool>(false)) {
            status.enabled.reset();
            addDetail(status, QStringLiteral("The active time service conflicts with the reported NTP disabled state."));
        }
    } else if (status.enabled == std::optional<bool>(true)) {
        status.enabled.reset();
        addDetail(status, QStringLiteral("NTP is reported enabled, but no active time service was identified."));
    } else if (!failed && installed.size() == 1) {
        status.provider = installed.first().provider;
        status.service = installed.first().service;
    } else {
        addDetail(status, installed.size() > 1
                  ? QStringLiteral("The time synchronization provider is ambiguous.")
                  : QStringLiteral("No supported time synchronization provider was identified."));
    }
    return status;
}

Status readStatus()
{
    QList<ServiceProbe> services;
    const QList<QPair<Provider, QString>> candidates = {
        {Provider::Chrony, QStringLiteral("chrony.service")},
        {Provider::Chrony, QStringLiteral("chronyd.service")},
        {Provider::Timesyncd, QStringLiteral("systemd-timesyncd.service")},
        {Provider::Unknown, QStringLiteral("ntp.service")},
        {Provider::Unknown, QStringLiteral("ntpsec.service")},
        {Provider::Unknown, QStringLiteral("ntpd.service")},
        {Provider::Unknown, QStringLiteral("openntpd.service")}
    };
    for (const auto &candidate : candidates) {
        const CommandResult result = run(QStringLiteral("/usr/bin/systemctl"),
            {QStringLiteral("show"), candidate.second, QStringLiteral("--property=Id"),
             QStringLiteral("--property=LoadState"), QStringLiteral("--property=ActiveState")});
        services.append(serviceProbeFromOutput(candidate.first, candidate.second,
                                                result.output, result.ok));
    }
    const CommandResult timedate = run(QStringLiteral("/usr/bin/timedatectl"),
        {QStringLiteral("show"), QStringLiteral("--property=NTP"),
         QStringLiteral("--property=NTPSynchronized")});
    Status status = statusFromProbes(services, timedate.output, timedate.ok);
    if (status.provider == Provider::Chrony) {
        status.canEditServer = !chronySourcesDirectory(
            readFile(QStringLiteral("/etc/chrony/chrony.conf"))).isEmpty();
        status.configuredServer = preferredServer(
            readFile(QStringLiteral("/etc/chrony/sources.d/90-controlpanel.sources")));
        const CommandResult sources = run(QStringLiteral("/usr/bin/chronyc"),
            {QStringLiteral("-c"), QStringLiteral("-N"), QStringLiteral("sources")});
        if (sources.ok)
            status.activeSource = selectedChronySource(sources.output);
        else
            addDetail(status, QStringLiteral("Chrony's selected source could not be read."));
        if (!status.canEditServer)
            addDetail(status, QStringLiteral("The standard Chrony sources directory is not configured."));
    } else if (status.provider == Provider::Timesyncd) {
        status.canEditServer = true;
        status.configuredServer = preferredServer(
            readFile(QStringLiteral("/etc/systemd/timesyncd.conf.d/90-controlpanel.conf")));
        const CommandResult source = run(QStringLiteral("/usr/bin/timedatectl"),
            {QStringLiteral("show-timesync"), QStringLiteral("--property=ServerName"),
             QStringLiteral("--property=ServerAddress")});
        if (source.ok) {
            const auto values = properties(source.output);
            status.activeSource = values.value(QStringLiteral("ServerName"));
            if (status.activeSource.isEmpty())
                status.activeSource = values.value(QStringLiteral("ServerAddress"));
        } else {
            addDetail(status, QStringLiteral("The selected time server could not be read."));
        }
    }
    return status;
}

} // namespace TimeSync
