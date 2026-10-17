#include <QtTest>
#include <QDir>
#include <KKeySequenceWidget>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPointer>
#include <QPushButton>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <AeroQt/stylesheet.h>
#include "settings/ShortcutsSettings.h"
#include "settings/CustomShortcutsSettings.h"
#include "settings/ShortcutImportCoordinator.h"
#include "ui/dialogs/ShortcutSchemeDialog.h"
#include "ui/pages/ShortcutsPage.h"

namespace {
const ShortcutActionId launch{QStringLiteral("launcher"), QStringLiteral("default"), QStringLiteral("open")};
const ShortcutActionId window{QStringLiteral("kwin"), QStringLiteral("default"), QStringLiteral("close")};
const ShortcutActionId layoutAction{QStringLiteral("KDE Keyboard Layout Switcher"), QStringLiteral("default"), QStringLiteral("Switch to Next Keyboard Layout")};
const ShortcutActionId perLayoutAction{QStringLiteral("KDE Keyboard Layout Switcher"), QStringLiteral("default"), QStringLiteral("Switch keyboard layout to English (US)")};
const ShortcutActionId copyAction{QStringLiteral("Edit"), QStringLiteral("default"), QStringLiteral("Copy")};
const ShortcutActionId pasteAction{QStringLiteral("Edit"), QStringLiteral("default"), QStringLiteral("Paste")};
const ShortcutActionId openAction{QStringLiteral("File"), QStringLiteral("default"), QStringLiteral("Open")};
QKeySequence key(const char *text) { return QKeySequence(QString::fromLatin1(text), QKeySequence::PortableText); }
ShortcutsSnapshot standardFixture()
{
    ShortcutsSnapshot state; state.available = state.writable = true;
    state.owner = QStringLiteral("standard-shortcuts:test"); state.interfaceSignature = QStringLiteral("standard-bindings-v1");
    state.actions = {
        {copyAction, QStringLiteral("Edit"), QStringLiteral("Default"), QStringLiteral("Copy"), {key("Ctrl+Shift+C")}, {key("Ctrl+C")}, true},
        {pasteAction, QStringLiteral("Edit"), QStringLiteral("Default"), QStringLiteral("Paste"), {key("Ctrl+Alt+V")}, {key("Ctrl+V")}, true},
        {openAction, QStringLiteral("File"), QStringLiteral("Default"), QStringLiteral("Open"), {key("Alt+O"), key("Alt+F2")}, {key("Ctrl+O")}, true}
    };
    state.revision = ShortcutsTypes::revision(state); return state;
}
QTreeWidgetItem *actionItem(QTreeWidget *tree, const ShortcutActionId &id)
{
    QTreeWidgetItemIterator it(tree);
    for (; *it; ++it) if ((*it)->data(0, Qt::UserRole).isValid() && (*it)->data(0, Qt::UserRole).value<ShortcutActionId>() == id) return *it;
    return nullptr;
}
void selectAction(ShortcutsPage &page, const ShortcutActionId &id)
{
    auto *tree = page.findChild<QTreeWidget *>("shortcutsActions");
    auto *item = actionItem(tree, id); QVERIFY(item); tree->setCurrentItem(item);
}
void capture(QWidget &page, const QString &name)
{
    const auto directory = qEnvironmentVariable("LINUX_CONTROL_UI_CAPTURE_DIR"); if (directory.isEmpty()) return;
    QDir().mkpath(directory); page.resize(1100, 900); page.show(); QApplication::processEvents();
    QVERIFY(page.grab().save(directory + QLatin1Char('/') + name + QStringLiteral(".png")));
}
}

class PageShortcutsBackend final : public ShortcutsBackend {
public:
    ShortcutsSnapshot current;
    ShortcutChanges requested;
    quint64 pending = 0;
    int reads = 0, writes = 0;
    bool holdWrite = false;
    PageShortcutsBackend() {
        current.available = current.writable = true; current.owner = QStringLiteral(":1.42"); current.interfaceSignature = QStringLiteral("test");
        current.actions = {
            {launch, QStringLiteral("Application Launcher"), QStringLiteral("Default"), QStringLiteral("Open launcher"), {key("Meta+Space"), key("Alt+F1")}, {key("Meta+L")}, true},
            {window, QStringLiteral("Window Manager"), QStringLiteral("Default"), QStringLiteral("Close window"), {key("Alt+F4"), key("Ctrl+Alt+Q")}, {key("Meta+Q")}, true},
            {layoutAction, QStringLiteral("Keyboard Layout Switcher"), QStringLiteral("Default"), QStringLiteral("Switch to Next Keyboard Layout"), {key("Meta+Alt+K")}, {key("Meta+Alt+N")}, true}
        };
        current.revision = ShortcutsTypes::revision(current);
    }
    void readCurrent(quint64 token) override { ++reads; emit readFinished(token, true, current, {}); }
    void writeChanges(quint64 token, const ShortcutsSnapshot &, const ShortcutChanges &next) override {
        ++writes; pending = token; requested = next; if (!holdWrite) finish();
    }
    void finish() {
        for (auto &action : current.actions) if (requested.contains(action.id)) {
            action.current = requested.value(action.id);
            if (action.layoutAction && !action.current.isEmpty()) action.registered = true;
        }
        current.revision = ShortcutsTypes::revision(current); emit writeFinished(pending, ShortcutsOutcome::Success, current, {});
    }
    void changed() { current.revision = ShortcutsTypes::revision(current); emit externalChange(); }
};

class PageCustomBackend final : public CustomShortcutsBackend {
public:
    int reads = 0, writes = 0;
    void readCurrent(quint64 token) override {
        ++reads; CustomShortcutsSnapshot state; state.available = state.writable = true;
        state.owner = QStringLiteral(":fake-custom-page"); state.interfaceSignature = QStringLiteral("fake-custom-v1");
        state.revision = CustomShortcutsTypes::revision(state); emit readFinished(token, true, state, {});
    }
    void writeChanges(quint64, const CustomShortcutsSnapshot &, const CustomShortcutChanges &) override { ++writes; }
};

