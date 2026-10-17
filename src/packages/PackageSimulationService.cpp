#include "PackageSimulationService.h"
#include <QCoreApplication>
#include <QDir>
#include <QTemporaryFile>
#include <QFileInfo>
#include <QHash>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSet>
#include <QTimer>

namespace {
constexpr qsizetype outputLimit = 2 * 1024 * 1024;
class AptSimulationBackend final : public PackageSimulationBackend {
public:
    AptSimulationBackend(const QString &executable, int timeout, QObject *parent)
        : PackageSimulationBackend(parent), m_executable(executable), m_timeoutMs(qBound(10, timeout, 120000))
    {
        m_timer.setSingleShot(true); m_killTimer.setSingleShot(true); m_killTimer.setInterval(250);
        connect(&m_timer, &QTimer::timeout, this, [this] { if (m_process) { m_result.timedOut = true; m_process->kill(); } });
        connect(&m_killTimer, &QTimer::timeout, this, [this] { if (m_process) m_process->kill(); });
    }
    bool isAvailable() const override { return QFileInfo(m_executable).isAbsolute() && QFileInfo(m_executable).isFile() && QFileInfo(m_executable).isExecutable(); }
    QString unavailableReason() const override { return isAvailable() ? QString() : tr("The APT simulation provider is unavailable: %1 is not an executable file.").arg(m_executable); }
    void start(quint64 id) override
    {
        if (m_process) {
            PackageSimulationProcessResult result; result.error = tr("An APT simulation is already running."); emit finished(id, result); return;
        }
        if (!isAvailable()) {
            PackageSimulationProcessResult result; result.error = unavailableReason(); emit finished(id, result); return;
        }
        m_id = id; m_result = {};
        auto *safety = new QTemporaryFile(QDir::tempPath() + QStringLiteral("/linux-control-apt-preview-XXXXXX.conf"), this);
        const QByteArray configuration = aptPackageSimulationSafetyConfiguration();
        if (!safety->open() || safety->write(configuration) != configuration.size() || !safety->flush()) {
            PackageSimulationProcessResult result; result.error = tr("The private read-only APT simulation configuration could not be prepared.");
            delete safety; emit finished(id, result); return;
        }
        m_safety = safety;
        m_probingVersion = true; m_timer.start(m_timeoutMs); startProcess();
    }
    void cancel(quint64 id) override
    {
        if (!m_process || id != m_id) return;
        m_result.cancelled = true; m_timer.stop(); m_process->terminate(); m_killTimer.start();
    }
private:
    void startProcess()
    {
        auto *process = new QProcess(this); m_process = process;
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.remove(QStringLiteral("APT_CONFIG"));
        environment.insert(QStringLiteral("LC_ALL"), QStringLiteral("C")); environment.insert(QStringLiteral("LANG"), QStringLiteral("C"));
        environment.insert(QStringLiteral("LANGUAGE"), QStringLiteral("C")); environment.insert(QStringLiteral("DEBIAN_FRONTEND"), QStringLiteral("noninteractive"));
        process->setProcessEnvironment(environment); process->setWorkingDirectory(QStringLiteral("/"));
        process->setStandardInputFile(QProcess::nullDevice());
        connect(process, &QProcess::readyReadStandardOutput, this, [this] { drain(); });
        connect(process, &QProcess::readyReadStandardError, this, [this] { drain(); });
        connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
            if (m_process != process) return;
            if (error == QProcess::FailedToStart) { m_result.error = process->errorString(); finish(-1, false); }
        });
        connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, [this, process](int code, QProcess::ExitStatus status) {
            if (m_process == process) finish(code, status == QProcess::NormalExit);
        });
        process->start(m_executable, m_probingVersion ? QStringList{QStringLiteral("--version")}
            : aptPackageSimulationArguments(m_safety->fileName()), QIODevice::ReadOnly);
    }
    void drain()
    {
        if (!m_process) return;
        auto append = [this](QByteArray &destination, const QByteArray &bytes) {
            const qsizetype remaining = outputLimit - m_result.standardOutput.size() - m_result.standardError.size();
            if (bytes.size() > remaining) {
                destination.append(bytes.left(qMax(qsizetype(0), remaining)));
                m_result.outputTruncated = true; m_process->kill();
            } else destination.append(bytes);
        };
        append(m_result.standardOutput, m_process->readAllStandardOutput());
        append(m_result.standardError, m_process->readAllStandardError());
    }
    void finish(int code, bool normal)
    {
        drain();
        m_result.exitCode = code; m_result.normalExit = normal;
        auto *process = m_process; m_process = nullptr; process->deleteLater();
        if (m_probingVersion && normal && code == 0 && !m_result.cancelled && !m_result.timedOut && !m_result.outputTruncated && m_result.error.isEmpty()) {
            // Hook order and simulation semantics are audited for this exact
            // release. Unknown versions fail closed before the resolver starts.
            if (m_result.standardError.trimmed().isEmpty() && QRegularExpression(QStringLiteral("^apt 3\\.2\\.0 \\([a-zA-Z0-9_-]+\\)\\n")).match(QString::fromUtf8(m_result.standardOutput)).hasMatch()) {
                m_probingVersion = false; m_result = {}; startProcess(); return;
            }
            m_result.error = tr("This APT version has not been audited for read-only simulation. Supported version: 3.2.0. No package preview was run.");
        }
        m_timer.stop(); m_killTimer.stop();
        delete m_safety; m_safety = nullptr;
        emit finished(m_id, m_result);
    }
    QString m_executable;
    int m_timeoutMs;
    quint64 m_id = 0;
    QProcess *m_process = nullptr;
    bool m_probingVersion = false;
    QTemporaryFile *m_safety = nullptr;
    QTimer m_timer, m_killTimer;
    PackageSimulationProcessResult m_result;
};
}
QByteArray aptPackageSimulationSafetyConfiguration()
{
    // APT 3.2 runs some install/JSON hooks even in simulation. A command-line
    // assignment appends to hook lists; #clear in a post-defaults -c file is
    // necessary to suppress them. The file is private and deleted on completion.
    return "#clear Version;\n"
           "#clear Binary;\n"
           "#clear APT::Install::Pre-Invoke;\n"
           "#clear APT::Install::Post-Invoke;\n"
           "#clear APT::Install::Post-Invoke-Success;\n"
           "#clear AptCli::Hooks;\n"
           "#clear DPkg::Pre-Invoke;\n"
           "#clear DPkg::Pre-Install-Pkgs;\n"
           "#clear DPkg::Post-Invoke;\n"
           "#clear APT::Update::Pre-Invoke;\n"
           "#clear APT::Update::Post-Invoke;\n"
           "#clear APT::Update::Post-Invoke-Success;\n"
           "#clear APT::Update::Post-Invoke-Stats;\n"
           "#clear APT::Update::Auth-Failure;\n";
}
QStringList aptPackageSimulationArguments(const QString &safetyConfig)
{
    Q_ASSERT(QFileInfo(safetyConfig).isAbsolute());
    return {QStringLiteral("-c"), safetyConfig, QStringLiteral("--simulate"), QStringLiteral("--no-download"), QStringLiteral("-o"), QStringLiteral("Debug::NoLocking=true"), QStringLiteral("-o"), QStringLiteral("quiet=1"), QStringLiteral("-o"), QStringLiteral("Binary=apt-get"),
        QStringLiteral("-o"), QStringLiteral("APT::Color=0"), QStringLiteral("-o"), QStringLiteral("APT::Output-Version=29"), QStringLiteral("-o"), QStringLiteral("APT::Solver=3.0"), QStringLiteral("-o"), QStringLiteral("APT::Planner=internal"),
        QStringLiteral("-o"), QStringLiteral("APT::Get::Show-Upgraded=true"), QStringLiteral("-o"), QStringLiteral("APT::Get::Show-Versions=false"),
        QStringLiteral("-o"), QStringLiteral("Dir::Cache::pkgcache="), QStringLiteral("-o"), QStringLiteral("Dir::Cache::srcpkgcache="),
        QStringLiteral("-o"), QStringLiteral("APT::Get::Assume-Yes=false"), QStringLiteral("-o"), QStringLiteral("APT::Get::Simulate=true"),
        QStringLiteral("dist-upgrade")};
}
PackageSimulationBackend *createAptPackageSimulationBackend(QObject *parent)
{ return new AptSimulationBackend(QStringLiteral("/usr/bin/apt-get"), 60000, parent); }
PackageSimulationBackend *createAptPackageSimulationBackend(const QString &path, int timeout, QObject *parent)
{ return new AptSimulationBackend(path, timeout, parent); }
PackageSimulationResult parseAptPackageSimulation(const QByteArray &bytes)
{
    PackageSimulationResult result;
    auto fail = [&result](const QString &error) { result.success = false; result.error = error; result.changes.clear(); return result; };
    if (bytes.isEmpty() || bytes.size() > outputLimit || bytes.contains('\0')) return fail(QObject::tr("APT did not return a complete readable simulation."));
    const QString output = QString::fromUtf8(bytes);
    if (output.contains(QChar::ReplacementCharacter)) return fail(QObject::tr("APT simulation output has invalid text encoding."));
    result.rawOutput = output;
    const QRegularExpression summary(QStringLiteral("^(\\d+) upgraded, (\\d+) newly installed, (\\d+) to remove and (\\d+) not upgraded\\.$"));
    const QString package = QStringLiteral("([a-z0-9][a-z0-9+.-]*(?::[a-z0-9][a-z0-9-]*)?)");
    const QString version = QStringLiteral("([0-9][A-Za-z0-9.+:~\\-]*)");
    const QRegularExpression install(QStringLiteral("^Inst ") + package + QStringLiteral("(?: \\[") + version + QStringLiteral("\\])? \\(") + version + QStringLiteral("(?: ([^\\r\\n]*))?\\)(?: \\[[^\\r\\n]*\\])?$"));
    const QRegularExpression remove(QStringLiteral("^Remv ") + package + QStringLiteral("(?: \\[") + version + QStringLiteral("\\])?(?: \\[[^\\r\\n]*\\])?$"));
    const QRegularExpression configure(QStringLiteral("^Conf ") + package + QStringLiteral(" \\(") + version + QStringLiteral("(?: ([^\\r\\n]*))?\\)(?: \\[[^\\r\\n]*\\])?$"));
    bool hasSummary = false;
    QHash<QString, int> operations;
    QSet<QString> configured;
    int upgrades = 0, installs = 0, removals = 0;
    const auto lines = output.split(QRegularExpression(QStringLiteral("[\\r\\n]+")));
    for (const QString &line : lines) {
        if (line.isEmpty()) continue;
        if (line.startsWith(QLatin1String("NOTE: This is only a simulation!"))) { continue; }
        const auto totals = summary.match(line);
        if (totals.hasMatch()) {
            if (hasSummary) return fail(QObject::tr("APT returned more than one simulation summary."));
            hasSummary = true;
            bool ok[4];
            result.upgraded = totals.captured(1).toInt(&ok[0]); result.installed = totals.captured(2).toInt(&ok[1]);
            result.removed = totals.captured(3).toInt(&ok[2]); result.notUpgraded = totals.captured(4).toInt(&ok[3]);
            if (!ok[0] || !ok[1] || !ok[2] || !ok[3]) return fail(QObject::tr("APT simulation counts are invalid."));
            continue;
        }
        if (line.startsWith(QLatin1String("Inst ")) || line.startsWith(QLatin1String("Remv "))) {
            if (!hasSummary) return fail(QObject::tr("APT returned package operations before its simulation summary."));
            const bool installing = line.startsWith(QLatin1String("Inst "));
            const auto match = (installing ? install : remove).match(line);
            if (!match.hasMatch() || operations.contains(match.captured(1))) return fail(QObject::tr("APT returned an invalid or repeated package operation: %1").arg(line));
            PackageChange change; change.package = match.captured(1); change.installedVersion = match.captured(2);
            if (installing) {
                change.candidateVersion = match.captured(3); change.source = match.captured(4);
                change.action = change.installedVersion.isEmpty() ? PackageChange::Action::Install : PackageChange::Action::Upgrade;
                if (change.action == PackageChange::Action::Install) ++installs; else ++upgrades;
            } else { change.action = PackageChange::Action::Remove; ++removals; }
            operations.insert(change.package, result.changes.size()); result.changes << change; continue;
        }
        if (line.startsWith(QLatin1String("Conf "))) {
            const auto match = configure.match(line);
            if (!hasSummary || !match.hasMatch() || configured.contains(match.captured(1))) return fail(QObject::tr("APT returned invalid package configuration metadata: %1").arg(line));
            const QString name = match.captured(1); configured.insert(name);
            if (operations.contains(name)) {
                const auto change = result.changes[operations.value(name)];
                if (change.action == PackageChange::Action::Remove || change.candidateVersion != match.captured(2))
                    return fail(QObject::tr("APT configuration metadata does not match its package plan: %1").arg(name));
            } else {
                PackageChange change; change.action = PackageChange::Action::Configure; change.package = name;
                change.candidateVersion = match.captured(2); change.source = match.captured(3); result.changes << change;
            }
            continue;
        }
        if (line.startsWith(QLatin1String("E:")) || line.startsWith(QLatin1String("Err:"))) return fail(QObject::tr("APT could not resolve package metadata: %1").arg(line));
        if (line.startsWith(QLatin1String("W:")) || line.startsWith(QLatin1String("N:"))) { result.warnings += line + QLatin1Char('\n'); continue; }
        if (line.startsWith(QLatin1Char(' '))) {
            const QString text = line.trimmed();
            const QRegularExpression packageList(QStringLiteral("^") + package + QStringLiteral("(?:\\s+") + package + QStringLiteral(")*$"));
            if (text == QLatin1String("apt-get needs root privileges for real execution.")
                || text == QLatin1String("Keep also in mind that locking is deactivated,")
                || text == QLatin1String("so don't depend on the relevance to the real current situation!")
                || packageList.match(text).hasMatch()) continue;
            return fail(QObject::tr("APT returned unsupported package-list or diagnostic metadata: %1").arg(text));
        }
        if (line.startsWith(QLatin1String("Reading package lists"))
            || line.startsWith(QLatin1String("Building dependency tree")) || line.startsWith(QLatin1String("Reading state information"))
            || line.startsWith(QLatin1String("Calculating upgrade")) || line.startsWith(QLatin1String("The following "))
            || line == QLatin1String("Use 'apt autoremove' to remove them.")) continue;
        return fail(QObject::tr("APT returned an unsupported simulation output format: %1").arg(line));
    }
    // The numeric totals are an independent consistency check against each
    // operation record. A missing/partial/unrecognized plan never becomes success.
    if (!hasSummary || upgrades != result.upgraded || installs != result.installed || removals != result.removed)
        return fail(QObject::tr("APT's package operations do not match its simulation summary. The preview is incomplete."));
    for (const auto &change : result.changes)
        if ((change.action == PackageChange::Action::Install || change.action == PackageChange::Action::Upgrade) && !configured.contains(change.package))
            return fail(QObject::tr("APT did not provide complete configuration metadata for %1.").arg(change.package));
    result.success = true; result.warnings = result.warnings.trimmed(); return result;
}

