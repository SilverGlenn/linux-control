#include "ControlPanelUpdateService.h"
#include "OperationManager.h"
#include "../release/BuildIdentity.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QPointer>
#include <QRegularExpression>
#include <sys/stat.h>
#include <unistd.h>

#ifndef CONTROL_SELF_UPDATE_TOOL_PATH
#define CONTROL_SELF_UPDATE_TOOL_PATH ""
#endif

namespace {
// The bootstrap installs this separately from the managed release payload.
// Refuse redirects or paths writable by another user, including parent paths.
bool trustedPath(const QString &path, QString *error)
{
    auto reject = [&]() {
        if (error) *error = QStringLiteral("The update helper or receipt is missing or has unsafe ownership, permissions or a redirected path.");
        return false;
    };
    if (!QDir::isAbsolutePath(path) || QDir::cleanPath(path) != path) return reject();
    QString current = path;
    bool first = true;
    while (true) {
        struct stat info {};
        if (::lstat(QFile::encodeName(current).constData(), &info) != 0
            || S_ISLNK(info.st_mode) || (info.st_uid != ::getuid() && info.st_uid != 0)) return reject();
        if (first ? !S_ISREG(info.st_mode) : !S_ISDIR(info.st_mode)) return reject();
        // A root-owned sticky directory is safe for a private owned child.
        const bool stickyRoot = !first && info.st_uid == 0 && (info.st_mode & S_ISVTX);
        if ((info.st_mode & (S_IWGRP | S_IWOTH)) && !stickyRoot) return reject();
        if (current == QStringLiteral("/")) break;
        current = QFileInfo(current).absolutePath();
        first = false;
    }
    return true;
}

bool identifier(const QString &value)
{
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._+~-]{0,199}$"));
    return pattern.match(value).hasMatch();
}
}

ControlPanelUpdateService *ControlPanelUpdateService::instance()
{
    static QPointer<ControlPanelUpdateService> service;
    if (!service) service = new ControlPanelUpdateService(QString::fromUtf8(CONTROL_SELF_UPDATE_TOOL_PATH), QCoreApplication::instance());
    return service;
}

ControlPanelUpdateService::ControlPanelUpdateService(const QString &helperPath, QObject *parent)
    : QObject(parent), m_helperPath(helperPath)
{
    QString error;
    if (!helperAvailable(&error)) {
        m_state.phase = Phase::NotConfigured;
        m_state.message = error;
    } else {
        m_state.message = QStringLiteral("Control Panel updates have not been checked. Checks and downloads run only when you request them.");
    }
}

QString ControlPanelUpdateService::currentBuildId() { return QString::fromUtf8(release::buildId()); }
bool ControlPanelUpdateService::busy() const { return m_state.phase == Phase::Checking || m_state.phase == Phase::Downloading; }

bool ControlPanelUpdateService::helperAvailable(QString *error) const
{
    if (m_helperPath.isEmpty() || !QFileInfo::exists(m_helperPath)) {
        if (error) *error = QStringLiteral("Control Panel updates are not configured. Install the trusted user-local updater first; no release server has been checked.");
        return false;
    }
    return trustedPath(m_helperPath, error);
}

void ControlPanelUpdateService::fail(const QString &message)
{
    m_state = {};
    m_state.phase = Phase::Error;
    m_state.message = message;
    emit changed();
}

void ControlPanelUpdateService::checkForUpdates() { run(QStringLiteral("check")); }
void ControlPanelUpdateService::refreshLocalStatus() { run(QStringLiteral("status")); }
void ControlPanelUpdateService::prepareUpdate()
{
    if (m_state.phase == Phase::Available) run(QStringLiteral("prepare"));
}

void ControlPanelUpdateService::run(const QString &command)
{
    if (busy() || m_state.installerLaunched) return;
    QString error;
    if (!helperAvailable(&error)) {
        m_state = {};
        m_state.phase = Phase::NotConfigured;
        m_state.message = error;
        emit changed();
        return;
    }
    QStringList arguments {m_helperPath, command};
    if (command == QStringLiteral("prepare")) arguments << QStringLiteral("--release-tag") << m_state.releaseTag;
    arguments << QStringLiteral("--current-build") << currentBuildId();
    auto *job = OperationManager::instance()->startProcess(
        command == QStringLiteral("prepare") ? QStringLiteral("Download Control Panel update")
            : command == QStringLiteral("status") ? QStringLiteral("Read Control Panel installation status")
            : QStringLiteral("Check for Control Panel updates"),
        QStringLiteral("/usr/bin/python3"), arguments, false,
        QStringLiteral("control-panel-self-update"), nullptr,
        command == QStringLiteral("prepare") ? 600000 : 120000);
    if (!job) { fail(QStringLiteral("Another Control Panel update operation is running. Wait for it to finish.")); return; }
    const QString tag = m_state.releaseTag;
    const QString targetBuild = m_state.buildId;
    m_state = {};
    if (command == QStringLiteral("prepare")) {
        m_state.releaseTag = tag;
        m_state.buildId = targetBuild;
    }
    m_state.phase = command == QStringLiteral("prepare") ? Phase::Downloading : Phase::Checking;
    m_state.message = command == QStringLiteral("prepare")
        ? QStringLiteral("Downloading and verifying the requested Control Panel update…")
        : command == QStringLiteral("status") ? QStringLiteral("Reading the local installation result…")
        : QStringLiteral("Checking published Control Panel releases…");
    connect(job, &ProcessJob::finished, this, [this, job, command]() { consumeResult(job, command); });
    emit changed();
}

