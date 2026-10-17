#include "UpdateService.h"
#include "../data/UpdateCheckStatus.h"
#include <QCoreApplication>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QProcessEnvironment>

static void useParseableLocale(ProcessJob *job)
{
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert("LC_ALL", "C");
    job->process()->setProcessEnvironment(environment);
}

UpdateService *UpdateService::instance()
{
    static QPointer<UpdateService> service;
    if (!service) service = new UpdateService(QCoreApplication::instance());
    return service;
}

UpdateService::UpdateService(QObject *parent) : QObject(parent)
{
    QSettings settings;
    settings.beginGroup("LinuxUpdate");
    m_state.lastCheck = settings.value("lastCheck").toDateTime();
    m_state.lastInstall = settings.value("lastInstall").toDateTime();
    m_state.everInstalled = settings.value("everInstalled", false).toBool();
    m_state.lastInstallOk = settings.value("lastInstallOk", false).toBool();
    refreshAvailability();
}

bool UpdateService::busy() const
{
    return m_state.phase == Phase::Checking || m_state.phase == Phase::Downloading
        || m_state.phase == Phase::Installing;
}

void UpdateService::refreshAvailability()
{
    if (busy()) return;
    const QString pacman = QStandardPaths::findExecutable("pacman");
    const QString providerKey = pacman + '\n' + QStandardPaths::findExecutable("checkupdates")
        + '\n' + QStandardPaths::findExecutable("yay");
    const bool available = !pacman.isEmpty();
    if (providerKey != m_providerKey) {
        ++m_checkGeneration;
        m_state.phase = Phase::Idle;
        m_state.packages.clear();
        m_state.packageSizes.clear();
        m_state.selectedNames.clear();
        m_state.selectedAur.clear();
        m_state.checkedNames.clear();
        m_state.output.clear();
        m_state.error.clear();
        m_state.aurWarning.clear();
        m_state.repositoryCheckSucceeded = m_state.aurCheckSucceeded = false;
        m_state.yayAvailable = false;
        m_providerKey = providerKey;
    }
    if (!available) {
        m_state.phase = Phase::Error;
        m_state.error = "Package updates are unavailable: this page requires pacman (Arch-based systems).";
    } else if (!m_state.pacmanAvailable) {
        m_state.phase = Phase::Idle;
        m_state.error.clear();
    }
    m_state.pacmanAvailable = available;
    emit changed();
}

void UpdateService::saveState()
{
    QSettings settings;
    settings.beginGroup("LinuxUpdate");
    settings.setValue("lastCheck", m_state.lastCheck);
    settings.setValue("lastInstall", m_state.lastInstall);
    settings.setValue("everInstalled", m_state.everInstalled);
    settings.setValue("lastInstallOk", m_state.lastInstallOk);
}

void UpdateService::parseUpdateList(const QByteArray &data, bool aur)
{
    static const QRegularExpression lineRe(R"(^(\S+)\s+(\S+)\s+->\s+(\S+)\s*$)");
    static const QRegularExpression nameRe(R"(^[a-zA-Z0-9][a-zA-Z0-9@._+\-]*$)");
    static const QRegularExpression versionRe(R"(^[a-zA-Z0-9.:_+\-~]+$)");
    for (const auto &line : QString::fromUtf8(data).split('\n')) {
        const auto match = lineRe.match(line.trimmed());
        if (!match.hasMatch() || !nameRe.match(match.captured(1)).hasMatch()
            || !versionRe.match(match.captured(2)).hasMatch()
            || !versionRe.match(match.captured(3)).hasMatch()) continue;
        bool duplicate = false;
        for (const auto &pkg : m_state.packages)
            if (pkg.name == match.captured(1)) { duplicate = true; break; }
        if (!duplicate)
            m_state.packages.append({match.captured(1), match.captured(2), match.captured(3), aur});
    }
}

