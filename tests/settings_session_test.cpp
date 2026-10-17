#include <QtTest>
#include <QSignalSpy>
#include "settings/ColorSchemeSettings.h"
#include "ui/SettingsNavigationGuard.h"

class FakeColorSchemeBackend : public ColorSchemeBackend {
public:
    int reads = 0;
    QStringList writes;
    int cancellations = 0;
    void readCurrent() override { ++reads; }
    void applyScheme(const QString &id) override { writes << id; }
    void cancel() override { ++cancellations; }
    void read(const QString &id) { emit readFinished(true, id, QString()); }
    void readError() { emit readFinished(false, QString(), QStringLiteral("Read failed")); }
    void write(bool success = true) { emit writeFinished(success, success ? QString() : QStringLiteral("Write failed")); }
    void changed() { emit externalChange(); }
};

class SettingsSessionTest : public QObject {
    Q_OBJECT
private slots:
    void init()
    {
        backend = new FakeColorSchemeBackend;
        session = new ColorSchemeSettings(backend);
        QTRY_COMPARE(backend->reads, 1);
        backend->read("LegacyScheme");
        QVERIFY(session->isReady());
        QVERIFY(!session->isBusy());
    }
    void cleanup() { delete session; }

    void selectionIsStagedWithoutAWrite()
    {
        QSignalSpy changed(session, &SettingsSession::stateChanged);
        session->selectScheme("Aero");
        QCOMPARE(session->appliedScheme(), "LegacyScheme");
        QCOMPARE(session->stagedScheme(), "Aero");
        QVERIFY(session->isDirty());
        QCOMPARE(backend->writes.size(), 0);
        QVERIFY(changed.size() > 0);
        session->revert();
        backend->read("LegacyScheme");
        QVERIFY(!session->isDirty());
    }

    void unapprovedSelectionsAreRejectedWithoutChangingTheBaseline()
    {
        for (const auto &id : {QStringLiteral("Breeze"), QStringLiteral("Dark"),
                               QStringLiteral("aero"), QStringLiteral("../Aero"), QString()}) {
            session->selectScheme(id);
            QCOMPARE(session->stagedScheme(), "LegacyScheme");
            QVERIFY(!session->isDirty());
        }
        QVERIFY(backend->writes.isEmpty());
        session->selectScheme("Aero");
        session->selectScheme("Breeze");
        QCOMPARE(session->stagedScheme(), "Aero");
        QVERIFY(session->isDirty());
    }

    void applyWaitsForAuthoritativeReadback()
    {
        QSignalSpy finished(session, &SettingsSession::applyFinished);
        session->selectScheme("Aero");
        session->apply();
        QVERIFY(session->isBusy());
        QVERIFY(backend->writes.isEmpty()); // Preflight read before the write.
        backend->read("LegacyScheme");
        QCOMPARE(backend->writes, QStringList{"Aero"});
        session->selectScheme("Other");
        QCOMPARE(session->stagedScheme(), "Aero");
        session->revert();
        QCOMPARE(session->stagedScheme(), "Aero");
        session->apply();
        QCOMPARE(backend->writes.size(), 1);
        backend->write();
        QVERIFY(session->isBusy());
        QVERIFY(finished.isEmpty());
        backend->read("Aero");
        QCOMPARE(finished.size(), 1);
        QVERIFY(finished.at(0).at(0).toBool());
        QVERIFY(!session->isDirty());
        QVERIFY(!session->isBusy());
        QCOMPARE(session->appliedScheme(), "Aero");
        QVERIFY(!session->resultText().isEmpty());
    }

    void failurePreservesStagedEditAndReadsActualState()
    {
        QSignalSpy finished(session, &SettingsSession::applyFinished);
        session->selectScheme("Aero");
        session->apply();
        backend->read("LegacyScheme");
        backend->write(false);
        QCOMPARE(backend->reads, 3);
        QVERIFY(finished.isEmpty());
        backend->read("LegacyScheme");
        QVERIFY(!finished.at(0).at(0).toBool());
        QVERIFY(session->isDirty());
        QCOMPARE(session->stagedScheme(), "Aero");
        QCOMPARE(session->errorText(), "Write failed");
    }

    void successExitWithMismatchedReadbackFails()
    {
        QSignalSpy finished(session, &SettingsSession::applyFinished);
        session->selectScheme("Aero");
        session->apply();
        backend->read("LegacyScheme");
        backend->write();
        backend->read("Other");
        QVERIFY(!finished.at(0).at(0).toBool());
        QCOMPARE(session->appliedScheme(), "Other");
        QCOMPARE(session->stagedScheme(), "Aero");
        QVERIFY(session->isDirty());
        QVERIFY(!session->errorText().isEmpty());
    }

