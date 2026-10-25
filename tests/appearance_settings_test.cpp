#include <QtTest>
#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QSignalSpy>
#include <QPointer>
#include "settings/AppearanceSettings.h"
#include "ui/pages/WindowBehaviorPage.h"

class FakeAppearanceBackend : public AppearanceBackend {
public:
    int reads = 0;
    QStringList writes;
    void readCurrent() override { ++reads; }
    void writeAction(const QString &expected, const QString &action) override
    {
        baseline = expected;
        writes << action;
    }
    void report(const QString &action, bool available = true)
    {
        AppearanceState state;
        state.doubleClickAction = action;
        state.writable = true;
        state.reloadAvailable = available;
        state.unavailableReason = available ? QString() : QStringLiteral("KWin unavailable");
        emit readFinished(true, state, QString());
    }
    void failedRead() { emit readFinished(false, {}, QStringLiteral("Read failed")); }
    void written(bool success = true)
    {
        emit writeFinished(success, success ? QString() : QStringLiteral("Saved but runtime verification failed"));
    }
    void changed() { emit externalChange(); }
    QString baseline;
};

class AppearanceSettingsTest : public QObject {
    Q_OBJECT
private slots:
    void init()
    {
        backend = new FakeAppearanceBackend;
        session = new AppearanceSettings(backend);
        QTRY_COMPARE(backend->reads, 1);
        backend->report("Maximize");
    }
    void cleanup() { delete session; }

