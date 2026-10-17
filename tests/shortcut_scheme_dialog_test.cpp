#include <QtTest>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileDialog>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <AeroQt/stylesheet.h>
#include "settings/CustomShortcutsSettings.h"
#include "settings/ShortcutsSettings.h"
#include "settings/ShortcutImportCoordinator.h"
#include "ui/dialogs/ShortcutSchemeDialog.h"

namespace {
const ShortcutActionId closeId{QStringLiteral("kwin"), QStringLiteral("default"), QStringLiteral("Close")};
const ShortcutActionId openId{QStringLiteral("launcher"), QStringLiteral("default"), QStringLiteral("Open")};
const ShortcutActionId contextId{QStringLiteral("launcher"), QStringLiteral("editor"), QStringLiteral("Open")};
const ShortcutActionId copyId{QStringLiteral("Edit"), QStringLiteral("default"), QStringLiteral("Copy")};
const ShortcutActionId pasteId{QStringLiteral("Edit"), QStringLiteral("default"), QStringLiteral("Paste")};
const ShortcutActionId targetId{QStringLiteral("existing.desktop"), QStringLiteral("default"), QStringLiteral("_launch")};
const ShortcutActionId importedId{QStringLiteral("imported.desktop"), QStringLiteral("default"), QStringLiteral("_launch")};
QKeySequence key(const char *value) { return QKeySequence(QString::fromLatin1(value), QKeySequence::PortableText); }
ShortcutsSnapshot fixture(bool standard = false)
{
    ShortcutsSnapshot state; state.available = state.writable = true;
    state.owner = standard ? QStringLiteral("standard:test") : QStringLiteral(":fake-global");
    state.interfaceSignature = QStringLiteral("fake-shortcuts-v1");
    if (standard) state.actions = {
        {copyId, QStringLiteral("Edit"), QStringLiteral("Default"), QStringLiteral("Copy"), {key("Ctrl+C")}, {}, true},
        {pasteId, QStringLiteral("Edit"), QStringLiteral("Default"), QStringLiteral("Paste"), {key("Ctrl+V")}, {}, true}
    };
    else state.actions = {
        {closeId, QStringLiteral("Window Manager"), QStringLiteral("Default"), QStringLiteral("Close window"), {key("Alt+F4"), key("Meta+Q")}, {}, true},
        {openId, QStringLiteral("Launcher"), QStringLiteral("Default"), QStringLiteral("Open launcher"), {key("Meta+Space")}, {}, true},
        {contextId, QStringLiteral("Launcher"), QStringLiteral("Editor"), QStringLiteral("Open editor"), {key("Meta+E")}, {}, true},
        {targetId, QStringLiteral("Existing command"), QStringLiteral("Default"), QStringLiteral("Launch command"), {key("Meta+J")}, {}, true}
    };
    state.revision = ShortcutsTypes::revision(state); return state;
}
CustomShortcutsSnapshot customFixture()
{
    CustomShortcutsSnapshot state; state.available = state.writable = true;
    state.owner = QStringLiteral(":fake-global"); state.interfaceSignature = QStringLiteral("fake-custom-v1");
    CustomShortcutEntry entry; entry.component = targetId.component; entry.name = QStringLiteral("Existing command"); entry.exec = QStringLiteral("echo existing");
    entry.command = entry.editable = entry.removable = entry.registered = true; state.entries.append(entry);
    state.contexts.insert(targetId.component, {QStringLiteral("default")});
    state.revision = CustomShortcutsTypes::revision(state); return state;
}
QTreeWidgetItem *binding(ShortcutSchemeDialog &dialog, const ShortcutActionId &id)
{
    QTreeWidgetItemIterator it(dialog.findChild<QTreeWidget *>("schemeAssignments"));
    for (; *it; ++it) if ((*it)->data(0, Qt::UserRole).isValid() && (*it)->data(0, Qt::UserRole).value<ShortcutActionId>() == id) return *it;
    return nullptr;
}
QTreeWidgetItem *command(ShortcutSchemeDialog &dialog, const QString &component)
{
    QTreeWidgetItemIterator it(dialog.findChild<QTreeWidget *>("schemeAssignments"));
    for (; *it; ++it) if ((*it)->data(0, Qt::UserRole + 2).toString() == component) return *it;
    return nullptr;
}
QPushButton *commit(ShortcutSchemeDialog &dialog) { return dialog.findChild<QPushButton *>("schemeCommit"); }
QString report(ShortcutSchemeDialog &dialog) { return dialog.findChild<QLabel *>("schemeReport")->text(); }
QByteArray bytes(const QString &path) { QFile file(path); if (!file.open(QIODevice::ReadOnly)) return {}; return file.readAll(); }
bool writeFile(const QString &path, const QByteArray &data) { QFile file(path); return file.open(QIODevice::WriteOnly) && file.write(data) == data.size(); }
void capture(ShortcutSchemeDialog &dialog, const QString &name)
{
    const auto directory = qEnvironmentVariable("LINUX_CONTROL_UI_CAPTURE_DIR"); if (directory.isEmpty()) return;
    QDir().mkpath(directory); dialog.show(); QApplication::processEvents();
    QVERIFY(dialog.grab().save(directory + QLatin1Char('/') + name + QStringLiteral(".png")));
}
const QByteArray bothScopes = "[kwin][Global Shortcuts]\nClose=Meta+X\n[StandardShortcuts]\nCopy=Ctrl+Shift+C\n";
const QByteArray importedCommand = "[Custom Commands][imported.desktop]\nName=Imported command\nExec=echo imported\n[imported.desktop][Global Shortcuts]\n_launch=Meta+I\n";
}