    void delayedWatchRefreshKeepsTheApplyErrorVisible()
    {
        session->selectScheme("Aero");
        session->apply();
        backend->read("LegacyScheme");
        backend->write(false);
        backend->read("LegacyScheme");
        backend->changed();
        backend->read("LegacyScheme");
        QCOMPARE(session->errorText(), "Write failed");
        QVERIFY(session->isDirty());
    }

    void busyNavigationCanRetryWithoutReceivingTheOldCompletion()
    {
        session->selectScheme("Aero");
        session->apply();
        backend->read("LegacyScheme");
        SettingsNavigationGuard guard;
        int prompts = 0;
        bool navigated = false;
        guard.request(session, [&] {
            ++prompts;
            return SettingsNavigationGuard::Choice::Apply;
        }, [&] { navigated = true; });
        QVERIFY(guard.pending());
        QCOMPARE(prompts, 0);
        backend->write(false);
        backend->read("LegacyScheme");
        QCOMPARE(prompts, 1);
        QVERIFY(guard.pending()); // The failed first result cannot cancel retry.
        QVERIFY(!navigated);
        QVERIFY(session->isBusy());
        QCOMPARE(backend->reads, 4);
        backend->read("LegacyScheme");
        QCOMPARE(backend->writes.size(), 2);
        backend->write();
        backend->read("Aero");
        QVERIFY(navigated);
        QVERIFY(!guard.pending());
    }

    void cleanExternalChangeRefreshesTheSelection()
    {
        backend->changed();
        QVERIFY(session->isBusy());
        backend->read("Other");
        QCOMPARE(session->appliedScheme(), "Other");
        QCOMPARE(session->stagedScheme(), "Other");
        QVERIFY(!session->isDirty());
        QVERIFY(!session->hasConflict());
    }

    void dirtyExternalChangeConflictsWithoutLosingSelection()
    {
        session->selectScheme("Aero");
        backend->changed();
        backend->read("Other");
        QVERIFY(session->hasConflict());
        QCOMPARE(session->appliedScheme(), "Other");
        QCOMPARE(session->stagedScheme(), "Aero");
        QSignalSpy finished(session, &SettingsSession::applyFinished);
        session->apply();
        QVERIFY(backend->writes.isEmpty());
        QVERIFY(!finished.at(0).at(0).toBool());
        session->revert();
        QVERIFY(!session->isDirty());
        backend->read("Newest");
        QCOMPARE(session->stagedScheme(), "Newest");
        QVERIFY(!session->hasConflict());
    }

    void preflightCatchesExternalChangeBeforeWatchNotification()
    {
        session->selectScheme("Aero");
        QSignalSpy finished(session, &SettingsSession::applyFinished);
        session->apply();
        backend->read("Other");
        QVERIFY(backend->writes.isEmpty());
        QVERIFY(session->hasConflict());
        QCOMPARE(session->stagedScheme(), "Aero");
        QVERIFY(!finished.at(0).at(0).toBool());
    }

    void revertReadsCurrentStateWithoutWriting()
    {
        session->selectScheme("Aero");
        session->revert();
        QVERIFY(!session->isDirty());
        QCOMPARE(session->stagedScheme(), "LegacyScheme");
        backend->read("Other");
        QCOMPARE(session->stagedScheme(), "Other");
        QVERIFY(backend->writes.isEmpty());
    }

    void cancelledApplyStillReadsBackAndRetainsError()
    {
        session->selectScheme("Aero");
        session->apply();
        backend->read("LegacyScheme");
        session->cancel();
        QCOMPARE(backend->cancellations, 1);
        backend->write(false);
        backend->read("LegacyScheme");
        QVERIFY(!session->isBusy());
        QVERIFY(session->isDirty());
        QVERIFY(!session->errorText().isEmpty());
    }

    void failedReadbackRequiresReloadBeforeRetry()
    {
        QSignalSpy finished(session, &SettingsSession::applyFinished);
        session->selectScheme("Aero");
        session->apply();
        backend->read("LegacyScheme");
        backend->write();
        backend->readError();
        QVERIFY(!session->isReady());
        QVERIFY(session->hasConflict());
        QVERIFY(session->isDirty());
        QVERIFY(!finished.at(0).at(0).toBool());
        session->revert();
        backend->read("Aero");
        QVERIFY(session->isReady());
        QVERIFY(!session->isDirty());
        QVERIFY(!session->hasConflict());
    }

    void changesDuringApplyAreRefreshedAfterReadback()
    {
        session->selectScheme("Aero");
        session->apply();
        backend->read("LegacyScheme");
        backend->changed();
        backend->write();
        backend->read("Aero");
        QTRY_COMPARE(backend->reads, 4);
        backend->read("Other");
        QCOMPARE(session->appliedScheme(), "Other");
        QCOMPARE(session->stagedScheme(), "Other");
        QVERIFY(!session->isDirty());
    }

private:
    FakeColorSchemeBackend *backend = nullptr;
    ColorSchemeSettings *session = nullptr;
};

QTEST_GUILESS_MAIN(SettingsSessionTest)
#include "settings_session_test.moc"
