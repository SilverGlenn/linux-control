#pragma once

#include <QObject>
#include <QProcess>
#include <QPointer>
#include <QHash>
#include <QStringList>

// Jobs are owned by the application, never by a subscribing page. Completed
// jobs remain available so a newly opened view can display their result.
class ProcessJob final : public QObject {
    Q_OBJECT
public:
    QProcess *process() const { return m_process; }
    QString title() const { return m_title; }
    bool active() const { return m_active; }
    bool mutating() const { return m_mutating; }
    QByteArray output() const { return m_output; }
    bool outputTruncated() const { return m_outputTruncated; }
    bool timedOut() const { return m_timedOut; }
    bool succeeded() const { return !m_active && m_exitStatus == QProcess::NormalExit && m_exitCode == 0 && m_error.isEmpty(); }
    int exitCode() const { return m_exitCode; }
    QProcess::ExitStatus exitStatus() const { return m_exitStatus; }
    QString errorString() const { return m_error; }
signals:
    void outputReady(const QByteArray &data);
    void finished();
    void changed();
private:
    friend class OperationManager;
    ProcessJob(const QString &title, bool mutating, const QString &key, QObject *parent);
    void drainOutput();
    void complete(int code, QProcess::ExitStatus status);
    QProcess *m_process;
    QString m_title, m_key, m_error;
    QByteArray m_output;
    bool m_active = true;
    bool m_outputTruncated = false, m_timedOut = false;
    bool m_mutating;
    int m_exitCode = -1;
    QProcess::ExitStatus m_exitStatus = QProcess::CrashExit;
};

class OperationManager final : public QObject {
    Q_OBJECT
public:
    static OperationManager *instance();
    QList<ProcessJob *> jobs() const { return m_jobs; }
    bool hasActiveJobs() const;
    bool hasActiveMutatingJobs() const;
    void setActivity(QObject *owner, const QString &title, bool active);
    QStringList activityTitles() const;
    // Starts on the next event-loop turn; attach all subscriptions immediately
    // after this returns. A conflict returns nullptr without starting a process.
    ProcessJob *startProcess(const QString &title, const QString &program,
                             const QStringList &arguments, bool mutating = false,
                             const QString &exclusiveKey = {}, QObject *mutationOwner = nullptr,
                             int timeoutMs = 0);
    // Reserve a transaction across multiple subprocesses (e.g. pacman + yay).
    bool acquireMutation(QObject *owner);
    void releaseMutation(QObject *owner);
signals:
    void changed();
private:
    explicit OperationManager(QObject *parent);
    QList<ProcessJob *> m_jobs;
    QPointer<QObject> m_mutationOwner;
    QHash<QObject *, QString> m_activities;
    QHash<QObject *, QMetaObject::Connection> m_activityConnections;
};
