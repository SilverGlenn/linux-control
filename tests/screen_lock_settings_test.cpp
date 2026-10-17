#include <QtTest>
#include <QPointer>
#include "screen_lock_fixture.h"
#include <limits>
#include <cmath>

class FixtureScreenLockBackend final : public ScreenLockBackend {
public:
    ScreenLockSnapshot current = screenLockFixture(), request;
    int reads = 0, writes = 0;
    bool failRead = false, holdRead = false, holdWrite = false, saved = true, acknowledged = true;
    QString writeError;
    quint64 readToken = 0, writeToken = 0;
    void readCurrent(quint64 token) override { ++reads; readToken = token; if (!holdRead) completeRead(); }
    void completeRead() { emit readFinished(readToken, !failRead, current, failRead ? QStringLiteral("Read failed") : QString()); }
    void writeChanges(quint64 token, const ScreenLockSnapshot &, const ScreenLockSnapshot &requested) override {
        ++writes; writeToken = token; request = requested; if (!holdWrite) completeWrite();
    }
    void completeWrite() {
        if (saved) current = savedScreenLockFixture(request);
        emit writeFinished(writeToken, saved, acknowledged, writeError);
    }
};
class ScreenLockSettingsTest final : public QObject {
    Q_OBJECT
private slots:
    void strictValues() {
        QVERIFY(ScreenLockTypes::validValue(ScreenLockKey::Timeout, 0.0));
        QVERIFY(ScreenLockTypes::validValue(ScreenLockKey::Timeout, 9999.0));
        QVERIFY(!ScreenLockTypes::validValue(ScreenLockKey::Timeout, 5));
        QVERIFY(!ScreenLockTypes::validValue(ScreenLockKey::Timeout, 0.5));
        QVERIFY(!ScreenLockTypes::validValue(ScreenLockKey::Timeout, 10000.0));
        QVERIFY(!ScreenLockTypes::validValue(ScreenLockKey::Timeout, std::numeric_limits<double>::infinity()));
        QVERIFY(ScreenLockTypes::validValue(ScreenLockKey::LockGrace, 599940));
        QVERIFY(!ScreenLockTypes::validValue(ScreenLockKey::LockGrace, 599941));
        QVERIFY(!ScreenLockTypes::validValue(ScreenLockKey::LockGrace, -1));
        QVERIFY(!ScreenLockTypes::validValue(ScreenLockKey::RequirePassword, QStringLiteral("false")));
        for (auto key : {ScreenLockKey::Lock, ScreenLockKey::LockOnStart, ScreenLockKey::Theme, ScreenLockKey::WallpaperPlugin})
            QVERIFY(!ScreenLockTypes::validValue(key, ScreenLockTypes::builtinDefault(key)));
    }
    void atomicCompanionsAndDormantPreservation() {
        auto base = screenLockFixture(); auto draft = base; QString error;
        base.fields[ScreenLockKey::Autolock].immutable = true; draft = base;
        QVERIFY(!ScreenLockTypes::selectTimeout(draft, base, 0.0, error));
        QVERIFY(ScreenLockTypes::sameDraft(draft, base));
        QVERIFY(ScreenLockTypes::selectTimeout(draft, base, 10.0, error));
        QCOMPARE(draft.intents.size(), 1); // unchanged protected companion is preserved
        base.fields[ScreenLockKey::RequirePassword].immutable = true; draft = base;
        QVERIFY(ScreenLockTypes::selectGrace(draft, base, 30, error));
        QCOMPARE(draft.intents.size(), 1);
        QVERIFY(!ScreenLockTypes::selectNoPassword(draft, base, error));
        base.fields[ScreenLockKey::RequirePassword].immutable = false; draft = base;
        QVERIFY(ScreenLockTypes::selectNoPassword(draft, base, error));
        QCOMPARE(draft.fields[ScreenLockKey::LockGrace].value, base.fields[ScreenLockKey::LockGrace].value);
        QCOMPARE(draft.intents.size(), 1);
        QVERIFY(ScreenLockTypes::selectGrace(draft, base, 60, error));
        QVERIFY(!draft.intents.contains(ScreenLockKey::RequirePassword));
    }
    void absentCompanionsReturnToNativeLoadedState() {
        auto base = screenLockFixture();
        for (auto key : {ScreenLockKey::Autolock, ScreenLockKey::RequirePassword}) base.fields[key].localPresent = base.fields[key].effectivePresent = false;
        auto draft = base; QString error;
        QVERIFY(ScreenLockTypes::selectTimeout(draft, base, 0.0, error));
        QVERIFY(draft.intents.contains(ScreenLockKey::Autolock));
        QVERIFY(ScreenLockTypes::selectTimeout(draft, base, 10.0, error));
        QVERIFY(!draft.intents.contains(ScreenLockKey::Autolock));
        QVERIFY(ScreenLockTypes::selectNoPassword(draft, base, error));
        QVERIFY(ScreenLockTypes::selectGrace(draft, base, 30, error));
        QVERIFY(!draft.intents.contains(ScreenLockKey::RequirePassword));
    }
    void nanDefaultsUseNativeInequalityRatherThanSnapshotEquality() {
        auto base = screenLockFixture(); auto &f = base.fields[ScreenLockKey::Timeout];
        f.value = f.defaultValue = std::numeric_limits<double>::quiet_NaN();
        f.defaultNoOp = false; f.resettable = true; f.inheritedPresent = true;
        auto draft = base; QString error;
        QVERIFY(ScreenLockTypes::restore(draft, base, ScreenLockKey::Timeout, error));
        QVERIFY(draft.intents.contains(ScreenLockKey::Timeout));
        QVERIFY(ScreenLockTypes::requestValid(base, draft, error));
        QVERIFY(ScreenLockTypes::matchesRequest(base, draft, savedScreenLockFixture(draft), error));
    }
    void timeoutSafety_data() {
        QTest::addColumn<double>("minutes"); QTest::addColumn<bool>("automatic"); QTest::addColumn<bool>("safe");
        const double edge = double(std::numeric_limits<int>::max()) / 60000.0;
        QTest::newRow("boundary") << edge << true << true;
        QTest::newRow("truncation-still-fits") << (double(std::numeric_limits<int>::max()) + 0.75) / 60000.0 << true << true;
        QTest::newRow("overflow") << (double(std::numeric_limits<int>::max()) + 1.0) / 60000.0 << true << false;
        QTest::newRow("submillisecond") << 0.0000001 << true << true;
        QTest::newRow("zero") << 0.0 << true << true;
        QTest::newRow("negative") << -50.0 << true << true;
        QTest::newRow("dormant-huge") << 1e99 << false << true;
        QTest::newRow("positive-infinity") << std::numeric_limits<double>::infinity() << true << false;
        QTest::newRow("negative-infinity-dormant") << -std::numeric_limits<double>::infinity() << true << true;
        QTest::newRow("nan-dormant") << std::numeric_limits<double>::quiet_NaN() << true << true;
    }
    void timeoutSafety() {
        QFETCH(double, minutes); QFETCH(bool, automatic); QFETCH(bool, safe);
        auto fields = screenLockFixture().fields; fields[ScreenLockKey::Timeout].value = minutes;
        fields[ScreenLockKey::Autolock].value = automatic; QString error;
        QCOMPARE(ScreenLockTypes::activeBranchValid(fields, error), safe);
    }
    void graceSafetyRegardlessOfPassword_data() {
        QTest::addColumn<int>("seconds"); QTest::addColumn<bool>("legacy"); QTest::addColumn<bool>("safe");
        QTest::newRow("maximum-safe") << 2147483 << true << true;
        QTest::newRow("positive-overflow") << 2147484 << true << false;
        QTest::newRow("negative-safe") << -2147483 << true << true;
        QTest::newRow("negative-overflow") << -2147484 << true << false;
        QTest::newRow("legacy-disabled") << std::numeric_limits<int>::max() << false << true;
    }
    void graceSafetyRegardlessOfPassword() {
        QFETCH(int, seconds); QFETCH(bool, legacy); QFETCH(bool, safe);
        auto fields = screenLockFixture().fields; fields[ScreenLockKey::LockGrace].value = seconds;
        fields[ScreenLockKey::RequirePassword].value = false; fields[ScreenLockKey::Lock].value = legacy;
        QString error; QCOMPARE(ScreenLockTypes::activeBranchValid(fields, error), safe);
    }
    void nativeDefaultsAndForgedHiddenFields() {
        auto base = screenLockFixture(); auto &f = base.fields[ScreenLockKey::Timeout];
        f.value = 10.0; f.defaultValue = 0.5; f.defaultNoOp = false; f.resettable = true; f.inheritedPresent = true;
        auto draft = base; QString error;
        QVERIFY(ScreenLockTypes::restore(draft, base, ScreenLockKey::Timeout, error));
        QCOMPARE(draft.fields[ScreenLockKey::Timeout].value, QVariant(0.5));
        QVERIFY(ScreenLockTypes::requestValid(base, draft, error));
        QVERIFY(ScreenLockTypes::matchesRequest(base, draft, savedScreenLockFixture(draft), error));
        auto forged = draft; forged.fields[ScreenLockKey::Theme].value = QStringLiteral("other");
        QVERIFY(!ScreenLockTypes::requestValid(base, forged, error));
        forged = draft; forged.intents[ScreenLockKey::Lock] = ScreenLockIntent::Set;
        QVERIFY(!ScreenLockTypes::requestValid(base, forged, error));
        forged = draft; forged.intents[ScreenLockKey::Timeout] = ScreenLockIntent(99);
        QVERIFY(!ScreenLockTypes::requestValid(base, forged, error));
        f.value = f.defaultValue; f.defaultNoOp = true; draft = base;
        QVERIFY(ScreenLockTypes::restore(draft, base, ScreenLockKey::Timeout, error)); QVERIFY(draft.intents.isEmpty());
        QVERIFY(draft.fields[ScreenLockKey::Timeout].localPresent);
    }
    void dormantNanDoesNotInventConflict() {
        auto base = screenLockFixture(); base.fields[ScreenLockKey::Timeout].value = std::numeric_limits<double>::quiet_NaN();
        auto draft = base; QString error;
        QVERIFY(ScreenLockTypes::sameConfiguration(base, draft));
        QVERIFY(ScreenLockTypes::stage(draft, base, ScreenLockKey::LockOnResume, false, error));
        QVERIFY(ScreenLockTypes::requestValid(base, draft, error));
        QVERIFY(ScreenLockTypes::matchesRequest(base, draft, savedScreenLockFixture(draft), error));
    }
    void unknownOwnedTupleCanBeRepairedExplicitly() {
        auto *b = new FixtureScreenLockBackend;
        b->current.fields[ScreenLockKey::Timeout].known = false;
        b->current.fields[ScreenLockKey::Autolock].known = false;
        ScreenLockSettings s(b); QTRY_VERIFY(!s.isBusy()); QVERIFY(!s.canApply());
        s.selectTimeout(10); QVERIFY(s.canApply()); QCOMPARE(b->writes, 0);
        s.apply(); QCOMPARE(b->writes, 1); QVERIFY(!s.isDirty());
    }
    void hiddenUnknownPreventsReloadAndCannotBeSilentlyRepaired() {
        auto *b = new FixtureScreenLockBackend; b->current.fields[ScreenLockKey::Lock].known = false;
        ScreenLockSettings s(b); QTRY_VERIFY(!s.isBusy());
        s.selectValue(ScreenLockKey::LockOnResume, false); QVERIFY(s.isDirty()); QVERIFY(!s.canApply());
        s.selectValue(ScreenLockKey::Lock, true); QVERIFY(!s.staged().fields[ScreenLockKey::Lock].known);
        s.apply(); QCOMPARE(b->writes, 0);
    }
    void offlineSaveDoesNotNeedFakeReloadAcknowledgement() {
        auto *b = new FixtureScreenLockBackend; b->acknowledged = false;
        b->current.runtime.owner.clear(); b->current.runtime.serviceAvailable = b->current.runtime.providerVerified = false;
        ScreenLockSettings s(b); QTRY_VERIFY(!s.isBusy());
        s.selectTimeout(10); s.apply(); QCOMPARE(b->writes, 1); QVERIFY(!s.isDirty());
        QVERIFY(s.resultText().contains("deferred")); QVERIFY(s.resultText().contains("No service was started"));
    }
    void preflightAndReadbackPreserveUncertainDraft() {
        auto *b = new FixtureScreenLockBackend; ScreenLockSettings s(b); QTRY_VERIFY(!s.isBusy());
        s.selectGrace(30); b->holdRead = true; s.apply();
        emit b->readFinished(b->readToken-1, true, b->current, {}); QVERIFY(s.isBusy());
        b->current.fields[ScreenLockKey::WallpaperPlugin].revision += "-external"; b->completeRead();
        QVERIFY(s.hasConflict()); QVERIFY(s.isDirty()); QCOMPARE(b->writes, 0);
        b->holdRead = false; s.revert(); QVERIFY(!s.isDirty());
        s.selectGrace(30); b->holdWrite = true; s.apply(); QVERIFY(s.isBusy());
        b->acknowledged = false; b->completeWrite(); QVERIFY(s.hasConflict()); QVERIFY(s.isDirty());
        b->failRead = true; s.revert(); QVERIFY(s.isDirty()); QVERIFY(s.hasConflict());
        b->failRead = false; s.refresh(); QVERIFY(s.isDirty()); QVERIFY(s.hasConflict());
        s.revert(); QVERIFY(!s.isDirty()); QVERIFY(!s.hasConflict());
    }
    void softDeadlineRetainsDraftAndWaitsForActualCompletion() {
        auto *b = new FixtureScreenLockBackend; b->holdWrite = true;
        ScreenLockSettings s(b); QTRY_VERIFY(!s.isBusy());
        QSignalSpy completed(&s, &SettingsSession::applyFinished);
        s.selectGrace(30); const auto draft = s.staged(); s.apply();
        QVERIFY(s.isBusy()); QCOMPARE(b->writes, 1);
        const auto initialStatus = s.applySupportText();
        emit b->operationPending(b->writeToken - 1, QStringLiteral("Stale progress"));
        QCOMPARE(s.applySupportText(), initialStatus);
        emit b->operationPending(b->writeToken, QStringLiteral("Saving is taking longer; please wait."));
        QVERIFY(s.applySupportText().contains("taking longer")); QVERIFY(s.errorText().isEmpty());
        QVERIFY(s.isBusy()); QVERIFY(s.isDirty()); QVERIFY(!s.hasConflict()); QCOMPARE(completed.size(), 0);
        QVERIFY(!s.canApply()); QVERIFY(!s.canEdit(ScreenLockKey::LockGrace));
        const int reads = b->reads;
        s.apply(); s.revert(); s.selectGrace(60); s.refresh();
        QCOMPARE(b->writes, 1); QCOMPARE(b->reads, reads);
        QVERIFY(ScreenLockTypes::sameDraft(s.staged(), draft));
        b->holdRead = true; b->completeWrite();
        QVERIFY(s.isBusy()); QCOMPARE(completed.size(), 0);
        QVERIFY(!s.applySupportText().contains("taking longer"));
        emit b->operationPending(b->writeToken, QStringLiteral("Late progress"));
        QVERIFY(!s.applySupportText().contains("Late progress"));
        b->completeRead();
        QVERIFY(!s.isBusy()); QVERIFY(!s.isDirty()); QVERIFY(s.errorText().isEmpty());
        QCOMPARE(completed.size(), 1); QVERIFY(completed.first().first().toBool());
        emit b->operationPending(b->writeToken, QStringLiteral("Late progress"));
        QVERIFY(s.applySupportText().isEmpty());
    }
    void deletionAtPendingProgressDoesNotCompleteOrRetry() {
        auto *b = new FixtureScreenLockBackend; b->holdWrite = true;
        QPointer<ScreenLockSettings> s = new ScreenLockSettings(b); QTRY_VERIFY(!s->isBusy());
        s->selectGrace(30); s->apply(); const auto token = b->writeToken;
        QSignalSpy completed(s, &SettingsSession::applyFinished);
        connect(s, &SettingsSession::stateChanged, this, [s] {
            if (s && s->applySupportText().contains("Still saving")) delete s;
        });
        emit b->operationPending(token, QStringLiteral("Still saving"));
        QVERIFY(!s); QCOMPARE(completed.size(), 0);
    }
    void sessionDeletionAtOutwardSignalsIsSafe() {
        auto *b = new FixtureScreenLockBackend; QPointer<ScreenLockSettings> s = new ScreenLockSettings(b);
        QTRY_VERIFY(!s->isBusy()); connect(s, &SettingsSession::stateChanged, this, [s] { if (s && s->isBusy()) delete s; });
        s->refresh(); QVERIFY(!s);
        b = new FixtureScreenLockBackend; s = new ScreenLockSettings(b); QTRY_VERIFY(!s->isBusy()); s->selectGrace(30);
        connect(s, &SettingsSession::stateChanged, this, [s] { if (s && !s->isBusy()) delete s; });
        s->apply(); QVERIFY(!s);
    }
};
QTEST_GUILESS_MAIN(ScreenLockSettingsTest)
#include "screen_lock_settings_test.moc"
