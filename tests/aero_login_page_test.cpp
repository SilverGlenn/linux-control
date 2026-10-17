#include <QtTest>
#include <QBuffer>
#include <QCheckBox>
#include <QColor>
#include <QCryptographicHash>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QImage>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>
#include "ui/pages/AeroLoginPage.h"
#include "aero_login_fixture.h"

namespace {
QByteArray png(const QColor &color=Qt::darkBlue)
{ QImage image(32,16,QImage::Format_ARGB32); image.fill(color); QByteArray bytes; QBuffer buffer(&bytes); buffer.open(QIODevice::WriteOnly); image.save(&buffer,"PNG"); return bytes; }
AeroLoginSnapshot fixture() { return aeroLoginFixture(); }
void accept(QDialog *dialog)
{ QVERIFY(dialog); auto *box=dialog->findChild<QDialogButtonBox *>(); QVERIFY(box); box->button(QDialogButtonBox::Ok)->click(); }
void choose(AeroLoginPage &page,const QString &path)
{
    bool selected=false; page.findChild<QPushButton *>("aeroLoginChooseImage")->click();
    QTimer::singleShot(0,&page,[&page,path,&selected]{ auto *dialog=page.findChild<QFileDialog *>("aeroLoginImageFileDialog"); QVERIFY(dialog); QVERIFY(dialog->testOption(QFileDialog::DontUseNativeDialog)); QCOMPARE(dialog->supportedSchemes(),QStringList{QStringLiteral("file")}); dialog->selectFile(path); selected=true; QVERIFY(QMetaObject::invokeMethod(dialog,"accept")); }); QTRY_VERIFY(selected);
}
QString imageFile(QTemporaryDir &directory,const QString &name=QStringLiteral("selected.png"),const QByteArray &bytes=png(Qt::red))
{ const auto path=directory.filePath(name); QFile file(path); if (!file.open(QIODevice::WriteOnly) || file.write(bytes)!=bytes.size()) return {}; return path; }
}
class PageAeroLoginBackend final:public AeroLoginBackend {
public:
    AeroLoginSnapshot current=fixture(),expected,requested; int reads=0,writes=0,recoveries=0; quint64 readToken=0,writeToken=0; bool hold=false;
    void readCurrent(quint64 token) override { ++reads; readToken=token; if (!hold) emit readFinished(token,true,current,{}); }
    void writeChanges(quint64 token,const AeroLoginSnapshot &before,const AeroLoginSnapshot &after) override { ++writes; writeToken=token; expected=before; requested=after; }
    void recover(quint64 token,const AeroLoginSnapshot &before) override { ++recoveries; writeToken=token; expected=before; }
    void changed() { emit externalChange(); }
    void finishRead() { emit readFinished(readToken,true,current,{}); }
    void failRead() { emit readFinished(readToken,false,{},QStringLiteral("Fake fresh read failed")); }
    void failWrite() { emit writeFinished(writeToken,false,QStringLiteral("Administrator authorization was canceled; no saved changes confirmed.")); }
    void complete() { current=savedAeroLoginFixture(requested); emit writeFinished(writeToken,true,{},current.revision); }
};
class AeroLoginPageTest final:public QObject {
    Q_OBJECT
private slots:
    void init() { backend=new PageAeroLoginBackend; session=new AeroLoginSettings(backend); QTRY_VERIFY(session->isReady()); }
    void cleanup() { if (session) { QTRY_VERIFY(!session->isPreparingImage()); delete session; } }
    void injectedViewContainsAllFourNativeChoicesAndNoThemePickerOrLiveActions() {
        AeroLoginPage page(nullptr,session,nullptr); QCOMPARE(page.objectName(),QStringLiteral("win7Page")); QCOMPARE(page.settingsSession(),static_cast<SettingsSession *>(session)); QCOMPARE(page.findChildren<QCheckBox *>().size(),4);
        for (auto name:{"aeroLoginStartupAnimation","aeroLoginStartupSound","aeroLoginForceUserSelection","aeroLoginRdpBackground"}) { auto *control=page.findChild<QCheckBox *>(name); QVERIFY(control); QVERIFY(!control->accessibleName().isEmpty()); QVERIFY(control->isEnabled()); }
        for (auto name:{"aeroLoginImageSummary","aeroLoginEffects","aeroLoginPendingImage","aeroLoginChanges","aeroLoginMessage","aeroLoginDetailsText"}) QCOMPARE(page.findChild<QLabel *>(name)->textFormat(),Qt::PlainText);
        QVERIFY(page.findChild<QWidget *>("aeroLoginDetails")->isHidden()); QVERIFY(page.findChild<QLabel *>("aeroLoginEffects")->text().contains("static estimates")); QVERIFY(!session->isDirty()); QCOMPARE(backend->writes,0); QCOMPARE(backend->recoveries,0);
    }
    void untrustedOrUnavailableHelperLeavesEveryMutationDisabledWithReason() {
        backend->current.trustedInstallation=false; backend->current.helperAvailable=false; backend->current.reason=QStringLiteral("Installation is owned by nobody and cannot be trusted."); backend->current.helperReason=QStringLiteral("Approved write helper is not installed."); session->refresh(); AeroLoginPage page(nullptr,session,nullptr);
        for (auto *check:page.findChildren<QCheckBox *>()) QVERIFY(!check->isEnabled()); for (auto name:{"aeroLoginChooseImage","aeroLoginRemoveImage","aeroLoginRestoreImage","aeroLoginDefaults","aeroLoginApply"}) QVERIFY(!page.findChild<QPushButton *>(name)->isEnabled());
        QVERIFY(page.findChild<QLabel *>("aeroLoginMessage")->text().contains("nobody")); QVERIFY(page.findChild<QLabel *>("aeroLoginMessage")->text().contains("not installed")); QCOMPARE(backend->writes,0);
    }
    void loginBooleansStageIndependentlyWithoutChangingImage() {
        AeroLoginPage page(nullptr,session,nullptr); const auto saved=session->staged().background;
        page.findChild<QCheckBox *>("aeroLoginStartupAnimation")->click(); page.findChild<QCheckBox *>("aeroLoginStartupSound")->click(); page.findChild<QCheckBox *>("aeroLoginForceUserSelection")->click();
        QCOMPARE(session->staged().intents.size(),3); QCOMPARE(session->staged().backgroundIntent,AeroLoginBackgroundIntent::Keep); QVERIFY(session->staged().background==saved); QCOMPARE(backend->writes,0);
    }
    void rdpCouplingUndoAndBaselineAbsenceStayTruthful() {
        AeroLoginPage page(nullptr,session,nullptr); auto *rdp=page.findChild<QCheckBox *>("aeroLoginRdpBackground"); rdp->click(); QVERIFY(session->staged().fields[AeroLoginKey::RdpBackground].value); QCOMPARE(session->staged().backgroundIntent,AeroLoginBackgroundIntent::RemoveOverride);
        QVERIFY(page.findChild<QLabel *>("aeroLoginPreview")->text().contains("Blue login")); QVERIFY(!page.findChild<QLabel *>("aeroLockPreview")->pixmap().isNull());
        rdp->click(); QVERIFY(!session->staged().fields[AeroLoginKey::RdpBackground].value); QCOMPARE(session->staged().backgroundIntent,AeroLoginBackgroundIntent::Keep); QVERIFY(!page.findChild<QLabel *>("aeroLoginPreview")->pixmap().isNull());
        session->revert(); backend->current.background={}; auto &field=backend->current.fields[AeroLoginKey::RdpBackground]; field.value=field.editorValue=true; field.localPresent=true; field.raw=QStringLiteral("true"); session->refresh(); rdp->click(); QVERIFY(!session->staged().fields[AeroLoginKey::RdpBackground].value); QCOMPARE(session->staged().backgroundIntent,AeroLoginBackgroundIntent::Keep); QVERIFY(page.findChild<QLabel *>("aeroLoginEffects")->text().contains("does not recreate an image")); QCOMPARE(backend->writes,0);
    }
    void approvedImageRestoreStagesRdpFalseAndIsNotRemoval() {
        backend->current.fields[AeroLoginKey::RdpBackground].value=true; backend->current.fields[AeroLoginKey::RdpBackground].raw=QStringLiteral("true"); backend->current.background={}; session->refresh(); AeroLoginPage page(nullptr,session,nullptr); page.findChild<QPushButton *>("aeroLoginRestoreImage")->click();
        QVERIFY(!session->staged().fields[AeroLoginKey::RdpBackground].value); QCOMPARE(session->staged().backgroundIntent,AeroLoginBackgroundIntent::RestoreApprovedImage); QVERIFY(!page.findChild<QLabel *>("aeroLoginPreview")->pixmap().isNull()); QCOMPARE(backend->writes,0);
    }
    void unknownAndNoncanonicalRawValuesStayVisibleUntilExplicitChoice() {
        auto &field=backend->current.fields[AeroLoginKey::StartupSound]; field.value=true; field.raw=QStringLiteral("off"); field.canonical=false; field.editorKnown=true; field.editorValue=false; backend->current.fields[AeroLoginKey::StartupAnimation].known=false; session->refresh(); AeroLoginPage page(nullptr,session,nullptr);
        QCOMPARE(page.findChild<QCheckBox *>("aeroLoginStartupAnimation")->checkState(),Qt::PartiallyChecked); const auto warning=page.findChild<QLabel *>("aeroLoginStartupSoundWarning")->text(); QVERIFY(warning.contains("off")); QVERIFY(warning.contains("differently")); QVERIFY(!session->isDirty()); page.findChild<QCheckBox *>("aeroLoginStartupAnimation")->click(); QVERIFY(session->staged().intents.contains(AeroLoginKey::StartupAnimation)); QCOMPARE(backend->writes,0);
    }
    void localImagePreparationStagesCanonicalBytesAndSurvivesNavigation() {
        QTemporaryDir directory; QVERIFY(directory.isValid()); const auto path=imageFile(directory,QStringLiteral("selected<&.png")); QVERIFY(!path.isEmpty());
        { AeroLoginPage page(nullptr,session,nullptr); choose(page,path); QTRY_COMPARE(session->staged().backgroundIntent,AeroLoginBackgroundIntent::ReplaceImage); QVERIFY(session->staged().replacement.valid); QVERIFY(!session->staged().fields[AeroLoginKey::RdpBackground].value); }
        AeroLoginPage reopened(nullptr,session,nullptr); QVERIFY(reopened.findChild<QLabel *>("aeroLoginImageSummary")->text().contains("selected<&.png")); QVERIFY(reopened.findChild<QLabel *>("aeroLoginImageSummary")->text().contains("32 × 16")); QVERIFY(reopened.findChild<QLabel *>("aeroLoginImageSummary")->text().contains("SHA-256")); QCOMPARE(backend->writes,0);
    }
    void pendingRefreshRetainsPreparedSelectionForExplicitReview() {
        QTemporaryDir directory; const auto path=imageFile(directory); AeroLoginPage page(nullptr,session,nullptr); bool armed=true;
        auto connection=connect(session,&SettingsSession::stateChanged,session,[this,&armed]{ if (armed && session->isPreparingImage()) { armed=false; backend->current.revision="external-revision"; session->refresh(); } });
        choose(page,path); QTRY_VERIFY(!session->isPreparingImage()); QTRY_VERIFY(session->property("aeroLoginPendingImage").value<AeroLoginImage>().valid); disconnect(connection); QVERIFY(session->isDirty()); QVERIFY(page.findChild<QPushButton *>("aeroLoginReviewImage")->isVisibleTo(&page));
        QTimer::singleShot(0,&page,[&page]{ auto *dialog=page.findChild<QDialog *>("aeroLoginImageReviewDialog"); QVERIFY(dialog); accept(dialog); }); page.findChild<QPushButton *>("aeroLoginReviewImage")->click(); QCOMPARE(session->staged().backgroundIntent,AeroLoginBackgroundIntent::ReplaceImage); QVERIFY(!session->property("aeroLoginPendingImage").value<AeroLoginImage>().valid); QCOMPARE(backend->writes,0);
    }
    void preparationOutlivesViewWithoutLosingSelectedBytes() {
        QTemporaryDir directory; const auto path=imageFile(directory); auto *page=new AeroLoginPage(nullptr,session,nullptr); QPointer<AeroLoginPage> guard(page);
        connect(session,&SettingsSession::stateChanged,session,[this,page]{ if (session->isPreparingImage()) delete page; },Qt::SingleShotConnection);
        choose(*page,path); QTRY_VERIFY(!guard); QTRY_COMPARE(session->staged().backgroundIntent,AeroLoginBackgroundIntent::ReplaceImage); AeroLoginPage reopened(nullptr,session,nullptr); QVERIFY(reopened.findChild<QLabel *>("aeroLoginImageSummary")->text().contains("selected.png")); QCOMPARE(backend->writes,0);
    }
    void preparingResultCanDeleteItsOwningSessionDuringPendingPropertyChange() {
        QTemporaryDir directory; const auto path=imageFile(directory); AeroLoginPage page(nullptr,session,nullptr); QPointer<AeroLoginSettings> guard(session);
        connect(session,&SettingsSession::stateChanged,session,[this]{ if (session && session->hasImageReview()) { auto *dying=session; session=nullptr; backend=nullptr; delete dying; } });
        choose(page,path); QTRY_VERIFY(!guard); QVERIFY(!page.isEnabled()); QVERIFY(page.findChild<QLabel *>("aeroLoginMessage")->text().contains("no longer available"));
    }
    void invalidOrOversizedImagesNeverReplaceTheDraft() {
        QTemporaryDir directory; const auto invalid=imageFile(directory,QStringLiteral("invalid.png"),QByteArray("not a PNG")); AeroLoginPage page(nullptr,session,nullptr); choose(page,invalid); QTRY_VERIFY(!session->isPreparingImage()); QTRY_VERIFY(!session->property("aeroLoginImageError").toString().isEmpty()); QVERIFY(!session->isDirty());
        const auto oversized=directory.filePath(QStringLiteral("oversized.png")); QFile file(oversized); QVERIFY(file.open(QIODevice::WriteOnly)); QVERIFY(file.resize(AeroLoginTypes::MaximumImageBytes+1)); file.close(); choose(page,oversized); QTRY_VERIFY(!session->isPreparingImage()); QTRY_VERIFY(session->property("aeroLoginImageError").toString().contains("16 MiB")); QVERIFY(!session->isDirty()); QCOMPARE(backend->writes,0);
    }
    void applyAlwaysReviewsBeforeAnyAuthorizationOrWriteAndCancelPreservesDraft() {
        AeroLoginPage page(nullptr,session,nullptr); page.findChild<QCheckBox *>("aeroLoginForceUserSelection")->click();
        QTimer::singleShot(0,&page,[&page,this]{ auto *dialog=page.findChild<QDialog *>("aeroLoginApplyReviewDialog"); QVERIFY(dialog); QCOMPARE(backend->writes,0); const auto text=dialog->findChild<QLabel *>("aeroLoginReviewText")->text(); QVERIFY(text.contains("Image before:")); QVERIFY(text.contains("Estimated login:")); QVERIFY(text.contains("Estimated lock screen:")); dialog->reject(); }); page.findChild<QPushButton *>("aeroLoginApply")->click(); QCOMPARE(backend->writes,0); QVERIFY(session->isDirty());
        QTimer::singleShot(0,&page,[&page]{ accept(page.findChild<QDialog *>("aeroLoginApplyReviewDialog")); }); page.findChild<QPushButton *>("aeroLoginApply")->click(); QCOMPARE(backend->writes,1); backend->failWrite(); QVERIFY(session->isDirty()); QVERIFY(page.findChild<QLabel *>("aeroLoginMessage")->text().contains("authorization"));
    }
    void staleApplyReviewKeepsTheNewerDraftAndDoesNotWrite() {
        AeroLoginPage page(nullptr,session,nullptr); page.findChild<QCheckBox *>("aeroLoginForceUserSelection")->click(); QTimer::singleShot(0,&page,[&page,this]{ session->selectValue(AeroLoginKey::StartupAnimation,true); accept(page.findChild<QDialog *>("aeroLoginApplyReviewDialog")); }); page.findChild<QPushButton *>("aeroLoginApply")->click(); QVERIFY(page.findChild<QLabel *>("aeroLoginMessage")->text().contains("stale")); QVERIFY(session->isDirty()); QCOMPARE(backend->writes,0);
    }
    void approvedDefaultsAreReviewedAndStagedWithoutTouchingUnownedState() {
        AeroLoginPage page(nullptr,session,nullptr); page.findChild<QCheckBox *>("aeroLoginForceUserSelection")->click(); QTimer::singleShot(0,&page,[&page]{ auto *dialog=page.findChild<QDialog *>("aeroLoginDefaultsReviewDialog"); QVERIFY(dialog); QVERIFY(dialog->findChild<QLabel *>("aeroLoginReviewText")->text().contains("selected SDDM theme are preserved")); accept(dialog); }); page.findChild<QPushButton *>("aeroLoginDefaults")->click(); QCOMPARE(session->staged().backgroundIntent,AeroLoginBackgroundIntent::RestoreApprovedImage); QVERIFY(!session->staged().fields[AeroLoginKey::RdpBackground].value); QCOMPARE(backend->writes,0);
    }
    void failedRevertRetainsPreparedCandidateUntilAFreshSuccessfulRevert() {
        const auto prepared=AeroLoginTypes::prepareImage(png(Qt::yellow)); session->setProperty("aeroLoginPendingImage",QVariant::fromValue(prepared)); session->setProperty("aeroLoginPendingImageName",QStringLiteral("retained.png")); AeroLoginPage page(nullptr,session,nullptr); backend->hold=true; page.findChild<QPushButton *>("aeroLoginRevert")->click(); backend->failRead(); QVERIFY(session->property("aeroLoginPendingImage").value<AeroLoginImage>().valid); backend->hold=false; page.findChild<QPushButton *>("aeroLoginRevert")->click(); QVERIFY(!session->property("aeroLoginPendingImage").value<AeroLoginImage>().valid); QCOMPARE(backend->writes,0);
    }
    void reviewedRecoveryUsesLatestObservedJournalAndRetainsTheDraft() {
        AeroLoginPage page(nullptr,session,nullptr); page.findChild<QCheckBox *>("aeroLoginStartupAnimation")->click(); const auto draft=session->staged(); backend->current.recoveryPending=backend->current.canRecover=true; backend->current.recoverySummary=QStringLiteral("Interrupted transaction 123"); backend->current.revision="journal-revision"; backend->changed(); QVERIFY(session->hasConflict()); QVERIFY(!session->applied().recoveryPending); QVERIFY(page.findChild<QPushButton *>("aeroLoginRecover")->isVisibleTo(&page)); QVERIFY(page.findChild<QPushButton *>("aeroLoginRecover")->isEnabled());
        QTimer::singleShot(0,&page,[&page,this]{ auto *dialog=page.findChild<QDialog *>("aeroLoginRecoveryReviewDialog"); QVERIFY(dialog); QVERIFY(dialog->findChild<QLabel *>("aeroLoginReviewText")->text().contains("transaction 123")); QCOMPARE(backend->recoveries,0); accept(dialog); }); page.findChild<QPushButton *>("aeroLoginRecover")->click(); QCOMPARE(backend->recoveries,1); QCOMPARE(backend->expected.revision,QByteArray("journal-revision")); QVERIFY(AeroLoginTypes::sameDraft(session->staged(),draft)); QCOMPARE(backend->writes,0);
    }
    void staleRecoveryReviewCannotRepairANewerObservedTransaction() {
        AeroLoginPage page(nullptr,session,nullptr); session->selectValue(AeroLoginKey::StartupAnimation,true); backend->current.recoveryPending=backend->current.canRecover=true; backend->current.recoverySummary=QStringLiteral("First transaction"); backend->current.revision="journal-one"; backend->changed();
        QTimer::singleShot(0,&page,[&page,this]{ auto *dialog=page.findChild<QDialog *>("aeroLoginRecoveryReviewDialog"); QVERIFY(dialog); backend->current.revision="journal-two"; backend->current.recoverySummary=QStringLiteral("Newer transaction"); backend->changed(); accept(dialog); }); page.findChild<QPushButton *>("aeroLoginRecover")->click(); QCOMPARE(backend->recoveries,0); QVERIFY(page.findChild<QLabel *>("aeroLoginMessage")->text().contains("stale")); QVERIFY(session->isDirty());
    }
    void verifiedApplyReportsSavedFilesWithoutClaimingDisplayAdoption() {
        AeroLoginPage page(nullptr,session,nullptr); page.findChild<QCheckBox *>("aeroLoginForceUserSelection")->click(); QTimer::singleShot(0,&page,[&page]{ accept(page.findChild<QDialog *>("aeroLoginApplyReviewDialog")); }); page.findChild<QPushButton *>("aeroLoginApply")->click(); QCOMPARE(backend->writes,1); backend->complete(); QVERIFY(!session->isDirty()); QVERIFY(page.findChild<QLabel *>("aeroLoginMessage")->text().contains("adoption remain unverified"));
    }
    void screenLockNavigationOnlyEmitsPageIntent() {
        AeroLoginPage page(nullptr,session,nullptr); QSignalSpy navigation(&page,&AeroLoginPage::navigationRequested); page.findChild<QPushButton *>("aeroLoginScreenLock")->click(); QCOMPARE(navigation.size(),1); QCOMPARE(navigation.first().first().value<PageId>(),PageId::ScreenLock); QCOMPARE(backend->writes,0);
    }
private:
    PageAeroLoginBackend *backend=nullptr; AeroLoginSettings *session=nullptr;
};
QTEST_MAIN(AeroLoginPageTest)
#include "aero_login_page_test.moc"