void UpdateService::checkForUpdates()
{
    if (busy()) return;
    refreshAvailability();
    if (!m_state.pacmanAvailable) return;
    auto *job = OperationManager::instance()->startProcess("Check for repository updates", "checkupdates", {}, false, "update-check", nullptr, 120000);
    if (!job) return;
    useParseableLocale(job);
    ++m_checkGeneration;
    m_state.phase = Phase::Checking;
    m_state.error.clear();
    m_state.aurWarning.clear();
    m_state.packages.clear();
    m_state.packageSizes.clear();
    m_state.selectedNames.clear();
    m_state.selectedAur.clear();
    m_state.checkedNames.clear();
    m_state.output.clear();
    m_state.repositoryCheckSucceeded = false;
    m_state.aurCheckSucceeded = false;
    m_state.yayAvailable = false;
    connect(job, &ProcessJob::finished, this, [this, job]() {
        m_state.output = QString::fromUtf8(job->output());
        if (job->timedOut()) {
            m_state.error = "The repository update check timed out. Update status is unavailable.";
        } else if (job->outputTruncated()) {
            m_state.error = "The repository update results exceeded the output limit. Update status is unavailable.";
        } else if (job->process()->error() == QProcess::FailedToStart) {
            m_state.error = "Update check unavailable because checkupdates is missing. Install pacman-contrib and try again.";
        } else if (!job->errorString().isEmpty()) {
            m_state.error = "The repository update check failed: " + job->errorString();
        } else if (job->exitStatus() != QProcess::NormalExit || (job->exitCode() != 0 && job->exitCode() != 2)) {
            m_state.error = "The repository update check failed.";
            if (!m_state.output.trimmed().isEmpty()) m_state.error += "\n\n" + m_state.output.trimmed();
        } else {
            m_state.repositoryCheckSucceeded = true;
            if (job->exitCode() == 0) {
                parseUpdateList(job->output(), false);
                if (m_state.packages.isEmpty()) {
                    m_state.repositoryCheckSucceeded = false;
                    m_state.error = "The repository update check returned no valid package results.";
                }
            }
        }
        if (!m_state.error.isEmpty()) { finishCheck(); return; }
        m_state.lastCheck = QDateTime::currentDateTime();
        saveState();
        runYayCheck();
    });
    emit changed();
}

void UpdateService::runYayCheck()
{
    auto *job = OperationManager::instance()->startProcess("Check for AUR updates", "yay", {"-Qua"}, false, "update-aur-check", nullptr, 120000);
    if (!job) {
        m_state.aurWarning = "Repository updates were checked, but an AUR check is already running.";
        finishCheck();
        return;
    }
    useParseableLocale(job);
    connect(job, &ProcessJob::finished, this, [this, job]() {
        m_state.yayAvailable = job->succeeded();
        if (job->timedOut()) {
            m_state.aurWarning = "Repository updates were checked, but the AUR update check timed out.";
        } else if (job->outputTruncated()) {
            m_state.aurWarning = "Repository updates were checked, but AUR update results exceeded the output limit.";
        } else if (job->process()->error() == QProcess::FailedToStart) {
            m_state.aurWarning = "Repository updates were checked, but AUR update status is unavailable because yay was not found.";
        } else if (job->succeeded()) {
            const int before = m_state.packages.size();
            parseUpdateList(job->output(), true);
            m_state.aurCheckSucceeded = job->output().trimmed().isEmpty() || m_state.packages.size() > before;
            if (!m_state.aurCheckSucceeded)
                m_state.aurWarning = "AUR update status is unavailable because yay returned results that could not be read.";
        } else {
            m_state.aurWarning = job->exitStatus() == QProcess::CrashExit
                ? "Repository updates were checked, but yay terminated unexpectedly before AUR results could be read."
                : "Repository updates were checked, but yay could not check AUR updates.";
        }
        finishCheck();
    });
}

