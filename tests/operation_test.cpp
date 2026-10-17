#include "operations/OperationManager.h"
#include <QtTest>
#include <QFile>
#include <QTemporaryDir>

class OperationTest final : public QObject {
    Q_OBJECT
private:
    QTemporaryDir m_dir;
    QString script(const QByteArray &body) {
        const auto path = m_dir.filePath(QString::number(++m_number));
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly)) return {};
        file.write("#!/bin/sh\n" + body);
        file.close();
        file.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        return path;
    }
    int m_number = 0;
private slots:
    void outputAndCompletionSurviveSubscriber() {
        auto *manager = OperationManager::instance();
        auto *job = manager->startProcess("Read fixture", script("printf 'first\\n'\n/bin/sleep 0.15\nprintf 'last\\n'\n"), {});
        QVERIFY(job);
        QVERIFY(job->active());
        QCOMPARE(job->parent(), manager);
        QSignalSpy finished(job, &ProcessJob::finished);
        auto *subscriber = new QObject;
        QObject::connect(job, &ProcessJob::outputReady, subscriber, [](const QByteArray &) {});
        QTRY_VERIFY(job->output().contains("first"));
        delete subscriber;
        QTRY_COMPARE(finished.count(), 1);
        QVERIFY(job->succeeded());
        QCOMPARE(job->output(), QByteArray("first\nlast\n"));
        QVERIFY(manager->jobs().contains(job));
        QVERIFY(!manager->hasActiveJobs());
    }
    void failedStartFinishesExactlyOnce() {
        auto *job = OperationManager::instance()->startProcess("Missing fixture", m_dir.filePath("missing"), {});
        QVERIFY(job);
        QSignalSpy finished(job, &ProcessJob::finished);
        QTRY_COMPARE(finished.count(), 1);
        QVERIFY(!job->active());
        QVERIFY(!job->succeeded());
        QVERIFY(!job->errorString().isEmpty());
        QTest::qWait(30);
        QCOMPARE(finished.count(), 1);
    }
    void duplicateAndMutationConflicts() {
        auto *manager = OperationManager::instance();
        const auto slow = script("exec /bin/sleep 0.1\n");
        auto *read = manager->startProcess("Read", slow, {}, false, "fixture-read");
        QVERIFY(read);
        QVERIFY(!manager->startProcess("Duplicate", slow, {}, false, "fixture-read"));
        QObject owner;
        QVERIFY(manager->acquireMutation(&owner));
        QVERIFY(manager->hasActiveMutatingJobs());
        QVERIFY(!manager->startProcess("Conflicting write", slow, {}, true));
        auto *write = manager->startProcess("Reserved write", slow, {}, true, "fixture-write", &owner);
        QVERIFY(write);
        QVERIFY(!manager->startProcess("Duplicate reserved write", slow, {}, true, {}, &owner));
        QTRY_VERIFY(!write->active());
        QVERIFY(manager->hasActiveMutatingJobs()); // reservation spans steps
        manager->releaseMutation(&owner);
        QTRY_VERIFY(!read->active());
        QVERIFY(!manager->hasActiveJobs());
    }
    void boundedReadTimesOut() {
        auto *job = OperationManager::instance()->startProcess("Slow read", script("exec /bin/sleep 5\n"), {}, false, {}, nullptr, 40);
        QSignalSpy finished(job, &ProcessJob::finished);
        QTRY_COMPARE(finished.count(), 1);
        QVERIFY(job->timedOut());
        QVERIFY(!job->succeeded());
        QVERIFY(!job->errorString().isEmpty());
    }
    void argumentsRemainSeparate() {
        auto *job = OperationManager::instance()->startProcess("Arguments", script("printf '%s\\n' \"$@\"\n"), {"a b", "$(touch should-not-exist)", "--literal"});
        QTRY_VERIFY(!job->active());
        QVERIFY(job->succeeded());
        QCOMPARE(job->output(), QByteArray("a b\n$(touch should-not-exist)\n--literal\n"));
    }
    void writeTimeoutIsIgnoredAndCrashIsFailure() {
        auto *manager = OperationManager::instance();
        auto *write = manager->startProcess("Write fixture", script("exec /bin/sleep 0.1\n"), {}, true, {}, nullptr, 10);
        QTRY_VERIFY(!write->active());
        QVERIFY(write->succeeded());
        QVERIFY(!write->timedOut());
        auto *crash = manager->startProcess("Crashing fixture", script("kill -KILL $$\n"), {});
        QTRY_VERIFY(!crash->active());
        QVERIFY(!crash->succeeded());
        QCOMPARE(crash->exitStatus(), QProcess::CrashExit);
    }
    void outputRetentionIsBoundedAndMarked() {
        auto *job = OperationManager::instance()->startProcess("Large output fixture", script("/usr/bin/head -c 1048600 /dev/zero\nprintf 'end\\n' >&2\n"), {});
        QTRY_VERIFY(!job->active());
        QVERIFY(job->succeeded());
        QVERIFY(job->outputTruncated());
        QVERIFY(job->output().size() <= 1024 * 1024);
        QVERIFY(job->output().endsWith("end\n"));
    }
    void nonProcessActivitiesAreTrackedUntilOwnerStopsOrDies() {
        auto *manager = OperationManager::instance();
        auto *owner = new QObject;
        QSignalSpy changed(manager, &OperationManager::changed);
        manager->setActivity(owner, "Read devices", true);
        QCOMPARE(manager->activityTitles(), QStringList{"Read devices"});
        QVERIFY(manager->hasActiveJobs());
        QVERIFY(!manager->hasActiveMutatingJobs());
        const int events = changed.count();
        manager->setActivity(owner, "Read devices", true);
        QCOMPARE(changed.count(), events);
        manager->setActivity(owner, "Refresh devices", true);
        QCOMPARE(manager->activityTitles(), QStringList{"Refresh devices"});
        manager->setActivity(owner, "Refresh devices", false);
        QVERIFY(!manager->hasActiveJobs());
        manager->setActivity(owner, "Read devices", true);
        delete owner;
        QVERIFY(manager->activityTitles().isEmpty());
        QVERIFY(!manager->hasActiveJobs());
    }
};

QTEST_GUILESS_MAIN(OperationTest)
#include "operation_test.moc"