class SchemeShortcutsBackend final : public ShortcutsBackend {
public:
    explicit SchemeShortcutsBackend(bool standard = false) : current(fixture(standard)) {}
    ShortcutsSnapshot current;
    bool holdRead = false;
    quint64 pendingRead = 0;
    int reads = 0, writes = 0;
    void readCurrent(quint64 token) override { ++reads; pendingRead = token; if (!holdRead) finishRead(); }
    void finishRead() { current.revision = ShortcutsTypes::revision(current); emit readFinished(pendingRead, true, current, {}); }
    void writeChanges(quint64 token, const ShortcutsSnapshot &, const ShortcutChanges &changes) override {
        ++writes;
        for (auto &action : current.actions) if (changes.contains(action.id)) action.current = changes.value(action.id);
        current.revision = ShortcutsTypes::revision(current); emit writeFinished(token, ShortcutsOutcome::Success, current, {});
    }
};
class SchemeCustomBackend final : public CustomShortcutsBackend {
public:
    CustomShortcutsSnapshot current = customFixture();
    int reads = 0, writes = 0;
    void readCurrent(quint64 token) override { ++reads; emit readFinished(token, true, current, {}); }
    void writeChanges(quint64 token, const CustomShortcutsSnapshot &, const CustomShortcutChanges &changes) override {
        ++writes;
        for (const auto &definition : changes.addCommands) {
            CustomShortcutEntry entry; entry.component = definition.component; entry.name = definition.name; entry.exec = definition.exec;
            entry.command = entry.registered = entry.editable = entry.removable = true; current.entries.append(entry);
            current.contexts.insert(definition.component, {QStringLiteral("default")});
            current.actions.append({{definition.component, QStringLiteral("default"), QStringLiteral("_launch")}, definition.name,
                QStringLiteral("Default"), QStringLiteral("Launch command"), {}, {}, true});
        }
        for (auto &entry : current.entries) if (changes.editCommands.contains(entry.component)) {
            const auto definition = changes.editCommands.value(entry.component); entry.name = definition.name; entry.exec = definition.exec;
        }
        current.revision = CustomShortcutsTypes::revision(current); emit writeFinished(token, ShortcutsOutcome::Success, current, {});
    }
};

