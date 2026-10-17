#include <QtTest>
#include <QPointer>
#include "night_light_fixture.h"

class FixtureNightLightBackend final : public NightLightBackend {
public:
    NightLightSnapshot current = nightLightFixture();
    int writes = 0, previews = 0, stops = 0, reads = 0;
    bool failRead = false, holdRead = false, holdWrite = false, holdPreview = false;
    bool saved = true, notified = true, previewSuccess = true, stopSuccess = true, badRuntime = false;
    QString writeError;
    quint64 readToken = 0, writeToken = 0, previewToken = 0;
    NightLightSnapshot request;
    void readCurrent(quint64 token) override {
        ++reads; readToken = token;
        if (!holdRead) emit readFinished(token, !failRead, current, failRead ? QStringLiteral("Read failed") : QString());
    }
    void completeRead() { emit readFinished(readToken, !failRead, current, failRead ? QStringLiteral("Read failed") : QString()); }
    void writeChanges(quint64 token, const NightLightSnapshot &, const NightLightSnapshot &requested) override {
        ++writes; writeToken = token; request = requested;
        if (!holdWrite) completeWrite();
    }
    void completeWrite() {
        if (saved) current = savedNightLightFixture(request);
        if (badRuntime) current.runtime.targetTemperature = 1000;
        emit writeFinished(writeToken, saved, notified, writeError);
    }
    void preview(quint64 token, int, const QString &owner, quint64 epoch) override {
        ++previews; previewToken = token;
        Q_ASSERT(owner == current.runtime.owner && epoch == current.runtime.epoch);
        if (!holdPreview) emit previewFinished(token, previewSuccess, false, previewSuccess ? QString() : QStringLiteral("Preview timed out"));
    }
    void stopPreview(quint64 token, const QString &owner, quint64 epoch) override {
        ++stops; Q_ASSERT(owner == current.runtime.owner && epoch == current.runtime.epoch);
        emit previewFinished(token, stopSuccess, true, stopSuccess ? QString() : QStringLiteral("Stop timed out"));
    }
};

