#include "ui/SettingsNavigationGuard.h"
#include "settings/SettingsSession.h"
#include <QtTest>

class FakeSession final : public SettingsSession {
public:
    int writes = 0, discards = 0;
    bool deferRevert = false;
    void edit() { m_dirty = true; emit stateChanged(); }
    void busy(bool value) { m_busy = value; emit stateChanged(); }
    void apply() override { ++writes; m_busy = true; emit stateChanged(); }
    void revert() override { ++discards; if (deferRevert) m_busy = true; else m_dirty = false; emit stateChanged(); }
    void finishRevert(bool success) {
        m_busy = false;
        m_dirty = !success; m_conflict = !success;
        m_error = success ? QString() : QStringLiteral("Reload failed; draft retained.");
        emit stateChanged();
    }
    void finish(bool success) {
        m_busy = false;
        if (success) m_dirty = false;
        emit stateChanged();
        emit applyFinished(success);
    }
};

class NavigationGuardTest final : public QObject {
    Q_OBJECT
private slots:
    void cancelAndDiscard() {
        FakeSession session;
        SettingsNavigationGuard guard;
        int historyChanges = 0;
        session.edit();
        guard.request(&session, [] { return SettingsNavigationGuard::Choice::Cancel; },
                      [&] { ++historyChanges; });
        QCOMPARE(historyChanges, 0);
        QVERIFY(session.isDirty());
        QVERIFY(!guard.pending());
        guard.request(&session, [] { return SettingsNavigationGuard::Choice::Discard; },
                      [&] { ++historyChanges; });
        QCOMPARE(historyChanges, 1);
        QCOMPARE(session.discards, 1);
        QCOMPARE(session.writes, 0);
    }
    void discardWaitsForVerifiedReloadAndDoesNotRepromptOnFailure_data() {
        QTest::addColumn<bool>("success");
        QTest::newRow("success") << true;
        QTest::newRow("failure") << false;
    }
    void discardWaitsForVerifiedReloadAndDoesNotRepromptOnFailure() {
        QFETCH(bool, success);
        FakeSession session; session.deferRevert = true; session.edit();
        SettingsNavigationGuard guard;
        int prompts = 0, moves = 0;
        guard.request(&session, [&] { ++prompts; return SettingsNavigationGuard::Choice::Discard; }, [&] { ++moves; });
        QVERIFY(guard.pending()); QCOMPARE(moves, 0); QCOMPARE(prompts, 1);
        session.finishRevert(success);
        QVERIFY(!guard.pending()); QCOMPARE(moves, success ? 1 : 0);
        QCOMPARE(prompts, 1); QCOMPARE(session.discards, 1);
        QCOMPARE(session.isDirty(), !success);
    }
    void applyWaitsForConfirmedSuccess() {
        FakeSession session;
        SettingsNavigationGuard guard;
        int moves = 0;
        session.edit();
        guard.request(&session, [] { return SettingsNavigationGuard::Choice::Apply; }, [&] { ++moves; });
        QCOMPARE(session.writes, 1);
        QCOMPARE(moves, 0);
        QVERIFY(guard.pending());
        // A second click cannot queue a second write or corrupt history.
        guard.request(&session, [] { return SettingsNavigationGuard::Choice::Discard; }, [&] { moves += 10; });
        session.finish(false);
        QCOMPARE(moves, 0);
        QVERIFY(session.isDirty());
        QVERIFY(!guard.pending());
        guard.request(&session, [] { return SettingsNavigationGuard::Choice::Apply; }, [&] { ++moves; });
        session.finish(true);
        QCOMPARE(moves, 1);
        QVERIFY(!guard.pending());
    }
    void alreadyBusyAndDestroyedSession() {
        SettingsNavigationGuard guard;
        auto *session = new FakeSession;
        int moves = 0;
        session->busy(true);
        guard.request(session, [] { return SettingsNavigationGuard::Choice::Cancel; }, [&] { ++moves; });
        QCOMPARE(moves, 0);
        session->busy(false);
        QCOMPARE(moves, 1);
        session->edit();
        guard.request(session, [] { return SettingsNavigationGuard::Choice::Apply; }, [&] { ++moves; });
        delete session;
        QVERIFY(!guard.pending());
        QCOMPARE(moves, 1);
    }
    void refreshStartsDuringThePrompt() {
        FakeSession session;
        SettingsNavigationGuard guard;
        int prompts = 0, moves = 0;
        session.edit();
        guard.request(&session, [&] {
            ++prompts;
            session.busy(true);
            return SettingsNavigationGuard::Choice::Apply;
        }, [&] { ++moves; });
        QVERIFY(guard.pending());
        QCOMPARE(session.writes, 0);
        session.busy(false);
        QCOMPARE(prompts, 1);
        QCOMPARE(session.writes, 1);
        session.finish(true);
        QCOMPARE(moves, 1);
        QVERIFY(!guard.pending());
    }
};
QTEST_GUILESS_MAIN(NavigationGuardTest)
#include "navigation_guard_test.moc"
