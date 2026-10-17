#include <QtTest>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QTimer>
#include <functional>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QPushButton>
#include <QPointer>
#include <QScrollArea>
#include <QSpinBox>
#include <AeroQt/stylesheet.h>
#include "ui/pages/DisplayPage.h"
#include "settings/DisplaySettings.h"
#include "display_fixture.h"

namespace {
DisplaySnapshot twoDisplays(bool secondEnabled = true)
{
    auto json = displayFixture().json; auto outputs = json.value("outputs").toArray();
    auto second = outputs.first().toObject(); second.insert("id", 2); second.insert("name", "HDMI-1");
    second.insert("enabled", secondEnabled); second.insert("priority", secondEnabled ? 2 : 0);
    second.insert("pos", QJsonObject{{"x", 1920}, {"y", 0}}); outputs.append(second); json.insert("outputs", outputs);
    auto screen = json.value("screen").toObject(); screen.insert("maxActiveOutputsCount", 3);
    screen.insert("currentSize", QJsonObject{{"width", secondEnabled ? 3840 : 1920}, {"height", 1080}}); json.insert("screen", screen);
    DisplaySnapshot result; QString error; DisplayProtocol::parse(QJsonDocument(json).toJson(), result, error); return result;
}
void capture(QWidget &page, const QString &name)
{
    const auto directory = qEnvironmentVariable("LINUX_CONTROL_UI_CAPTURE_DIR"); if (directory.isEmpty()) return;
    QDir().mkpath(directory); page.resize(1120, 1120); page.show(); QApplication::processEvents();
    QVERIFY(page.grab().save(directory + QLatin1Char('/') + name + QStringLiteral(".png")));
}
void selectDisplay(DisplayPage &page, int id)
{ auto *outputs = page.findChild<QComboBox *>("displayOutput"); outputs->setCurrentIndex(outputs->findData(id)); }
void position(DisplayPage &page, int x, int y)
{
    auto *horizontal = page.findChild<QSpinBox *>("displayPositionX"); auto *vertical = page.findChild<QSpinBox *>("displayPositionY");
    horizontal->setValue(x); vertical->setValue(y); QVERIFY(QMetaObject::invokeMethod(horizontal, "editingFinished"));
}
void reviewCustomScale(DisplayPage *page, const std::function<void(QDialog *)> &review)
{
    auto *scale=page->findChild<QComboBox *>("displayScale"); scale->setCurrentIndex(scale->findData(QStringLiteral("custom"),Qt::UserRole+1));
    bool visited=false;
    QTimer::singleShot(0,page,[&] { auto *dialog=page->findChild<QDialog *>("displayCustomScaleDialog"); visited=true; QVERIFY(dialog); review(dialog); });
    QCoreApplication::processEvents(); QVERIFY(visited);
}

}
class PageDisplayBackend : public DisplayBackend {
public:
    DisplaySnapshot current = displayFixture(), baseline, requested;
    int reads = 0, previews = 0, reverts = 0, keeps = 0;
    bool rollback = true, holdRead = false;
    int seconds = 15;
    bool rollbackAvailable() const override { return rollback; }
    void readCurrent() override { ++reads; if (!holdRead) finishRead(); }
    void finishRead() { emit readFinished(true, current, {}); }
    void beginPreview(const DisplaySnapshot &before, const DisplaySnapshot &after) override
    { ++previews; baseline = before; requested = after; emit previewReady(after, seconds); }
    void keepPreview() override { ++keeps; current = requested; emit transactionFinished(true, false, current, {}); }
    void revertPreview() override { ++reverts; current = baseline; emit transactionFinished(false, true, current, {}); }
    void changed() { emit externalChange(); }
};
class DisplayPageTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { if (qEnvironmentVariableIsSet("LINUX_CONTROL_UI_CAPTURE_DIR")) Aero::registerStylesheet(qApp); }
    void pageRecreationRetainsStagedAndPreviewSession() {
        auto *backend = new PageDisplayBackend; DisplaySettings session(backend); QTRY_VERIFY(session.isReady());
        auto *page = new DisplayPage(new QScrollArea, &session, nullptr);
        QCOMPARE(page->settingsSession(), static_cast<SettingsSession *>(&session)); QCOMPARE(page->objectName(), QStringLiteral("win7Page"));
        auto *modes = page->findChild<QComboBox *>("displayResolution"); modes->setCurrentIndex(modes->findData(QStringLiteral("2")));
        QVERIFY(session.isDirty()); QCOMPARE(backend->previews, 0); delete page;
        page = new DisplayPage(new QScrollArea, &session, nullptr); QCOMPARE(page->findChild<QComboBox *>("displayResolution")->currentData().toString(), QStringLiteral("2"));
        page->findChild<QPushButton *>("displayApply")->click(); QVERIFY(session.awaitingConfirmation()); QVERIFY(!page->findChild<QPushButton *>("displayApply")->isEnabled());
        delete page; QVERIFY(session.isBusy()); page = new DisplayPage(new QScrollArea, &session, nullptr);
        QVERIFY(!page->findChild<QPushButton *>("displayKeep")->isHidden()); page->findChild<QPushButton *>("displayRevertPreview")->click();
        QCOMPARE(backend->reverts, 1); QVERIFY(!session.isBusy()); delete page;
    }
    void fullLayoutFieldsStageAcrossOutputSwitchesWithoutPreviewing() {
        auto *backend = new PageDisplayBackend; backend->current = twoDisplays(); DisplaySettings session(backend); QTRY_VERIFY(session.isReady());
        DisplayPage page(nullptr, &session, nullptr); QCOMPARE(page.findChild<QComboBox *>("displayOutput")->count(), 2);
        auto *scale = page.findChild<QComboBox *>("displayScale"); scale->setCurrentIndex(scale->findData(1.5));
        auto *rotation = page.findChild<QComboBox *>("displayRotation"); rotation->setCurrentIndex(rotation->findData(2));
        QVERIFY(session.isDirty()); QVERIFY(page.findChild<QComboBox *>("displayOutput")->isEnabled());
        selectDisplay(page, 2); QCOMPARE(session.selectedOutput(), 2); position(page, 1600, 120);
        QCOMPARE(session.staged().output(1)->scale, 1.5); QCOMPARE(session.staged().output(1)->rotation, 2);
        QCOMPARE(session.staged().output(2)->position, QPoint(1600, 120)); QCOMPARE(session.snapshot().output(2)->position, QPoint(1920, 0));
        selectDisplay(page, 1); QCOMPARE(scale->currentData().toDouble(), 1.5); QCOMPARE(rotation->currentData().toInt(), 2);
        QVERIFY(page.findChild<QLabel *>("displayDraftChanges")->text().contains("HDMI-1")); QCOMPARE(backend->previews, 0);
        QVERIFY(page.findChild<QLabel *>("displayMonitor")->accessibleDescription().contains("HDMI-1"));
        capture(page, QStringLiteral("display-multi-output-draft"));
    }
    void relativePresetsUseLogicalScaleAndOrientationAndNormalizeNegativePositions() {
        auto *backend = new PageDisplayBackend; backend->current = twoDisplays(); DisplaySettings session(backend); QTRY_VERIFY(session.isReady());
        session.selectScale(1.5); session.selectRotation(2); // 1080/1.5 by 1920/1.5 = 720 by 1280.
        DisplayPage page(nullptr, &session, nullptr); selectDisplay(page, 2);
        auto *reference = page.findChild<QComboBox *>("displayRelativeOutput"); reference->setCurrentIndex(reference->findData(1));
        auto *placement = page.findChild<QComboBox *>("displayPlacement"); auto *move = page.findChild<QPushButton *>("displayArrange"); QVERIFY(move->isEnabled());
        placement->setCurrentIndex(placement->findData(QStringLiteral("right"))); move->click(); QCOMPARE(session.staged().output(2)->position, QPoint(720, 0));
        placement->setCurrentIndex(placement->findData(QStringLiteral("below"))); move->click(); QCOMPARE(session.staged().output(2)->position, QPoint(0, 1280));
        placement->setCurrentIndex(placement->findData(QStringLiteral("left"))); move->click();
        QCOMPARE(session.staged().output(2)->position, QPoint(0, 0)); QCOMPARE(session.staged().output(1)->position, QPoint(1920, 0));
        QCOMPARE(backend->previews, 0);
    }
    void disabledOutputsRemainSelectableAndCannotDisableAllForApply() {
        auto *backend = new PageDisplayBackend; backend->current = twoDisplays(false); DisplaySettings session(backend); QTRY_VERIFY(session.isReady());
        DisplayPage page(nullptr, &session, nullptr); selectDisplay(page, 2);
        auto *enabled = page.findChild<QCheckBox *>("displayEnabled"); QVERIFY(!enabled->isChecked()); QVERIFY(enabled->isEnabled());
        enabled->setChecked(true); QVERIFY(session.staged().output(2)->enabled); QVERIFY(session.isDirty());
        enabled->setChecked(false); selectDisplay(page, 1); enabled->setChecked(false);
        QVERIFY(!session.staged().output(1)->enabled); QVERIFY(!session.staged().output(2)->enabled);
        QVERIFY(!page.findChild<QPushButton *>("displayApply")->isEnabled()); QVERIFY(!page.findChild<QLabel *>("displaySupport")->text().isEmpty());
        QVERIFY(enabled->isEnabled()); enabled->setChecked(true); QVERIFY(session.staged().output(1)->enabled); QCOMPARE(backend->previews, 0);
    }
    void currentRawScaleAndReadOnlyRotationArePreserved() {
        auto *backend = new PageDisplayBackend; auto state = twoDisplays(); state.output(1)->scale = 1.3333333333333333; state.output(1)->rotation = 16;
        backend->current = DisplayProtocol::normalized(state); DisplaySettings session(backend); QTRY_VERIFY(session.isReady());
        DisplayPage page(nullptr, &session, nullptr);
        QCOMPARE(page.findChild<QComboBox *>("displayScale")->currentData().toDouble(), session.staged().output(1)->scale);
        QCOMPARE(page.findChild<QComboBox *>("displayRotation")->currentData().toInt(), 16);
        QVERIFY(page.findChild<QComboBox *>("displayRotation")->currentText().contains("read-only"));
        QVERIFY(!page.findChild<QComboBox *>("displayRotation")->isEnabled()); QVERIFY(!session.isDirty()); QCOMPARE(backend->previews, 0);
    }
    void unsupportedFieldFeaturesAndMissingRollbackDisableTheirControls() {
        auto *backend = new PageDisplayBackend; auto state = twoDisplays(); state.json.insert("features", 1); backend->current = state;
        DisplaySettings session(backend); QTRY_VERIFY(session.isReady()); DisplayPage page(nullptr, &session, nullptr);
        const bool editable = session.selectedSupportText().isEmpty();
        QCOMPARE(page.findChild<QComboBox *>("displayScale")->isEnabled(), editable && DisplayProtocol::canScale(session.staged()));
        QCOMPARE(page.findChild<QComboBox *>("displayRotation")->isEnabled(), editable && DisplayProtocol::canRotate(session.staged()));
        QCOMPARE(page.findChild<QSpinBox *>("displayPositionX")->isEnabled(), editable && DisplayProtocol::canPosition(session.staged()));
        backend->rollback = false; session.refresh(); QVERIFY(!page.findChild<QCheckBox *>("displayEnabled")->isEnabled());
        QVERIFY(page.findChild<QLabel *>("displaySelectedSupport")->text().contains("helper")); QCOMPARE(backend->previews, 0);
    }
    void nativeBoundaryCoordinatesRemainVisibleAndFocusDoesNotStage() {
        auto *backend = new PageDisplayBackend; auto state = twoDisplays(); state.output(2)->position = QPoint(DisplayProtocol::MaximumPosition, 0);
        backend->current = DisplayProtocol::normalized(state); DisplaySettings session(backend); QTRY_VERIFY(session.isReady());
        DisplayPage page(nullptr, &session, nullptr); selectDisplay(page, 2); auto *x = page.findChild<QSpinBox *>("displayPositionX");
        QCOMPARE(x->value(), DisplayProtocol::MaximumPosition); QVERIFY(x->toolTip().contains(QString::number(DisplayProtocol::MaximumPosition)));
        QMetaObject::invokeMethod(x,"editingFinished"); QVERIFY(!session.isDirty());
        position(page,DisplayProtocol::MaximumPosition+1,0); QCOMPARE(x->value(),DisplayProtocol::MaximumPosition);
        QCOMPARE(session.staged().output(2)->position.x(),DisplayProtocol::MaximumPosition); QVERIFY(!session.isDirty()); QCOMPARE(backend->previews,0);
    }
    void fullCoordinateBoundsAndScalePresetsRemainStagedOnly() {
        auto *backend = new PageDisplayBackend; backend->current=twoDisplays(); DisplaySettings session(backend); QTRY_VERIFY(session.isReady()); DisplayPage page(nullptr,&session,nullptr);
        selectDisplay(page,2); auto *x=page.findChild<QSpinBox *>("displayPositionX"); QCOMPARE(x->minimum(),-DisplayProtocol::MaximumPosition); QCOMPARE(x->maximum(),DisplayProtocol::MaximumPosition);
        position(page,40000,0); QCOMPARE(session.staged().output(2)->position.x(),40000);
        auto *scale=page.findChild<QComboBox *>("displayScale"); QVERIFY(scale->findData(4.0)>=0); scale->setCurrentIndex(scale->findData(4.0)); QCOMPARE(session.staged().output(2)->scale,4.0); QCOMPARE(backend->previews,0);
    }
    void customScaleCancelAndQuantizedStagePreserveDraftAcrossPageRecreation() {
        auto *backend=new PageDisplayBackend; backend->current=twoDisplays(); DisplaySettings session(backend); QTRY_VERIFY(session.isReady());
        auto *page=new DisplayPage(nullptr,&session,nullptr);
        reviewCustomScale(page,[&](QDialog *dialog) {
            auto *percent=dialog->findChild<QDoubleSpinBox *>("displayCustomScalePercent"); QCOMPARE(percent->minimum(),50.0); QCOMPARE(percent->maximum(),300.0);
            percent->setValue(133.7); dialog->findChild<QPushButton *>("displayCustomScaleCancel")->click(); }); QVERIFY(!session.isDirty());
        reviewCustomScale(page,[&](QDialog *dialog) {
            dialog->findChild<QDoubleSpinBox *>("displayCustomScalePercent")->setValue(133.7); QVERIFY(dialog->findChild<QLabel *>("displayCustomScaleNative")->text().contains("1/120"));
            dialog->findChild<QPushButton *>("displayCustomScaleStage")->click(); });
        QVERIFY(qAbs(session.staged().output(1)->scale - (160.0/120))<0.000001); QVERIFY(session.isDirty()); delete page;
        page=new DisplayPage(nullptr,&session,nullptr); QCOMPARE(page->findChild<QComboBox *>("displayScale")->currentData().toDouble(),session.staged().output(1)->scale);
        QVERIFY(page->findChild<QComboBox *>("displayScale")->currentText().contains("staged custom")); QVERIFY(!page->findChild<QComboBox *>("displayScale")->currentText().contains("current"));
        QCOMPARE(backend->previews,0); delete page;
    }
    void customScaleCannotStageAgainstChangedDraft() {
        auto *backend=new PageDisplayBackend; backend->current=twoDisplays(); DisplaySettings session(backend); QTRY_VERIFY(session.isReady()); DisplayPage page(nullptr,&session,nullptr);
        reviewCustomScale(&page,[&](QDialog *dialog) {
            dialog->findChild<QDoubleSpinBox *>("displayCustomScalePercent")->setValue(125); session.selectRotation(2);
            dialog->findChild<QPushButton *>("displayCustomScaleStage")->click(); QVERIFY(dialog->findChild<QLabel *>("displayCustomScaleStatus")->text().contains("changed")); dialog->reject(); });
        QCOMPARE(session.staged().output(1)->scale,1.0); QCOMPARE(session.staged().output(1)->rotation,2); QCOMPARE(backend->previews,0);
    }
    void relativePlacementOutsideEditorBoundsExplainsAndRetainsDraft() {
        auto *backend=new PageDisplayBackend; auto state=twoDisplays(); state.output(2)->position=QPoint(DisplayProtocol::MaximumPosition,0); backend->current=DisplayProtocol::normalized(state);
        DisplaySettings session(backend); QTRY_VERIFY(session.isReady()); DisplayPage page(nullptr,&session,nullptr);
        auto *reference=page.findChild<QComboBox *>("displayRelativeOutput"); reference->setCurrentIndex(reference->findData(2));
        auto *placement=page.findChild<QComboBox *>("displayPlacement"); placement->setCurrentIndex(placement->findData(QStringLiteral("right")));
        page.findChild<QPushButton *>("displayArrange")->click(); QVERIFY(!session.isDirty()); QCOMPARE(session.staged().output(1)->position,QPoint(0,0));
        QVERIFY(page.findChild<QLabel *>("displayStatus")->text().contains("outside")); QCOMPARE(backend->previews,0);
    }
    void customScaleDialogClosesSafelyWhenItsPageIsDestroyed() {
        auto *backend=new PageDisplayBackend; backend->current=twoDisplays(); DisplaySettings session(backend); QTRY_VERIFY(session.isReady());
        QPointer<DisplayPage> page=new DisplayPage(nullptr,&session,nullptr); QPointer<QDialog> dialog;
        reviewCustomScale(page,[&](QDialog *opened) { dialog=opened; delete page.data(); });
        QVERIFY(page.isNull()); QVERIFY(dialog.isNull()); QVERIFY(!session.isDirty()); QCOMPARE(backend->previews,0);
    }
    void keepCommitsTheWholeCandidateAndRestoresOnlyAfterVerifiedOutcome() {
        auto *backend = new PageDisplayBackend; backend->current = twoDisplays(); DisplaySettings session(backend); QTRY_VERIFY(session.isReady());
        DisplayPage page(nullptr, &session, nullptr); selectDisplay(page, 2); position(page, 1920, 100);
        page.findChild<QComboBox *>("displayScale")->setCurrentIndex(page.findChild<QComboBox *>("displayScale")->findData(1.25));
        page.findChild<QPushButton *>("displayApply")->click(); QCOMPARE(backend->previews, 1); QVERIFY(session.awaitingConfirmation());
        QVERIFY(!page.findChild<QCheckBox *>("displayEnabled")->isEnabled()); QVERIFY(!page.findChild<QSpinBox *>("displayPositionX")->isEnabled());
        capture(page, QStringLiteral("display-confirm-layout")); page.findChild<QPushButton *>("displayKeep")->click();
        QCOMPARE(backend->keeps, 1); QVERIFY(!session.isDirty()); QVERIFY(!session.isBusy());
        QCOMPARE(session.snapshot().output(2)->scale, 1.25); QCOMPARE(session.snapshot().output(2)->position, QPoint(1920, 100));
    }
    void timeoutRestorationSurvivesPageDestruction() {
        auto *backend = new PageDisplayBackend; backend->seconds = 1; backend->current = twoDisplays(); DisplaySettings session(backend); QTRY_VERIFY(session.isReady());
        auto *page = new DisplayPage(nullptr, &session, nullptr); selectDisplay(*page, 2); position(*page, 1920, 100); page->findChild<QPushButton *>("displayApply")->click();
        QVERIFY(session.awaitingConfirmation()); delete page; QTRY_COMPARE_WITH_TIMEOUT(backend->reverts, 1, 2500);
        QVERIFY(!session.isBusy()); page = new DisplayPage(nullptr, &session, nullptr);
        QVERIFY(page->findChild<QPushButton *>("displayKeep")->isHidden()); QCOMPARE(session.snapshot().output(2)->position, QPoint(1920, 0)); delete page;
    }
    void staleExternalLayoutRetainsDraftAndBlocksApplyUntilRevert() {
        auto *backend = new PageDisplayBackend; backend->current = twoDisplays(); DisplaySettings session(backend); QTRY_VERIFY(session.isReady());
        DisplayPage page(nullptr, &session, nullptr); selectDisplay(page, 2); position(page, 1920, 100);
        backend->current.output(1)->scale = 1.25; backend->current = DisplayProtocol::normalized(backend->current); backend->changed();
        QVERIFY(session.hasConflict()); QCOMPARE(session.staged().output(2)->position, QPoint(1920, 100));
        QVERIFY(!page.findChild<QPushButton *>("displayApply")->isEnabled()); QVERIFY(!page.findChild<QComboBox *>("displayScale")->isEnabled());
        page.findChild<QPushButton *>("displayRevert")->click(); QVERIFY(!session.hasConflict()); QVERIFY(!session.isDirty()); QCOMPARE(backend->previews, 0);
    }
    void accessibleControlsHaveKeyboardBuddiesAndPlainStatus() {
        auto *backend = new PageDisplayBackend; backend->current = twoDisplays(); DisplaySettings session(backend); QTRY_VERIFY(session.isReady()); DisplayPage page(nullptr, &session, nullptr);
        for (const auto &name : {"displayOutput", "displayResolution", "displayScale", "displayRotation", "displayPositionX", "displayPositionY", "displayRelativeOutput", "displayPlacement"}) {
            auto *control = page.findChild<QWidget *>(name); QVERIFY(control); QVERIFY(!control->accessibleName().isEmpty());
            bool buddy = false; for (auto *label : page.findChildren<QLabel *>()) buddy = buddy || label->buddy() == control; QVERIFY(buddy);
        }
        QCOMPARE(page.findChild<QLabel *>("displayStatus")->textFormat(), Qt::PlainText); QCOMPARE(page.findChild<QLabel *>("displaySupport")->textFormat(), Qt::PlainText);
        QCOMPARE(backend->previews, 0);
    }
};
QTEST_MAIN(DisplayPageTest)
#include "display_page_test.moc"