void UpdateService::finishCheck()
{
    if (!m_state.error.isEmpty()) m_state.phase = Phase::Error;
    else {
        int repo = 0, aur = 0;
        for (const auto &pkg : m_state.packages) { if (pkg.aur) ++aur; else ++repo; }
        switch (classifyPackageUpdateCheck(m_state.repositoryCheckSucceeded, repo, m_state.aurCheckSucceeded, aur)) {
        case PackageUpdateCheckState::RepositoryCheckFailed:
            m_state.phase = Phase::Error;
            m_state.error = "Repository update status is unavailable.";
            break;
        case PackageUpdateCheckState::FullyUpToDate: m_state.phase = Phase::UpToDate; break;
        case PackageUpdateCheckState::NoKnownUpdatesAurUnknown: m_state.phase = Phase::Incomplete; break;
        case PackageUpdateCheckState::UpdatesAvailable:
        case PackageUpdateCheckState::UpdatesAvailableAurUnknown: m_state.phase = Phase::Available; break;
        }
    }
    for (const auto &pkg : m_state.packages) {
        m_state.selectedNames << pkg.name;
        m_state.checkedNames.insert(pkg.name);
        if (pkg.aur) m_state.selectedAur << pkg.name;
    }
    emit changed();
    if (m_state.phase == Phase::Available) fetchPackageSizes();
}

void UpdateService::setCheckedNames(const QSet<QString> &names)
{
    if (!busy()) m_state.checkedNames = names;
}

void UpdateService::setSelection(const QSet<QString> &names)
{
    if (busy()) return;
    m_state.selectedNames.clear();
    m_state.selectedAur.clear();
    for (const auto &pkg : m_state.packages) {
        if (!names.contains(pkg.name)) continue;
        m_state.selectedNames << pkg.name;
        if (pkg.aur) m_state.selectedAur << pkg.name;
    }
    m_state.checkedNames = names;
    emit changed();
}

void UpdateService::fetchPackageSizes()
{
    QStringList args{"-Si"};
    for (const auto &pkg : m_state.packages) args << pkg.name;
    auto *job = OperationManager::instance()->startProcess("Read package download sizes", m_state.yayAvailable ? "yay" : "pacman", args, false, {}, nullptr, 30000);
    if (!job) return;
    useParseableLocale(job);
    const auto generation = m_checkGeneration;
    connect(job, &ProcessJob::finished, this, [this, job, generation]() {
        if (generation != m_checkGeneration || !job->succeeded() || job->outputTruncated()) return;
        static const QRegularExpression nameRe(R"(Name\s*:\s*(\S+))");
        static const QRegularExpression sizeRe(R"(Download Size\s*:\s*([\d.]+)\s*(B|KiB|MiB|GiB))");
        for (const auto &block : QString::fromUtf8(job->output()).split("\n\n", Qt::SkipEmptyParts)) {
            const auto name = nameRe.match(block), size = sizeRe.match(block);
            if (!name.hasMatch() || !size.hasMatch()) continue;
            double kib = size.captured(1).toDouble();
            const auto unit = size.captured(2);
            if (unit == "GiB") kib *= 1024 * 1024;
            else if (unit == "MiB") kib *= 1024;
            else if (unit == "B") kib /= 1024;
            m_state.packageSizes.insert(name.captured(1), kib >= 1024
                ? QString("%1 MB").arg(kib / 1024, 0, 'f', 1) : QString("%1 KB").arg(qRound(kib)));
        }
        emit changed();
    });
}

QString UpdateService::installUpdates()
{
    if (busy()) return "An update operation is already running.";
    if (!m_state.pacmanAvailable || m_state.phase != Phase::Available)
        return "Check for available package updates before installing.";
    if (m_state.selectedNames.isEmpty()) return "No updates are selected.";
    int repositoryTotal = 0, repositorySelected = 0;
    for (const auto &pkg : m_state.packages) {
        if (pkg.aur) continue;
        ++repositoryTotal;
        if (m_state.selectedNames.contains(pkg.name)) ++repositorySelected;
    }
    if (repositorySelected > 0 && repositorySelected != repositoryTotal)
        return "Arch Linux does not support partial upgrades. Select all repository updates to continue, or deselect them and install only selected AUR updates.";
    if (!m_state.selectedAur.isEmpty() && !m_state.yayAvailable)
        return "AUR installation is unavailable because yay could not be started.";
    if (!OperationManager::instance()->acquireMutation(this))
        return "Another package transaction is running. Wait for it to finish before installing updates.";
    m_state.output.clear();
    m_state.error.clear();
    m_state.currentPackage.clear();
    m_state.installCount = m_state.downloadCount = 0;
    m_seenInstalled.clear();
    m_seenDownloaded.clear();
    m_state.phase = Phase::Downloading;
    runInstall(repositorySelected == 0);
    return {};
}

