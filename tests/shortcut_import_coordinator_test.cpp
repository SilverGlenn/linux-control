#include <QtTest>
#include <QCryptographicHash>
#include <QFile>
#include <QPointer>
#include <QTemporaryDir>
#include <functional>
#include "settings/ShortcutImportCoordinator.h"
#include "settings/ShortcutsSettings.h"

namespace {
const ShortcutActionId closeId{QStringLiteral("kwin"), QStringLiteral("default"), QStringLiteral("Close")};
const ShortcutActionId copyId{QStringLiteral("Edit"), QStringLiteral("default"), QStringLiteral("Copy")};
const ShortcutActionId importedId{QStringLiteral("imported.desktop"), QStringLiteral("default"), QStringLiteral("_launch")};
const ShortcutActionId targetId{QStringLiteral("existing.desktop"), QStringLiteral("default"), QStringLiteral("_launch")};
QKeySequence key(const char *text) { return QKeySequence(QString::fromLatin1(text), QKeySequence::PortableText); }
ShortcutAction action(const ShortcutActionId &id, const QList<QKeySequence> &keys = {})
{ ShortcutAction result; result.id = id; result.componentFriendly = id.component; result.friendlyName = id.action; result.current = keys; return result; }
ShortcutsSnapshot shortcutFixture(bool standard)
{
    ShortcutsSnapshot result; result.available = result.writable = true;
    result.owner = standard ? QStringLiteral("standard:test") : QStringLiteral(":1.42"); result.interfaceSignature = QStringLiteral("fixture-global-v2");
    result.actions = standard ? QList<ShortcutAction>{action(copyId, {key("Ctrl+C")})}
        : QList<ShortcutAction>{action(closeId, {key("Alt+F4")}), action(targetId)};
    result.revision = ShortcutsTypes::revision(result); return result;
}
CustomShortcutsSnapshot customFixture()
{
    CustomShortcutsSnapshot result; result.available = result.writable = true; result.owner = QStringLiteral(":1.42");
    result.interfaceSignature = QStringLiteral("fixture-custom-v2");
    CustomShortcutEntry entry; entry.component = targetId.component; entry.name = QStringLiteral("Existing"); entry.exec = QStringLiteral("echo existing");
    entry.path = QStringLiteral("/fixture/existing.desktop"); entry.identity = "existing"; entry.command = entry.registered = entry.editable = entry.removable = true;
    result.entries << entry; result.actions << action(targetId); result.contexts.insert(targetId.component, {QStringLiteral("default")});
    result.revision = CustomShortcutsTypes::revision(result); return result;
}
const QByteArray bindings = "[kwin][Global Shortcuts]\nClose=Meta+X\n[StandardShortcuts]\nCopy=Ctrl+Shift+C\n";
const QByteArray dependency = "[Custom Commands][imported.desktop]\nName=Imported\nExec=echo imported\n[imported.desktop][Global Shortcuts]\n_launch=Meta+I\n";
class GlobalBackend final : public ShortcutsBackend {
public:
    explicit GlobalBackend(bool standard = false) : current(shortcutFixture(standard)) {}
    ShortcutsSnapshot current;
    bool holdRead = false, holdWrite = false;
    quint64 readToken = 0, writeToken = 0; int reads = 0, writes = 0;
    ShortcutChanges requested;
    void readCurrent(quint64 token) override { ++reads; readToken = token; if (!holdRead) finishRead(); }
    void finishRead() { current.revision = ShortcutsTypes::revision(current); emit readFinished(readToken, true, current, {}); }
    void writeChanges(quint64 token, const ShortcutsSnapshot &, const ShortcutChanges &changes) override {
        ++writes; writeToken = token; requested = changes; if (!holdWrite) finishWrite();
    }
    void finishWrite() {
        for (auto &entry : current.actions) if (requested.contains(entry.id)) entry.current = requested.value(entry.id);
        current.revision = ShortcutsTypes::revision(current); emit writeFinished(writeToken, ShortcutsOutcome::Success, current, {});
    }
    void changed() { current.revision = ShortcutsTypes::revision(current); emit externalChange(); }
};
class CustomBackend final : public CustomShortcutsBackend {
public:
    CustomShortcutsSnapshot current = customFixture();
    CustomShortcutChanges requested;
    int reads = 0, writes = 0; quint64 writeToken = 0;
    std::function<void(const CustomShortcutsSnapshot &)> publishRegistration;
    void readCurrent(quint64 token) override { ++reads; current.revision = CustomShortcutsTypes::revision(current); emit readFinished(token, true, current, {}); }
    void writeChanges(quint64 token, const CustomShortcutsSnapshot &, const CustomShortcutChanges &changes) override { ++writes; writeToken = token; requested = changes; }
    void finish(ShortcutsOutcome outcome = ShortcutsOutcome::Success) {
        if (outcome == ShortcutsOutcome::Success) {
            for (const auto &definition : requested.addCommands) {
                CustomShortcutEntry entry; entry.component = definition.component; entry.name = definition.name; entry.exec = definition.exec;
                entry.path = QStringLiteral("/fixture/") + definition.component; entry.identity = "created";
                entry.command = entry.registered = entry.editable = entry.removable = true; current.entries << entry;
                current.actions << action({definition.component, QStringLiteral("default"), QStringLiteral("_launch")});
                current.contexts.insert(definition.component, {QStringLiteral("default")});
            }
            for (auto &entry : current.entries) if (requested.editCommands.contains(entry.component)) {
                const auto value = requested.editCommands.value(entry.component); entry.name = value.name; entry.exec = value.exec;
            }
            current.revision = CustomShortcutsTypes::revision(current);
            if (publishRegistration) publishRegistration(current);
        }
        emit writeFinished(writeToken, outcome, current, outcome == ShortcutsOutcome::Success ? QString{} : QStringLiteral("Injected native failure"));
    }
};
}
class ShortcutImportCoordinatorTest final : public QObject {
    Q_OBJECT
private slots:
    void init() {
        gb = new GlobalBackend; sb = new GlobalBackend(true); cb = new CustomBackend;
        globals = new ShortcutsSettings(gb); standards = new ShortcutsSettings(sb); custom = new CustomShortcutsSettings(cb);
        QTRY_VERIFY(globals->isReady() && standards->isReady() && custom->isReady());
        cb->publishRegistration = [this](const auto &snapshot) {
            for (const auto &entry : snapshot.actions) if (!gb->current.find(entry.id)) gb->current.actions << entry;
            gb->current.revision = ShortcutsTypes::revision(gb->current);
        };
        coordinator = new ShortcutImportCoordinator(globals, standards, custom);
    }
    void cleanup() { delete coordinator; delete globals; delete standards; delete custom; }
    void sourceAndOriginalChoicesSurviveObserverDeathMappingAndFreshReview() {
        const auto source = dependency + bindings;
        QTemporaryDir dir; QVERIFY(dir.isValid()); const auto path = dir.filePath(QStringLiteral("review.kksrc"));
        QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(source), qint64(source.size())); file.close();
        QVERIFY(coordinator->loadFile(path)); QVERIFY(coordinator->review());
        auto *view = new QObject; connect(coordinator, &ShortcutImportCoordinator::changed, view, [] {});
        QVERIFY(coordinator->setCommandSelected(importedId.component, false));
        QVERIFY(coordinator->setBindingSelected(ShortcutScheme::Scope::Global, importedId, false));
        QVERIFY(coordinator->setBindingSelected(ShortcutScheme::Scope::Standard, copyId, false));
        delete view; QVERIFY(file.remove());
        QCOMPARE(coordinator->sourceBytes(), source); QCOMPARE(coordinator->sourcePath(), path);
        QCOMPARE(coordinator->sourceHash(), QCryptographicHash::hash(source, QCryptographicHash::Sha256));
        QVERIFY(coordinator->mapCommand(importedId.component, targetId.component)); QVERIFY(coordinator->review());
        QVERIFY(!coordinator->commandSelected(targetId.component));
        QVERIFY(!coordinator->bindingSelected(ShortcutScheme::Scope::Global, targetId));
        QVERIFY(!coordinator->bindingSelected(ShortcutScheme::Scope::Standard, copyId));
        globals->refresh(); QVERIFY(coordinator->review());
        QVERIFY(!coordinator->commandSelected(targetId.component)); QVERIFY(!coordinator->bindingSelected(ShortcutScheme::Scope::Global, targetId));
        QCOMPARE(cb->writes + gb->writes + sb->writes, 0);
    }
    void verifiedCustomApplyRefreshesReadonlyThenRequiresFreshReviewAndStagesBothScopesAtomically() {
        QVERIFY(coordinator->loadData(dependency + bindings)); QVERIFY(coordinator->review());
        QVERIFY(coordinator->stageDefinitions()); QVERIFY(custom->isDirty()); QCOMPARE(cb->writes, 0);
        const int reads = gb->reads; custom->apply(); QCOMPARE(cb->writes, 1); cb->finish();
        QTRY_VERIFY(gb->reads > reads); QTRY_VERIFY(!globals->isBusy());
        QVERIFY(!coordinator->reviewCurrent()); QVERIFY(!coordinator->stageBindings());
        QVERIFY(coordinator->review()); QVERIFY(coordinator->canStageBindings());
        bool sawSignal = false, bothReadyAtSignal = true;
        connect(globals, &SettingsSession::stateChanged, this, [&] {
            sawSignal = true;
            bothReadyAtSignal &= standards->staged().find(copyId)->current == QList<QKeySequence>{key("Ctrl+Shift+C")};
        });
        QVERIFY(coordinator->stageBindings()); QVERIFY(sawSignal); QVERIFY(bothReadyAtSignal);
        QCOMPARE(globals->staged().find(importedId)->current, QList<QKeySequence>{key("Meta+I")});
        QCOMPARE(globals->staged().find(closeId)->current, QList<QKeySequence>{key("Meta+X")});
        QCOMPARE(standards->staged().find(copyId)->current, QList<QKeySequence>{key("Ctrl+Shift+C")});
        QCOMPARE(gb->writes, 0); QCOMPARE(sb->writes, 0); QCOMPARE(cb->writes, 1);
    }
    void dirtyGlobalsDeferDependencyRefreshWithoutLosingDraft() {
        QVERIFY(coordinator->loadData(dependency)); QVERIFY(coordinator->review()); QVERIFY(coordinator->stageDefinitions());
        QVERIFY(globals->selectShortcuts(closeId, {key("Meta+D")}));
        const auto draft = ShortcutsTypes::revision(globals->staged()); const int reads = gb->reads;
        custom->apply(); cb->finish(); QCoreApplication::processEvents();
        QCOMPARE(gb->reads, reads); QCOMPARE(ShortcutsTypes::revision(globals->staged()), draft);
        QVERIFY(!coordinator->canRefreshGlobals()); QVERIFY(!coordinator->refreshGlobals());
        globals->revert(); QTRY_VERIFY(gb->reads >= reads + 2); QTRY_VERIFY(!globals->isBusy());
        QTRY_VERIFY(coordinator->phase() == ShortcutImportCoordinator::Phase::ReviewBindings);
        QVERIFY(coordinator->review()); QVERIFY(coordinator->stageBindings()); QCOMPARE(gb->writes, 0);
    }
    void busyGlobalsCannotRefreshOrBeOverwritten() {
        QVERIFY(coordinator->loadData(dependency)); QVERIFY(coordinator->review()); QVERIFY(coordinator->stageDefinitions());
        gb->holdRead = true; globals->refresh(); QVERIFY(globals->isBusy()); const int reads = gb->reads;
        custom->apply(); cb->finish(); QCoreApplication::processEvents();
        QVERIFY(!coordinator->canRefreshGlobals()); QVERIFY(!coordinator->refreshGlobals()); QCOMPARE(gb->reads, reads);
        gb->holdRead = false; gb->finishRead(); QTRY_VERIFY(gb->reads >= reads + 1); QTRY_VERIFY(!globals->isBusy());
        QTRY_VERIFY(coordinator->phase() == ShortcutImportCoordinator::Phase::ReviewBindings);
        QVERIFY(coordinator->review()); QCOMPARE(gb->writes, 0); QCOMPARE(sb->writes, 0);
    }
    void conflictedGlobalsRetainTheirDraftUntilExplicitResolution() {
        QVERIFY(coordinator->loadData(dependency)); QVERIFY(coordinator->review()); QVERIFY(coordinator->stageDefinitions());
        QVERIFY(globals->selectShortcuts(closeId, {key("Meta+D")}));
        gb->current.actions.first().current = {key("Meta+F12")}; gb->changed(); QVERIFY(globals->hasConflict());
        const auto draft = ShortcutsTypes::revision(globals->staged()); const int reads = gb->reads;
        custom->apply(); cb->finish(); QCoreApplication::processEvents();
        QVERIFY(!coordinator->refreshGlobals()); QCOMPARE(gb->reads, reads); QCOMPARE(ShortcutsTypes::revision(globals->staged()), draft);
        QVERIFY(globals->hasConflict()); QVERIFY(globals->isDirty()); QCOMPARE(gb->writes, 0);
    }
    void unsuccessfulCustomApplyNeverAdvancesDependencies_data() {
        QTest::addColumn<int>("outcome");
        QTest::newRow("failed") << int(ShortcutsOutcome::Failed); QTest::newRow("partial") << int(ShortcutsOutcome::Partial);
        QTest::newRow("uncertain") << int(ShortcutsOutcome::Uncertain);
    }
    void unsuccessfulCustomApplyNeverAdvancesDependencies() {
        QFETCH(int, outcome); QVERIFY(coordinator->loadData(dependency)); QVERIFY(coordinator->review()); QVERIFY(coordinator->stageDefinitions());
        const int reads = gb->reads; custom->apply(); cb->finish(ShortcutsOutcome(outcome)); QCoreApplication::processEvents();
        QCOMPARE(gb->reads, reads); QVERIFY(!coordinator->canStageBindings()); QVERIFY(coordinator->hasDocument());
        QVERIFY(custom->isDirty()); QCOMPARE(gb->writes + sb->writes, 0);
    }
    void unrelatedSuccessfulCustomApplyDoesNotSatisfyImportedDefinitions() {
        QVERIFY(coordinator->loadData(dependency)); QVERIFY(coordinator->review());
        QVERIFY(custom->stageAddCommand(QStringLiteral("Unrelated"), QStringLiteral("echo unrelated"), QStringLiteral("unrelated.desktop")));
        const int reads = gb->reads; custom->apply(); cb->finish(); QCoreApplication::processEvents();
        QCOMPARE(gb->reads, reads); QVERIFY(!coordinator->canStageBindings()); QVERIFY(!custom->applied().find(importedId.component));
    }
    void nativeOwnerDriftBlocksButRetainsSource() {
        QVERIFY(coordinator->loadData(dependency)); QVERIFY(coordinator->review()); QVERIFY(coordinator->stageDefinitions());
        custom->apply(); cb->finish(); QTRY_VERIFY(gb->reads > 1);
        gb->current.owner = QStringLiteral(":1.99"); gb->changed();
        QVERIFY(coordinator->review()); QVERIFY(!coordinator->canStageBindings()); QVERIFY(!coordinator->stageBindings());
        QCOMPARE(coordinator->sourceBytes(), dependency); QVERIFY(coordinator->hasDocument()); QCOMPARE(gb->writes + sb->writes, 0);
    }
    void completedImportBecomesPendingUntilBothNativeSnapshotsShareTheNewOwner() {
        QVERIFY(coordinator->loadData(dependency)); QVERIFY(coordinator->review()); QVERIFY(coordinator->stageDefinitions());
        custom->apply(); cb->finish();
        QTRY_VERIFY(coordinator->phase() == ShortcutImportCoordinator::Phase::ReviewBindings);
        QVERIFY(coordinator->review()); QVERIFY(coordinator->stageBindings());
        QCOMPARE(gb->writes, 0); globals->apply(); QCOMPARE(gb->writes, 1);
        QVERIFY(!globals->isDirty()); QVERIFY(coordinator->phase() == ShortcutImportCoordinator::Phase::Complete);
        QVERIFY(!coordinator->hasPendingWork());
        const auto importedKeys = globals->applied().find(importedId)->current;
        gb->current.owner = QStringLiteral(":1.99"); gb->changed();
        QCOMPARE(globals->applied().find(importedId)->current, importedKeys);
        QCOMPARE(custom->applied().owner, QStringLiteral(":1.42"));
        QVERIFY(coordinator->phase() != ShortcutImportCoordinator::Phase::Complete);
        QVERIFY(coordinator->hasPendingWork()); QCOMPARE(coordinator->sourceBytes(), dependency);
        cb->current.owner = QStringLiteral(":1.99"); custom->refresh();
        QCOMPARE(custom->applied().owner, globals->applied().owner);
        QVERIFY(coordinator->phase() == ShortcutImportCoordinator::Phase::Complete);
        QVERIFY(!coordinator->hasPendingWork()); QCOMPARE(gb->writes, 1); QCOMPARE(cb->writes, 1); QCOMPARE(sb->writes, 0);
    }
    void destroyedRequiredSessionBlocksButRetainsSource() {
        QVERIFY(coordinator->loadData(bindings)); QVERIFY(coordinator->review()); delete standards; standards = nullptr; sb = nullptr;
        QVERIFY(!coordinator->canStageBindings()); QVERIFY(!coordinator->stageBindings());
        QCOMPARE(coordinator->sourceBytes(), bindings); QVERIFY(coordinator->hasDocument());
    }
    void firstDraftObserverCanDestroyTheOtherSessionDuringAtomicStaging() {
        QVERIFY(coordinator->loadData(bindings)); QVERIFY(coordinator->review());
        QPointer<ShortcutsSettings> guard(standards); bool observed = false, sawCompleteDraft = false;
        connect(globals, &SettingsSession::stateChanged, this, [&] {
            if (!standards) return;
            observed = true;
            sawCompleteDraft = standards->staged().find(copyId)->current == QList<QKeySequence>{key("Ctrl+Shift+C")};
            QCOMPARE(sb->writes, 0); delete standards; standards = nullptr; sb = nullptr;
        });
        QVERIFY(coordinator->stageBindings()); QVERIFY(observed); QVERIFY(sawCompleteDraft); QVERIFY(guard.isNull());
        QCOMPARE(globals->staged().find(closeId)->current, QList<QKeySequence>{key("Meta+X")});
        QVERIFY(!coordinator->canStageBindings()); QCOMPARE(coordinator->sourceBytes(), bindings); QCOMPARE(gb->writes, 0);
    }
    void customSessionLossAfterVerifiedApplyRetainsPendingBindings() {
        QVERIFY(coordinator->loadData(dependency)); QVERIFY(coordinator->review()); QVERIFY(coordinator->stageDefinitions());
        custom->apply(); cb->finish(); QTRY_VERIFY(gb->reads > 1); QTRY_VERIFY(!globals->isBusy());
        QVERIFY(coordinator->review()); QVERIFY(coordinator->canStageBindings());
        delete custom; custom = nullptr; cb = nullptr;
        QVERIFY(!coordinator->canStageDefinitions()); QVERIFY(!coordinator->canStageBindings()); QVERIFY(!coordinator->stageBindings());
        QCOMPARE(coordinator->sourceBytes(), dependency); QVERIFY(coordinator->hasDocument());
        QCOMPARE(gb->writes + sb->writes, 0);
    }
    void customApplyFinishedAfterReplacementCannotAdvanceTheNewReview() {
        QVERIFY(coordinator->loadData(dependency)); QVERIFY(coordinator->review()); QVERIFY(coordinator->stageDefinitions());
        custom->apply(); const int reads = gb->reads; QVERIFY(custom->isBusy());
        QVERIFY(coordinator->loadData(bindings)); cb->finish(); QCoreApplication::processEvents();
        QCOMPARE(gb->reads, reads); QCOMPARE(coordinator->sourceBytes(), bindings);
        QVERIFY(coordinator->review()); QVERIFY(coordinator->stageBindings());
        QCOMPARE(gb->writes + sb->writes, 0);
    }
    void queuedDependencyCallbacksCannotRefreshReplacementOrDiscardedReview_data() {
        QTest::addColumn<bool>("replace"); QTest::newRow("replacement") << true; QTest::newRow("discard") << false;
    }
    void queuedDependencyCallbacksCannotRefreshReplacementOrDiscardedReview() {
        QFETCH(bool, replace); QVERIFY(coordinator->loadData(dependency)); QVERIFY(coordinator->review()); QVERIFY(coordinator->stageDefinitions());
        custom->apply(); cb->finish(); const int reads = gb->reads; const auto generation = coordinator->generation();
        if (replace) { QVERIFY(coordinator->loadData(bindings)); QVERIFY(coordinator->review()); } else coordinator->discard();
        QVERIFY(coordinator->generation() != generation); QCoreApplication::processEvents(); QCOMPARE(gb->reads, reads);
        if (replace) { QCOMPARE(coordinator->sourceBytes(), bindings); QVERIFY(coordinator->reviewCurrent()); }
        else { QVERIFY(!coordinator->hasDocument()); QVERIFY(!coordinator->hasPendingWork()); }
        QVERIFY(!globals->isDirty()); QVERIFY(!standards->isDirty()); QCOMPARE(gb->writes + sb->writes, 0);
    }
    void invalidReplacementPreservesOldSourceAndRequiresNewReview() {
        QVERIFY(coordinator->loadData(bindings, QStringLiteral("/fixture/valid.kksrc"))); QVERIFY(coordinator->review());
        const auto hash = coordinator->sourceHash(); QVERIFY(!coordinator->loadData("[invalid", QStringLiteral("/fixture/bad.kksrc")));
        QCOMPARE(coordinator->sourceBytes(), bindings); QCOMPARE(coordinator->sourceHash(), hash);
        QCOMPARE(coordinator->sourcePath(), QStringLiteral("/fixture/valid.kksrc")); QVERIFY(!coordinator->reviewCurrent());
        QVERIFY(!coordinator->stageBindings()); QVERIFY(coordinator->review()); QVERIFY(coordinator->stageBindings()); QCOMPARE(gb->writes + sb->writes, 0);
    }
    void selectedUnresolvedBindingsRefuseWholeStageButUncheckedUnknownIsAllowed() {
        const ShortcutActionId unknown{QStringLiteral("unknown"), QStringLiteral("default"), QStringLiteral("NoSuchAction")};
        QVERIFY(coordinator->loadData(bindings + "[unknown][Global Shortcuts]\nNoSuchAction=Meta+U\n")); QVERIFY(coordinator->review());
        QVERIFY(coordinator->setBindingSelected(ShortcutScheme::Scope::Global, unknown, true));
        const auto global = ShortcutsTypes::revision(globals->staged()), standard = ShortcutsTypes::revision(standards->staged());
        QVERIFY(!coordinator->stageBindings()); QCOMPARE(ShortcutsTypes::revision(globals->staged()), global); QCOMPARE(ShortcutsTypes::revision(standards->staged()), standard);
        QVERIFY(coordinator->setBindingSelected(ShortcutScheme::Scope::Global, unknown, false)); QVERIFY(coordinator->stageBindings());
        QVERIFY(globals->isDirty() && standards->isDirty()); QCOMPARE(gb->writes + sb->writes, 0);
    }
    void discardNeverRevertsAnySettingsDraft() {
        QVERIFY(coordinator->loadData(dependency + bindings)); QVERIFY(coordinator->review()); QVERIFY(coordinator->stageDefinitions());
        QVERIFY(globals->selectShortcuts(closeId, {key("Meta+D")})); QVERIFY(standards->selectShortcuts(copyId, {key("Ctrl+Alt+C")}));
        const auto global = ShortcutsTypes::revision(globals->staged()), standard = ShortcutsTypes::revision(standards->staged());
        const auto definitions = CustomShortcutsTypes::revision(custom->staged()); const int reads = gb->reads + sb->reads + cb->reads;
        coordinator->discard(); QCoreApplication::processEvents(); QVERIFY(!coordinator->hasDocument());
        QCOMPARE(ShortcutsTypes::revision(globals->staged()), global); QCOMPARE(ShortcutsTypes::revision(standards->staged()), standard);
        QCOMPARE(CustomShortcutsTypes::revision(custom->staged()), definitions); QCOMPARE(gb->reads + sb->reads + cb->reads, reads);
        QCOMPARE(gb->writes + sb->writes + cb->writes, 0);
    }
private:
    GlobalBackend *gb = nullptr, *sb = nullptr; CustomBackend *cb = nullptr;
    ShortcutsSettings *globals = nullptr, *standards = nullptr; CustomShortcutsSettings *custom = nullptr;
    ShortcutImportCoordinator *coordinator = nullptr;
};
QTEST_GUILESS_MAIN(ShortcutImportCoordinatorTest)
#include "shortcut_import_coordinator_test.moc"
