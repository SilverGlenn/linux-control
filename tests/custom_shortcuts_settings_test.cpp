#include <QtTest>
#include <QPointer>
#include <KShell>
#include <algorithm>
#include "settings/CustomShortcutsSettings.h"

namespace {
ShortcutAction launch(const QString &component)
{
    ShortcutAction action; action.id = {component, QStringLiteral("default"), QStringLiteral("_launch")};
    action.componentFriendly = component; action.friendlyName = component; return action;
}
CustomShortcutsSnapshot fixture()
{
    CustomShortcutsSnapshot state; state.owner = QStringLiteral(":1.42"); state.interfaceSignature = QStringLiteral("audited-test");
    state.available = state.writable = true;
    CustomShortcutApplication app; app.id = QStringLiteral("vendor.unlisted.desktop"); app.name = QStringLiteral("Vendor application");
    app.path = QStringLiteral("/fixture/vendor.unlisted.desktop"); app.exec = QStringLiteral("vendor --open"); app.identity = "app"; app.defaults = {launch(app.id)};
    state.applications << app;
    CustomShortcutEntry command; command.component = QStringLiteral("net.local.existing.desktop"); command.name = QStringLiteral("Existing command");
    command.exec = QStringLiteral("printf %%s old"); command.path = QStringLiteral("/fixture/net.local.existing.desktop"); command.identity = "command";
    command.command = command.registered = command.editable = command.removable = true;
    state.entries << command; state.actions << launch(command.component);
    state.contexts.insert(command.component, {QStringLiteral("default")});
    state.revision = CustomShortcutsTypes::revision(state); return state;
}
CustomShortcutEntry definition(const QString &component, const QString &name = QStringLiteral("New command"), const QString &exec = QStringLiteral("printf %s new"))
{ CustomShortcutEntry result; result.component = component; result.name = name; result.exec = exec; return result; }
}
class FakeCustomBackend final : public CustomShortcutsBackend {
public:
    CustomShortcutsSnapshot current = fixture(), expected;
    CustomShortcutChanges requested;
    bool automaticRead = true, automaticWrite = true, readSuccess = true;
    quint64 readToken = 0, writeToken = 0;
    int reads = 0, writes = 0;
    void readCurrent(quint64 token) override {
        ++reads; readToken = token;
        if (automaticRead) emit readFinished(token, readSuccess, current, readSuccess ? QString{} : QStringLiteral("Read failed"));
    }
    void writeChanges(quint64 token, const CustomShortcutsSnapshot &baseline, const CustomShortcutChanges &changes) override {
        ++writes; writeToken = token; expected = baseline; requested = changes;
        if (automaticWrite) finish();
    }
    void finish() {
        for (const auto &id : requested.addApplications) {
            const auto *app = current.application(id); if (!app) continue;
            auto existing = std::find_if(current.entries.begin(), current.entries.end(), [&](const auto &entry) { return entry.component == id; });
            if (existing != current.entries.end()) existing->registered = true;
            else {
                CustomShortcutEntry entry; entry.component = app->id; entry.name = app->name; entry.exec = app->exec; entry.path = app->path;
                entry.identity = app->identity; entry.registered = entry.removable = true; current.entries << entry;
            }
            current.actions << app->defaults; current.contexts.insert(id, {QStringLiteral("default")});
        }
        for (const auto &command : requested.addCommands) {
            auto entry = definition(command.component, command.name, command.exec); entry.path = QStringLiteral("/fixture/") + command.component; entry.identity = "new";
            entry.command = entry.registered = entry.editable = entry.removable = true; current.entries << entry; current.actions << launch(command.component);
            current.contexts.insert(command.component, {QStringLiteral("default")});
        }
        for (const auto &command : requested.editCommands) for (auto &entry : current.entries) if (entry.component == command.component) {
            entry.name = command.name; entry.exec = command.exec; entry.identity = "edited";
        }
        for (const auto &id : requested.removeComponents) {
            current.entries.erase(std::remove_if(current.entries.begin(), current.entries.end(), [&](const auto &entry) { return entry.component == id; }), current.entries.end());
            current.actions.erase(std::remove_if(current.actions.begin(), current.actions.end(), [&](const auto &entry) { return entry.id.component == id; }), current.actions.end());
            current.contexts.remove(id);
        }
        current.revision = CustomShortcutsTypes::revision(current); report(ShortcutsOutcome::Success, current);
    }
    void report(ShortcutsOutcome outcome, const CustomShortcutsSnapshot &state, const QString &error = {})
    { emit writeFinished(writeToken, outcome, state, error); }
    void changed() { current.revision = CustomShortcutsTypes::revision(current); emit externalChange(); }
};
class CustomShortcutsSettingsTest final : public QObject {
    Q_OBJECT
private slots:
    void init() { backend = new FakeCustomBackend; session = new CustomShortcutsSettings(backend); QTRY_VERIFY(session->isReady()); }
    void cleanup() { delete session; }
    void applicationCreationIsStagedAndVerified() {
        QVERIFY(session->stageAddApplication(QStringLiteral("vendor.unlisted.desktop"))); QCOMPARE(backend->writes, 0); QVERIFY(session->isDirty());
        QVERIFY(session->staged().find(QStringLiteral("vendor.unlisted.desktop"))->pendingAddition);
        QSignalSpy result(session, &SettingsSession::applyFinished); session->apply(); QCOMPARE(backend->writes, 1);
        QVERIFY(!session->isDirty()); QVERIFY(session->applied().find(QStringLiteral("vendor.unlisted.desktop"))->registered);
        QCOMPARE(result.count(), 1); QVERIFY(result.takeFirst()[0].toBool());
    }
    void commandCreateEditRemoveAndRestoreRemainDrafts() {
        const auto id = QStringLiteral("net.local.new.desktop");
        QVERIFY(session->stageAddCommand(QStringLiteral("New"), QStringLiteral("printf %s new"), id));
        QCOMPARE(session->staged().find(id)->exec, QStringLiteral("printf %%s new"));
        QVERIFY(session->stageEditCommand(id, QStringLiteral("Changed"), QStringLiteral("echo changed")));
        QVERIFY(session->stageRemove(id)); QVERIFY(!session->staged().find(id)); QVERIFY(!session->isDirty());
        const auto old = backend->current.entries.first().component;
        QVERIFY(session->stageEditCommand(old, QStringLiteral("Renamed"), QStringLiteral("echo replacement")));
        QVERIFY(session->stageRemove(old)); QVERIFY(session->staged().find(old)->pendingRemoval);
        QVERIFY(session->stageRestore(old)); QCOMPARE(session->staged().find(old)->name, QStringLiteral("Renamed"));
        QCOMPARE(backend->writes, 0); session->apply(); QVERIFY(!session->isDirty()); QCOMPARE(backend->requested.editCommands.size(), 1);
        QVERIFY(session->stageRemove(old)); session->apply(); QVERIFY(!session->applied().find(old)); QCOMPARE(backend->writes, 2);
    }
    void definitionImportIsAtomicAcrossAddsAndEdits() {
        const auto old = backend->current.entries.first().component;
        const auto initial = CustomShortcutsTypes::revision(session->staged());
        QVERIFY(!session->stageCommands({definition(old, QStringLiteral("Edited")), definition(QStringLiteral("../invalid.desktop"))}));
        QCOMPARE(CustomShortcutsTypes::revision(session->staged()), initial); QVERIFY(!session->isDirty());
        QVERIFY(!session->stageCommands({definition(QStringLiteral("new.desktop")), definition(QStringLiteral("new.desktop"))}));
        QCOMPARE(CustomShortcutsTypes::revision(session->staged()), initial);
        QVERIFY(session->stageCommands({definition(old, QStringLiteral("Edited")), definition(QStringLiteral("new.desktop"))}));
        QCOMPARE(session->staged().find(old)->name, QStringLiteral("Edited")); QVERIFY(session->staged().find(QStringLiteral("new.desktop"))->pendingAddition);
        QCOMPARE(backend->writes, 0); session->apply(); QCOMPARE(backend->requested.addCommands.size(), 1); QCOMPARE(backend->requested.editCommands.size(), 1); QVERIFY(!session->isDirty());
    }
    void pendingFlagsChangeDraftRevision() {
        const auto before = CustomShortcutsTypes::revision(session->staged());
        QVERIFY(session->stageRemove(backend->current.entries.first().component)); QVERIFY(CustomShortcutsTypes::revision(session->staged()) != before);
        session->revert(); QCOMPARE(CustomShortcutsTypes::revision(session->staged()), before);
        auto state = fixture(); state.entries.first().pendingAddition = true; QVERIFY(CustomShortcutsTypes::revision(state) != before);
    }
    void importedExistingCommandCanAtomicallyEditAndRegisterWithoutAutomaticApply() {
        makeExistingCommandUnregistered(); const auto id = backend->current.entries.first().component;
        QVERIFY(session->stageCommands({definition(id, QStringLiteral("Imported edit"), QStringLiteral("echo edited"))}, true));
        QVERIFY(session->staged().find(id)->pendingAddition); QVERIFY(!session->staged().find(id)->registered);
        QCOMPARE(session->staged().find(id)->name, QStringLiteral("Imported edit")); QCOMPARE(backend->writes, 0);
        QSignalSpy result(session, &SettingsSession::applyFinished); session->apply();
        QCOMPARE(backend->requested.addApplications, QStringList{id}); QCOMPARE(backend->requested.editCommands.size(), 1);
        QVERIFY(!session->isDirty()); QVERIFY(session->applied().find(id)->registered);
        QCOMPARE(session->applied().find(id)->path, QStringLiteral("/fixture/net.local.existing.desktop"));
        QCOMPARE(session->applied().find(id)->name, QStringLiteral("Imported edit"));
        QCOMPARE(result.count(), 1); QVERIFY(result.takeFirst()[0].toBool());
    }
    void importedRegistrationDuplicateOrInvalidPreservesWholeExistingDraft() {
        makeExistingCommandUnregistered(); const auto id = backend->current.entries.first().component;
        QVERIFY(session->stageAddCommand(QStringLiteral("Prior draft"), QStringLiteral("echo prior"), QStringLiteral("prior.desktop")));
        const auto before = CustomShortcutsTypes::revision(session->staged());
        QVERIFY(!session->stageCommands({definition(id, QStringLiteral("Changed")), definition(id)}, true));
        QCOMPARE(CustomShortcutsTypes::revision(session->staged()), before);
        QVERIFY(!session->stageCommands({definition(id, QStringLiteral("Changed")), definition(QStringLiteral("../bad.desktop"))}, true));
        QCOMPARE(CustomShortcutsTypes::revision(session->staged()), before);
        QVERIFY(!session->stageCommands({definition(id, QStringLiteral("Changed")), definition(QStringLiteral("vendor.unlisted.desktop"))}, true));
        QCOMPARE(CustomShortcutsTypes::revision(session->staged()), before); // An installed app identity cannot become a new command.
        QVERIFY(!session->staged().find(id)->pendingAddition); QVERIFY(session->staged().find(QStringLiteral("prior.desktop")));
        QCOMPARE(backend->writes, 0);
        QVERIFY(session->stageCommands({definition(id, QStringLiteral("Edit only"))}));
        QVERIFY(!session->staged().find(id)->pendingAddition); // Compatibility: the default remains edit-only.
    }
    void combinedEditRegistrationFalseSuccessKeepsDraft_data() {
        QTest::addColumn<int>("fault");
        QTest::newRow("not-registered") << 0; QTest::newRow("old-name") << 1; QTest::newRow("old-exec") << 2;
        QTest::newRow("missing-action") << 3; QTest::newRow("wrong-defaults") << 4;
        QTest::newRow("changed-path") << 5; QTest::newRow("changed-owner") << 6;
        QTest::newRow("foreign-context-change") << 7;
        QTest::newRow("additional-registered-context") << 8; QTest::newRow("missing-registered-context") << 9;
    }
    void combinedEditRegistrationFalseSuccessKeepsDraft() {
        QFETCH(int, fault); makeExistingCommandUnregistered(); backend->automaticWrite = false;
        const auto id = backend->current.entries.first().component;
        QVERIFY(session->stageCommands({definition(id, QStringLiteral("Imported edit"), QStringLiteral("echo edited"))}, true));
        session->apply(); auto report = backend->current;
        report.entries.first().registered = true; report.entries.first().name = QStringLiteral("Imported edit");
        report.entries.first().exec = QStringLiteral("echo edited"); report.entries.first().identity = "edited";
        report.actions << launch(id); report.contexts.insert(id, {QStringLiteral("default")});
        if (fault == 0) report.entries.first().registered = false;
        if (fault == 1) report.entries.first().name = QStringLiteral("Existing command");
        if (fault == 2) report.entries.first().exec = QStringLiteral("printf %%s old");
        if (fault == 3) report.actions.clear();
        if (fault == 4) report.actions.first().defaults = {QKeySequence(QStringLiteral("Meta+F10"))};
        if (fault == 5) report.entries.first().path = QStringLiteral("/foreign/moved.desktop");
        if (fault == 6) report.owner = QStringLiteral(":1.99");
        if (fault == 7) report.contexts.insert(QStringLiteral("untouched.desktop"), {QStringLiteral("default"), QStringLiteral("foreign")});
        if (fault == 8) report.contexts[id] << QStringLiteral("foreign");
        if (fault == 9) report.contexts.remove(id);
        report.revision = CustomShortcutsTypes::revision(report);
        QSignalSpy result(session, &SettingsSession::applyFinished); backend->report(ShortcutsOutcome::Success, report);
        QVERIFY(session->isDirty()); QVERIFY(session->hasConflict()); QCOMPARE(result.count(), 1); QVERIFY(!result.takeFirst()[0].toBool());
        session->revert(); QVERIFY(!session->canApply());
    }
    void successfulRemovalCannotLeaveNativeContextTopologyBehind() {
        backend->automaticWrite = false; const auto id = backend->current.entries.first().component;
        QVERIFY(session->stageRemove(id)); session->apply(); auto report = backend->current;
        report.entries.clear(); report.actions.clear(); // The component's remaining context is independent evidence.
        report.revision = CustomShortcutsTypes::revision(report);
        QSignalSpy result(session, &SettingsSession::applyFinished); backend->report(ShortcutsOutcome::Success, report);
        QVERIFY(session->isDirty()); QVERIFY(session->hasConflict()); QCOMPARE(result.count(), 1); QVERIFY(!result.takeFirst()[0].toBool());
    }
    void editingACommandCannotSilentlyChangeItsContextTopology() {
        backend->automaticWrite = false; const auto id = backend->current.entries.first().component;
        QVERIFY(session->stageEditCommand(id, QStringLiteral("Edited"), QStringLiteral("echo edited"))); session->apply();
        auto report = backend->current; report.entries.first().name = QStringLiteral("Edited"); report.entries.first().exec = QStringLiteral("echo edited");
        report.contexts[id] << QStringLiteral("foreign"); report.revision = CustomShortcutsTypes::revision(report);
        QSignalSpy result(session, &SettingsSession::applyFinished); backend->report(ShortcutsOutcome::Success, report);
        QVERIFY(session->isDirty()); QVERIFY(session->hasConflict()); QCOMPARE(result.count(), 1); QVERIFY(!result.takeFirst()[0].toBool());
    }
    void unsafeDefinitionsAndImmutableCommandsCannotStage() {
        const auto before = CustomShortcutsTypes::revision(session->staged());
        QVERIFY(!session->stageAddCommand(QStringLiteral("New\nname"), QStringLiteral("echo hi")));
        QVERIFY(!session->stageAddCommand(QStringLiteral("Name"), QStringLiteral("https://example.com")));
        QVERIFY(!session->stageAddCommand(QStringLiteral("Name"), QStringLiteral("echo hi"), QStringLiteral("a|b.desktop")));
        QCOMPARE(CustomShortcutsTypes::revision(session->staged()), before);
        backend->current.entries.first().editable = backend->current.entries.first().removable = false; backend->changed();
        QVERIFY(!session->stageEditCommand(backend->current.entries.first().component, QStringLiteral("No"), QStringLiteral("echo no")));
        QVERIFY(!session->stageRemove(backend->current.entries.first().component)); QVERIFY(!session->isDirty());
    }
    void localUrlAndPercentNormalizationIsStable() {
        QString normalized, error;
        QVERIFY(CustomShortcutsTypes::normalizeCommand(QStringLiteral("Local"), QStringLiteral("file:///tmp/local%20file"), normalized, error));
        QCOMPARE(normalized, KShell::quoteArg(QStringLiteral("/tmp/local file")));
        QVERIFY(CustomShortcutsTypes::normalizeCommand(QStringLiteral("Percent"), QStringLiteral("printf %%s"), normalized, error)); QCOMPARE(normalized, QStringLiteral("printf %%s"));
    }
    void draftsSurviveViewLifetimeAndExternalChanges() {
        auto *view = new QObject; connect(session, &SettingsSession::stateChanged, view, [] {});
        QVERIFY(session->stageAddCommand(QStringLiteral("Draft"), QStringLiteral("echo draft"), QStringLiteral("draft.desktop"))); delete view;
        QVERIFY(session->staged().find(QStringLiteral("draft.desktop"))); backend->current.entries.first().name = QStringLiteral("External"); backend->changed();
        QVERIFY(session->hasConflict()); QVERIFY(session->isDirty()); QVERIFY(!session->canApply());
        session->apply(); QCOMPARE(backend->writes, 0); session->revert(); QVERIFY(session->canApply()); QVERIFY(!session->isDirty());
        QCOMPARE(session->staged().entries.first().name, QStringLiteral("External"));
    }
    void failedReadAndFailedRevertPreserveDraft() {
        QVERIFY(session->stageAddCommand(QStringLiteral("Draft"), QStringLiteral("echo draft"), QStringLiteral("draft.desktop")));
        backend->readSuccess = false; session->revert(); QVERIFY(session->isDirty()); QVERIFY(session->hasConflict()); QVERIFY(!session->isReady());
        QVERIFY(session->staged().find(QStringLiteral("draft.desktop"))); backend->readSuccess = true; session->revert(); QVERIFY(!session->isDirty()); QVERIFY(session->canApply());
    }
    void falseSuccessCannotDiscardDraft_data() {
        QTest::addColumn<int>("fault"); QTest::newRow("unchanged-command") << 0; QTest::newRow("unrelated-entry") << 1;
        QTest::newRow("unrelated-action") << 2; QTest::newRow("owner-restart") << 3; QTest::newRow("interface-change") << 4;
    }
    void falseSuccessCannotDiscardDraft() {
        QFETCH(int, fault); backend->automaticWrite = false;
        QVERIFY(session->stageAddCommand(QStringLiteral("Draft"), QStringLiteral("echo draft"), QStringLiteral("draft.desktop"))); session->apply();
        auto report = backend->current;
        if (fault) {
            auto entry = definition(QStringLiteral("draft.desktop"), QStringLiteral("Draft"), QStringLiteral("echo draft")); entry.command = entry.registered = true;
            report.entries << entry; report.actions << launch(entry.component);
        }
        if (fault == 1) report.entries.first().name = QStringLiteral("Foreign edit");
        if (fault == 2) report.actions.first().current = {QKeySequence(QStringLiteral("Meta+F12"))};
        if (fault == 3) report.owner = QStringLiteral(":1.99");
        if (fault == 4) report.interfaceSignature = QStringLiteral("changed");
        QSignalSpy result(session, &SettingsSession::applyFinished); backend->report(ShortcutsOutcome::Success, report);
        QVERIFY(session->hasConflict()); QVERIFY(session->isDirty()); QCOMPARE(result.count(), 1); QVERIFY(!result.takeFirst()[0].toBool());
        session->revert(); QVERIFY(!session->canApply()); backend->current.owner = QStringLiteral(":1.99"); backend->changed(); QVERIFY(session->canApply());
    }
    void partialIsRecoverableButUncertainLocksOwner() {
        backend->automaticWrite = false; QVERIFY(session->stageRemove(backend->current.entries.first().component)); session->apply();
        backend->report(ShortcutsOutcome::Partial, backend->current, QStringLiteral("Known rejection")); QVERIFY(session->hasConflict()); QVERIFY(session->isDirty());
        session->revert(); QVERIFY(session->canApply());
        QVERIFY(session->stageRemove(backend->current.entries.first().component)); session->apply();
        backend->report(ShortcutsOutcome::Uncertain, backend->current, QStringLiteral("Timeout")); session->refresh(); QVERIFY(!session->canApply());
        session->revert(); QVERIFY(!session->canApply()); backend->current.owner = QStringLiteral(":1.80"); backend->changed(); QVERIFY(session->canApply());
    }
    void staleSignalsAndQueuedRefreshRespectOperationTokens() {
        backend->automaticRead = false; session->refresh(); const auto read = backend->readToken;
        emit backend->readFinished(read - 1, true, backend->current, {}); QVERIFY(session->isBusy());
        emit backend->writeFinished(read, ShortcutsOutcome::Success, backend->current, {}); QVERIFY(session->isBusy());
        emit backend->readFinished(read, true, backend->current, {}); QVERIFY(!session->isBusy());
        backend->automaticRead = true; backend->automaticWrite = false;
        QVERIFY(session->stageAddCommand(QStringLiteral("Draft"), QStringLiteral("echo draft"), QStringLiteral("draft.desktop"))); session->apply(); const auto write = backend->writeToken;
        emit backend->writeFinished(write - 1, ShortcutsOutcome::Failed, backend->current, {}); QVERIFY(session->isBusy());
        backend->changed(); const int reads = backend->reads; backend->finish(); QTRY_COMPARE(backend->reads, reads + 1); QVERIFY(!session->isDirty());
    }
    void backendLifetimeBelongsToSession() {
        auto *owned = new FakeCustomBackend; QPointer<FakeCustomBackend> guard(owned);
        auto *other = new CustomShortcutsSettings(owned); delete other; QVERIFY(guard.isNull());
    }
private:
    void makeExistingCommandUnregistered() {
        auto &entry = backend->current.entries.first(); entry.registered = false;
        CustomShortcutApplication app; app.id = entry.component; app.name = entry.name; app.exec = entry.exec; app.path = entry.path;
        app.command = true; app.identity = entry.identity; app.defaults = {launch(entry.component)}; backend->current.applications << app;
        backend->current.actions.clear(); backend->current.contexts.remove(entry.component); backend->changed();
    }
    FakeCustomBackend *backend = nullptr;
    CustomShortcutsSettings *session = nullptr;
};
QTEST_GUILESS_MAIN(CustomShortcutsSettingsTest)
#include "custom_shortcuts_settings_test.moc"