void UpdateService::runInstall(bool aur)
{
    m_stepOutput.clear();
    m_state.phase = Phase::Downloading;
    m_state.currentPackage.clear();
    QString program = "pkexec";
    QStringList args{"pacman", "--color", "never", "-Syu", "--noconfirm"};
    if (aur) {
        program = "yay";
        args = {"-S", "--noconfirm", "--sudo", "pkexec", "--cleanafter"};
        args << m_state.selectedAur;
    }
    auto *job = OperationManager::instance()->startProcess(aur ? "Install AUR updates" : "Upgrade system packages", program, args, true, "package-transaction", this);
    if (!job) { finishInstall(false, "Another package transaction is running."); return; }
    useParseableLocale(job);
    connect(job, &ProcessJob::outputReady, this, &UpdateService::consumeInstallOutput);
    connect(job, &ProcessJob::finished, this, [this, job, aur]() {
        if (job->succeeded() && !aur && !m_state.selectedAur.isEmpty()) {
            runInstall(true);
            return;
        }
        finishInstall(job->succeeded(), job->errorString());
    });
    emit changed();
}

void UpdateService::consumeInstallOutput(const QByteArray &data)
{
    m_state.output += QString::fromUtf8(data);
    if (m_state.output.size() > 1024 * 1024) m_state.output = m_state.output.right(1024 * 1024);
    m_stepOutput += QString::fromUtf8(data);
    if (m_stepOutput.size() > 1024 * 1024) m_stepOutput = m_stepOutput.right(1024 * 1024);
    static const QRegularExpression ansiRe(R"(\x1B\[[0-9;?]*[ -/]*[@-~])");
    QString clean = m_stepOutput;
    clean.remove(ansiRe);
    const int installStart = clean.indexOf(":: Processing package changes...");
    if (installStart >= 0 || m_state.phase == Phase::Installing) {
        m_state.phase = Phase::Installing;
        static const QRegularExpression installRe(R"((?:installing|upgrading|reinstalling|removing|downgrading)\s+(\S+?)\.\.\.)");
        auto matches = installRe.globalMatch(installStart >= 0 ? clean.mid(installStart) : clean);
        while (matches.hasNext()) {
            m_state.currentPackage = matches.next().captured(1);
            if (m_state.selectedNames.contains(m_state.currentPackage)) m_seenInstalled.insert(m_state.currentPackage);
        }
        m_state.installCount = m_seenInstalled.size();
    } else {
        const int retrieve = clean.indexOf(":: Retrieving packages...");
        if (retrieve >= 0) {
            const QString tail = clean.mid(retrieve);
            for (const auto &name : m_state.selectedNames) {
                const QRegularExpression filename(QRegularExpression::escape(name) + "-[0-9]");
                if (filename.match(tail).hasMatch()) m_seenDownloaded.insert(name);
            }
            m_state.downloadCount = m_seenDownloaded.size();
        }
    }
    emit changed();
}

void UpdateService::finishInstall(bool ok, const QString &error)
{
    m_state.lastInstall = QDateTime::currentDateTime();
    m_state.everInstalled = true;
    m_state.lastInstallOk = ok;
    m_state.error = error;
    if (!ok && m_state.error.isEmpty()) m_state.error = m_state.output.right(2000).trimmed();
    m_state.phase = Phase::Done;
    saveState();
    OperationManager::instance()->releaseMutation(this);
    emit changed();
}