class NightLightSettingsTest final : public QObject {
    Q_OBJECT
private slots:
    void stagedValuesAndStrictRequests() {
        auto *b = new FixtureNightLightBackend; NightLightSettings s(b); QTRY_VERIFY(!s.isBusy());
        const auto before = s.applied(); s.selectValue(NightLightKey::NightTemperature, 4200);
        QVERIFY(s.isDirty()); QCOMPARE(b->writes, 0); QCOMPARE(s.applied().fields, before.fields);
        QCOMPARE(s.staged().fields[NightLightKey::DayTemperature].value, QVariant(6500));
        s.selectValue(NightLightKey::NightTemperature, QStringLiteral("4300"));
        QCOMPARE(s.staged().fields[NightLightKey::NightTemperature].value, QVariant(4200));
        QString error; QVERIFY(NightLightTypes::requestValid(before, s.staged(), error));
        auto forged = s.staged(); forged.fields[NightLightKey::DayTemperature].value = 5000;
        QVERIFY(!NightLightTypes::requestValid(before, forged, error));
        forged = s.staged(); forged.intents[NightLightKey::Mode] = NightLightIntent(99);
        QVERIFY(!NightLightTypes::matchesRequest(before, forged, savedNightLightFixture(forged), error));
    }
    void absentSameNumberOverrideAndNativeNoOpDefaults() {
        auto a = nightLightFixture(); auto &f = a.fields[NightLightKey::NightTemperature];
        f.localPresent = f.effectivePresent = false; auto draft = a; QString error;
        QVERIFY(NightLightTypes::stage(draft, a, NightLightKey::NightTemperature, 4500, error));
        QVERIFY(draft.intents.contains(NightLightKey::NightTemperature));
        QVERIFY(NightLightTypes::restore(draft, a, NightLightKey::NightTemperature, error));
        QVERIFY(NightLightTypes::sameDraft(draft, a));
        auto &raw = a.fields[NightLightKey::DayTemperature]; raw.value = 20000; raw.defaultNoOp = true;
        draft = a; QVERIFY(NightLightTypes::restore(draft, a, NightLightKey::DayTemperature, error));
        QCOMPARE(draft.fields[NightLightKey::DayTemperature].value, QVariant(20000)); QVERIFY(draft.intents.isEmpty());
        QVERIFY(NightLightTypes::stage(draft, a, NightLightKey::DayTemperature, 6500, error));
        QVERIFY(draft.intents.contains(NightLightKey::DayTemperature));
    }
    void resetPreservesGlobalShadowAndInheritedSource() {
        auto a = nightLightFixture(); auto &f = a.fields[NightLightKey::NightTemperature];
        f.value = 4000; f.defaultNoOp = false; f.resettable = true; f.globalPresent = true; f.globalRevision = "global-still-present";
        auto draft = a; QString error;
        QVERIFY(NightLightTypes::restore(draft, a, NightLightKey::NightTemperature, error));
        auto actual = savedNightLightFixture(draft);
        QVERIFY(actual.fields[NightLightKey::NightTemperature].globalPresent);
        QVERIFY(NightLightTypes::matchesRequest(a, draft, actual, error));
        actual.fields[NightLightKey::NightTemperature].effectivePresent = true;
        QVERIFY(!NightLightTypes::matchesRequest(a, draft, actual, error));
        f.localPresent = false; draft = a;
        QVERIFY(NightLightTypes::restore(draft, a, NightLightKey::NightTemperature, error));
        actual = savedNightLightFixture(draft); QVERIFY(!actual.fields[NightLightKey::NightTemperature].globalPresent);
        QVERIFY(NightLightTypes::matchesRequest(a, draft, actual, error));
        f.localPresent = true; f.inheritedPresent = true; f.defaultValue = 4300; draft = a;
        QVERIFY(NightLightTypes::restore(draft, a, NightLightKey::NightTemperature, error));
        actual = savedNightLightFixture(draft); QVERIFY(actual.fields[NightLightKey::NightTemperature].localPresent);
        QVERIFY(NightLightTypes::matchesRequest(a, draft, actual, error));
        actual.fields[NightLightKey::NightTemperature].inheritedRevision += "-foreign";
        QVERIFY(!NightLightTypes::matchesRequest(a, draft, actual, error));
    }
    void unknownAndImmutableDefaultsStayUnavailable() {
        auto *b = new FixtureNightLightBackend;
        auto &f = b->current.fields[NightLightKey::DayTemperature]; f.known = false; f.defaultKnown = false; f.value = QStringLiteral("bad");
        auto &locked = b->current.fields[NightLightKey::Mode]; locked.immutable = true; locked.writable = false;
        NightLightSettings s(b); QTRY_VERIFY(!s.isBusy());
        s.restoreDefault(NightLightKey::DayTemperature); QVERIFY(!s.isDirty()); QVERIFY(!s.errorText().isEmpty());
        s.selectValue(NightLightKey::Mode, 0); QVERIFY(!s.isDirty());
        s.selectValue(NightLightKey::DayTemperature, 6200); QVERIFY(s.isDirty());
        QCOMPARE(s.staged().fields[NightLightKey::DayTemperature].value, QVariant(6200));
    }
    void applyVerifiesSavedAndObservableState() {
        auto *b = new FixtureNightLightBackend; NightLightSettings s(b); QTRY_VERIFY(!s.isBusy());
        s.selectValue(NightLightKey::NightTemperature, 4000); QSignalSpy done(&s, &SettingsSession::applyFinished);
        s.apply(); QTRY_COMPARE(done.size(), 1); QCOMPARE(done[0][0].toBool(), true);
        QCOMPARE(b->writes, 1); QVERIFY(!s.isDirty()); QVERIFY(!s.hasConflict());
        QVERIFY(s.resultText().contains(QStringLiteral("inactive temperature")));
        QCOMPARE(b->previews, 0); QCOMPARE(b->stops, 0);
    }
    void saveUncertaintyAndFailedRevertKeepDraft_data() {
        QTest::addColumn<bool>("saved"); QTest::addColumn<bool>("notified"); QTest::addColumn<bool>("badRuntime"); QTest::addColumn<QString>("error");
        QTest::newRow("failed-save") << false << false << false << QStringLiteral("Disk failed");
        QTest::newRow("failed-notification") << true << false << false << QString();
        QTest::newRow("uncertain-write-even-matching-readback") << true << true << false << QStringLiteral("Earlier partial result");
        QTest::newRow("runtime-mismatch") << true << true << true << QString();
    }
    void saveUncertaintyAndFailedRevertKeepDraft() {
        QFETCH(bool,saved); QFETCH(bool,notified); QFETCH(bool,badRuntime); QFETCH(QString,error);
        auto *b = new FixtureNightLightBackend; b->saved = saved; b->notified = notified; b->badRuntime = badRuntime; b->writeError = error;
        NightLightSettings s(b); QTRY_VERIFY(!s.isBusy()); s.selectValue(NightLightKey::NightTemperature, 4000);
        QSignalSpy done(&s, &SettingsSession::applyFinished); s.apply(); QTRY_COMPARE(done.size(), 1);
        QVERIFY(!done[0][0].toBool()); QVERIFY(s.isDirty()); QVERIFY(s.hasConflict()); QVERIFY(!s.canApply());
        s.refresh(); QVERIFY(s.isDirty()); QVERIFY(s.hasConflict());
        b->failRead = true; s.revert(); QVERIFY(s.isDirty()); QVERIFY(s.hasConflict());
        QCOMPARE(s.staged().fields[NightLightKey::NightTemperature].value, QVariant(4000));
        b->failRead = false; s.revert(); QVERIFY(!s.isDirty()); QVERIFY(!s.hasConflict());
    }
    void foreignPreflightAndProviderEpochAreConflicts() {
        auto *b = new FixtureNightLightBackend; NightLightSettings s(b); QTRY_VERIFY(!s.isBusy());
        s.selectValue(NightLightKey::NightTemperature, 4000);
        b->current.fields[NightLightKey::DayTemperature].value = 6000; s.apply();
        QCOMPARE(b->writes, 0); QVERIFY(s.isDirty()); QVERIFY(s.hasConflict());
        s.revert(); s.selectValue(NightLightKey::NightTemperature, 4100);
        ++b->current.runtime.epoch; emit b->externalChange();
        QVERIFY(s.hasConflict()); QVERIFY(s.isDirty()); QCOMPARE(b->writes, 0);
    }
    void staleRepliesAndJobsDoNotDependOnViews() {
        auto *b = new FixtureNightLightBackend; NightLightSettings s(b); QTRY_VERIFY(!s.isBusy());
        s.selectValue(NightLightKey::NightTemperature, 4000); b->holdWrite = true; s.apply();
        QVERIFY(s.isBusy()); emit b->writeFinished(b->writeToken - 1, true, true, {}); QVERIFY(s.isBusy());
        b->completeWrite(); QVERIFY(!s.isBusy()); QVERIFY(!s.isDirty());
        const auto applied = s.applied(); emit b->readFinished(b->readToken - 1, false, {}, QStringLiteral("Stale"));
        QVERIFY(s.isReady()); QVERIFY(NightLightTypes::sameConfiguration(s.applied(), applied));
    }
    void pendingSaveRetainsDraftAndPreventsPreviewOrRetryUntilReadback() {
        auto *b = new FixtureNightLightBackend; b->holdWrite = true;
        NightLightSettings s(b); QTRY_VERIFY(!s.isBusy()); QSignalSpy completed(&s, &SettingsSession::applyFinished);
        s.selectValue(NightLightKey::NightTemperature, 3900); const auto draft = s.staged(); s.apply();
        QVERIFY(s.isBusy()); const auto generic = s.applySupportText();
        emit b->operationPending(b->writeToken - 1, QStringLiteral("Stale progress")); QCOMPARE(s.applySupportText(), generic);
        emit b->operationPending(b->writeToken, QStringLiteral("Saving continues in the background"));
        QVERIFY(s.applySupportText().contains("continues")); QVERIFY(s.errorText().isEmpty()); QVERIFY(s.isDirty()); QVERIFY(!s.hasConflict());
        const int reads = b->reads; s.apply(); s.revert(); s.refresh(); s.selectValue(NightLightKey::NightTemperature, 4000); s.previewTemperature(NightLightKey::NightTemperature);
        QCOMPARE(b->writes, 1); QCOMPARE(b->reads, reads); QCOMPARE(b->previews, 0); QCOMPARE(completed.size(), 0);
        QVERIFY(NightLightTypes::sameDraft(s.staged(), draft)); b->holdRead = true; b->completeWrite(); QVERIFY(s.isBusy());
        emit b->operationPending(b->writeToken, QStringLiteral("Late progress")); QVERIFY(!s.applySupportText().contains("Late progress"));
        b->completeRead(); QVERIFY(!s.isBusy()); QVERIFY(!s.isDirty()); QVERIFY(s.errorText().isEmpty()); QCOMPARE(completed.size(), 1); QVERIFY(completed[0][0].toBool());
    }
    void outwardStateChangesMayDestroyTheSession() {
        auto *b = new FixtureNightLightBackend; QPointer<NightLightSettings> s = new NightLightSettings(b); QTRY_VERIFY(!s->isBusy());
        connect(s, &SettingsSession::stateChanged, this, [s] { if (s && s->isBusy()) delete s; }); s->refresh(); QVERIFY(!s);
        b = new FixtureNightLightBackend; s = new NightLightSettings(b); QTRY_VERIFY(!s->isBusy()); s->selectValue(NightLightKey::NightTemperature, 3900);
        connect(s, &SettingsSession::stateChanged, this, [s] { if (s && !s->isBusy()) delete s; }); s->apply(); QVERIFY(!s);
        b = new FixtureNightLightBackend; b->holdWrite = true; s = new NightLightSettings(b); QTRY_VERIFY(!s->isBusy()); s->selectValue(NightLightKey::NightTemperature, 3900); s->apply();
        const auto token = b->writeToken; connect(s, &SettingsSession::stateChanged, this, [s] { if (s && s->applySupportText().contains("Still saving")) delete s; });
        emit b->operationPending(token, QStringLiteral("Still saving")); QVERIFY(!s);
        b = new FixtureNightLightBackend; s = new NightLightSettings(b); QTRY_VERIFY(!s->isBusy());
        connect(s, &SettingsSession::stateChanged, this, [s] { if (s && s->previewText().contains("Requesting")) delete s; });
        s->previewTemperature(NightLightKey::NightTemperature); QVERIFY(!s);
        b = new FixtureNightLightBackend; s = new NightLightSettings(b); QTRY_VERIFY(!s->isBusy()); s->previewTemperature(NightLightKey::NightTemperature);
        connect(s, &SettingsSession::stateChanged, this, [s] { if (s && s->previewText().contains("Ending")) delete s; });
        s->revert(); QVERIFY(!s);
    }
    void previewUsesSeparateLeaseAndRetainsUncertainStop() {
        auto *b = new FixtureNightLightBackend; NightLightSettings s(b); QTRY_VERIFY(!s.isBusy());
        s.previewTemperature(NightLightKey::NightTemperature); QCOMPARE(b->previews, 1); QVERIFY(s.previewActive());
        QCOMPARE(b->writes, 0); QVERIFY(!s.isDirty()); QVERIFY(!s.canApply());
        b->stopSuccess = false; s.stopPreview(); QVERIFY(s.previewActive()); QCOMPARE(b->stops, 1);
        QVERIFY(s.previewText().contains(QStringLiteral("timed out")));
        b->stopSuccess = true; s.stopPreview(); QVERIFY(!s.previewActive()); QCOMPARE(b->stops, 2);
        b->previewSuccess = false; s.previewTemperature(NightLightKey::NightTemperature);
        QVERIFY(s.previewActive()); s.stopPreview(); QVERIFY(!s.previewActive());
    }
    void delayedPreviewCleansAfterPageReleaseAndOwnerLossInvalidatesReply() {
        auto *b = new FixtureNightLightBackend; NightLightSettings s(b); QTRY_VERIFY(!s.isBusy());
        b->holdPreview = true; s.previewTemperature(NightLightKey::NightTemperature); s.stopPreview();
        QCOMPARE(b->stops, 0); emit b->previewFinished(b->previewToken, true, false, {});
        QCOMPARE(b->stops, 1); QVERIFY(!s.previewActive());
        s.previewTemperature(NightLightKey::NightTemperature); const auto oldToken = b->previewToken;
        ++b->current.runtime.epoch; emit b->externalChange(); QVERIFY(!s.previewActive());
        emit b->previewFinished(oldToken, true, false, {}); QVERIFY(!s.previewActive()); QCOMPARE(b->stops, 1);
    }
    void localExpiryDoesNotSendAGlobalStop() {
        auto *b = new FixtureNightLightBackend; NightLightSettings s(b); QTRY_VERIFY(!s.isBusy());
        s.previewTemperature(NightLightKey::NightTemperature); QVERIFY(s.previewActive());
        QTRY_VERIFY_WITH_TIMEOUT(!s.previewActive(), 17000);
        QCOMPARE(b->stops, 0); QCOMPARE(b->writes, 0);
        QVERIFY(s.previewText().contains(QStringLiteral("elapsed")));
    }
};
QTEST_MAIN(NightLightSettingsTest)
#include "night_light_settings_test.moc"