    void controlsStageAndSessionSurvivesPageRecreation()
    {
        auto *page = new WindowBehaviorPage(nullptr, session, nullptr);
        auto *combo = page->findChild<QComboBox *>("titlebarDoubleClickAction");
        QVERIFY(combo);
        QVERIFY(!combo->accessibleName().isEmpty());
        combo->setCurrentIndex(combo->findData("Minimize"));
        QVERIFY(session->isDirty());
        QVERIFY(backend->writes.isEmpty());
        QCOMPARE(session->stagedAction(), "Minimize");
        delete page;
        WindowBehaviorPage replacement(nullptr, session, nullptr);
        QCOMPARE(replacement.settingsSession(), static_cast<SettingsSession *>(session));
        QCOMPARE(replacement.findChild<QComboBox *>("titlebarDoubleClickAction")->currentData().toString(), "Minimize");
        QVERIFY(replacement.findChild<QPushButton *>("applyWindowBehavior")->isEnabled());
    }
    void applyNeedsBothRuntimeCompletionAndPersistedReadback()
    {
        QSignalSpy finished(session, &SettingsSession::applyFinished);
        session->selectAction("Minimize");
        session->apply();
        QVERIFY(session->isBusy());
        QVERIFY(backend->writes.isEmpty());
        backend->report("Maximize");
        QCOMPARE(backend->writes, QStringList{"Minimize"});
        QCOMPARE(backend->baseline, "Maximize");
        session->selectAction("Nothing");
        session->revert();
        QCOMPARE(session->stagedAction(), "Minimize");
        backend->written();
        QVERIFY(finished.isEmpty());
        backend->report("Minimize");
        QCOMPARE(finished.size(), 1);
        QVERIFY(finished[0][0].toBool());
        QVERIFY(!session->isDirty());
        QVERIFY(!session->hasConflict());
        QVERIFY(session->resultText().contains("verified"));
    }
    void preflightDetectsUnannouncedExternalChange()
    {
        session->selectAction("Minimize");
        session->apply();
        backend->report("Nothing");
        QVERIFY(backend->writes.isEmpty());
        QVERIFY(session->hasConflict());
        QCOMPARE(session->stagedAction(), "Minimize");
        QCOMPARE(session->appliedAction(), "Nothing");
    }
    void externalChangesConflictAndRevertReconciles()
    {
        session->selectAction("Minimize");
        backend->changed();
        backend->report("Nothing");
        QVERIFY(session->hasConflict());
        session->apply();
        QVERIFY(backend->writes.isEmpty());
        session->revert();
        backend->report("Nothing");
        QVERIFY(!session->isDirty());
        QVERIFY(!session->hasConflict());
        QCOMPARE(session->stagedAction(), "Nothing");
    }
    void missingServiceDisablesApplyAndReportsReason()
    {
        session->selectAction("Minimize");
        backend->changed();
        backend->report("Maximize", false);
        WindowBehaviorPage page(nullptr, session, nullptr);
        QVERIFY(!session->canApply());
        QVERIFY(!page.findChild<QPushButton *>("applyWindowBehavior")->isEnabled());
        QVERIFY(page.findChild<QLabel *>("windowBehaviorMessage")->text().contains("KWin unavailable"));
        session->apply();
        QVERIFY(backend->writes.isEmpty());
    }
    void failedReloadReportsSavedValueWithoutClaimingSuccess()
    {
        QSignalSpy finished(session, &SettingsSession::applyFinished);
        session->selectAction("Minimize");
        session->apply();
        backend->report("Maximize");
        backend->written(false);
        backend->report("Minimize");
        QVERIFY(!finished[0][0].toBool());
        QCOMPARE(session->appliedAction(), "Minimize");
        QVERIFY(session->resultText().isEmpty());
        QVERIFY(session->hasConflict());
        QVERIFY(session->errorText().contains("runtime verification failed"));
        backend->changed();
        backend->report("Minimize");
        QVERIFY(session->errorText().contains("runtime verification failed"));
    }
    void failedReadbackRequiresExplicitReconciliation()
    {
        session->selectAction("Minimize");
        session->apply();
        backend->report("Maximize");
        backend->written();
        backend->failedRead();
        QVERIFY(!session->isReady());
        QVERIFY(session->hasConflict());
        QCOMPARE(session->stagedAction(), "Minimize");
        const int writes = backend->writes.size();
        session->apply();
        QCOMPARE(backend->writes.size(), writes);
    }
    void mismatchedReadbackPreservesSelection()
    {
        session->selectAction("Minimize");
        session->apply();
        backend->report("Maximize");
        backend->written();
        backend->report("Nothing");
        QVERIFY(session->isDirty());
        QVERIFY(session->hasConflict());
        QCOMPARE(session->stagedAction(), "Minimize");
    }
    void existingKdeActionIsPreservedInTheControl()
    {
        backend->changed();
        backend->report("Maximize (vertical only)");
        WindowBehaviorPage page(nullptr, session, nullptr);
        auto *combo = page.findChild<QComboBox *>("titlebarDoubleClickAction");
        QCOMPARE(combo->currentData().toString(), "Maximize (vertical only)");
        QVERIFY(!session->isDirty());
        QVERIFY(backend->writes.isEmpty());
        session->selectAction("Close"); // Existing actions can be read, not newly authored.
        QCOMPARE(session->stagedAction(), "Maximize (vertical only)");
    }
    void pendingStatusIsScopedToWriteAndSurvivesPageReplacement()
    {
        emit backend->operationPending("stale idle progress"); QVERIFY(session->pendingMessage().isEmpty());
        session->selectAction("Minimize"); session->apply();
        emit backend->operationPending("stale preflight progress"); QVERIFY(session->pendingMessage().isEmpty());
        backend->report("Maximize"); emit backend->operationPending("Saving continues in the background.");
        QVERIFY(session->isBusy()); QVERIFY(session->isDirty()); QVERIFY(session->errorText().isEmpty());
        WindowBehaviorPage page(nullptr, session, nullptr);
        QVERIFY(page.findChild<QLabel *>("windowBehaviorStatus")->text().contains("continues"));
        QVERIFY(!page.findChild<QPushButton *>("applyWindowBehavior")->isEnabled());
        QVERIFY(!page.findChild<QPushButton *>("revertWindowBehavior")->isEnabled());
        backend->written(); emit backend->operationPending("late write progress"); QVERIFY(session->pendingMessage().isEmpty());
        backend->report("Minimize"); QVERIFY(!session->isDirty()); QVERIFY(!session->isBusy());
    }
    void explicitFailureCannotBeErasedByMatchingReadback()
    {
        session->selectAction("Minimize"); session->apply(); backend->report("Maximize");
        QSignalSpy finished(session, &SettingsSession::applyFinished);
        emit backend->writeFinished(true, "The saved source receipt changed."); backend->report("Minimize");
        QCOMPARE(finished.size(), 1); QVERIFY(!finished[0][0].toBool());
        QVERIFY(session->isDirty()); QVERIFY(session->hasConflict()); QVERIFY(session->errorText().contains("receipt"));
        backend->changed(); backend->report("Minimize"); QVERIFY(session->isDirty()); QVERIFY(session->hasConflict());
        session->revert(); backend->failedRead(); QVERIFY(session->isDirty()); QCOMPARE(session->stagedAction(), "Minimize"); QVERIFY(session->errorText().contains("Read failed"));
        session->revert(); backend->report("Minimize"); QVERIFY(!session->isDirty()); QVERIFY(!session->hasConflict());
    }
    void observersMayDeleteTheSession_data()
    {
        QTest::addColumn<int>("transition");
        QTest::newRow("refresh-start") << 0; QTest::newRow("apply-start") << 1;
        QTest::newRow("apply-finished") << 2; QTest::newRow("pending-progress") << 3;
        QTest::newRow("blocked-apply") << 4; QTest::newRow("refresh-finished") << 5;
    }
    void observersMayDeleteTheSession()
    {
        QFETCH(int, transition); QPointer<AppearanceSettings> guard(session);
        const auto remove = [this] { auto *victim = session; session = nullptr; backend = nullptr; delete victim; };
        if (transition == 0) {
            connect(session, &SettingsSession::stateChanged, this, remove); session->refresh();
        } else if (transition == 1) {
            session->selectAction("Minimize"); connect(session, &SettingsSession::stateChanged, this, remove); session->apply();
        } else if (transition == 4) {
            session->selectAction("Minimize"); backend->changed(); backend->report("Nothing");
            connect(session, &SettingsSession::applyFinished, this, remove); session->apply();
        } else if (transition == 5) {
            session->refresh(); connect(session, &SettingsSession::stateChanged, this, remove); backend->report("Maximize");
        } else {
            session->selectAction("Minimize"); session->apply(); backend->report("Maximize");
            if (transition == 3) {
                connect(session, &SettingsSession::stateChanged, this, remove); emit backend->operationPending("Saving continues.");
            } else {
                connect(session, &SettingsSession::applyFinished, this, remove); backend->written(); backend->report("Minimize");
            }
        }
        QVERIFY(!guard); QVERIFY(!session);
    }
private:
    FakeAppearanceBackend *backend = nullptr;
    AppearanceSettings *session = nullptr;
};
QTEST_MAIN(AppearanceSettingsTest)
#include "appearance_settings_test.moc"
