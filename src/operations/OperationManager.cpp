#include "OperationManager.h"
#include <QCoreApplication>
#include <QTimer>

ProcessJob::ProcessJob(const QString &title, bool mutating, const QString &key, QObject *parent)
    : QObject(parent), m_process(new QProcess(this)), m_title(title), m_key(key), m_mutating(mutating)
{
    m_process->setProcessChannelMode(QProcess::MergedChannels);
    connect(m_process, &QProcess::readyRead, this, &ProcessJob::drainOutput);
    connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (m_error.isEmpty()) m_error = m_process->errorString();
        if (error == QProcess::FailedToStart) {
            complete(-1, QProcess::CrashExit);
        }
    });
    connect(m_process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this, &ProcessJob::complete);
}

void ProcessJob::drainOutput()
{
    const QByteArray data = m_process->readAll();
    if (data.isEmpty()) return;
    m_output += data;
    constexpr int retainedLimit = 1024 * 1024;
    if (m_output.size() > retainedLimit) {
        m_output = m_output.right(retainedLimit);
        m_outputTruncated = true;
    }
    emit outputReady(data);
    emit changed();
}

void ProcessJob::complete(int code, QProcess::ExitStatus status)
{
    if (!m_active) return; // FailedToStart and finished must produce one result.
    drainOutput();
    m_exitCode = code;
    m_exitStatus = status;
    m_active = false;
    emit finished();
    emit changed();
}

OperationManager::OperationManager(QObject *parent) : QObject(parent) {}

OperationManager *OperationManager::instance()
{
    static QPointer<OperationManager> manager;
    if (!manager) manager = new OperationManager(QCoreApplication::instance());
    return manager;
}

bool OperationManager::hasActiveJobs() const
{
    if (!m_activities.isEmpty()) return true;
    if (m_mutationOwner) return true;
    for (auto *job : m_jobs) if (job->active()) return true;
    return false;
}

void OperationManager::setActivity(QObject *owner, const QString &title, bool active)
{
    if (!owner) return;
    if (!active) {
        if (!m_activities.remove(owner)) return;
        QObject::disconnect(m_activityConnections.take(owner));
    } else {
        if (m_activities.contains(owner) && m_activities.value(owner) == title) return;
        if (!m_activities.contains(owner)) {
            m_activityConnections.insert(owner, connect(owner, &QObject::destroyed, this, [this, owner]() {
                m_activityConnections.remove(owner);
                if (m_activities.remove(owner)) emit changed();
            }));
        }
        m_activities.insert(owner, title);
    }
    emit changed();
}

QStringList OperationManager::activityTitles() const
{
    QStringList titles = m_activities.values();
    titles.sort(Qt::CaseInsensitive);
    return titles;
}

bool OperationManager::hasActiveMutatingJobs() const
{
    if (m_mutationOwner) return true;
    for (auto *job : m_jobs) if (job->active() && job->mutating()) return true;
    return false;
}

bool OperationManager::acquireMutation(QObject *owner)
{
    if (!owner || hasActiveMutatingJobs()) return false;
    m_mutationOwner = owner;
    connect(owner, &QObject::destroyed, this, [this]() { emit changed(); });
    emit changed();
    return true;
}

void OperationManager::releaseMutation(QObject *owner)
{
    if (m_mutationOwner != owner) return;
    m_mutationOwner = nullptr;
    emit changed();
}

ProcessJob *OperationManager::startProcess(const QString &title, const QString &program,
                                         const QStringList &arguments, bool mutating,
                                         const QString &key, QObject *mutationOwner, int timeoutMs)
{
    if (mutating) {
        if (m_mutationOwner && m_mutationOwner != mutationOwner) return nullptr;
        for (auto *job : m_jobs)
            if (job->active() && job->mutating()) return nullptr;
    }
    if (!key.isEmpty())
        for (auto *job : m_jobs)
            if (job->active() && job->m_key == key) return nullptr;
    auto *job = new ProcessJob(title, mutating, key, this);
    m_jobs.append(job);
    connect(job, &ProcessJob::changed, this, &OperationManager::changed);
    QTimer::singleShot(0, job, [job, program, arguments, timeoutMs]() {
        job->process()->start(program, arguments);
        if (!job->mutating() && timeoutMs > 0) {
            QTimer::singleShot(timeoutMs, job, [job]() {
                if (!job->active()) return;
                job->m_timedOut = true;
                job->m_error = "Operation timed out.";
                job->process()->kill();
            });
        }
    });
    emit changed();
    return job;
}