PackageSimulationService *PackageSimulationService::instance()
{
    static QPointer<PackageSimulationService> service;
    if (!service) service = new PackageSimulationService(createAptPackageSimulationBackend(), QCoreApplication::instance());
    return service;
}
PackageSimulationService::PackageSimulationService(PackageSimulationBackend *backend, QObject *parent)
    : QObject(parent), m_backend(backend)
{
    Q_ASSERT(backend); backend->setParent(this);
    connect(backend, &PackageSimulationBackend::finished, this, &PackageSimulationService::complete);
}
void PackageSimulationService::startPreview()
{
    if (isBusy()) return;
    m_result = {};
    if (!providerAvailable()) {
        m_result.error = unavailableReason(); m_state = State::Failed; emit changed(); return;
    }
    m_state = State::Running; ++m_requestId; emit changed(); m_backend->start(m_requestId);
}
void PackageSimulationService::cancel()
{
    if (m_state != State::Running) return;
    m_state = State::Cancelling; emit changed(); m_backend->cancel(m_requestId);
}
void PackageSimulationService::complete(quint64 id, const PackageSimulationProcessResult &process)
{
    if (id != m_requestId || !isBusy()) return;
    if (m_state == State::Cancelling || process.cancelled) { m_state = State::Cancelled; m_result = {}; }
    else if (process.timedOut) { m_state = State::TimedOut; m_result = {}; m_result.error = tr("APT simulation exceeded its time limit. No complete preview is available."); }
    else if (process.outputTruncated || !process.normalExit || process.exitCode != 0 || !process.error.isEmpty()) {
        m_state = State::Failed; m_result = {};
        m_result.error = process.outputTruncated ? tr("APT simulation output exceeded its safety limit; the preview is incomplete.")
            : !process.error.isEmpty() ? process.error : tr("APT simulation failed (exit code %1).").arg(process.exitCode);
    } else {
        m_result = parseAptPackageSimulation(process.standardOutput);
        m_state = m_result.success ? State::Succeeded : State::Failed;
    }
    const QString stderrErrors = QString::fromUtf8(process.standardError);
    if (m_result.success && QRegularExpression(QStringLiteral("(?:^|\\n)(?:E:|Err:)")).match(stderrErrors).hasMatch()) {
        m_result.success = false; m_result.changes.clear(); m_result.error = tr("APT reported package metadata errors on its diagnostic channel."); m_state = State::Failed;
    }
    m_result.completedAt = QDateTime::currentDateTime();
    const QString stderrText = QString::fromUtf8(process.standardError).trimmed();
    if (!stderrText.isEmpty()) m_result.warnings += (m_result.warnings.isEmpty() ? QString() : QStringLiteral("\n")) + stderrText;
    if (m_result.rawOutput.isEmpty()) m_result.rawOutput = QString::fromUtf8(process.standardOutput);
    emit changed();
}
QString PackageSimulationService::statusText() const
{
    switch (m_state) {
    case State::Idle: return tr("Ready to preview changes using the currently cached APT indexes.");
    case State::Running: return tr("Resolving a full-upgrade simulation…");
    case State::Cancelling: return tr("Cancelling the simulation; waiting for APT to stop…");
    case State::Succeeded: return tr("Preview complete: %1 upgrade(s), %2 new package(s), %3 removal(s), %4 not upgraded.").arg(m_result.upgraded).arg(m_result.installed).arg(m_result.removed).arg(m_result.notUpgraded);
    case State::Cancelled: return tr("Simulation cancelled. No complete preview is available.");
    case State::TimedOut: case State::Failed: return m_result.error;
    }
    return {};
}