class ShortcutsPageTest final : public QObject {
    Q_OBJECT
private slots:
    void focusRegisteredActionDoesNotWriteOrInvoke() {
        auto *backend = new PageShortcutsBackend;
        const ShortcutActionId lock{QStringLiteral("ksmserver"), QStringLiteral("discovered-context"), QStringLiteral("Lock Session")};
        backend->current.actions.append({lock, QStringLiteral("Session"), QStringLiteral("Discovered"), QStringLiteral("Lock"), {key("Meta+L"), key("Alt+L")}, {key("Meta+L")}, true});
        backend->current.revision = ShortcutsTypes::revision(backend->current);
        ShortcutsSettings session(backend); QTRY_VERIFY(!session.isBusy());
        ShortcutsPage page(nullptr, &session, nullptr);
        page.focusExistingAction(lock.component, lock.action);
        auto *tree = page.findChild<QTreeWidget *>("shortcutsActions"); QVERIFY(tree->currentItem());
        QCOMPARE(tree->currentItem()->data(0, Qt::UserRole).value<ShortcutActionId>(), lock);
        QCOMPARE(backend->writes, 0); QVERIFY(!session.isDirty());
        QCOMPARE(session.staged().find(lock)->current.size(), 2);
        page.focusExistingAction(QStringLiteral("missing"), QStringLiteral("not registered"));
        QCOMPARE(backend->writes, 0); QVERIFY(!session.isDirty());
    }

