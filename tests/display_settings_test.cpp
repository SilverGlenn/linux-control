#include <QtTest>
#include <QSignalSpy>
#include <QProcess>
#include <limits>
#include "settings/DisplaySettings.h"
#include "display/KdeDisplayBackend.h"
#include "display_fixture.h"

class FakeDisplayBackend : public DisplayBackend {
public:
    bool available = true;
    QString restriction;
    int reads = 0, previews = 0, keeps = 0, reverts = 0;
    DisplaySnapshot requested;
    bool rollbackAvailable() const override { return available; }
    QString editRestrictionReason() const override { return restriction; }
    void readCurrent() override { ++reads; }
    void beginPreview(const DisplaySnapshot &, const DisplaySnapshot &candidate) override
    { ++previews; requested = candidate; }
    void keepPreview() override { ++keeps; }
    void revertPreview() override { ++reverts; }
    void read(const DisplaySnapshot &snapshot) { emit readFinished(true, snapshot, {}); }
    void preview(int seconds = 15) { emit previewReady(requested, seconds); }
    void finish(bool kept, bool restored, const DisplaySnapshot &snapshot, const QString &error = {})
    { emit transactionFinished(kept, restored, snapshot, error); }
};

class DisplaySettingsTest : public QObject {
    Q_OBJECT
    FakeDisplayBackend *backend = nullptr;
    DisplaySettings *session = nullptr;
private slots:
    void init()
    {
        backend = new FakeDisplayBackend;
        session = new DisplaySettings(backend);
        QTRY_COMPARE(backend->reads, 1);
        backend->read(displayFixture());
    }
    void cleanup() { delete session; }
    void stagingDoesNotWrite()
    {
        session->selectMode("2");
        QVERIFY(session->isDirty()); QCOMPARE(backend->previews, 0);
        session->selectMode("1"); QVERIFY(!session->isDirty());
        session->selectMode("invented"); QCOMPARE(session->stagedMode(), QStringLiteral("1"));
    }
    void candidateScopeIsSeparateFromHelperAvailability()
    {
        backend->restriction=QStringLiteral("Display changes withheld in this candidate.");
        QVERIFY(session->rollbackAvailable());
        QVERIFY(session->selectedSupportText().contains("withheld"));
        session->selectMode("2"); QVERIFY(!session->isDirty());
        QSignalSpy finished(session,&SettingsSession::applyFinished);
        session->apply(); QCOMPARE(finished.size(),1); QVERIFY(!finished.first()[0].toBool());
        QCOMPARE(backend->previews,0); QVERIFY(session->errorText().contains("withheld"));
    }
    void productionBackendRejectsDirectPreviewBeforeLaunchingHelper()
    {
        KdeDisplayBackend production;
        QSignalSpy finished(&production,&DisplayBackend::transactionFinished),preview(&production,&DisplayBackend::previewReady);
        production.beginPreview(displayFixture(),displayFixture("2"));
        QCOMPARE(finished.size(),1); QVERIFY(preview.isEmpty());
        QVERIFY(!finished.first()[0].toBool()); QVERIFY(finished.first()[3].toString().contains("local candidate"));
        QVERIFY(DisplayProtocol::same(finished.first()[2].value<DisplaySnapshot>(),displayFixture()));
        QVERIFY(production.findChildren<QProcess *>().isEmpty());
    }
    void verifiedKeepCompletesApplyOnlyAfterConfirmation()
    {
        QSignalSpy finished(session, &SettingsSession::applyFinished);
        session->selectMode("2"); session->apply();
        QCOMPARE(backend->previews, 0); QVERIFY(session->isBusy());
        backend->read(displayFixture()); QCOMPARE(backend->previews, 1);
        backend->preview(); QVERIFY(session->awaitingConfirmation());
        QVERIFY(session->isBusy()); QVERIFY(finished.isEmpty());
        session->keep(); QCOMPARE(backend->keeps, 1);
        QVERIFY(finished.isEmpty());
        backend->finish(true, false, displayFixture("2"));
        QVERIFY(!session->isBusy()); QVERIFY(!session->isDirty());
        QCOMPARE(finished.size(), 1); QVERIFY(finished[0][0].toBool());
        QCOMPARE(session->snapshot().output(1)->currentMode, QStringLiteral("2"));
    }
    void timeoutRequestsRollbackAndWaitsForVerification()
    {
        QSignalSpy finished(session, &SettingsSession::applyFinished);
        session->selectMode("2"); session->apply(); backend->read(displayFixture());
        backend->preview(1);
        QTRY_COMPARE_WITH_TIMEOUT(backend->reverts, 1, 2500);
        QVERIFY(session->isBusy()); QVERIFY(finished.isEmpty());
        backend->finish(false, true, displayFixture());
        QVERIFY(!session->isBusy()); QVERIFY(!session->isDirty());
        QVERIFY(!finished[0][0].toBool()); QVERIFY(session->errorText().isEmpty());
        QVERIFY(session->resultText().contains("restored"));
    }
    void revertDuringPreviewUsesBackendRollback()
    {
        session->selectMode("2"); session->apply(); backend->read(displayFixture()); backend->preview();
        session->revert(); session->revert(); QCOMPARE(backend->reverts, 1);
        QVERIFY(session->isBusy()); backend->finish(false, true, displayFixture());
        QVERIFY(!session->awaitingConfirmation()); QVERIFY(!session->isBusy());
    }
    void preflightRejectsExternalChanges()
    {
        session->selectMode("2"); session->apply();
        backend->read(displayModified(displayFixture(), "scale", 2));
        QCOMPARE(backend->previews, 0); QVERIFY(session->hasConflict()); QVERIFY(!session->isBusy());
        session->apply(); QCOMPARE(backend->previews, 0);
        session->revert(); backend->read(displayModified(displayFixture(), "scale", 2));
        QVERIFY(!session->hasConflict()); QVERIFY(!session->isDirty());
    }
    void falseSuccessAndUnknownRollbackAreTruthful()
    {
        QSignalSpy finished(session, &SettingsSession::applyFinished);
        session->selectMode("2"); session->apply(); backend->read(displayFixture()); backend->preview();
        backend->finish(true, false, displayFixture());
        QVERIFY(!finished[0][0].toBool()); QVERIFY(session->hasConflict());
        QVERIFY(!session->errorText().isEmpty()); QVERIFY(session->resultText().isEmpty());
    }
    void wholeLayoutCanBeStagedAcrossOutputSelections()
    {
        const auto original = displayMultiFixture();
        session->refresh(); backend->read(original);
        session->selectMode("2");
        session->selectOutput(2);
        QCOMPARE(session->selectedOutput(), 2);
        session->selectScale(1.25);
        session->selectRotation(2);
        session->selectPosition(1280, 100);
        QCOMPARE(session->staged().output(1)->currentMode, QStringLiteral("2"));
        QCOMPARE(session->staged().output(2)->scale, 1.25);
        QCOMPARE(session->staged().output(2)->rotation, 2);
        QCOMPARE(session->staged().output(2)->position, QPoint(1280, 100));
        QVERIFY(DisplayProtocol::same(session->snapshot(), original));
        QCOMPARE(backend->previews, 0);
        QSignalSpy finished(session, &SettingsSession::applyFinished);
        session->apply(); backend->read(original);
        QCOMPARE(backend->previews, 1);
        QVERIFY(DisplayProtocol::same(backend->requested, session->staged()));
        backend->preview(); QVERIFY(session->awaitingConfirmation());
        session->keep(); backend->finish(true, false, backend->requested);
        QVERIFY(!session->isDirty()); QVERIFY(!session->hasConflict());
        QCOMPARE(finished.size(), 1); QVERIFY(finished[0][0].toBool());
    }
    void allOffDraftCanBeRepairedWithoutWriting()
    {
        const auto original = displayMultiFixture();
        session->refresh(); backend->read(original);
        session->selectEnabled(false);
        session->selectOutput(2); session->selectEnabled(false);
        QVERIFY(session->isDirty()); QVERIFY(!session->supportText().isEmpty());
        session->apply(); QCOMPARE(backend->previews, 0);
        session->selectOutput(1); session->selectEnabled(true);
        QVERIFY(session->staged().output(1)->enabled);
        QVERIFY(!session->staged().output(2)->enabled);
        QVERIFY(session->supportText().isEmpty());
        QCOMPARE(backend->previews, 0);
        session->revert(); backend->read(original);
        QVERIFY(!session->isDirty());
        QVERIFY(session->staged().output(2)->enabled);
    }
    void unsupportedFieldsAndInvalidNumbersDoNotMutateDraft()
    {
        session->selectScale(std::numeric_limits<double>::quiet_NaN());
        session->selectScale(0.49); session->selectScale(4.01);
        session->selectRotation(16); session->selectRotation(3);
        session->selectPosition(DisplayProtocol::MaximumPosition + 1, 0);
        QVERIFY(!session->isDirty());
        auto limited = displayFixture(); limited.json.insert("features", 0);
        DisplaySnapshot parsed; QString error;
        QVERIFY(DisplayProtocol::parse(QJsonDocument(limited.json).toJson(), parsed, error));
        session->refresh(); backend->read(parsed);
        session->selectScale(2); session->selectRotation(2); session->selectPosition(0, 200);
        QVERIFY(!session->isDirty());
        QCOMPARE(backend->previews, 0);
    }
    void enablingOutputWithStaleModeChoosesAnAdvertisedMode()
    {
        auto original = displayMultiFixture();
        original.output(2)->enabled = false;
        original.output(2)->currentMode = "stale";
        original = DisplayProtocol::normalized(original);
        QVERIFY(original.isValid());
        session->refresh(); backend->read(original);
        session->selectOutput(2); session->selectEnabled(true);
        QVERIFY(session->staged().output(2)->enabled);
        QCOMPARE(session->staged().output(2)->currentMode, original.output(2)->modes.first().id);
        QVERIFY(session->supportText().isEmpty()); QCOMPARE(backend->previews, 0);
    }
    void uncertainFinalStateRetainsDraftAndRequiresExplicitRevert()
    {
        session->selectMode("2"); session->apply(); backend->read(displayFixture());
        backend->preview();
        backend->finish(false, false, backend->requested, "Native acknowledgement was lost.");
        QVERIFY(session->isDirty()); QVERIFY(session->hasConflict());
        QCOMPARE(session->stagedMode(), QStringLiteral("2"));
        session->refresh(); backend->read(backend->requested);
        QVERIFY(session->isDirty()); QVERIFY(session->hasConflict());
        session->apply(); QCOMPARE(backend->previews, 1);
        session->revert(); backend->read(backend->requested);
        QVERIFY(!session->isDirty()); QVERIFY(!session->hasConflict());
    }
    void hotplugDuringPreflightRetainsWholeDraft()
    {
        const auto original = displayMultiFixture();
        session->refresh(); backend->read(original);
        session->selectOutput(2); session->selectScale(1.5);
        session->apply(); backend->read(displayFixture());
        QVERIFY(session->isDirty()); QVERIFY(session->hasConflict());
        QCOMPARE(session->staged().output(2)->scale, 1.5);
        QCOMPARE(backend->previews, 0);
        session->revert(); backend->read(displayFixture());
        QVERIFY(!session->staged().output(2));
        QCOMPARE(session->selectedOutput(), 1);
    }
    void failedRevertRetainsDraftUntilFreshReadSucceeds()
    {
        session->selectMode("2");
        session->revert();
        QVERIFY(session->isBusy()); QVERIFY(session->isDirty());
        emit backend->readFinished(false, {}, "Display read failed.");
        QVERIFY(!session->isBusy()); QVERIFY(!session->isReady());
        QVERIFY(session->isDirty()); QVERIFY(session->hasConflict());
        QCOMPARE(session->stagedMode(), QStringLiteral("2"));
        session->revert(); backend->read(displayFixture());
        QVERIFY(session->isReady()); QVERIFY(!session->isDirty()); QVERIFY(!session->hasConflict());
        QCOMPARE(session->stagedMode(), QStringLiteral("1"));
    }
    void refreshDuringPreviewIsQueuedUntilTransactionFinishes()
    {
        session->selectMode("2"); session->apply(); backend->read(displayFixture());
        backend->preview();
        emit backend->externalChange(); emit backend->externalChange();
        QCOMPARE(backend->reads, 2);
        backend->finish(true, false, backend->requested);
        QTRY_COMPARE(backend->reads, 3);
        backend->read(backend->requested);
        QVERIFY(!session->isBusy()); QVERIFY(!session->isDirty());
    }
    void signedPlacementBeyondLegacyShortRangeStagesWithoutClamping()
    {
        const auto original=displayMultiFixture(); session->refresh(); backend->read(original);
        session->selectOutput(2); session->selectPosition(-50000,100000);
        QVERIFY(session->isDirty()); QCOMPARE(session->staged().output(2)->position,QPoint(0,100000));
        QCOMPARE(session->staged().output(1)->position,QPoint(50000,0));
        QVERIFY(session->supportText().isEmpty()); QCOMPARE(backend->previews,0);
        const auto draft=session->staged(); session->selectPosition(DisplayProtocol::MaximumPosition+1,0);
        QVERIFY(DisplayProtocol::same(draft,session->staged()));
    }
    void unrelatedRawScaleSurvivesControllerEditsAndReadback()
    {
        auto original=displayMultiFixture(); original.output(2)->scale=4.01;
        // This fixture deliberately models a saved raw value without passing it
        // through an explicit scale editor/quantizer.
        auto outputs=original.json.value("outputs").toArray(); auto second=outputs[1].toObject();
        second.insert("scale",4.01); outputs[1]=second; original.json.insert("outputs",outputs);
        session->refresh(); backend->read(original); session->selectOutput(1); session->selectScale(1.37);
        QCOMPARE(session->staged().output(1)->scale,164.0/120.0);
        QCOMPARE(session->staged().output(2)->scale,4.01); QVERIFY(session->supportText().isEmpty());
        session->selectOutput(2); session->selectScale(1.5);
        QVERIFY(!session->supportText().isEmpty()); session->apply(); QCOMPARE(backend->previews,0);
    }
    void helperUnavailableDisablesApply()
    {
        backend->available = false;
        session->selectMode("2"); session->apply();
        QCOMPARE(backend->previews, 0); QVERIFY(!session->supportText().isEmpty());
    }
};
QTEST_GUILESS_MAIN(DisplaySettingsTest)
#include "display_settings_test.moc"