void ControlPanelUpdateService::consumeResult(ProcessJob *job, const QString &command)
{
    if (job->timedOut()) { fail(QStringLiteral("The Control Panel update operation timed out. No installation was started.")); return; }
    if (job->outputTruncated() || job->output().size() > 16384) { fail(QStringLiteral("The updater returned too much output. No installation was started.")); return; }
    if (job->exitStatus() != QProcess::NormalExit || !job->errorString().isEmpty()) {
        fail(QStringLiteral("The trusted updater could not complete the request. No installation was started. ") + job->errorString());
        return;
    }
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(job->output(), &parseError);
    const auto object = document.object();
    const QString status = object.value(QStringLiteral("status")).toString();
    const QString message = object.value(QStringLiteral("message")).toString();
    const QString build = object.value(QStringLiteral("build_id")).toString();
    const QString tag = object.value(QStringLiteral("release_tag")).toString();
    const QString receipt = object.value(QStringLiteral("receipt")).toString();
    const QHash<QString, Phase> phases {
        {"idle", Phase::Idle}, {"not_configured", Phase::NotConfigured}, {"no_releases", Phase::NoReleases},
        {"available", Phase::Available}, {"up_to_date", Phase::UpToDate}, {"incompatible", Phase::Incompatible},
        {"ready", Phase::Ready}, {"installed", Phase::Installed}, {"rolled_back", Phase::RolledBack},
        {"manual_repair", Phase::ManualRepair}, {"error", Phase::Error}
    };
    bool valid = parseError.error == QJsonParseError::NoError && document.isObject()
        && object.value("schema").isDouble() && object.value("schema").toDouble() == 1
        && phases.contains(status) && !message.trimmed().isEmpty() && message.size() <= 4096;
    for (auto it = object.begin(); it != object.end(); ++it) {
        if (it.key() == QStringLiteral("schema")) continue;
        if (!QStringList{"status", "message", "build_id", "release_tag", "receipt"}.contains(it.key()) || !it.value().isString()) valid = false;
    }
    if ((!build.isEmpty() && !identifier(build)) || (!tag.isEmpty() && !identifier(tag))) valid = false;
    if (status == QStringLiteral("available") || status == QStringLiteral("ready"))
        valid = valid && identifier(build) && identifier(tag) && build != currentBuildId();
    if (status == QStringLiteral("ready"))
        valid = valid && command != QStringLiteral("check") && receipt.size() <= 4096 && trustedPath(receipt, nullptr);
    if (command == QStringLiteral("prepare")) {
        const bool failureStatus = status == QStringLiteral("error") || status == QStringLiteral("incompatible")
            || status == QStringLiteral("manual_repair") || status == QStringLiteral("not_configured");
        if (status == QStringLiteral("ready")) valid = valid && tag == m_state.releaseTag && build == m_state.buildId;
        else valid = valid && failureStatus;
    }
    if (!receipt.isEmpty() && status != QStringLiteral("ready")) valid = false;
    if (job->exitCode() != 0 && status != QStringLiteral("error") && status != QStringLiteral("incompatible")
        && status != QStringLiteral("manual_repair") && status != QStringLiteral("not_configured")) valid = false;
    if (!valid) { fail(QStringLiteral("The updater returned an invalid or incomplete result. No installation was started.")); return; }
    m_state = {};
    m_state.phase = phases.value(status);
    m_state.message = message;
    m_state.buildId = build;
    m_state.releaseTag = tag;
    m_state.receipt = receipt;
    emit changed();
}

bool ControlPanelUpdateService::launchInstaller(QString *error)
{
    auto reject = [&](const QString &message) { if (error) *error = message; return false; };
    if (busy() || m_state.phase != Phase::Ready || m_state.installerLaunched)
        return reject(QStringLiteral("A verified Control Panel update is not ready to install."));
    QString trustError;
    if (!helperAvailable(&trustError) || !trustedPath(m_state.receipt, &trustError)) return reject(trustError);
    if (!identifier(m_state.releaseTag) || !identifier(m_state.buildId) || m_state.buildId == currentBuildId())
        return reject(QStringLiteral("The prepared update identity is invalid. Check for updates again."));
    if (!QProcess::startDetached(QStringLiteral("/usr/bin/python3"),
            {m_helperPath, QStringLiteral("apply"), QStringLiteral("--receipt"), m_state.receipt,
             QStringLiteral("--wait-seconds"), QStringLiteral("30")}))
        return reject(QStringLiteral("The updater could not start. Control Panel will stay open."));
    m_state.installerLaunched = true;
    m_state.message = QStringLiteral("Installation requested. The updater will wait for Control Panel to close.");
    emit changed();
    return true;
}
