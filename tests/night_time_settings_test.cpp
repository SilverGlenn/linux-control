#include <QtTest>
#include "night_time_fixture.h"
#include <limits>

class FixtureNightTimeBackend final : public NightTimeBackend {
public:
    NightTimeSnapshot current = nightTimeFixture(), request;
    int reads = 0, writes = 0;
    bool failRead = false, holdRead = false, holdWrite = false, saved = true, notified = true;
    QString writeError;
    quint64 readToken = 0, writeToken = 0;
    void readCurrent(quint64 token) override {
        ++reads; readToken = token;
        if (!holdRead) completeRead();
    }
    void completeRead() { emit readFinished(readToken, !failRead, current, failRead ? QStringLiteral("Read failed") : QString()); }
    void writeChanges(quint64 token, const NightTimeSnapshot &, const NightTimeSnapshot &requested) override {
        ++writes; writeToken = token; request = requested; if (!holdWrite) completeWrite();
    }
    void completeWrite() {
        if (saved) current = savedNightTimeFixture(request);
        emit writeFinished(writeToken, saved, notified, writeError);
    }
};

class NightTimeSettingsTest final : public QObject {
    Q_OBJECT
private slots:
    void strictValues_data() {
        QTest::addColumn<NightTimeKey>("key"); QTest::addColumn<QVariant>("value"); QTest::addColumn<bool>("valid");
        QTest::newRow("source-name-rejected") << NightTimeKey::Source << QVariant("Times") << false;
        QTest::newRow("source-unknown") << NightTimeKey::Source << QVariant(2) << false;
        QTest::newRow("source") << NightTimeKey::Source << QVariant(1) << true;
        QTest::newRow("bool-string") << NightTimeKey::Automatic << QVariant("false") << false;
        QTest::newRow("bool") << NightTimeKey::Automatic << QVariant(false) << true;
        QTest::newRow("lat-int") << NightTimeKey::Latitude << QVariant(30) << false;
        QTest::newRow("lat") << NightTimeKey::Latitude << QVariant(-90.0) << true;
        QTest::newRow("lat-infinite") << NightTimeKey::Latitude << QVariant(std::numeric_limits<double>::infinity()) << false;
        QTest::newRow("lon-nan") << NightTimeKey::Longitude << QVariant(std::numeric_limits<double>::quiet_NaN()) << false;
        QTest::newRow("lon-outside") << NightTimeKey::Longitude << QVariant(181.0) << false;
        QTest::newRow("lon") << NightTimeKey::Longitude << QVariant(180.0) << true;
        QTest::newRow("time-string") << NightTimeKey::SunriseStart << QVariant("06:00") << false;
        QTest::newRow("time-invalid") << NightTimeKey::SunriseStart << QVariant(QTime()) << false;
        QTest::newRow("time-seconds-preserved") << NightTimeKey::SunriseStart << QVariant(QTime(6, 0, 17)) << true;
        QTest::newRow("duration-int") << NightTimeKey::TransitionDuration << QVariant(1800) << false;
        QTest::newRow("duration-small") << NightTimeKey::TransitionDuration << QVariant(uint(59)) << false;
        QTest::newRow("duration-big") << NightTimeKey::TransitionDuration << QVariant(uint(36001)) << false;
        QTest::newRow("duration") << NightTimeKey::TransitionDuration << QVariant(uint(1800)) << true;
    }
    void strictValues() {
        QFETCH(NightTimeKey, key); QFETCH(QVariant, value); QFETCH(bool, valid);
        QCOMPARE(NightTimeTypes::validValue(key, value), valid);
    }
    void draftAndForgedRequests() {
        auto *b = new FixtureNightTimeBackend; NightTimeSettings s(b); QTRY_VERIFY(!s.isBusy());
        s.selectValue(NightTimeKey::Source, 1); QVERIFY(s.isDirty()); QCOMPARE(b->writes, 0);
        QString error; QVERIFY(NightTimeTypes::requestValid(s.applied(), s.staged(), error));
        auto forged = s.staged(); forged.fields[NightTimeKey::Automatic].value = false;
        QVERIFY(!NightTimeTypes::requestValid(s.applied(), forged, error));
        forged = s.staged(); forged.daemonFields[NightTimeKey::Latitude].value = 12.0;
        QVERIFY(!NightTimeTypes::requestValid(s.applied(), forged, error));
        forged = s.staged(); forged.intents[NightTimeKey::Longitude] = NightTimeIntent(99);
        QVERIFY(!NightTimeTypes::requestValid(s.applied(), forged, error));
    }
    void dualViewsAndPrivacy() {
        auto base = nightTimeFixture();
        auto &editor = base.fields[NightTimeKey::Automatic];
        editor.value = false; editor.localPresent = false; editor.globalPresent = true;
        editor.globalRevision = "global-false"; editor.defaultNoOp = false; editor.resettable = true;
        auto draft = base; QString error;
        QVERIFY(NightTimeTypes::privacyText(base, draft).contains("automatic location"));
        QVERIFY(NightTimeTypes::privacyText(base, draft).contains("editor choice and daemon input differ"));
        QVERIFY(NightTimeTypes::restore(draft, base, NightTimeKey::Automatic, error));
        QCOMPARE(NightTimeTypes::projectedDaemonFields(base, draft)[NightTimeKey::Automatic].value, QVariant(true));
        QVERIFY(NightTimeTypes::matchesRequest(base, draft, savedNightTimeFixture(draft), error));
        auto bad = savedNightTimeFixture(draft); bad.fields[NightTimeKey::Automatic].value = false;
        QVERIFY(!NightTimeTypes::matchesRequest(base, draft, bad, error));
    }
    void globalInvalidSourceDoesNotCrashGuardValidDaemon() {
        auto base = nightTimeFixture(); base.fields[NightTimeKey::Source].value = 77;
        base.fields[NightTimeKey::Source].known = false; auto draft = base; QString error;
        QVERIFY(NightTimeTypes::stage(draft, base, NightTimeKey::Latitude, 1.0, error));
        QVERIFY(NightTimeTypes::requestValid(base, draft, error));
        base.daemonFields[NightTimeKey::Source].known = false; draft = base;
        QVERIFY(NightTimeTypes::stage(draft, base, NightTimeKey::Latitude, 1.0, error));
        QVERIFY(!NightTimeTypes::requestValid(base, draft, error));
        QVERIFY(NightTimeTypes::stage(draft, base, NightTimeKey::Source, 1, error));
        QVERIFY(NightTimeTypes::requestValid(base, draft, error));
    }
    void activeAndDormantBranchValidation() {
        auto fields = nightTimeFixture().daemonFields; QString error;
        fields[NightTimeKey::SunriseStart].known = false;
        QVERIFY(!NightTimeTypes::activeBranchValid(fields, error)); // automatic can need fallback
        fields[NightTimeKey::Automatic].value = false;
        QVERIFY(NightTimeTypes::activeBranchValid(fields, error)); // manual solar ignores dormant Times
        fields[NightTimeKey::Latitude].value = 500.0;
        QVERIFY(!NightTimeTypes::activeBranchValid(fields, error));
        fields[NightTimeKey::Source].value = 1;
        fields[NightTimeKey::SunriseStart].known = true;
        fields[NightTimeKey::Latitude].known = false;
        QVERIFY(NightTimeTypes::activeBranchValid(fields, error)); // custom times ignore manual coordinates
        fields[NightTimeKey::TransitionDuration].value = std::numeric_limits<uint>::max();
        QVERIFY(!NightTimeTypes::activeBranchValid(fields, error));
        fields[NightTimeKey::TransitionDuration].value = uint(45);
        QVERIFY(NightTimeTypes::activeBranchValid(fields, error)); // retained native value, outside editor range
    }
    void nativeValidatorAndAtomicCompanions() {
        QCOMPARE(NightTimeTypes::validatedTime(QTime(5,0), QTime(6,0), 7200), QTime(4,0));
        QCOMPARE(NightTimeTypes::validatedTime(QTime(23,0), QTime(1,0), 10800), QTime(4,0));
        QCOMPARE(NightTimeTypes::validatedTime(QTime(6,0), QTime(6,0), 1800), QTime(6,30));
        auto base = nightTimeFixture(); base.fields[NightTimeKey::SunsetStart].value = QTime(7,0);
        base.fields[NightTimeKey::SunsetStart].writable = false;
        auto draft = base; QString error;
        QVERIFY(!NightTimeTypes::selectDuration(draft, base, 7200, error));
        QVERIFY(NightTimeTypes::sameDraft(draft, base));
        base.fields[NightTimeKey::SunsetStart].writable = true; draft = base;
        QVERIFY(NightTimeTypes::selectDuration(draft, base, 7200, error));
        QCOMPARE(draft.fields[NightTimeKey::SunriseStart].value, QVariant(QTime(6,0)));
        QCOMPARE(draft.fields[NightTimeKey::SunsetStart].value, QVariant(QTime(8,0)));
        QVERIFY(draft.intents.contains(NightTimeKey::SunsetStart));
        base.fields[NightTimeKey::Automatic].immutable = true; draft = base;
        QVERIFY(!NightTimeTypes::selectManualLocation(draft, base, 12.0, 34.0, error));
        QVERIFY(NightTimeTypes::sameDraft(draft, base));
    }
    void malformedTupleCanBeRepairedWithoutInventingCompanions() {
        auto *b = new FixtureNightTimeBackend;
        b->current.fields[NightTimeKey::Source].value = 1;
        for (auto key : {NightTimeKey::SunriseStart, NightTimeKey::SunsetStart, NightTimeKey::TransitionDuration}) {
            auto &f = b->current.fields[key]; f.known = false;
            f.value = key == NightTimeKey::TransitionDuration ? QVariant(uint(0)) : QVariant(QTime());
        }
        b->current.daemonFields = b->current.fields;
        NightTimeSettings s(b); QTRY_VERIFY(!s.isBusy()); QVERIFY(!s.canApply());
        s.selectDuration(1800); QCOMPARE(s.staged().intents.size(),1); QVERIFY(!s.canApply());
        QVERIFY(!s.staged().fields[NightTimeKey::SunriseStart].known);
        s.selectTime(NightTimeKey::SunriseStart,QTime(6,0)); QCOMPARE(s.staged().intents.size(),2); QVERIFY(!s.canApply());
        QVERIFY(!s.staged().fields[NightTimeKey::SunsetStart].known);
        s.selectTime(NightTimeKey::SunsetStart,QTime(18,0)); QVERIFY(s.canApply());
        QCOMPARE(b->writes,0); s.apply(); QCOMPARE(b->writes,1); QVERIFY(!s.isDirty());
    }
    void nativeDefaultsNoOpAndInheritance() {
        auto base = nightTimeFixture(); auto &f = base.fields[NightTimeKey::TransitionDuration];
        f.value = uint(2400); f.defaultValue = uint(45); f.defaultNoOp = false; f.resettable = true; f.inheritedPresent = true;
        auto draft = base; QString error;
        QVERIFY(NightTimeTypes::restore(draft, base, NightTimeKey::TransitionDuration, error));
        QVERIFY(NightTimeTypes::requestValid(base, draft, error));
        auto actual = savedNightTimeFixture(draft);
        QVERIFY(NightTimeTypes::matchesRequest(base, draft, actual, error));
        QVERIFY(actual.fields[NightTimeKey::TransitionDuration].localPresent);
        f.value = f.defaultValue; f.defaultNoOp = true; draft = base;
        QVERIFY(NightTimeTypes::restore(draft, base, NightTimeKey::TransitionDuration, error));
        QVERIFY(draft.intents.isEmpty());
    }
    void nativeFractionalTimeWritesAreReviewedWithoutTouchingNoOps() {
        auto base = nightTimeFixture(); auto &f = base.fields[NightTimeKey::SunriseStart];
        f.value = QTime(6,0,17,123); f.defaultNoOp = false; f.resettable = true;
        base.daemonFields[NightTimeKey::SunriseStart] = f;
        auto draft = base; QString error;
        QVERIFY(NightTimeTypes::stage(draft, base, NightTimeKey::SunriseStart, f.value, error));
        QVERIFY(draft.intents.isEmpty()); QCOMPARE(draft.fields[NightTimeKey::SunriseStart].value.toTime().msec(),123);
        QVERIFY(NightTimeTypes::stage(draft, base, NightTimeKey::SunriseStart, QTime(7,15,12,345), error));
        QCOMPARE(draft.fields[NightTimeKey::SunriseStart].value,QVariant(QTime(7,15,12)));
        QVERIFY(NightTimeTypes::requestValid(base,draft,error));
        QVERIFY(NightTimeTypes::matchesRequest(base,draft,savedNightTimeFixture(draft),error));
        f.inheritedPresent = true; f.defaultValue = QTime(8,0,0,250); draft = base;
        QVERIFY(NightTimeTypes::restore(draft,base,NightTimeKey::SunriseStart,error));
        QCOMPARE(draft.fields[NightTimeKey::SunriseStart].value,QVariant(QTime(8,0,0)));
        QCOMPARE(draft.fields[NightTimeKey::SunriseStart].defaultValue,QVariant(QTime(8,0,0,250)));
        QVERIFY(NightTimeTypes::requestValid(base,draft,error));
        QVERIFY(NightTimeTypes::matchesRequest(base,draft,savedNightTimeFixture(draft),error));
    }
    void absentOverrideMustStayExplicitUntilReset() {
        auto base = nightTimeFixture(); auto &f = base.fields[NightTimeKey::Longitude];
        f.localPresent = f.effectivePresent = false; auto draft = base; QString error;
        QVERIFY(NightTimeTypes::stage(draft, base, NightTimeKey::Longitude, 0.0, error));
        QVERIFY(draft.intents.contains(NightTimeKey::Longitude));
        QVERIFY(NightTimeTypes::restore(draft, base, NightTimeKey::Longitude, error));
        QVERIFY(NightTimeTypes::sameDraft(draft, base));
    }
    void cacheRefreshDoesNotConflictButGlobalPolicyDoes() {
        auto *b = new FixtureNightTimeBackend; NightTimeSettings s(b); QTRY_VERIFY(!s.isBusy());
        s.selectValue(NightTimeKey::Source, 1);
        b->current.cachedLocation = {true, 12, 34, {}}; s.refresh();
        QVERIFY(!s.hasConflict()); QVERIFY(s.isDirty()); QVERIFY(s.cachedLocation().available);
        b->current.fields[NightTimeKey::Automatic].globalRevision += "-external"; s.refresh();
        QVERIFY(s.hasConflict()); QVERIFY(s.isDirty()); QCOMPARE(b->writes, 0);
    }
    void preflightRaceAndStaleToken() {
        auto *b = new FixtureNightTimeBackend; NightTimeSettings s(b); QTRY_VERIFY(!s.isBusy());
        s.selectValue(NightTimeKey::Source, 1); b->holdRead = true; s.apply();
        emit b->readFinished(b->readToken-1, true, b->current, {}); QVERIFY(s.isBusy());
        b->current.daemonFields[NightTimeKey::SunsetStart].revision += "-external"; b->completeRead();
        QVERIFY(s.hasConflict()); QVERIFY(s.isDirty()); QCOMPARE(b->writes, 0);
    }
    void observersMayDeleteSessionDuringReadAndFinish() {
        auto *b = new FixtureNightTimeBackend;
        QPointer<NightTimeSettings> s = new NightTimeSettings(b);
        QTRY_VERIFY(!s->isBusy());
        connect(s, &NightTimeSettings::stateChanged, this, [s] { if (s && s->isBusy()) delete s; });
        s->refresh(); QVERIFY(!s);
        b = new FixtureNightTimeBackend; s = new NightTimeSettings(b);
        QTRY_VERIFY(!s->isBusy()); s->selectValue(NightTimeKey::Source, 1);
        connect(s, &NightTimeSettings::stateChanged, this, [s] { if (s && !s->isBusy()) delete s; });
        s->apply(); QVERIFY(!s);
    }
    void applyReadbackAndOfflineNoActivation() {
        auto *b = new FixtureNightTimeBackend; b->current.runtime.owner.clear(); b->current.runtime.serviceAvailable = false;
        b->current.runtime.providerVerified = false; b->current.runtime.apiVersion = 0;
        NightTimeSettings s(b); QTRY_VERIFY(!s.isBusy()); QVERIFY(s.canEdit(NightTimeKey::Source));
        s.selectValue(NightTimeKey::Source, 1); s.apply();
        QVERIFY(!s.isBusy()); QVERIFY(!s.isDirty()); QCOMPARE(b->writes, 1);
        QVERIFY(s.resultText().contains("No service was started")); QVERIFY(s.resultText().contains("unverified"));
    }
    void pendingSaveRetainsDraftUntilTerminalReadback() {
        auto *b = new FixtureNightTimeBackend; b->holdWrite = true; NightTimeSettings s(b); QTRY_VERIFY(!s.isBusy());
        QSignalSpy completed(&s, &SettingsSession::applyFinished); s.selectValue(NightTimeKey::Source, 1); const auto draft = s.staged(); s.apply();
        const auto generic = s.applySupportText(); emit b->operationPending(b->writeToken - 1, QStringLiteral("Stale progress")); QCOMPARE(s.applySupportText(), generic);
        emit b->operationPending(b->writeToken, QStringLiteral("Saving continues in the background"));
        QVERIFY(s.applySupportText().contains("continues")); QVERIFY(s.errorText().isEmpty()); QVERIFY(s.isBusy()); QVERIFY(s.isDirty()); QVERIFY(!s.hasConflict());
        const auto reads = b->reads; s.apply(); s.revert(); s.refresh(); s.selectDuration(3600);
        QCOMPARE(b->writes, 1); QCOMPARE(b->reads, reads); QCOMPARE(completed.size(), 0); QVERIFY(NightTimeTypes::sameDraft(s.staged(), draft));
        b->holdRead = true; b->completeWrite(); QVERIFY(s.isBusy()); emit b->operationPending(b->writeToken, QStringLiteral("Late progress")); QVERIFY(!s.applySupportText().contains("Late progress"));
        b->completeRead(); QVERIFY(!s.isBusy()); QVERIFY(!s.isDirty()); QCOMPARE(completed.size(), 1); QVERIFY(completed[0][0].toBool());
    }
    void deletionDuringPendingProgressDoesNotCompleteApply() {
        auto *b = new FixtureNightTimeBackend; b->holdWrite = true; QPointer<NightTimeSettings> s = new NightTimeSettings(b); QTRY_VERIFY(!s->isBusy());
        s->selectValue(NightTimeKey::Source, 1); s->apply(); const auto token = b->writeToken; QSignalSpy completed(s, &SettingsSession::applyFinished);
        connect(s, &SettingsSession::stateChanged, this, [s] { if (s && s->applySupportText().contains("Still saving")) delete s; });
        emit b->operationPending(token, QStringLiteral("Still saving")); QVERIFY(!s); QCOMPARE(completed.size(), 0);
    }
    void jobLivesBeyondViewAndUncertaintyRequiresSuccessfulRevert() {
        auto *b = new FixtureNightTimeBackend; b->holdWrite = true; NightTimeSettings s(b); QTRY_VERIFY(!s.isBusy());
        s.selectValue(NightTimeKey::Source, 1); s.apply(); QVERIFY(s.isBusy());
        b->notified = false; b->completeWrite(); QVERIFY(!s.isBusy()); QVERIFY(s.hasConflict()); QVERIFY(s.isDirty());
        b->failRead = true; s.revert(); QVERIFY(s.isDirty()); QVERIFY(s.hasConflict());
        b->failRead = false; s.refresh(); QVERIFY(s.isDirty()); QVERIFY(s.hasConflict());
        s.revert(); QVERIFY(!s.isDirty()); QVERIFY(!s.hasConflict());
    }
};
QTEST_GUILESS_MAIN(NightTimeSettingsTest)
#include "night_time_settings_test.moc"