class ShortcutSchemeDialogTest final : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { if (qEnvironmentVariableIsSet("LINUX_CONTROL_UI_CAPTURE_DIR")) Aero::registerStylesheet(qApp); }
    void init() {
        globalBackend = new SchemeShortcutsBackend; standardBackend = new SchemeShortcutsBackend(true); customBackend = new SchemeCustomBackend;
        globals = new ShortcutsSettings(globalBackend); standards = new ShortcutsSettings(standardBackend); custom = new CustomShortcutsSettings(customBackend);
        QTRY_VERIFY(globals->isReady()); QTRY_VERIFY(standards->isReady()); QTRY_VERIFY(custom->isReady());
    }
    void cleanup() { delete globals; delete standards; delete custom; }
    void bothScopesStageAtomicallyBeforeEitherSignalWithoutWrites() {
        ShortcutSchemeDialog dialog(globals, standards, ShortcutSchemeDialog::Mode::Import);
        QVERIFY(dialog.loadData(bothScopes)); QVERIFY(commit(dialog)->isEnabled());
        QVERIFY(!dialog.findChild<QTreeWidget *>("schemeAssignments")->accessibleName().isEmpty());
        QCOMPARE(binding(dialog, closeId)->text(1), key("Alt+F4").toString(QKeySequence::NativeText) + QStringLiteral("; ") + key("Meta+Q").toString(QKeySequence::NativeText));
        QCOMPARE(binding(dialog, closeId)->text(2), key("Meta+X").toString(QKeySequence::NativeText));
        int observations = 0;
        const auto observer = [&] {
            ++observations; QCOMPARE(globals->staged().find(closeId)->current, QList<QKeySequence>{key("Meta+X")});
            QCOMPARE(standards->staged().find(copyId)->current, QList<QKeySequence>{key("Ctrl+Shift+C")});
        };
        connect(globals, &SettingsSession::stateChanged, &dialog, observer);
        connect(standards, &SettingsSession::stateChanged, &dialog, observer);
        capture(dialog, QStringLiteral("scheme-import-both-scopes"));
        QVERIFY(dialog.stageSelected()); QCOMPARE(observations, 2); QCOMPARE(dialog.result(), int(QDialog::Accepted));
        QVERIFY(globals->isDirty()); QVERIFY(standards->isDirty()); QCOMPARE(globalBackend->writes, 0); QCOMPARE(standardBackend->writes, 0);
    }
    void oneInvalidScopeLeavesBothDraftsUntouchedThenExplicitReassignmentWorks() {
        ShortcutSchemeDialog dialog(globals, standards, ShortcutSchemeDialog::Mode::Import);
        QVERIFY(dialog.loadData("[kwin][Global Shortcuts]\nClose=Meta+X\n[StandardShortcuts]\nCopy=Ctrl+V\n"));
        QSignalSpy globalSignals(globals, &SettingsSession::stateChanged); QSignalSpy standardSignals(standards, &SettingsSession::stateChanged);
        QVERIFY(!dialog.stageSelected()); QVERIFY(!globals->isDirty()); QVERIFY(!standards->isDirty());
        QCOMPARE(globalSignals.count(), 0); QCOMPARE(standardSignals.count(), 0); QVERIFY(report(dialog).contains("Paste"));
        dialog.findChild<QCheckBox *>("schemeReassign")->setChecked(true); QVERIFY(dialog.stageSelected());
        QCOMPARE(standards->staged().find(copyId)->current, QList<QKeySequence>{key("Ctrl+V")});
        QVERIFY(standards->staged().find(pasteId)->current.isEmpty()); QCOMPARE(globalBackend->writes, 0); QCOMPARE(standardBackend->writes, 0);
    }
    void selectedCollisionsNeverStealFromAnotherSelectedAssignment() {
        ShortcutSchemeDialog dialog(globals, standards, ShortcutSchemeDialog::Mode::Import);
        QVERIFY(dialog.loadData("[kwin][Global Shortcuts]\nClose=Meta+Z\n[launcher][Global Shortcuts]\nOpen=Meta+Z\n"));
        dialog.findChild<QCheckBox *>("schemeReassign")->setChecked(true);
        QVERIFY(!dialog.stageSelected()); QVERIFY(!globals->isDirty()); QVERIFY(report(dialog).contains("another selected assignment"));
        binding(dialog, openId)->setCheckState(0, Qt::Unchecked); QVERIFY(dialog.stageSelected());
        QCOMPARE(globals->staged().find(openId)->current, QList<QKeySequence>{key("Meta+Space")});
        QCOMPARE(globals->staged().find(closeId)->current, QList<QKeySequence>{key("Meta+Z")}); QCOMPARE(globalBackend->writes, 0);
    }
    void prefixConflictsRemoveOnlyReviewedAlternative() {
        ShortcutSchemeDialog dialog(globals, standards, ShortcutSchemeDialog::Mode::Import);
        QVERIFY(dialog.loadData("[launcher][Global Shortcuts]\nOpen=Alt+F4, Ctrl+K\n"));
        QVERIFY(!dialog.stageSelected()); QVERIFY(report(dialog).contains("Alt+F4"));
        dialog.findChild<QCheckBox *>("schemeReassign")->setChecked(true); QVERIFY(dialog.stageSelected());
        QCOMPARE(globals->staged().find(closeId)->current, QList<QKeySequence>{key("Meta+Q")}); QCOMPARE(globalBackend->writes, 0);
    }
    void immutableUnknownAndMissingScopesRemainReviewable() {
        globalBackend->current.actions[0].available = false; globals->refresh();
        ShortcutSchemeDialog dialog(globals, nullptr, ShortcutSchemeDialog::Mode::Import);
        QVERIFY(dialog.loadData("[kwin][Global Shortcuts]\nClose=Meta+X\nUnknown=Meta+U\n[unknown][Global Shortcuts]\nMissing=Meta+M\n[StandardShortcuts]\nCopy=Ctrl+Shift+C\n"));
        auto *immutable = binding(dialog, closeId); QVERIFY(immutable); QVERIFY(!(immutable->flags() & Qt::ItemIsUserCheckable));
        QVERIFY(immutable->text(3).contains("Read-only")); QVERIFY(report(dialog).contains("read-only"));
        QVERIFY(binding(dialog, {QStringLiteral("kwin"), QStringLiteral("default"), QStringLiteral("Unknown")}));
        QVERIFY(report(dialog).contains("not registered")); QVERIFY(!dialog.stageSelected()); QVERIFY(!globals->isDirty());
        QCOMPARE(globalBackend->writes, 0); capture(dialog, QStringLiteral("scheme-unresolved"));
    }
    void malformedInputClearsPreviousSelectionAndCannotStage() {
        ShortcutSchemeDialog dialog(globals, standards, ShortcutSchemeDialog::Mode::Import); QVERIFY(dialog.loadData(bothScopes));
        QVERIFY(!dialog.loadData("[kwin][Global Shortcuts]\nClose=Ctrl+NotARealKey\n"));
        QVERIFY(!dialog.errorText().isEmpty()); QVERIFY(!commit(dialog)->isEnabled()); QVERIFY(!binding(dialog, closeId));
        QVERIFY(!dialog.stageSelected()); QVERIFY(!globals->isDirty()); QVERIFY(!standards->isDirty());
    }
    void staleDraftAndRoundTripRevertRequireNewReview() {
        ShortcutSchemeDialog dialog(globals, standards, ShortcutSchemeDialog::Mode::Import); QVERIFY(dialog.loadData(bothScopes));
        QVERIFY(globals->selectShortcuts(openId, {key("Meta+N")})); QVERIFY(!dialog.stageSelected());
        QVERIFY(report(dialog).contains("Update review")); globals->revert(); QVERIFY(!globals->isDirty());
        QVERIFY(!dialog.stageSelected()); dialog.reviewAgain(); QVERIFY(dialog.stageSelected());
        QCOMPARE(globalBackend->writes, 0); QCOMPARE(standardBackend->writes, 0);
    }
    void jobsAndSessionLifetimesInvalidateThePinnedReview() {
        ShortcutSchemeDialog dialog(globals, standards, ShortcutSchemeDialog::Mode::Import); QVERIFY(dialog.loadData(bothScopes));
        globalBackend->holdRead = true; globals->refresh(); QVERIFY(globals->isBusy()); QVERIFY(!dialog.stageSelected());
        dialog.reviewAgain(); QVERIFY(!commit(dialog)->isEnabled()); globalBackend->finishRead(); QVERIFY(!globals->isBusy());
        QVERIFY(!dialog.stageSelected()); dialog.reviewAgain(); QVERIFY(commit(dialog)->isEnabled());
        delete standards; standards = nullptr; standardBackend = nullptr; QVERIFY(!dialog.standardSession());
        QVERIFY(!dialog.stageSelected()); QVERIFY(!globals->isDirty()); dialog.reviewAgain();
        QVERIFY(dialog.stageSelected()); QVERIFY(globals->isDirty()); QCOMPARE(globalBackend->writes, 0);
    }
    void cancellationAndRevertDoNotWrite() {
        { ShortcutSchemeDialog dialog(globals, standards, ShortcutSchemeDialog::Mode::Import); QVERIFY(dialog.loadData(bothScopes)); dialog.reject(); }
        QVERIFY(!globals->isDirty()); QVERIFY(!standards->isDirty());
        { ShortcutSchemeDialog dialog(globals, standards, ShortcutSchemeDialog::Mode::Import); QVERIFY(dialog.loadData(bothScopes)); QVERIFY(dialog.stageSelected()); }
        globals->revert(); standards->revert(); QVERIFY(!globals->isDirty()); QVERIFY(!standards->isDirty());
        QCOMPARE(globalBackend->writes, 0); QCOMPARE(standardBackend->writes, 0);
    }
    void exportUsesSelectedDraftScopesFourChordsAndContextsWithOverwriteGuard() {
        const auto chords = key("Ctrl+K, Ctrl+C, Ctrl+U, Ctrl+X"); QVERIFY(globals->selectShortcuts(contextId, {chords}));
        ShortcutSchemeDialog dialog(globals, standards, ShortcutSchemeDialog::Mode::Export); QTemporaryDir directory;
        auto *tree = dialog.findChild<QTreeWidget *>("schemeAssignments");
        binding(dialog, closeId)->parent()->parent()->setCheckState(0, Qt::Unchecked);
        binding(dialog, contextId)->setCheckState(0, Qt::Checked);
        binding(dialog, copyId)->parent()->parent()->setCheckState(0, Qt::Unchecked);
        QVERIFY(tree->topLevelItemCount() == 2); QVERIFY(report(dialog).contains("versioned extension"));
        const auto path = directory.filePath(QStringLiteral("selection.kksrc")); QVERIFY(writeFile(path, "original contents"));
        QVERIFY(!dialog.exportSelected(path)); QCOMPARE(bytes(path), QByteArray("original contents"));
        dialog.findChild<QLineEdit *>("schemeName")->setText(QStringLiteral("Selected draft")); QVERIFY(dialog.exportSelected(path, true));
        const auto read = ShortcutScheme::readFile(path); QVERIFY2(read.success, qPrintable(read.error));
        QCOMPARE(read.document.name, QStringLiteral("Selected draft")); QCOMPARE(read.document.bindings.size(), 1);
        QCOMPARE(read.document.bindings.first().id, contextId); QCOMPARE(read.document.bindings.first().keys, QList<QKeySequence>{chords});
        QCOMPARE(globalBackend->writes, 0); QCOMPARE(standardBackend->writes, 0); QVERIFY(globals->isDirty());
        capture(dialog, QStringLiteral("scheme-export-selection"));
    }
    void exportRejectsStaleBusyAndDestroyedSources() {
        QTemporaryDir directory; const auto path = directory.filePath(QStringLiteral("stale.kksrc"));
        ShortcutSchemeDialog dialog(globals, standards, ShortcutSchemeDialog::Mode::Export);
        globalBackend->holdRead = true; globals->refresh(); QVERIFY(!dialog.exportSelected(path));
        dialog.reviewAgain(); QVERIFY(!dialog.exportSelected(path)); globalBackend->finishRead(); QVERIFY(!dialog.exportSelected(path));
        dialog.reviewAgain(); delete globals; globals = nullptr; globalBackend = nullptr;
        QVERIFY(!dialog.exportSelected(path)); QVERIFY(!QFileInfo::exists(path));
    }
    void customDefinitionsStageSeparatelyAndRetainThePendingLaunchReview() {
        ShortcutSchemeDialog dialog(globals, standards, ShortcutSchemeDialog::Mode::Import); dialog.setCustomSession(custom);
        QVERIFY(dialog.loadData(importedCommand)); QVERIFY(command(dialog, importedId.component)); QVERIFY(binding(dialog, importedId));
        QVERIFY(!commit(dialog)->isEnabled()); QVERIFY(dialog.findChild<QPushButton *>("schemeStageCommands")->isEnabled());
        QVERIFY(dialog.stageCommandDefinitions()); QVERIFY(custom->isDirty()); QVERIFY(!globals->isDirty()); QVERIFY(!standards->isDirty());
        QCOMPARE(customBackend->writes, 0); QVERIFY(custom->staged().find(importedId.component)->pendingAddition);
        QCOMPARE(dialog.result(), int(QDialog::Rejected)); QVERIFY(binding(dialog, importedId)); QVERIFY(!dialog.stageSelected());
        QVERIFY(report(dialog).contains("Apply on Custom Keyboard Shortcuts")); capture(dialog, QStringLiteral("scheme-custom-pending"));
        custom->apply(); QCOMPARE(customBackend->writes, 1); QVERIFY(!custom->isDirty());
        globalBackend->current.actions.append({importedId, QStringLiteral("Imported command"), QStringLiteral("Default"), QStringLiteral("Launch command"), {}, {}, true});
        globals->refresh(); QVERIFY(!dialog.stageSelected()); dialog.reviewAgain(); QVERIFY(dialog.stageSelected());
        QCOMPARE(globals->staged().find(importedId)->current, QList<QKeySequence>{key("Meta+I")});
        QCOMPARE(globalBackend->writes, 0); QCOMPARE(standardBackend->writes, 0); QVERIFY(command(dialog, importedId.component));
    }
    void definitionSelectionIsPreservedWhenStagingAndCancelKeepsTheDraft() {
        ShortcutSchemeDialog dialog(globals, standards, ShortcutSchemeDialog::Mode::Import); dialog.setCustomSession(custom);
        QVERIFY(dialog.loadData(importedCommand + "[Custom Commands][other.desktop]\nName=Other command\nExec=echo other\n"));
        command(dialog, QStringLiteral("other.desktop"))->setCheckState(0, Qt::Unchecked);
        QVERIFY(dialog.stageCommandDefinitions()); QVERIFY(custom->staged().find(importedId.component)); QVERIFY(!custom->staged().find(QStringLiteral("other.desktop")));
        QCOMPARE(command(dialog, QStringLiteral("other.desktop"))->checkState(0), Qt::Unchecked);
        dialog.reject(); QVERIFY(custom->isDirty()); custom->revert(); QVERIFY(!custom->isDirty()); QCOMPARE(customBackend->writes, 0);
    }
    void selectedCommandDefinitionsStageAsOneDraftTransaction() {
        ShortcutSchemeDialog dialog(globals, standards, ShortcutSchemeDialog::Mode::Import); dialog.setCustomSession(custom);
        QVERIFY(dialog.loadData(importedCommand + "[Custom Commands][other.desktop]\nName=Other command\nExec=echo other\n"));
        int observations = 0;
        connect(custom, &SettingsSession::stateChanged, &dialog, [&] {
            ++observations; QVERIFY(custom->staged().find(importedId.component)); QVERIFY(custom->staged().find(QStringLiteral("other.desktop")));
        });
        QVERIFY(dialog.stageCommandDefinitions()); QCOMPARE(observations, 1); QCOMPARE(customBackend->writes, 0);
        QVERIFY(binding(dialog, importedId)); QVERIFY(!globals->isDirty());
    }
    void mappingIsExplicitAndRequiresAnExistingManageableCommand() {
        ShortcutSchemeDialog dialog(globals, standards, ShortcutSchemeDialog::Mode::Import); dialog.setCustomSession(custom); QVERIFY(dialog.loadData(importedCommand));
        QVERIFY(!dialog.mapCommand(importedId.component, QStringLiteral("missing.desktop")));
        QVERIFY(dialog.mapCommand(importedId.component, targetId.component)); QVERIFY(report(dialog).contains("imported.desktop → existing.desktop"));
        QVERIFY(!binding(dialog, importedId)); QVERIFY(binding(dialog, targetId));
        auto *definition = command(dialog, targetId.component); QVERIFY(definition); QVERIFY(definition->text(1).contains("echo existing")); QVERIFY(definition->text(2).contains("echo imported"));
        QVERIFY(dialog.stageCommandDefinitions()); QCOMPARE(custom->staged().find(targetId.component)->exec, QStringLiteral("echo imported"));
        QVERIFY(!custom->staged().find(importedId.component)); QVERIFY(dialog.stageSelected());
        QCOMPARE(globals->staged().find(targetId)->current, QList<QKeySequence>{key("Meta+I")});
        QCOMPARE(customBackend->writes, 0); QCOMPARE(globalBackend->writes, 0);
    }
    void noCustomSessionRetainsDefinitionsWithoutPretendingToImportThem() {
        ShortcutSchemeDialog dialog(globals, standards, ShortcutSchemeDialog::Mode::Import); QVERIFY(dialog.loadData(importedCommand));
        auto *definition = command(dialog, importedId.component); QVERIFY(definition); QVERIFY(!(definition->flags() & Qt::ItemIsUserCheckable));
        QVERIFY(definition->text(3).contains("not connected")); QVERIFY(!dialog.stageCommandDefinitions()); QVERIFY(!dialog.stageSelected());
        QVERIFY(binding(dialog, importedId)); QCOMPARE(globalBackend->writes, 0);
    }
    void exportDefinitionsAndBindingsHaveIndependentSelections() {
        ShortcutSchemeDialog dialog(globals, standards, ShortcutSchemeDialog::Mode::Export); dialog.setCustomSession(custom); QTemporaryDir directory;
        dialog.findChild<QPushButton *>("schemeSelectNone")->click(); command(dialog, targetId.component)->setCheckState(0, Qt::Checked);
        const auto definitionOnly = directory.filePath(QStringLiteral("definition.kksrc")); QVERIFY(dialog.exportSelected(definitionOnly));
        auto read = ShortcutScheme::readFile(definitionOnly); QVERIFY(read.success); QVERIFY(read.document.bindings.isEmpty()); QCOMPARE(read.document.commands.size(), 1);
        command(dialog, targetId.component)->setCheckState(0, Qt::Unchecked); binding(dialog, targetId)->setCheckState(0, Qt::Checked);
        const auto bindingOnly = directory.filePath(QStringLiteral("binding.kksrc")); QVERIFY(dialog.exportSelected(bindingOnly));
        read = ShortcutScheme::readFile(bindingOnly); QVERIFY(read.success); QCOMPARE(read.document.bindings.size(), 1); QVERIFY(read.document.commands.isEmpty());
        QCOMPARE(customBackend->writes, 0); QCOMPARE(globalBackend->writes, 0);
    }
    void builtinSelectionLoadsOnlyTheRequestedFileAndSupportsScopeChecks() {
        QTemporaryDir directory; const auto first = directory.filePath(QStringLiteral("global.kksrc")); const auto second = directory.filePath(QStringLiteral("both.kksrc"));
        QVERIFY(writeFile(first, "[kwin][Global Shortcuts]\nClose=Meta+X\n")); QVERIFY(writeFile(second, bothScopes));
        ShortcutSchemeDialog dialog(globals, standards, ShortcutSchemeDialog::Mode::Import);
        auto *builtins = dialog.findChild<QComboBox *>("schemeBuiltIns"); QVERIFY(!builtins->isEnabled());
        dialog.setBuiltInSchemes({{QStringLiteral("First"), first, {}}, {QStringLiteral("Both scopes"), second, {}}});
        QCOMPARE(builtins->count(), 2); builtins->setCurrentIndex(1); dialog.findChild<QPushButton *>("schemeReviewBuiltIn")->click();
        QCOMPARE(dialog.findChild<QLineEdit *>("schemePath")->text(), second); QVERIFY(binding(dialog, closeId)); QVERIFY(binding(dialog, copyId));
        binding(dialog, closeId)->parent()->parent()->setCheckState(0, Qt::Unchecked); QVERIFY(dialog.stageSelected());
        QVERIFY(!globals->isDirty()); QVERIFY(standards->isDirty()); QCOMPARE(globalBackend->writes, 0); QCOMPARE(standardBackend->writes, 0);
    }
    void filePickersUseTheStyledQtDialog() {
        ShortcutSchemeDialog importing(globals, standards, ShortcutSchemeDialog::Mode::Import);
        bool sawOpen = false;
        QTimer::singleShot(0, &importing, [&] {
            auto *picker = importing.findChild<QFileDialog *>();
            sawOpen = picker && picker->testOption(QFileDialog::DontUseNativeDialog) && picker->fileMode() == QFileDialog::ExistingFile;
            if (picker) picker->reject();
        });
        importing.findChild<QPushButton *>("schemeBrowse")->click(); QVERIFY(sawOpen);
        ShortcutSchemeDialog exporting(globals, standards, ShortcutSchemeDialog::Mode::Export);
        bool sawSave = false;
        QTimer::singleShot(0, &exporting, [&] {
            auto *picker = exporting.findChild<QFileDialog *>();
            sawSave = picker && picker->testOption(QFileDialog::DontUseNativeDialog) && picker->acceptMode() == QFileDialog::AcceptSave
                && !picker->testOption(QFileDialog::DontConfirmOverwrite);
            if (picker) picker->reject();
        });
        commit(exporting)->click(); QVERIFY(sawSave); QCOMPARE(globalBackend->writes, 0); QCOMPARE(standardBackend->writes, 0);
    }
    void coordinatorReviewRetainsSourceMappingsAndChecksAcrossViews() {
        ShortcutImportCoordinator coordinator(globals, standards, custom); QTemporaryDir directory;
        const auto source = importedCommand + bothScopes;
        const auto path = directory.filePath(QStringLiteral("retained.kksrc")); QVERIFY(writeFile(path, source));
        auto *dialog = new ShortcutSchemeDialog(globals, standards, ShortcutSchemeDialog::Mode::Import); dialog->setImportCoordinator(&coordinator);
        QCOMPARE(dialog->importCoordinator(), &coordinator); QVERIFY(dialog->loadFile(path));
        binding(*dialog, importedId)->setCheckState(0, Qt::Unchecked); command(*dialog, importedId.component)->setCheckState(0, Qt::Unchecked);
        binding(*dialog, copyId)->setCheckState(0, Qt::Unchecked); dialog->findChild<QCheckBox *>("schemeReassign")->setChecked(true);
        QVERIFY(dialog->mapCommand(importedId.component, targetId.component)); const auto hash = coordinator.sourceHash();
        dialog->reject(); delete dialog; QVERIFY(coordinator.hasDocument());
        dialog = new ShortcutSchemeDialog(globals, standards, ShortcutSchemeDialog::Mode::Import); dialog->setImportCoordinator(&coordinator);
        QCOMPARE(dialog->findChild<QLineEdit *>("schemePath")->text(), path); QCOMPARE(coordinator.sourceBytes(), source); QCOMPARE(coordinator.sourceHash(), hash);
        QCOMPARE(coordinator.migrations().value(importedId.component), targetId.component);
        QCOMPARE(binding(*dialog, targetId)->checkState(0), Qt::Unchecked); QCOMPARE(command(*dialog, targetId.component)->checkState(0), Qt::Unchecked);
        QCOMPARE(binding(*dialog, copyId)->checkState(0), Qt::Unchecked); QCOMPARE(binding(*dialog, closeId)->checkState(0), Qt::Checked);
        QVERIFY(dialog->findChild<QCheckBox *>("schemeReassign")->isChecked()); QVERIFY(report(*dialog).contains("imported.desktop → existing.desktop"));
        QCOMPARE(dialog->findChild<QLabel *>("schemeWorkflow")->textFormat(), Qt::PlainText);
        capture(*dialog, QStringLiteral("scheme-persistent-resumed")); delete dialog;
        QCOMPARE(globalBackend->writes, 0); QCOMPARE(standardBackend->writes, 0); QCOMPARE(customBackend->writes, 0);
    }
    void coordinatorUnresolvedRowsRequireExplicitExclusionBeforeAtomicStaging() {
        ShortcutImportCoordinator coordinator(globals, standards, nullptr);
        ShortcutSchemeDialog dialog(globals, standards, ShortcutSchemeDialog::Mode::Import); dialog.setImportCoordinator(&coordinator);
        QVERIFY(dialog.loadData(bothScopes + "[missing][Global Shortcuts]\nUnknown=Meta+U\n"));
        const ShortcutActionId unknown{QStringLiteral("missing"), QStringLiteral("default"), QStringLiteral("Unknown")};
        auto *unresolved = binding(dialog, unknown); QVERIFY(unresolved); QVERIFY(unresolved->flags() & Qt::ItemIsUserCheckable);
        QCOMPARE(unresolved->checkState(0), Qt::Checked); QVERIFY(unresolved->text(3).contains("Uncheck to exclude"));
        QVERIFY(!commit(dialog)->isEnabled()); QVERIFY(!dialog.stageSelected()); QVERIFY(!globals->isDirty()); QVERIFY(!standards->isDirty());
        binding(dialog, unknown)->setCheckState(0, Qt::Unchecked); QVERIFY(commit(dialog)->isEnabled()); QVERIFY(dialog.stageSelected());
        QVERIFY(globals->isDirty()); QVERIFY(standards->isDirty()); QCOMPARE(coordinator.phase(), ShortcutImportCoordinator::Phase::Staged);
        QVERIFY(binding(dialog, unknown)); QCOMPARE(binding(dialog, unknown)->checkState(0), Qt::Unchecked);
        QCOMPARE(globalBackend->writes, 0); QCOMPARE(standardBackend->writes, 0);
    }
    void coordinatorCustomWorkflowSurvivesNavigationAndRequiresFreshBindingReview() {
        ShortcutImportCoordinator coordinator(globals, standards, custom);
        auto *dialog = new ShortcutSchemeDialog(globals, standards, ShortcutSchemeDialog::Mode::Import); dialog->setImportCoordinator(&coordinator);
        QVERIFY(dialog->loadData(importedCommand + bothScopes + "[Custom Commands][other.desktop]\nName=Other\nExec=echo other\n"));
        command(*dialog, QStringLiteral("other.desktop"))->setCheckState(0, Qt::Unchecked); binding(*dialog, copyId)->setCheckState(0, Qt::Unchecked);
        QVERIFY(dialog->stageCommandDefinitions()); QCOMPARE(coordinator.phase(), ShortcutImportCoordinator::Phase::AwaitCustomApply);
        QVERIFY(custom->isDirty()); QVERIFY(!commit(*dialog)->isEnabled());
        capture(*dialog, QStringLiteral("scheme-persistent-custom-apply")); dialog->show();
        QSignalSpy navigation(dialog, &ShortcutSchemeDialog::navigationRequested); bool closedBeforeNavigation = false;
        connect(dialog, &ShortcutSchemeDialog::navigationRequested, dialog, [&](PageId page) { closedBeforeNavigation = !dialog->isVisible(); QCOMPARE(page, PageId::CustomShortcuts); });
        dialog->findChild<QPushButton *>("schemeContinueCustom")->click(); QCOMPARE(navigation.count(), 1); QVERIFY(closedBeforeNavigation);
        delete dialog; QVERIFY(coordinator.hasDocument()); QCOMPARE(customBackend->writes, 0);
        const int readsBeforeApply = globalBackend->reads;
        globalBackend->current.actions.append({importedId, QStringLiteral("Imported command"), QStringLiteral("Default"), QStringLiteral("Launch command"), {}, {}, true});
        custom->apply(); QCOMPARE(customBackend->writes, 1);
        QTRY_COMPARE(coordinator.phase(), ShortcutImportCoordinator::Phase::ReviewBindings); QCOMPARE(globalBackend->reads, readsBeforeApply + 1); QVERIFY(!coordinator.reviewCurrent());
        dialog = new ShortcutSchemeDialog(globals, standards, ShortcutSchemeDialog::Mode::Import); dialog->setImportCoordinator(&coordinator);
        QVERIFY(!commit(*dialog)->isEnabled()); QVERIFY(binding(*dialog, importedId));
        QCOMPARE(command(*dialog, QStringLiteral("other.desktop"))->checkState(0), Qt::Unchecked); QCOMPARE(binding(*dialog, copyId)->checkState(0), Qt::Unchecked);
        QVERIFY(dialog->findChild<QLabel *>("schemeWorkflow")->text().contains("Update review"));
        dialog->reviewAgain(); QVERIFY(commit(*dialog)->isEnabled()); capture(*dialog, QStringLiteral("scheme-persistent-bindings-review"));
        QVERIFY(dialog->stageSelected()); QCOMPARE(globals->staged().find(importedId)->current, QList<QKeySequence>{key("Meta+I")});
        QVERIFY(globals->isDirty()); QVERIFY(!standards->isDirty()); QCOMPARE(globalBackend->writes, 0); QCOMPARE(standardBackend->writes, 0);
        delete dialog; QCOMPARE(coordinator.phase(), ShortcutImportCoordinator::Phase::Staged);
    }
    void coordinatorCloseSuspendsAndDiscardNeverRevertsStagedSettings() {
        ShortcutImportCoordinator coordinator(globals, standards, custom);
        auto *dialog = new ShortcutSchemeDialog(globals, standards, ShortcutSchemeDialog::Mode::Import); dialog->setImportCoordinator(&coordinator); QVERIFY(dialog->loadData(bothScopes));
        QVERIFY(dialog->stageSelected()); dialog->findChild<QPushButton *>("schemeCancel")->click(); delete dialog;
        QVERIFY(coordinator.hasDocument()); QVERIFY(globals->isDirty()); QVERIFY(standards->isDirty());
        dialog = new ShortcutSchemeDialog(globals, standards, ShortcutSchemeDialog::Mode::Import); dialog->setImportCoordinator(&coordinator);
        dialog->findChild<QPushButton *>("schemeDiscardReview")->click(); QVERIFY(!coordinator.hasDocument());
        QVERIFY(globals->isDirty()); QVERIFY(standards->isDirty()); QCOMPARE(dialog->findChild<QTreeWidget *>("schemeAssignments")->topLevelItemCount(), 0);
        QVERIFY(!commit(*dialog)->isEnabled()); delete dialog; QCOMPARE(globalBackend->writes, 0); QCOMPARE(standardBackend->writes, 0);
    }
    void destroyedCoordinatorCannotFallBackToStandaloneStaging() {
        auto *coordinator = new ShortcutImportCoordinator(globals, standards, custom); QVERIFY(coordinator->loadData(bothScopes));
        ShortcutSchemeDialog dialog(globals, standards, ShortcutSchemeDialog::Mode::Import); dialog.setImportCoordinator(coordinator);
        delete coordinator; QVERIFY(!dialog.importCoordinator()); QVERIFY(binding(dialog, closeId));
        QVERIFY(!dialog.findChild<QTreeWidget *>("schemeAssignments")->isEnabled()); QVERIFY(!commit(dialog)->isEnabled());
        QVERIFY(!dialog.stageSelected()); QVERIFY(!dialog.stageCommandDefinitions()); QVERIFY(!globals->isDirty()); QVERIFY(!standards->isDirty());
        QVERIFY(report(dialog).contains("no longer connected")); QCOMPARE(globalBackend->writes, 0);
    }
    void coordinatorStaleErrorSurvivesReopeningWithoutResettingChoices() {
        ShortcutImportCoordinator coordinator(globals, standards, custom);
        auto *dialog = new ShortcutSchemeDialog(globals, standards, ShortcutSchemeDialog::Mode::Import); dialog->setImportCoordinator(&coordinator); QVERIFY(dialog->loadData(bothScopes));
        binding(*dialog, copyId)->setCheckState(0, Qt::Unchecked); QVERIFY(globals->selectShortcuts(openId, {key("Meta+N")}));
        QVERIFY(!dialog->stageSelected()); delete dialog;
        dialog = new ShortcutSchemeDialog(globals, standards, ShortcutSchemeDialog::Mode::Import); dialog->setImportCoordinator(&coordinator);
        QVERIFY(!commit(*dialog)->isEnabled()); QVERIFY(report(*dialog).contains("Update review")); QCOMPARE(binding(*dialog, copyId)->checkState(0), Qt::Unchecked);
        dialog->reviewAgain(); QVERIFY(dialog->stageSelected()); QCOMPARE(globals->staged().find(openId)->current, QList<QKeySequence>{key("Meta+N")});
        QVERIFY(!standards->isDirty()); delete dialog; QCOMPARE(globalBackend->writes, 0);
    }
    void coordinatorRefreshIsReadOnlyAndCannotReplaceAnExistingGlobalDraft() {
        ShortcutImportCoordinator coordinator(globals, standards, nullptr);
        auto *dialog = new ShortcutSchemeDialog(globals, standards, ShortcutSchemeDialog::Mode::Import); dialog->setImportCoordinator(&coordinator); QVERIFY(dialog->loadData(bothScopes));
        QVERIFY(globals->selectShortcuts(openId, {key("Meta+N")})); const int previousReads = globalBackend->reads;
        QVERIFY(!dialog->findChild<QPushButton *>("schemeRefreshBindings")->isEnabled());
        dialog->findChild<QPushButton *>("schemeRefreshBindings")->click(); QCOMPARE(globalBackend->reads, previousReads);
        QCOMPARE(globals->staged().find(openId)->current, QList<QKeySequence>{key("Meta+N")});
        globals->revert(); dialog->reviewAgain(); globalBackend->holdRead = true;
        QVERIFY(dialog->findChild<QPushButton *>("schemeRefreshBindings")->isEnabled()); dialog->findChild<QPushButton *>("schemeRefreshBindings")->click();
        QCOMPARE(coordinator.phase(), ShortcutImportCoordinator::Phase::RefreshingGlobals); delete dialog;
        dialog = new ShortcutSchemeDialog(globals, standards, ShortcutSchemeDialog::Mode::Import); dialog->setImportCoordinator(&coordinator);
        QVERIFY(!commit(*dialog)->isEnabled()); QVERIFY(!dialog->findChild<QPushButton *>("schemeRefreshBindings")->isEnabled());
        globalBackend->finishRead(); QVERIFY(!coordinator.reviewCurrent()); dialog->reviewAgain(); QVERIFY(commit(*dialog)->isEnabled());
        delete dialog; QCOMPARE(globalBackend->writes, 0); QCOMPARE(standardBackend->writes, 0);
    }
    void coordinatorViewsSynchronizeChoicesWithoutRecursiveTreeChanges() {
        ShortcutImportCoordinator coordinator(globals, standards, custom); QVERIFY(coordinator.loadData(bothScopes));
        ShortcutSchemeDialog first(globals, standards, ShortcutSchemeDialog::Mode::Import), second(globals, standards, ShortcutSchemeDialog::Mode::Import);
        first.setImportCoordinator(&coordinator); second.setImportCoordinator(&coordinator);
        binding(first, closeId)->setCheckState(0, Qt::Unchecked); QCOMPARE(binding(second, closeId)->checkState(0), Qt::Unchecked);
        first.findChild<QPushButton *>("schemeSelectNone")->click(); QVERIFY(!coordinator.bindingSelected(ShortcutScheme::Scope::Standard, copyId));
        second.findChild<QPushButton *>("schemeSelectAll")->click(); QCOMPARE(binding(first, closeId)->checkState(0), Qt::Checked); QCOMPARE(binding(first, copyId)->checkState(0), Qt::Checked);
        first.findChild<QCheckBox *>("schemeReassign")->setChecked(true); QVERIFY(second.findChild<QCheckBox *>("schemeReassign")->isChecked());
        QVERIFY(!globals->isDirty()); QVERIFY(!standards->isDirty()); QCOMPARE(globalBackend->writes, 0);
    }
private:
    SchemeShortcutsBackend *globalBackend = nullptr, *standardBackend = nullptr;
    SchemeCustomBackend *customBackend = nullptr;
    ShortcutsSettings *globals = nullptr, *standards = nullptr;
    CustomShortcutsSettings *custom = nullptr;
};
QTEST_MAIN(ShortcutSchemeDialogTest)
#include "shortcut_scheme_dialog_test.moc"