    void initTestCase() {
        if (qEnvironmentVariableIsSet("LINUX_CONTROL_UI_CAPTURE_DIR")) Aero::registerStylesheet(qApp);
    }
    void init() { backend = new PageShortcutsBackend; session = new ShortcutsSettings(backend); QTRY_VERIFY(session->isReady()); }
    void cleanup() { delete session; }
    void hierarchySearchAndLayoutActionsComeFromProvider() {
        ShortcutsPage page(nullptr, session); QCOMPARE(page.settingsSession(), static_cast<SettingsSession *>(session));
        auto *tree = page.findChild<QTreeWidget *>("shortcutsActions"); QVERIFY(tree); QVERIFY(!tree->accessibleName().isEmpty());
        QCOMPARE(tree->topLevelItemCount(), 3); auto *layout = actionItem(tree, layoutAction); QVERIFY(layout);
        QCOMPARE(layout->parent()->text(0), "Default"); QCOMPARE(layout->text(1), key("Meta+Alt+K").toString(QKeySequence::NativeText));
        auto *search = page.findChild<QLineEdit *>("shortcutsSearch"); search->setText("keyboard layout");
        QVERIFY(!layout->isHidden()); QVERIFY(actionItem(tree, launch)->isHidden());
        search->setText(key("Alt+F4").toString(QKeySequence::NativeText)); QVERIFY(!actionItem(tree, window)->isHidden());
        QVERIFY(layout->isHidden()); search->clear(); QVERIFY(!layout->isHidden());
        selectAction(page, layoutAction); QVERIFY(page.findChild<QLabel *>("shortcutsDefaultKeys")->text().contains(key("Meta+Alt+N").toString(QKeySequence::NativeText)));
        QCOMPARE(backend->writes, 0); capture(page, QStringLiteral("shortcuts-actions"));
    }
    void injectedPagesOpenSchemeReviewsWithoutCreatingCompanionProviders() {
        ShortcutsPage page(nullptr, session); QCOMPARE(page.objectName(), QStringLiteral("win7Page"));
        QVERIFY(!page.companionSession()); QVERIFY(!page.customSession()); QVERIFY(!page.importCoordinator());
        page.findChild<QPushButton *>("shortcutsImport")->click();
        QPointer<ShortcutSchemeDialog> dialog = page.findChild<ShortcutSchemeDialog *>(); QVERIFY(dialog);
        QCOMPARE(dialog->globalSession(), session); QVERIFY(!dialog->standardSession());
        dialog->reject(); QTRY_VERIFY(!dialog);
        page.findChild<QPushButton *>("shortcutsExport")->click();
        dialog = page.findChild<ShortcutSchemeDialog *>(); QVERIFY(dialog); QCOMPARE(dialog->globalSession(), session);
        QVERIFY(!dialog->standardSession()); dialog->reject(); QTRY_VERIFY(!dialog);
        QCOMPARE(backend->reads, 1); QCOMPARE(backend->writes, 0);
    }
    void explicitCompanionAndCustomSessionsAreObservedWithoutBeingOwned() {
        auto *standardBackend = new PageShortcutsBackend; standardBackend->current = standardFixture();
        auto *standardSession = new ShortcutsSettings(standardBackend); QTRY_VERIFY(standardSession->isReady());
        auto *customBackend = new PageCustomBackend; auto *customSession = new CustomShortcutsSettings(customBackend); QTRY_VERIFY(customSession->isReady());
        ShortcutsPage page(nullptr, session); page.setCompanionSession(standardSession); page.setCustomSession(customSession);
        QCOMPARE(page.companionSession(), standardSession); QCOMPARE(page.customSession(), customSession);
        QVERIFY(standardSession->selectShortcuts(copyAction, {key("Ctrl+Alt+C")}));
        QVERIFY(customSession->stageAddCommand(QStringLiteral("Example"), QStringLiteral("echo literal"), QStringLiteral("example.desktop")));
        const auto status = page.findChild<QLabel *>("shortcutsStatus")->text();
        QVERIFY(status.contains("Standard application shortcuts have pending")); QVERIFY(status.contains("Custom shortcut definitions have pending"));
        page.findChild<QPushButton *>("shortcutsImport")->click();
        QPointer<ShortcutSchemeDialog> dialog = page.findChild<ShortcutSchemeDialog *>(); QVERIFY(dialog);
        QCOMPARE(dialog->globalSession(), session); QCOMPARE(dialog->standardSession(), standardSession);
        QVERIFY(dialog->loadData("[Custom Commands][new.desktop]\nName=New\nExec=echo new\n"));
        QVERIFY(dialog->findChild<QPushButton *>("schemeStageCommands")->isEnabled());
        dialog->reject(); QTRY_VERIFY(!dialog); QCOMPARE(customBackend->writes, 0); QCOMPARE(standardBackend->writes, 0);
        delete standardSession; QVERIFY(!page.companionSession()); delete customSession; QVERIFY(!page.customSession());
        QVERIFY(!page.findChild<QLabel *>("shortcutsStatus")->text().contains("Custom shortcut definitions have pending"));
        QCOMPARE(backend->writes, 0);
    }
    void retainedImportResumesAcrossPageDestructionAndForwardsNavigation() {
        auto *customBackend = new PageCustomBackend; CustomShortcutsSettings customSession(customBackend); QTRY_VERIFY(customSession.isReady());
        ShortcutImportCoordinator coordinator(session, nullptr, &customSession);
        auto *page = new ShortcutsPage(nullptr, session); page->setImportCoordinator(&coordinator); QCOMPARE(page->importCoordinator(), &coordinator);
        page->findChild<QPushButton *>("shortcutsImport")->click();
        QPointer<ShortcutSchemeDialog> dialog = page->findChild<ShortcutSchemeDialog *>(); QVERIFY(dialog); QCOMPARE(dialog->importCoordinator(), &coordinator);
        const QByteArray source = "[launcher][Global Shortcuts]\nopen=Meta+O\n[Custom Commands][new.desktop]\nName=New command\nExec=echo literal\n";
        QVERIFY(dialog->loadData(source));
        auto *tree = dialog->findChild<QTreeWidget *>("schemeAssignments"); auto *item = actionItem(tree, launch); QVERIFY(item); item->setCheckState(0, Qt::Unchecked);
        QVERIFY(page->findChild<QPushButton *>("shortcutsImport")->text().contains("Resume"));
        QVERIFY(page->findChild<QLabel *>("shortcutsStatus")->text().contains("Retained scheme import"));
        delete page; QVERIFY(!dialog); QCOMPARE(coordinator.sourceBytes(), source);
        page = new ShortcutsPage(nullptr, session); page->setImportCoordinator(&coordinator); page->findChild<QPushButton *>("shortcutsImport")->click();
        dialog = page->findChild<ShortcutSchemeDialog *>(); QVERIFY(dialog); tree = dialog->findChild<QTreeWidget *>("schemeAssignments");
        QCOMPARE(actionItem(tree, launch)->checkState(0), Qt::Unchecked); QVERIFY(!dialog->standardSession());
        QSignalSpy navigation(page, &ShortcutsPage::navigationRequested); bool closedBeforeNavigation = false;
        connect(page, &ShortcutsPage::navigationRequested, page, [&](PageId id) { closedBeforeNavigation = !dialog || !dialog->isVisible(); QCOMPARE(id, PageId::CustomShortcuts); });
        dialog->findChild<QPushButton *>("schemeContinueCustom")->click(); QCOMPARE(navigation.count(), 1); QVERIFY(closedBeforeNavigation); QTRY_VERIFY(!dialog);
        QVERIFY(coordinator.hasDocument()); QVERIFY(!session->isDirty()); QCOMPARE(backend->writes, 0); QCOMPARE(customBackend->writes, 0);
        capture(*page, QStringLiteral("shortcuts-retained-import")); delete page; QCOMPARE(backend->reads, 1); QCOMPARE(customBackend->reads, 1);
    }
    void exportRemainsIndependentOfTheInjectedImportCoordinator() {
        ShortcutImportCoordinator coordinator(session, nullptr, nullptr);
        QVERIFY(coordinator.loadData("[launcher][Global Shortcuts]\nopen=Meta+O\n")); const auto generation = coordinator.generation();
        ShortcutsPage page(nullptr, session); page.setImportCoordinator(&coordinator); page.findChild<QPushButton *>("shortcutsExport")->click();
        QPointer<ShortcutSchemeDialog> dialog = page.findChild<ShortcutSchemeDialog *>(); QVERIFY(dialog); QVERIFY(!dialog->importCoordinator());
        QCOMPARE(dialog->globalSession(), session); QVERIFY(!dialog->standardSession()); dialog->reject(); QTRY_VERIFY(!dialog);
        QCOMPARE(coordinator.generation(), generation); QVERIFY(coordinator.hasDocument()); QVERIFY(!session->isDirty()); QCOMPARE(backend->writes, 0);
    }
    void destroyedImportCoordinatorClearsThePageLinkWithoutCreatingProviders() {
        auto *coordinator = new ShortcutImportCoordinator(session, nullptr, nullptr);
        QVERIFY(coordinator->loadData("[launcher][Global Shortcuts]\nopen=Meta+O\n"));
        ShortcutsPage page(nullptr, session); page.setImportCoordinator(coordinator); QVERIFY(page.findChild<QPushButton *>("shortcutsImport")->text().contains("Resume"));
        delete coordinator; QVERIFY(!page.importCoordinator()); QVERIFY(!page.findChild<QPushButton *>("shortcutsImport")->text().contains("Resume"));
        page.findChild<QPushButton *>("shortcutsImport")->click(); QPointer<ShortcutSchemeDialog> dialog = page.findChild<ShortcutSchemeDialog *>(); QVERIFY(dialog);
        QVERIFY(!dialog->importCoordinator()); QVERIFY(!dialog->standardSession()); dialog->reject(); QTRY_VERIFY(!dialog);
        QCOMPARE(backend->reads, 1); QCOMPARE(backend->writes, 0);
    }
    void addReplaceRemoveAlternativesAndFourChordsAreStaged() {
        ShortcutsPage page(nullptr, session); selectAction(page, launch);
        auto *editor = page.findChild<KKeySequenceWidget *>("shortcutsSequence"); QVERIFY(editor); QVERIFY(editor->multiKeyShortcutsAllowed());
        QVERIFY(!editor->accessibleName().isEmpty());
        const auto chord = key("Ctrl+K, Ctrl+C, Ctrl+U, Ctrl+X"); editor->setKeySequence(chord);
        page.findChild<QPushButton *>("shortcutsAdd")->click();
        QCOMPARE(session->staged().find(launch)->current.size(), 3); QCOMPARE(session->staged().find(launch)->current.last(), chord);
        auto *alternatives = page.findChild<QListWidget *>("shortcutsAlternatives"); alternatives->setCurrentRow(1);
        QCOMPARE(editor->keySequence(), key("Alt+F1")); editor->setKeySequence(key("Meta+O"));
        page.findChild<QPushButton *>("shortcutsReplace")->click();
        QCOMPARE(session->staged().find(launch)->current.at(1), key("Meta+O"));
        alternatives->setCurrentRow(0); page.findChild<QPushButton *>("shortcutsRemove")->click();
        QCOMPARE(session->staged().find(launch)->current, (QList<QKeySequence>{key("Meta+O"), chord}));
        QCOMPARE(backend->writes, 0); QVERIFY(page.findChild<QPushButton *>("shortcutsApply")->isEnabled());
        capture(page, QStringLiteral("shortcuts-sequences"));
        page.findChild<QPushButton *>("shortcutsApply")->click(); QCOMPARE(backend->writes, 1); QVERIFY(!session->isDirty());
    }
    void nativeRecorderCapturesFourChordsAndLocksTheTargetUntilFinished() {
        if (QGuiApplication::platformName() != QLatin1String("offscreen")) QSKIP("Recording tests require offscreen; no live shortcut inhibition is exercised.");
        ShortcutsPage page(nullptr, session); selectAction(page, launch); page.show(); QApplication::processEvents();
        auto *editor = page.findChild<KKeySequenceWidget *>("shortcutsSequence"); QVERIFY(editor);
        QCOMPARE(editor->checkForConflictsAgainst(), KKeySequenceWidget::ShortcutTypes(KKeySequenceWidget::None));
        const auto accepted = KKeySequenceRecorder::ModifierAndKey | KKeySequenceRecorder::Key | KKeySequenceRecorder::Modifier;
        QCOMPARE(editor->patterns(), accepted); QVERIFY(editor->multiKeyShortcutsAllowed());
        QVERIFY(editor->focusProxy()); QVERIFY(!editor->focusProxy()->accessibleName().isEmpty());
        // Start with a real staged change so Apply's recording lock is tested.
        page.findChild<QPushButton *>("shortcutsActionDefaults")->click(); QVERIFY(session->isDirty());
        QSignalSpy recording(editor, &KKeySequenceWidget::recordingChanged);
        editor->captureKeySequence(); QVERIFY(editor->isRecording());
        QVERIFY(!page.findChild<QTreeWidget *>("shortcutsActions")->isEnabled());
        QVERIFY(!page.findChild<QListWidget *>("shortcutsAlternatives")->isEnabled());
        QVERIFY(!page.findChild<QPushButton *>("shortcutsAdd")->isEnabled());
        QVERIFY(!page.findChild<QPushButton *>("shortcutsApply")->isEnabled());
        QVERIFY(!page.findChild<QPushButton *>("shortcutsAllDefaults")->isEnabled());
        QVERIFY(!page.findChild<QPushButton *>("shortcutsRefresh")->isEnabled());
        // QWindow events exercise the SDK recorder; setting keySequence alone
        // would not test capture. Offscreen cannot verify compositor inhibition.
        QTest::keyClick(page.windowHandle(), Qt::Key_E, Qt::MetaModifier);
        QTest::keyClick(page.windowHandle(), Qt::Key_K, Qt::ControlModifier);
        QTest::keyClick(page.windowHandle(), Qt::Key_U, Qt::ControlModifier);
        QTest::keyClick(page.windowHandle(), Qt::Key_F8, Qt::AltModifier);
        QTRY_VERIFY(!editor->isRecording());
        QCOMPARE(editor->keySequence(), key("Meta+E, Ctrl+K, Ctrl+U, Alt+F8"));
        QCOMPARE(recording.count(), 2); QCOMPARE(backend->writes, 0);
        QVERIFY(page.findChild<QTreeWidget *>("shortcutsActions")->isEnabled());
        QVERIFY(page.findChild<QPushButton *>("shortcutsAdd")->isEnabled());
        QVERIFY(page.findChild<QPushButton *>("shortcutsApply")->isEnabled());
        page.findChild<QPushButton *>("shortcutsAdd")->click();
        QCOMPARE(session->staged().find(launch)->current.last(), key("Meta+E, Ctrl+K, Ctrl+U, Alt+F8"));
        QCOMPARE(backend->writes, 0);
    }
    void recordingAnAssignedSequenceOnlyOpensOurStagedConflictWorkflow() {
        if (QGuiApplication::platformName() != QLatin1String("offscreen")) QSKIP("Recording tests require offscreen; no live shortcut inhibition is exercised.");
        ShortcutsPage page(nullptr, session); selectAction(page, launch); page.show(); QApplication::processEvents();
        auto *editor = page.findChild<KKeySequenceWidget *>("shortcutsSequence");
        QSignalSpy stolen(editor, &KKeySequenceWidget::stealShortcut);
        editor->captureKeySequence(); QTest::keyClick(page.windowHandle(), Qt::Key_F4, Qt::AltModifier);
        QTRY_VERIFY(!editor->isRecording()); QCOMPARE(editor->keySequence(), key("Alt+F4"));
        QCOMPARE(stolen.count(), 0); QVERIFY(!session->isDirty()); QCOMPARE(backend->writes, 0);
        page.findChild<QPushButton *>("shortcutsAdd")->click(); QVERIFY(!session->isDirty());
        QVERIFY(page.findChild<QLabel *>("shortcutsConflicts")->text().contains("Close window"));
        page.findChild<QPushButton *>("shortcutsReassign")->click(); QVERIFY(session->isDirty());
        QCOMPARE(session->staged().find(window)->current, (QList<QKeySequence>{key("Ctrl+Alt+Q")}));
        QCOMPARE(backend->writes, 0);
    }
    void recorderAcceptsPlainAndModifierOnlyPatterns() {
        if (QGuiApplication::platformName() != QLatin1String("offscreen")) QSKIP("Recording tests require offscreen; no live shortcut inhibition is exercised.");
        ShortcutsPage page(nullptr, session); selectAction(page, launch); page.show(); QApplication::processEvents();
        auto *editor = page.findChild<KKeySequenceWidget *>("shortcutsSequence");
        editor->captureKeySequence(); QTest::keyClick(page.windowHandle(), Qt::Key_A);
        QTRY_VERIFY(!editor->isRecording()); QCOMPARE(editor->keySequence(), key("A"));
        page.findChild<QPushButton *>("shortcutsAdd")->click(); QVERIFY(session->staged().find(launch)->current.contains(key("A")));
        editor->captureKeySequence();
        QTest::keyPress(page.windowHandle(), Qt::Key_Meta);
        QTest::keyRelease(page.windowHandle(), Qt::Key_Meta);
        QTRY_VERIFY(!editor->isRecording()); QCOMPARE(editor->keySequence(), QKeySequence(Qt::Key_Meta));
        page.findChild<QPushButton *>("shortcutsAdd")->click();
        QVERIFY(session->staged().find(launch)->current.contains(QKeySequence(Qt::Key_Meta))); QCOMPARE(backend->writes, 0);
    }
    void destroyingPageDuringRecordingDestroysRecorderAndLeavesDraftIntact() {
        if (QGuiApplication::platformName() != QLatin1String("offscreen")) QSKIP("Recording tests require offscreen; no live shortcut inhibition is exercised.");
        auto *page = new ShortcutsPage(nullptr, session); selectAction(*page, launch); page->show(); QApplication::processEvents();
        page->findChild<QPushButton *>("shortcutsActionDefaults")->click();
        auto *editor = page->findChild<KKeySequenceWidget *>("shortcutsSequence");
        QPointer<KKeySequenceRecorder> recorder = editor->findChild<KKeySequenceRecorder *>(); QVERIFY(recorder);
        editor->captureKeySequence(); QTest::keyClick(page->windowHandle(), Qt::Key_E, Qt::MetaModifier);
        QVERIFY(editor->isRecording()); delete page; QVERIFY(recorder.isNull());
        QCOMPARE(session->staged().find(launch)->current, (QList<QKeySequence>{key("Meta+L")}));
        QCOMPARE(backend->writes, 0); QVERIFY(session->isDirty());
        ShortcutsPage replacement(nullptr, session); selectAction(replacement, launch); replacement.show(); QApplication::processEvents();
        auto *nextEditor = replacement.findChild<KKeySequenceWidget *>("shortcutsSequence");
        QVERIFY(!nextEditor->isRecording()); nextEditor->captureKeySequence();
        QTest::keyClick(replacement.windowHandle(), Qt::Key_J, Qt::ControlModifier);
        QTRY_VERIFY(!nextEditor->isRecording()); QCOMPARE(nextEditor->keySequence(), key("Ctrl+J"));
        QCOMPARE(session->staged().find(launch)->current, (QList<QKeySequence>{key("Meta+L")}));
        QCOMPARE(backend->writes, 0);
    }
    void stagedAlternativesSurvivePageReconstruction() {
        auto *page = new ShortcutsPage(nullptr, session); selectAction(*page, launch);
        page->findChild<KKeySequenceWidget *>("shortcutsSequence")->setKeySequence(key("Meta+O"));
        page->findChild<QPushButton *>("shortcutsAdd")->click(); delete page;
        page = new ShortcutsPage(nullptr, session); selectAction(*page, launch);
        QCOMPARE(page->findChild<QListWidget *>("shortcutsAlternatives")->count(), 3);
        QVERIFY(page->findChild<QPushButton *>("shortcutsApply")->isEnabled()); QCOMPARE(backend->writes, 0); delete page;
    }
    void conflictingShortcutRequiresExplicitReassignAndPreservesOtherAlternatives() {
        ShortcutsPage page(nullptr, session); selectAction(page, launch);
        page.findChild<KKeySequenceWidget *>("shortcutsSequence")->setKeySequence(key("Alt+F4"));
        page.findChild<QPushButton *>("shortcutsAdd")->click();
        QVERIFY(!session->isDirty()); QCOMPARE(session->staged().find(window)->current.size(), 2);
        auto *conflicts = page.findChild<QLabel *>("shortcutsConflicts"); QVERIFY(conflicts->text().contains("Close window"));
        auto *reassign = page.findChild<QPushButton *>("shortcutsReassign"); QVERIFY(!reassign->isHidden()); QVERIFY(reassign->isEnabled());
        capture(page, QStringLiteral("shortcuts-conflicts")); reassign->click();
        QVERIFY(session->isDirty()); QCOMPARE(session->staged().find(window)->current, (QList<QKeySequence>{key("Ctrl+Alt+Q")}));
        QVERIFY(session->staged().find(launch)->current.contains(key("Alt+F4"))); QVERIFY(reassign->isHidden()); QCOMPARE(backend->writes, 0);
    }
    void conflictProposalCanBeCancelledOrInvalidatedByRefresh() {
        ShortcutsPage page(nullptr, session); selectAction(page, launch);
        auto *editor = page.findChild<KKeySequenceWidget *>("shortcutsSequence"); editor->setKeySequence(key("Alt+F4"));
        page.findChild<QPushButton *>("shortcutsAdd")->click(); page.findChild<QPushButton *>("shortcutsCancelConflict")->click();
        QVERIFY(!session->isDirty()); QVERIFY(page.findChild<QPushButton *>("shortcutsReassign")->isHidden());
        page.findChild<QPushButton *>("shortcutsAdd")->click(); QVERIFY(!page.findChild<QPushButton *>("shortcutsReassign")->isHidden());
        backend->changed(); QVERIFY(page.findChild<QPushButton *>("shortcutsReassign")->isHidden()); QVERIFY(!session->isDirty());
    }
    void selfShadowingAlternativesAreRejectedWithoutSilentRemoval() {
        ShortcutsPage page(nullptr, session); selectAction(page, launch);
        page.findChild<KKeySequenceWidget *>("shortcutsSequence")->setKeySequence(key("Meta+Space, Ctrl+C"));
        page.findChild<QPushButton *>("shortcutsAdd")->click(); QVERIFY(!session->isDirty());
        QVERIFY(page.findChild<QLabel *>("shortcutsStatus")->text().contains("shadow"));
        QCOMPARE(session->staged().find(launch)->current.size(), 2); QCOMPARE(backend->writes, 0);
    }
    void selectedAndAllDefaultsAreStagedWithSelectionPreserved() {
        ShortcutsPage page(nullptr, session); selectAction(page, window);
        page.findChild<QPushButton *>("shortcutsActionDefaults")->click();
        auto *tree = page.findChild<QTreeWidget *>("shortcutsActions");
        QVERIFY(tree->currentItem()->data(0, Qt::UserRole).value<ShortcutActionId>() == window);
        QCOMPARE(session->staged().find(window)->current, (QList<QKeySequence>{key("Meta+Q")}));
        QCOMPARE(session->staged().find(launch)->current.size(), 2);
        page.findChild<QPushButton *>("shortcutsAllDefaults")->click();
        QCOMPARE(session->staged().find(launch)->current, (QList<QKeySequence>{key("Meta+L")})); QCOMPARE(backend->writes, 0);
        page.findChild<QPushButton *>("shortcutsRevert")->click(); QVERIFY(!session->isDirty());
        QVERIFY(tree->currentItem()->data(0, Qt::UserRole).value<ShortcutActionId>() == window);
        QCOMPARE(page.findChild<QListWidget *>("shortcutsAlternatives")->count(), 2);
    }
    void jobSurvivesPageDestructionAndBusyReplacementCannotWrite() {
        backend->holdWrite = true; auto *page = new ShortcutsPage(nullptr, session); selectAction(*page, launch);
        page->findChild<QPushButton *>("shortcutsActionDefaults")->click(); page->findChild<QPushButton *>("shortcutsApply")->click();
        QCOMPARE(backend->writes, 1); QVERIFY(session->isBusy()); delete page;
        ShortcutsPage replacement(nullptr, session); selectAction(replacement, launch);
        QVERIFY(!replacement.findChild<KKeySequenceWidget *>("shortcutsSequence")->isEnabled());
        QVERIFY(!replacement.findChild<QPushButton *>("shortcutsApply")->isEnabled());
        QVERIFY(!replacement.findChild<QPushButton *>("shortcutsRevert")->isEnabled());
        backend->finish(); QVERIFY(!session->isBusy()); QVERIFY(!session->isDirty());
        QCOMPARE(replacement.findChild<QListWidget *>("shortcutsAlternatives")->item(0)->text(), key("Meta+L").toString(QKeySequence::NativeText));
    }
    void unsupportedAndUnavailableActionsExplainReadonlyState() {
        backend->current.actions[1].available = false; backend->changed();
        ShortcutsPage page(nullptr, session); selectAction(page, window);
        QVERIFY(!page.findChild<KKeySequenceWidget *>("shortcutsSequence")->isEnabled());
        QVERIFY(page.findChild<QLabel *>("shortcutsCapability")->text().contains("read-only or unavailable"));
        selectAction(page, launch); page.findChild<KKeySequenceWidget *>("shortcutsSequence")->setKeySequence(key("Alt+F4"));
        page.findChild<QPushButton *>("shortcutsAdd")->click();
        QVERIFY(!page.findChild<QPushButton *>("shortcutsReassign")->isEnabled());
        QVERIFY(page.findChild<QLabel *>("shortcutsConflicts")->text().contains("cannot be reassigned"));
        backend->current.writable = false; backend->current.reason = QStringLiteral("Unsupported shortcut interface"); backend->changed();
        selectAction(page, launch); QVERIFY(!page.findChild<KKeySequenceWidget *>("shortcutsSequence")->isEnabled());
        QVERIFY(page.findChild<QLabel *>("shortcutsStatus")->text().contains("Unsupported shortcut interface"));
        QVERIFY(!page.findChild<QPushButton *>("shortcutsAllDefaults")->isEnabled()); QCOMPARE(backend->writes, 0);
    }
    void failedSaveShowsErrorAndPreservesDraft() {
        backend->holdWrite = true; ShortcutsPage page(nullptr, session); selectAction(page, launch);
        page.findChild<QPushButton *>("shortcutsActionDefaults")->click(); page.findChild<QPushButton *>("shortcutsApply")->click();
        emit backend->writeFinished(backend->pending, ShortcutsOutcome::Failed, backend->current, QStringLiteral("Could not save shortcuts"));
        QVERIFY(session->isDirty()); QVERIFY(page.findChild<QLabel *>("shortcutsStatus")->text().contains("Could not save shortcuts"));
        QVERIFY(!page.findChild<QPushButton *>("shortcutsApply")->isEnabled());
        QCOMPARE(page.findChild<QListWidget *>("shortcutsAlternatives")->item(0)->text(), key("Meta+L").toString(QKeySequence::NativeText));
        QVERIFY(page.findChild<QPushButton *>("shortcutsRevert")->isEnabled());
    }
    void standardScopeLabelsSearchAndSidebarDescribeCommonApplicationActions() {
        auto *standardBackend = new PageShortcutsBackend; standardBackend->current = standardFixture();
        ShortcutsSettings standardSession(standardBackend); QTRY_VERIFY(standardSession.isReady());
        ShortcutsPage page(nullptr, &standardSession, ShortcutsPage::Scope::StandardApplications);
        QCOMPARE(page.scope(), ShortcutsPage::Scope::StandardApplications); QCOMPARE(page.property("shortcutScope").toString(), "standard");
        QCOMPARE(page.settingsSession(), static_cast<SettingsSession *>(&standardSession));
        QCOMPARE(page.findChild<QLabel *>("shortcutsTitle")->text(), "Standard Application Shortcuts");
        const auto introduction = page.findChild<QLabel *>("shortcutsIntroduction")->text();
        QVERIFY(introduction.contains("inside applications")); QVERIFY(introduction.contains("Copy, Open, and Save"));
        QVERIFY(introduction.contains("separately from global")); QVERIFY(introduction.contains("restart"));
        auto *tree = page.findChild<QTreeWidget *>("shortcutsActions");
        QCOMPARE(tree->headerItem()->text(0), "Category / action"); QVERIFY(tree->accessibleName().contains("category"));
        QCOMPARE(tree->topLevelItemCount(), 2); QVERIFY(actionItem(tree, copyAction)); QVERIFY(actionItem(tree, openAction));
        QCOMPARE(actionItem(tree, copyAction)->parent()->text(0), "Edit");
        auto *search = page.findChild<QLineEdit *>("shortcutsSearch"); search->setText("Edit");
        QVERIFY(!actionItem(tree, copyAction)->isHidden()); QVERIFY(!actionItem(tree, pasteAction)->isHidden());
        QVERIFY(actionItem(tree, openAction)->isHidden()); search->setText("Open");
        QVERIFY(!actionItem(tree, openAction)->isHidden()); QVERIFY(actionItem(tree, copyAction)->isHidden());
        search->clear(); selectAction(page, copyAction);
        QVERIFY(page.findChild<QLabel *>("shortcutsStatus")->text().contains("Standard application"));
        const auto standardLinks = ShortcutsPage::sidebarLinks(ShortcutsPage::Scope::StandardApplications);
        QCOMPARE(standardLinks.first().target.page, PageId::Shortcuts);
        const auto globalLinks = ShortcutsPage::sidebarLinks(); QCOMPARE(globalLinks.first().target.page, PageId::StandardShortcuts);
        QCOMPARE(standardLinks.at(1).target.page, PageId::CustomShortcuts); QCOMPARE(globalLinks.at(1).target.page, PageId::CustomShortcuts);
        QCOMPARE(standardBackend->writes, 0); QCOMPARE(backend->writes, 0);
        capture(page, QStringLiteral("shortcuts-standard-actions"));
    }
    void supportedUnregisteredLayoutBindingStagesAndIsCreatedOnlyOnApply() {
        backend->current.layouts.available = true; backend->current.layouts.owner = QStringLiteral(":1.55");
        backend->current.layouts.version = QStringLiteral("6.6.6");
        backend->current.layouts.actionNames = QStringList{perLayoutAction.action};
        ShortcutAction synthetic; synthetic.id = perLayoutAction;
        synthetic.componentFriendly = QStringLiteral("Keyboard Layout Switcher"); synthetic.contextFriendly = QStringLiteral("Default");
        synthetic.registered = false; synthetic.layoutAction = true; synthetic.available = true;
        backend->current.actions.append(synthetic); backend->changed();
        auto *page = new ShortcutsPage(nullptr, session); selectAction(*page, perLayoutAction);
        auto *editor = page->findChild<KKeySequenceWidget *>("shortcutsSequence"); QVERIFY(editor->isEnabled());
        const auto capability = page->findChild<QLabel *>("shortcutsCapability")->text();
        QVERIFY(capability.contains("reloads keyboard-layout shortcuts")); QVERIFY(capability.contains("not been verified"));
        QVERIFY(capability.contains("unassigned binding will be created")); QCOMPARE(backend->writes, 0);
        QCOMPARE(page->findChild<QListWidget *>("shortcutsAlternatives")->count(), 0);
        editor->setKeySequence(key("Meta+Alt+U")); page->findChild<QPushButton *>("shortcutsAdd")->click();
        QVERIFY(session->isDirty()); QVERIFY(!session->staged().find(perLayoutAction)->registered); QCOMPARE(backend->writes, 0);
        delete page; page = new ShortcutsPage(nullptr, session); selectAction(*page, perLayoutAction);
        QCOMPARE(page->findChild<QListWidget *>("shortcutsAlternatives")->item(0)->text(), key("Meta+Alt+U").toString(QKeySequence::NativeText));
        QCOMPARE(backend->writes, 0); page->findChild<QPushButton *>("shortcutsApply")->click();
        QCOMPARE(backend->writes, 1); QVERIFY(!session->isDirty()); QVERIFY(session->applied().find(perLayoutAction)->registered);
        QVERIFY(!page->findChild<QLabel *>("shortcutsCapability")->text().contains("unassigned binding will be created"));
        QVERIFY(page->findChild<QLabel *>("shortcutsCapability")->text().contains("not been verified")); delete page;
    }
    void unavailableLayoutAdapterExplainsSyntheticBindingWithoutMislabelingNextAction() {
        backend->current.layouts.available = false; backend->current.layouts.reason = QStringLiteral("KWin does not expose a verified keyboard-layout adapter.");
        ShortcutAction synthetic; synthetic.id = perLayoutAction;
        synthetic.componentFriendly = QStringLiteral("Keyboard Layout Switcher");
        synthetic.registered = false; synthetic.layoutAction = true; synthetic.available = false;
        backend->current.actions.append(synthetic); backend->changed();
        ShortcutsPage page(nullptr, session); selectAction(page, perLayoutAction);
        const auto capability = page.findChild<QLabel *>("shortcutsCapability")->text();
        QVERIFY(capability.contains(backend->current.layouts.reason)); QVERIFY(capability.contains("cannot be created"));
        QVERIFY(!page.findChild<KKeySequenceWidget *>("shortcutsSequence")->isEnabled());
        QVERIFY(!page.findChild<QPushButton *>("shortcutsAdd")->isEnabled()); QCOMPARE(backend->writes, 0);
        selectAction(page, layoutAction); const auto nextCapability = page.findChild<QLabel *>("shortcutsCapability")->text();
        QVERIFY(!nextCapability.contains("per-layout")); QVERIFY(!nextCapability.contains("reload"));
        QVERIFY(page.findChild<KKeySequenceWidget *>("shortcutsSequence")->isEnabled());
        page.findChild<QPushButton *>("shortcutsActionDefaults")->click(); QVERIFY(session->isDirty()); QCOMPARE(backend->writes, 0);
    }
    void standardAndGlobalSessionsStageAndApplyIndependently() {
        auto *standardBackend = new PageShortcutsBackend; standardBackend->current = standardFixture();
        ShortcutsSettings standardSession(standardBackend); QTRY_VERIFY(standardSession.isReady());
        ShortcutsPage standardPage(nullptr, &standardSession, ShortcutsPage::Scope::StandardApplications);
        ShortcutsPage globalPage(nullptr, session); selectAction(standardPage, copyAction); selectAction(globalPage, launch);
        // Reusing a global binding inside an application does not steal it.
        standardPage.findChild<KKeySequenceWidget *>("shortcutsSequence")->setKeySequence(key("Meta+Space"));
        standardPage.findChild<QPushButton *>("shortcutsAdd")->click();
        QVERIFY(standardSession.isDirty()); QVERIFY(!session->isDirty());
        QVERIFY(standardSession.staged().find(copyAction)->current.contains(key("Meta+Space")));
        QCOMPARE(session->staged().find(launch)->current, (QList<QKeySequence>{key("Meta+Space"), key("Alt+F1")}));
        globalPage.findChild<QPushButton *>("shortcutsActionDefaults")->click(); QVERIFY(session->isDirty());
        standardPage.findChild<QPushButton *>("shortcutsApply")->click(); QCOMPARE(standardBackend->writes, 1);
        QVERIFY(!standardSession.isDirty()); QVERIFY(session->isDirty()); QCOMPARE(backend->writes, 0);
        QVERIFY(standardPage.findChild<QLabel *>("shortcutsStatus")->text().contains("restart"));
        QVERIFY(!standardPage.findChild<QLabel *>("shortcutsStatus")->text().contains("Global shortcuts"));
        globalPage.findChild<QPushButton *>("shortcutsRevert")->click(); QVERIFY(!session->isDirty());
        QVERIFY(standardSession.staged().find(copyAction)->current.contains(key("Meta+Space")));
        QCOMPARE(standardBackend->writes, 1); QCOMPARE(backend->writes, 0);
    }
    void standardConflictsAndSelectedAllDefaultsRemainWithinTheirScope() {
        auto *standardBackend = new PageShortcutsBackend; standardBackend->current = standardFixture();
        ShortcutsSettings standardSession(standardBackend); QTRY_VERIFY(standardSession.isReady());
        ShortcutsPage page(nullptr, &standardSession, ShortcutsPage::Scope::StandardApplications); selectAction(page, copyAction);
        page.findChild<KKeySequenceWidget *>("shortcutsSequence")->setKeySequence(key("Alt+O"));
        page.findChild<QPushButton *>("shortcutsAdd")->click(); QVERIFY(!standardSession.isDirty());
        const auto conflictText = page.findChild<QLabel *>("shortcutsConflicts")->text();
        QVERIFY(conflictText.contains("standard application")); QVERIFY(conflictText.contains("Open"));
        page.findChild<QPushButton *>("shortcutsReassign")->click(); QVERIFY(standardSession.isDirty());
        QCOMPARE(standardSession.staged().find(openAction)->current, (QList<QKeySequence>{key("Alt+F2")}));
        QCOMPARE(standardSession.staged().find(pasteAction)->current, (QList<QKeySequence>{key("Ctrl+Alt+V")}));
        page.findChild<QPushButton *>("shortcutsActionDefaults")->click();
        QCOMPARE(standardSession.staged().find(copyAction)->current, (QList<QKeySequence>{key("Ctrl+C")}));
        QCOMPARE(standardSession.staged().find(pasteAction)->current, (QList<QKeySequence>{key("Ctrl+Alt+V")}));
        page.findChild<QPushButton *>("shortcutsAllDefaults")->click();
        QCOMPARE(standardSession.staged().find(pasteAction)->current, (QList<QKeySequence>{key("Ctrl+V")}));
        QCOMPARE(standardSession.staged().find(openAction)->current, (QList<QKeySequence>{key("Ctrl+O")}));
        QVERIFY(!session->isDirty()); QCOMPARE(backend->writes, 0); QCOMPARE(standardBackend->writes, 0);
        page.findChild<QPushButton *>("shortcutsRevert")->click(); QVERIFY(!standardSession.isDirty());
        QCOMPARE(standardSession.staged().find(openAction)->current.size(), 2);
    }
    void standardDraftAndApplyJobSurviveViewsWithoutOwningTheSession() {
        auto *standardBackend = new PageShortcutsBackend; standardBackend->current = standardFixture(); standardBackend->holdWrite = true;
        standardBackend->current.reason = QStringLiteral("Applications may need to restart to adopt these standard bindings.");
        standardBackend->current.revision = ShortcutsTypes::revision(standardBackend->current);
        QPointer<PageShortcutsBackend> backendLifetime = standardBackend;
        QPointer<ShortcutsSettings> standardSession = new ShortcutsSettings(standardBackend); QTRY_VERIFY(standardSession->isReady());
        auto *page = new ShortcutsPage(nullptr, standardSession, ShortcutsPage::Scope::StandardApplications); selectAction(*page, copyAction);
        page->findChild<QPushButton *>("shortcutsActionDefaults")->click(); delete page;
        QVERIFY(standardSession); QVERIFY(standardSession->isDirty()); QCOMPARE(standardBackend->writes, 0);
        page = new ShortcutsPage(nullptr, standardSession, ShortcutsPage::Scope::StandardApplications); selectAction(*page, copyAction);
        QVERIFY(page->findChild<QLabel *>("shortcutsStatus")->text().contains("pending standard application"));
        QCOMPARE(page->findChild<QListWidget *>("shortcutsAlternatives")->item(0)->text(), key("Ctrl+C").toString(QKeySequence::NativeText));
        page->findChild<QPushButton *>("shortcutsApply")->click(); QCOMPARE(standardBackend->writes, 1); QVERIFY(standardSession->isBusy());
        QVERIFY(page->findChild<QLabel *>("shortcutsStatus")->text().contains("standard application")); delete page;
        QVERIFY(standardSession); QVERIFY(backendLifetime);
        page = new ShortcutsPage(nullptr, standardSession, ShortcutsPage::Scope::StandardApplications); selectAction(*page, copyAction);
        QVERIFY(!page->findChild<QPushButton *>("shortcutsApply")->isEnabled()); standardBackend->finish();
        QVERIFY(!standardSession->isBusy()); QVERIFY(!standardSession->isDirty());
        QVERIFY(page->findChild<QLabel *>("shortcutsStatus")->text().contains("restart"));
        delete page; QVERIFY(standardSession); delete standardSession.data();
        QVERIFY(standardSession.isNull()); QVERIFY(backendLifetime.isNull()); QCOMPARE(backend->writes, 0);
    }
private:
    PageShortcutsBackend *backend = nullptr;
    ShortcutsSettings *session = nullptr;
};
QTEST_MAIN(ShortcutsPageTest)
#include "shortcuts_page_test.moc"
