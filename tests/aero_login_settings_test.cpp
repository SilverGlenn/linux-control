#include <QtTest>
#include <QPointer>
#include <QSignalSpy>
#include "aero_login_fixture.h"

namespace {
QByteArray revision(const char *text) { return QCryptographicHash::hash(text,QCryptographicHash::Sha256); }
}
class SettingsAeroLoginBackend final:public AeroLoginBackend {
public:
    AeroLoginSnapshot current=aeroLoginFixture(),expected,requested;
    int reads=0,writes=0,recoveries=0; quint64 readToken=0,writeToken=0; bool holdRead=false;
    void readCurrent(quint64 token) override { ++reads; readToken=token; if (!holdRead) emit readFinished(token,true,current,{}); }
    void writeChanges(quint64 token,const AeroLoginSnapshot &before,const AeroLoginSnapshot &after) override { ++writes; writeToken=token; expected=before; requested=after; }
    void recover(quint64 token,const AeroLoginSnapshot &before) override { ++recoveries; writeToken=token; expected=before; }
    void read() { emit readFinished(readToken,true,current,{}); }
    void failedRead() { emit readFinished(readToken,false,{},QStringLiteral("Fresh read failed")); }
    void changed() { emit externalChange(); }
    void saved(bool receipt=true) { current=savedAeroLoginFixture(requested); emit writeFinished(writeToken,true,{},receipt?current.revision:QByteArray()); }
    void result(bool saved,const QString &error,const QByteArray &receipt={}) { emit writeFinished(writeToken,saved,error,receipt); }
    void pending() { emit operationPending(writeToken,QStringLiteral("Administrator authorization is still pending.")); }
};
class AeroLoginSettingsTest final:public QObject {
    Q_OBJECT
private slots:
    void init() { backend=new SettingsAeroLoginBackend; session=new AeroLoginSettings(backend); QTRY_VERIFY(session->isReady()); }
    void cleanup() { delete session; }
    void allFourChoicesStageStrictOwnedIntentsWithoutWriting() {
        const auto baseline=session->applied();
        for (auto key:{AeroLoginKey::StartupAnimation,AeroLoginKey::StartupSound,AeroLoginKey::ForceUserSelection}) session->selectValue(key,!baseline.fields[key].value);
        QCOMPARE(session->staged().intents.size(),3); QCOMPARE(session->staged().backgroundIntent,AeroLoginBackgroundIntent::Keep); QVERIFY(session->staged().background==baseline.background); QString error; QVERIFY2(AeroLoginTypes::requestValid(baseline,session->staged(),error),qPrintable(error)); QCOMPARE(backend->writes,0);
        session->selectValue(static_cast<AeroLoginKey>(99),true); QCOMPARE(session->staged().intents.size(),3);
    }
    void forgedFieldMetadataUnknownIntentsAndUnownedImageStateAreRejected() {
        const auto baseline=session->applied(); QString error;
        auto forged=baseline; forged.fields[AeroLoginKey::StartupAnimation].value=true; QVERIFY(!AeroLoginTypes::requestValid(baseline,forged,error));
        forged=baseline; forged.fields[AeroLoginKey::StartupAnimation].writable=false; QVERIFY(!AeroLoginTypes::requestValid(baseline,forged,error));
        forged=baseline; forged.intents[static_cast<AeroLoginKey>(99)]=AeroLoginIntent::Set; QVERIFY(!AeroLoginTypes::requestValid(baseline,forged,error));
        forged=baseline; forged.intents[AeroLoginKey::StartupAnimation]=static_cast<AeroLoginIntent>(99); QVERIFY(!AeroLoginTypes::requestValid(baseline,forged,error));
        forged=baseline; forged.replacement=aeroLoginImageFixture(Qt::red); QVERIFY(!AeroLoginTypes::requestValid(baseline,forged,error));
        forged=baseline; forged.helperRevision=revision("forged-helper"); QVERIFY(!AeroLoginTypes::requestValid(baseline,forged,error));
    }
    void equalEffectiveChoiceStillCreatesAnExplicitCanonicalUserOverride() {
        const auto baseline=session->applied(); QVERIFY(!baseline.fields[AeroLoginKey::StartupAnimation].localPresent);
        session->selectValue(AeroLoginKey::StartupAnimation,baseline.fields[AeroLoginKey::StartupAnimation].value); QVERIFY(session->isDirty()); QCOMPARE(session->staged().intents.value(AeroLoginKey::StartupAnimation),AeroLoginIntent::Set); QCOMPARE(backend->writes,0);
        session->apply(); QCOMPARE(backend->writes,1); backend->saved(); QVERIFY(!session->isDirty()); QVERIFY(session->applied().fields[AeroLoginKey::StartupAnimation].localPresent); QVERIFY(session->applied().fields[AeroLoginKey::StartupAnimation].canonical);
    }
    void blueBackgroundCouplingIsAtomicAndFalseUndoesOnlyPendingRemoval() {
        session->selectValue(AeroLoginKey::RdpBackground,true); QVERIFY(session->staged().fields[AeroLoginKey::RdpBackground].value); QCOMPARE(session->staged().backgroundIntent,AeroLoginBackgroundIntent::RemoveOverride);
        session->selectValue(AeroLoginKey::RdpBackground,false); QCOMPARE(session->staged().backgroundIntent,AeroLoginBackgroundIntent::Keep); QVERIFY(session->staged().background.present);
        session->revert(); backend->current.background={}; auto &field=backend->current.fields[AeroLoginKey::RdpBackground]; field.value=field.editorValue=true; field.localPresent=true; field.raw=QStringLiteral("true"); session->refresh();
        session->selectValue(AeroLoginKey::RdpBackground,false); QCOMPARE(session->staged().backgroundIntent,AeroLoginBackgroundIntent::Keep); QVERIFY(!session->staged().background.present); QVERIFY(!session->staged().fields[AeroLoginKey::RdpBackground].value);
    }
    void matchingProtectedRdpCompanionAllowsImageButRequiredChangeRejectsEverything() {
        auto baseline=session->applied(); baseline.fields[AeroLoginKey::RdpBackground].writable=false; auto draft=baseline; QString error; const auto picture=aeroLoginImageFixture(Qt::red);
        QVERIFY2(AeroLoginTypes::selectImage(draft,baseline,picture,error),qPrintable(error)); QVERIFY(!draft.intents.contains(AeroLoginKey::RdpBackground)); QCOMPARE(draft.backgroundIntent,AeroLoginBackgroundIntent::ReplaceImage);
        draft=baseline; QVERIFY(!AeroLoginTypes::removeImage(draft,baseline,error)); QVERIFY(AeroLoginTypes::sameDraft(draft,baseline));
        baseline.fields[AeroLoginKey::RdpBackground].value=true; draft=baseline; QVERIFY(AeroLoginTypes::removeImage(draft,baseline,error)); QVERIFY(!draft.intents.contains(AeroLoginKey::RdpBackground));
        draft=baseline; QVERIFY(!AeroLoginTypes::restoreImage(draft,baseline,error)); QVERIFY(AeroLoginTypes::sameDraft(draft,baseline));
    }
    void imageAndApprovedDefaultsUsePinnedBytesAndDistinctConsumerFallbacks() {
        const auto picture=aeroLoginImageFixture(Qt::yellow); session->selectImage(picture); QCOMPARE(session->staged().backgroundIntent,AeroLoginBackgroundIntent::ReplaceImage); QVERIFY(session->staged().replacement==picture); QVERIFY(!session->staged().fields[AeroLoginKey::RdpBackground].value);
        session->removeImage(); QVERIFY(AeroLoginTypes::loginProjection(session->staged()).contains("blue")); QVERIFY(AeroLoginTypes::lockProjection(session->staged()).contains("default picture")); QCOMPARE(session->staged().backgroundIntent,AeroLoginBackgroundIntent::RemoveOverride);
        session->restoreImage(); QCOMPARE(session->staged().backgroundIntent,AeroLoginBackgroundIntent::RestoreApprovedImage); QVERIFY(!session->staged().fields[AeroLoginKey::RdpBackground].value); QCOMPARE(backend->writes,0);
        auto forged=picture; forged.digest="invalid"; const auto before=session->staged(); session->selectImage(forged); QVERIFY(AeroLoginTypes::sameDraft(before,session->staged()));
    }
    void approvedDefaultsAreAllOrNothingAcrossFourFieldsAndImage() {
        for (auto key:AeroLoginTypes::keys()) { auto &field=backend->current.fields[key]; field.localPresent=true; field.value=!field.defaultValue; field.raw=field.value?QStringLiteral("true"):QStringLiteral("false"); }
        session->refresh(); session->restoreDefaults(); QCOMPARE(session->staged().intents.size(),4); for (auto key:AeroLoginTypes::keys()) QCOMPARE(session->staged().fields[key].value,session->applied().fields[key].defaultValue); QCOMPARE(session->staged().backgroundIntent,AeroLoginBackgroundIntent::RestoreApprovedImage); QCOMPARE(backend->writes,0);
        session->revert(); backend->current.fields[AeroLoginKey::StartupSound].defaultKnown=false; session->refresh(); const auto before=session->staged(); session->restoreDefaults(); QVERIFY(AeroLoginTypes::sameDraft(before,session->staged()));
        backend->current.fields[AeroLoginKey::StartupSound].defaultKnown=true; backend->current.approvedDefault={}; session->refresh(); const auto withoutImage=session->staged(); session->restoreDefaults(); QVERIFY(AeroLoginTypes::sameDraft(withoutImage,session->staged()));
    }
    void verifiedPostRevisionIsRequiredEvenForMatchingSavedValues_data() { QTest::addColumn<int>("receipt"); QTest::newRow("missing")<<0; QTest::newRow("different")<<1; QTest::newRow("matching")<<2; }
    void verifiedPostRevisionIsRequiredEvenForMatchingSavedValues() {
        QFETCH(int,receipt); session->selectValue(AeroLoginKey::StartupAnimation,true); const auto draft=session->staged(); QSignalSpy done(session,&SettingsSession::applyFinished); session->apply(); QCOMPARE(backend->writes,1); backend->current=savedAeroLoginFixture(backend->requested);
        backend->result(true,{},receipt==0?QByteArray():receipt==1?revision("different-post-state"):backend->current.revision); QCOMPARE(done.size(),1); QCOMPARE(done.first().first().toBool(),receipt==2);
        if (receipt==2) { QVERIFY(!session->isDirty()); QVERIFY(!session->hasConflict()); QVERIFY(session->resultText().contains("adoption remain unverified")); }
        else { QVERIFY(session->isDirty()); QVERIFY(session->hasConflict()); QVERIFY(!session->canApply()); QVERIFY(AeroLoginTypes::sameDraft(draft,session->staged())); }
    }
    void canceledAuthorizationWithUnchangedFilesKeepsAnImmediatelyRetryableDraft() {
        session->selectValue(AeroLoginKey::ForceUserSelection,true); const auto before=session->staged(); session->apply(); backend->result(false,QStringLiteral("Authorization canceled")); QVERIFY(!session->isBusy()); QVERIFY(!session->hasConflict()); QVERIFY(session->canEdit()); QVERIFY(session->canApply()); QVERIFY(session->isDirty()); QVERIFY(AeroLoginTypes::sameDraft(before,session->staged()));
        session->apply(); QCOMPARE(backend->writes,2); backend->saved(); QVERIFY(!session->isDirty()); QVERIFY(!session->hasConflict());
    }
    void foreignReadbackAndFailedRevertNeverDiscardDraftImageBytes() {
        const auto picture=aeroLoginImageFixture(Qt::red); session->selectImage(picture); const auto draft=session->staged(); session->apply(); backend->current=savedAeroLoginFixture(backend->requested); const auto receipt=backend->current.revision; backend->current.revision=revision("foreign-readback"); backend->result(true,{},receipt);
        QVERIFY(session->hasConflict()); QVERIFY(session->isDirty()); QVERIFY(AeroLoginTypes::sameDraft(draft,session->staged())); backend->holdRead=true; session->revert(); backend->failedRead(); QVERIFY(session->hasConflict()); QVERIFY(session->staged().replacement==picture); QVERIFY(!session->canApply());
        backend->holdRead=false; session->revert(); QVERIFY(!session->hasConflict()); QVERIFY(!session->isDirty()); QCOMPARE(session->staged().backgroundIntent,AeroLoginBackgroundIntent::Keep);
    }
    void preflightSourceOrHelperChangeBlocksAnyBackendWrite() {
        session->selectValue(AeroLoginKey::StartupAnimation,true); const auto draft=session->staged(); backend->holdRead=true; session->apply(); backend->current.helperRevision=revision("new-helper"); backend->read(); QCOMPARE(backend->writes,0); QVERIFY(session->hasConflict()); QVERIFY(AeroLoginTypes::sameDraft(draft,session->staged()));
    }
    void heldAuthorizationOutlivesDeletedViewAndDuplicateLateRepliesAreIgnored() {
        auto *view=new QObject; connect(session,&SettingsSession::stateChanged,view,[]{}); session->selectValue(AeroLoginKey::StartupAnimation,true); session->apply(); QCOMPARE(backend->writes,1); const quint64 token=backend->writeToken; backend->pending(); QVERIFY(session->isBusy()); QVERIFY(session->errorText().contains("pending")); delete view; session->apply(); QCOMPARE(backend->writes,1);
        backend->saved(); QVERIFY(!session->isBusy()); QVERIFY(!session->isDirty()); const auto verified=session->applied(); emit backend->writeFinished(token,false,QStringLiteral("late old cancellation")); QVERIFY(AeroLoginTypes::sameConfiguration(verified,session->applied())); QVERIFY(!session->hasConflict());
    }
    void validPreparedCandidateIsDirtyAndBlocksApplyUntilReviewedOrReverted() {
        const auto image=aeroLoginImageFixture(Qt::yellow); session->setProperty("aeroLoginPendingImage",QVariant::fromValue(image)); QVERIFY(session->hasImageReview()); QVERIFY(session->isDirty()); QVERIFY(!AeroLoginTypes::dirty(session->staged())); QVERIFY(!session->canApply()); session->apply(); QCOMPARE(backend->writes,0);
        session->selectImage(image); QVERIFY(session->isDirty()); QVERIFY(!session->canApply()); session->setProperty("aeroLoginPendingImage",QVariant()); QVERIFY(session->isDirty()); QVERIFY(session->canApply()); session->revert(); QVERIFY(!session->isDirty());
    }
    void candidateOnlyRefreshAdoptsNewBaselineWhileRetainingReviewAndCloseDirtyState() {
        const auto image=aeroLoginImageFixture(Qt::yellow); session->setProperty("aeroLoginPendingImage",QVariant::fromValue(image)); backend->current.revision=revision("new-clean-baseline"); auto &field=backend->current.fields[AeroLoginKey::ForceUserSelection]; field.value=true; field.localPresent=true; field.raw=QStringLiteral("true"); backend->changed();
        QCOMPARE(session->applied().revision,backend->current.revision); QVERIFY(!session->hasConflict()); QVERIFY(session->isDirty()); QVERIFY(session->hasImageReview()); QVERIFY(session->property("aeroLoginPendingImage").value<AeroLoginImage>()==image); QVERIFY(session->canEdit()); QVERIFY(!session->canApply());
    }
    void typedDraftPlusCandidateRefreshRetainsConflictAndBothFormsOfInput() {
        session->selectValue(AeroLoginKey::StartupAnimation,true); const auto image=aeroLoginImageFixture(Qt::yellow); session->setProperty("aeroLoginPendingImage",QVariant::fromValue(image)); const auto draft=session->staged(); backend->current.revision=revision("foreign-typed-baseline"); backend->changed(); QVERIFY(session->hasConflict()); QVERIFY(session->isDirty()); QVERIFY(AeroLoginTypes::sameDraft(draft,session->staged())); QVERIFY(session->property("aeroLoginPendingImage").value<AeroLoginImage>()==image); QVERIFY(!session->canEdit());
    }
    void preparationIsAnAppOwnedBusyJobAndPendingRefreshKeepsCandidate() {
        auto *view=new QObject; QVERIFY(session->beginImagePreparation()); QVERIFY(session->isBusy()); QVERIFY(session->isPreparingImage()); QVERIFY(!session->beginImagePreparation()); delete view; const int reads=backend->reads; session->refresh(); QCOMPARE(backend->reads,reads);
        const auto image=aeroLoginImageFixture(Qt::yellow); session->setProperty("aeroLoginPendingImage",QVariant::fromValue(image)); backend->holdRead=true; session->endImagePreparation(); QVERIFY(!session->isPreparingImage()); QVERIFY(session->isBusy()); QCOMPARE(backend->reads,reads+1); backend->current.revision=revision("post-preparation-refresh"); backend->read(); QVERIFY(!session->isBusy()); QVERIFY(!session->hasConflict()); QVERIFY(session->isDirty()); QVERIFY(session->hasImageReview()); QCOMPARE(backend->writes,0);
    }
    void failedRevertKeepsReviewCandidateAndSuccessfulRevertClearsAllSourceMetadata() {
        session->setProperty("aeroLoginPendingImage",QVariant::fromValue(aeroLoginImageFixture(Qt::red))); session->setProperty("aeroLoginPendingImageName",QStringLiteral("chosen.png")); session->setProperty("aeroLoginImageSource",QStringLiteral("older.png")); backend->holdRead=true; session->revert(); backend->failedRead(); QVERIFY(session->isDirty()); QVERIFY(session->hasImageReview()); QVERIFY(session->hasConflict());
        backend->holdRead=false; session->revert(); QVERIFY(!session->isDirty()); QVERIFY(!session->hasImageReview()); QVERIFY(session->property("aeroLoginPendingImageName").toString().isEmpty()); QVERIFY(session->property("aeroLoginImageSource").toString().isEmpty());
    }
    void latestObservedJournalCanBeRecoveredWhileOldDraftRemainsConflicted() {
        session->selectValue(AeroLoginKey::StartupAnimation,true); const auto draft=session->staged(); backend->current.recoveryPending=backend->current.canRecover=true; backend->current.recoverySummary=QStringLiteral("Interrupted transaction"); backend->current.revision=revision("journal-state"); backend->changed(); QVERIFY(session->hasConflict()); QVERIFY(session->canRecover()); QVERIFY(!session->applied().recoveryPending);
        session->recover(); QCOMPARE(backend->recoveries,1); QCOMPARE(backend->expected.revision,revision("journal-state")); backend->current.recoveryPending=backend->current.canRecover=false; backend->current.recoverySummary.clear(); backend->current.revision=revision("recovered-state"); backend->result(true,{},backend->current.revision);
        QVERIFY(!session->isBusy()); QVERIFY(session->hasConflict()); QVERIFY(session->isDirty()); QVERIFY(AeroLoginTypes::sameDraft(draft,session->staged())); QVERIFY(session->resultText().contains("draft is retained")); session->revert(); QVERIFY(!session->isDirty()); QVERIFY(!session->hasConflict()); QCOMPARE(session->applied().revision,backend->current.revision);
    }
    void changedRecoveryPreflightAndUnverifiedRecoveryReceiptNeverClearConflict() {
        backend->current.recoveryPending=backend->current.canRecover=true; backend->current.revision=revision("journal-one"); session->refresh(); backend->holdRead=true; session->recover(); backend->current.revision=revision("journal-foreign"); backend->read(); QCOMPARE(backend->recoveries,0); QVERIFY(session->hasConflict());
        backend->holdRead=false; session->recover(); QCOMPARE(backend->recoveries,1); backend->current.recoveryPending=backend->current.canRecover=false; backend->current.revision=revision("recovered-without-receipt"); backend->result(true,{}); QVERIFY(session->hasConflict()); QVERIFY(!session->canApply());
    }
    void pendingPropertyAndPreparationCallbacksAllowSynchronousSessionDeletion() {
        QPointer<AeroLoginSettings> guard(session); connect(session,&SettingsSession::stateChanged,session,[this]{ if (session && session->hasImageReview()) { auto *dying=session; session=nullptr; backend=nullptr; delete dying; } }); session->setProperty("aeroLoginPendingImage",QVariant::fromValue(aeroLoginImageFixture(Qt::red))); QVERIFY(!guard);
        backend=new SettingsAeroLoginBackend; session=new AeroLoginSettings(backend); QTRY_VERIFY(session->isReady()); QVERIFY(session->beginImagePreparation()); session->refresh(); guard=session; connect(session,&SettingsSession::stateChanged,session,[this]{ if (session && !session->isPreparingImage()) { auto *dying=session; session=nullptr; backend=nullptr; delete dying; } }); session->endImagePreparation(); QVERIFY(!guard);
    }
private:
    SettingsAeroLoginBackend *backend=nullptr; AeroLoginSettings *session=nullptr;
};
QTEST_MAIN(AeroLoginSettingsTest)
#include "aero_login_settings_test.moc"
