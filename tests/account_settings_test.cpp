#include <QTest>
#include <QSignalSpy>
#include <QLineEdit>
#include <QPushButton>
#include "settings/AccountSettings.h"
#include "ui/pages/UserAccountsPage.h"
#include "operations/OperationManager.h"

class FakeAccountBackend final : public AccountBackend {
public:
    QList<quint64> reads, authorizations, writes, cancellations;
    AccountState expected;
    QString requested;
    void readCurrent(quint64 token) override { reads.append(token); }
    void authorize(quint64 token) override { authorizations.append(token); }
    void cancelAuthorization(quint64 token) override { cancellations.append(token); }
    void writeName(quint64 token, const AccountState &state, const QString &name) override
    { writes.append(token); expected = state; requested = name; }
    void report(const AccountState &state) { emit readFinished(reads.last(), true, state, {}); }
    void reportFor(quint64 token, const AccountState &state) { emit readFinished(token, true, state, {}); }
    void readFailed() { emit readFinished(reads.last(), false, {}, "Account read failed"); }
    void authorized(AccountOutcome outcome = AccountOutcome::Success)
    { emit authorizationFinished(authorizations.last(), outcome, {}); }
    void authorizedFor(quint64 token) { emit authorizationFinished(token, AccountOutcome::Success, {}); }
    void written(AccountOutcome outcome = AccountOutcome::Success, const QString &error = {})
    { emit writeFinished(writes.last(), outcome, error); }
    void changed() { emit externalChange(); }
};
class AccountSettingsTest : public QObject {
    Q_OBJECT
private slots:
    void init()
    {
        backend = new FakeAccountBackend;
        session = new AccountSettings(backend);
        QTRY_COMPARE(backend->reads.size(), 1);
        baseline = {1000, "fixture", "Original Name", "/org/freedesktop/Accounts/User1000", ":1.99", {}, true, {}};
        backend->report(baseline);
        QVERIFY(session->isReady());
    }
    void cleanup()
    {
        delete session;
        QVERIFY(!OperationManager::instance()->hasActiveJobs());
    }
    void stagedNameSurvivesViewRecreation()
    {
        auto *page = new UserAccountsPage(nullptr, session, nullptr);
        auto *name = page->findChild<QLineEdit *>("accountDisplayName"); QVERIFY(name);
        name->selectAll(); QTest::keyClicks(name, "Native Name");
        QCOMPARE(session->stagedName(), "Native Name");
        QVERIFY(session->isDirty()); QVERIFY(backend->writes.isEmpty());
        delete page;
        UserAccountsPage replacement(nullptr, session, nullptr);
        QCOMPARE(replacement.settingsSession(), static_cast<SettingsSession *>(session));
        QCOMPARE(replacement.findChild<QLineEdit *>("accountDisplayName")->text(), "Native Name");
        QVERIFY(replacement.findChild<QPushButton *>("accountApply")->isEnabled());
    }
    void authorizationAndFreshReadbackRequired()
    {
        QSignalSpy finished(session, &SettingsSession::applyFinished);
        startAuthorization();
        QVERIFY(OperationManager::instance()->hasActiveMutatingJobs());
        QVERIFY(OperationManager::instance()->activityTitles().contains("Authorizing current-user name change"));
        QVERIFY(session->canCancel());
        backend->authorized();
        QVERIFY(backend->writes.isEmpty());
        backend->report(baseline);
        QCOMPARE(backend->writes.size(), 1); QCOMPARE(backend->requested, "New Name");
        QVERIFY(!session->canCancel());
        session->cancel(); QVERIFY(session->isBusy()); // A sent request is never called undone.
        backend->written(); QVERIFY(finished.isEmpty());
        auto saved = baseline; saved.realName = "New Name"; backend->report(saved);
        QCOMPARE(finished.size(), 1); QVERIFY(finished.first().first().toBool());
        QVERIFY(!session->isDirty()); QVERIFY(!session->hasConflict());
        QVERIFY(session->resultText().contains("verified"));
    }
    void denialAndDismissalAreDistinct()
    {
        QSignalSpy finished(session, &SettingsSession::applyFinished);
        startAuthorization(); backend->authorized(AccountOutcome::Denied);
        QVERIFY(!session->isBusy()); QCOMPARE(session->lastOutcome(), AccountOutcome::Denied);
        QVERIFY(session->errorText().contains("denied")); QVERIFY(session->isDirty());
        QVERIFY(backend->writes.isEmpty()); QVERIFY(!finished.last().first().toBool());
        session->apply(); backend->report(baseline); backend->authorized(AccountOutcome::Cancelled);
        QCOMPARE(session->lastOutcome(), AccountOutcome::Cancelled);
        QVERIFY(session->errorText().contains("canceled")); QVERIFY(backend->writes.isEmpty());
    }
    void cancellationIgnoresLateAuthorizationAndPreflight()
    {
        QSignalSpy finished(session, &SettingsSession::applyFinished);
        startAuthorization();
        const quint64 old = backend->authorizations.last();
        session->cancel(); QCOMPARE(session->lastOutcome(), AccountOutcome::Cancelled);
        QVERIFY(!session->isBusy()); QVERIFY(session->isDirty());
        QVERIFY(session->resultText().contains("before any account change"));
        session->apply(); const quint64 current = backend->reads.last();
        backend->authorizedFor(old); backend->reportFor(old, baseline);
        QVERIFY(backend->writes.isEmpty()); QVERIFY(session->isBusy());
        backend->reportFor(current, baseline); backend->authorized();
        session->cancel(); // Cancellation during the second preflight is also before dispatch.
        backend->reportFor(current, baseline);
        QVERIFY(backend->writes.isEmpty()); QVERIFY(!session->isBusy());
        QCOMPARE(finished.size(), 2);
    }
    void externalNameAndIdentityConflictsNeverWrite()
    {
        session->selectName("New Name"); session->apply();
        auto external = baseline; external.realName = "External Name";
        backend->report(external);
        QVERIFY(session->hasConflict()); QVERIFY(backend->authorizations.isEmpty());
        QCOMPARE(session->stagedName(), "New Name");
        session->revert(); backend->report(external); QVERIFY(!session->isDirty());
        session->selectName("Another Name"); session->apply(); backend->report(external); backend->authorized();
        auto replacement = external; replacement.userName = "someone-else";
        backend->report(replacement); QVERIFY(session->hasConflict()); QVERIFY(backend->writes.isEmpty());
    }
    void changedDuringAuthorizationRequiresRevert()
    {
        startAuthorization(); backend->authorized();
        auto changed = baseline; changed.realName = "Changed while authorizing";
        backend->report(changed);
        QVERIFY(backend->writes.isEmpty()); QVERIFY(session->hasConflict());
        QCOMPARE(session->stagedName(), "New Name");
    }
    void falseSuccessAndDifferentIdentityAreRejected()
    {
        QSignalSpy finished(session, &SettingsSession::applyFinished);
        startWrite(); backend->written(); backend->report(baseline);
        QVERIFY(!finished.last().first().toBool()); QVERIFY(session->hasConflict());
        QVERIFY(session->errorText().contains("not verified"));
        session->revert(); backend->report(baseline);
        startWrite(); backend->written();
        auto otherOwner = baseline; otherOwner.realName = "New Name"; otherOwner.serviceOwner = ":1.100";
        backend->report(otherOwner);
        QVERIFY(!finished.last().first().toBool()); QVERIFY(session->resultText().isEmpty());
    }
    void uncertainWriteCannotRaceFutureApply()
    {
        QSignalSpy finished(session, &SettingsSession::applyFinished);
        startWrite(); backend->written(AccountOutcome::Uncertain, "Request may still complete");
        auto looksSaved = baseline; looksSaved.realName = "New Name"; backend->report(looksSaved);
        QVERIFY(!finished.last().first().toBool()); QVERIFY(!session->canApply());
        QVERIFY(session->errorText().contains("may still complete"));
        session->revert(); backend->report(looksSaved); session->selectName("Later Name"); session->apply();
        QCOMPARE(backend->writes.size(), 1); QVERIFY(!session->canApply());
        QVERIFY(session->errorText().contains("earlier request"));
    }
    void readFailureAndCapabilityLossAreTruthful()
    {
        startWrite(); backend->written(); backend->readFailed();
        QVERIFY(!session->isReady()); QVERIFY(session->hasConflict()); QVERIFY(session->resultText().isEmpty());
        session->revert(); auto unavailable = baseline; unavailable.editable = false; unavailable.unavailableReason = "Method disappeared";
        backend->report(unavailable); QVERIFY(!session->canApply());
        session->selectName("New Name"); session->apply();
        QCOMPARE(backend->writes.size(), 1); QCOMPARE(session->errorText(), "Method disappeared");
    }
    void failedApplyRemainsVisibleAcrossAutomaticRefreshes()
    {
        startWrite();
        backend->written(AccountOutcome::Denied, "Authorization was denied by the fixture");
        backend->report(baseline);
        const QString retained = session->errorText();
        QVERIFY(retained.contains("denied"));
        backend->changed(); backend->readFailed();
        QCOMPARE(session->errorText(), retained);
        backend->changed(); backend->report(baseline);
        QCOMPARE(session->errorText(), retained);
        QVERIFY(session->hasConflict());
        session->revert(); backend->report(baseline);
        QVERIFY(session->errorText().isEmpty()); QVERIFY(!session->hasConflict());
    }
    void validatorsRejectDatabaseSeparatorsAndPermitUnicode()
    {
        QVERIFY(AccountSettings::validName(""));
        QVERIFY(AccountSettings::validName(QString::fromUtf8("José 山田 😀")));
        QVERIFY(!AccountSettings::validName("Full,Name")); QVERIFY(!AccountSettings::validName("Full:Name"));
        QVERIFY(!AccountSettings::validName("Name\nInjected"));
        QVERIFY(!AccountSettings::validName(QString(257, 'a')));
        QVERIFY(!AccountSettings::validName(QString(QChar(0xD800))));
    }
private:
    void startAuthorization()
    { session->selectName("New Name"); session->apply(); backend->report(baseline); QCOMPARE(backend->authorizations.size(), 1); }
    void startWrite()
    { session->selectName("New Name"); session->apply(); backend->report(baseline); backend->authorized(); backend->report(baseline); }
    FakeAccountBackend *backend = nullptr;
    AccountSettings *session = nullptr;
    AccountState baseline;
};
QTEST_MAIN(AccountSettingsTest)
#include "account_settings_test.moc"
