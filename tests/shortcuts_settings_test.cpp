#include <QtTest>
#include <QPointer>
#include "settings/ShortcutsSettings.h"

namespace {
const ShortcutActionId launch{QStringLiteral("launcher"), QStringLiteral("default"), QStringLiteral("open")};
const ShortcutActionId window{QStringLiteral("kwin"), QStringLiteral("default"), QStringLiteral("close")};
const ShortcutActionId otherContext{QStringLiteral("launcher"), QStringLiteral("editing"), QStringLiteral("edit")};
QKeySequence key(const char *text) { return QKeySequence(QString::fromLatin1(text), QKeySequence::PortableText); }
ShortcutsSnapshot fixture()
{
    ShortcutsSnapshot state;
    state.owner = QStringLiteral(":1.42"); state.interfaceSignature = QStringLiteral("test-v1");
    state.available = state.writable = true;
    state.actions = {
        {launch, QStringLiteral("Application Launcher"), QStringLiteral("Default"), QStringLiteral("Open launcher"), {key("Meta+Space"), key("Alt+F1")}, {key("Meta+L")}, true},
        {window, QStringLiteral("Window Manager"), QStringLiteral("Default"), QStringLiteral("Close window"), {key("Alt+F4"), key("Ctrl+Alt+Q")}, {key("Meta+Q")}, true},
        {otherContext, QStringLiteral("Application Launcher"), QStringLiteral("Editing"), QStringLiteral("Edit launcher"), {key("Meta+E")}, {}, true}
    };
    state.revision = ShortcutsTypes::revision(state); return state;
}
void assign(ShortcutsSnapshot &state, const ShortcutActionId &id, const QList<QKeySequence> &keys)
{ for (auto &action : state.actions) if (action.id == id) action.current = keys; state.revision = ShortcutsTypes::revision(state); }
}

class FakeShortcutsBackend final : public ShortcutsBackend {
public:
    ShortcutsSnapshot current = fixture(), expected;
    ShortcutChanges requested;
    int reads = 0, writes = 0;
    quint64 readToken = 0, writeToken = 0;
    bool autoRead = true, autoWrite = true, readSuccess = true;
    void readCurrent(quint64 token) override {
        ++reads; readToken = token;
        if (autoRead) emit readFinished(token, readSuccess, current, readSuccess ? QString() : QStringLiteral("Read failed"));
    }
    void writeChanges(quint64 token, const ShortcutsSnapshot &baseline, const ShortcutChanges &next) override {
        ++writes; writeToken = token; expected = baseline; requested = next;
        if (autoWrite) finish();
    }
    void finish() {
        for (auto i = requested.cbegin(); i != requested.cend(); ++i) assign(current, i.key(), i.value());
        emit writeFinished(writeToken, ShortcutsOutcome::Success, current, {});
    }
    void report(ShortcutsOutcome outcome, const ShortcutsSnapshot &value, const QString &error = {})
    { emit writeFinished(writeToken, outcome, value, error); }
    void changed() { current.revision = ShortcutsTypes::revision(current); emit externalChange(); }
};

class ShortcutsSettingsTest final : public QObject {
    Q_OBJECT
private slots:
    void init() {
        backend = new FakeShortcutsBackend; session = new ShortcutsSettings(backend);
        QTRY_VERIFY(session->isReady());
    }
    void cleanup() { delete session; }
    void alternativesAndFourChordsStageUntilApply() {
        const QList<QKeySequence> next{key("Ctrl+K, Ctrl+C, Ctrl+U, Ctrl+X"), key("Meta+O")};
        QVERIFY(session->selectShortcuts(launch, next)); QVERIFY(session->isDirty()); QCOMPARE(backend->writes, 0);
        QCOMPARE(session->staged().find(launch)->current, next);
        QCOMPARE(session->applied().find(launch)->current.size(), 2);
        QSignalSpy applied(session, &SettingsSession::applyFinished); session->apply();
        QCOMPARE(backend->writes, 1); QCOMPARE(backend->requested.size(), 1);
        QCOMPARE(backend->requested.value(launch), next); QVERIFY(!session->isDirty());
        QCOMPARE(applied.count(), 1); QVERIFY(applied.takeFirst().at(0).toBool());
    }
    void softDeadlineKeepsOwnershipAndClearsProgressAtCompletion() {
        backend->autoWrite=false; QVERIFY(session->selectShortcuts(launch,{key("Meta+O")})); session->apply();
        emit backend->operationPending(QStringLiteral("Shortcut save continues until actual completion"));
        QVERIFY(session->isBusy()); QVERIFY(session->isDirty()); QVERIFY(session->resultText().contains("continues"));
        backend->finish(); QVERIFY(!session->isBusy()); QVERIFY(!session->resultText().contains("continues"));
    }
    void stateObserverMayDeleteControllerBeforeBackendSubmission() {
        QVERIFY(session->selectShortcuts(launch,{key("Meta+O")})); QPointer<ShortcutsSettings> guarded=session;
        connect(session,&SettingsSession::stateChanged,this,[this] { if (session && session->isBusy()) { delete session; session=nullptr; backend=nullptr; } });
        session->apply(); QVERIFY(!guarded);
    }
    void terminalObserverMayDeleteControllerWithoutQueuedRefresh() {
        backend->autoWrite=false; QVERIFY(session->selectShortcuts(launch,{key("Meta+O")})); session->apply(); session->refresh();
        QPointer<ShortcutsSettings> guarded=session;
        connect(session,&SettingsSession::applyFinished,this,[this] { delete session; session=nullptr; backend=nullptr; });
        backend->finish(); QVERIFY(!guarded); QCoreApplication::processEvents();
    }
    void reorderedAlternativesDoNotCreateWrites() {
        const auto original = session->staged().find(launch)->current;
        QVERIFY(session->selectShortcuts(launch, {original.at(1), original.at(0)}));
        QVERIFY(!session->isDirty()); session->apply(); QCOMPARE(backend->writes, 0);
    }
    void equalAndContiguousShadowConflictsRequireExplicitReassignment() {
        const QList<QKeySequence> occupied{key("Alt+F4")};
        QVERIFY(!session->selectShortcuts(launch, occupied)); QVERIFY(!session->isDirty());
        QCOMPARE(session->conflictsFor(launch, occupied).size(), 1);
        const QList<QKeySequence> shadow{key("Ctrl+K, Alt+F4, Ctrl+U")};
        QCOMPARE(session->conflictsFor(launch, shadow).size(), 1);
        QVERIFY(!session->selectShortcuts(launch, shadow));
        QVERIFY(session->selectShortcuts(launch, shadow, true));
        QCOMPARE(session->staged().find(window)->current, (QList<QKeySequence>{key("Ctrl+Alt+Q")}));
        QCOMPARE(session->staged().find(launch)->current, shadow); QCOMPARE(backend->writes, 0);
        session->apply(); QCOMPARE(backend->requested.size(), 2); QVERIFY(!session->isDirty());
    }
    void separateContextsInOneComponentCanShareKeys() {
        QVERIFY(session->conflictsFor(launch, {key("Meta+E")}).isEmpty());
        QVERIFY(session->selectShortcuts(launch, {key("Meta+E")}));
        QCOMPARE(session->staged().find(otherContext)->current, (QList<QKeySequence>{key("Meta+E")}));
        QVERIFY(!session->selectShortcuts(window, {key("Meta+E")}));
    }
    void unavailableConflictingActionCannotBeStolen() {
        backend->current.actions[1].available = false; backend->changed();
        QVERIFY(!session->selectShortcuts(launch, {key("Alt+F4")}, true)); QVERIFY(!session->isDirty());
        QCOMPARE(session->staged().find(window)->current.size(), 2);
        QVERIFY(!session->selectShortcuts(window, {})); QCOMPARE(backend->writes, 0);
    }
    void invalidAlternativesDoNotMutateDraft() {
        const auto previous = session->staged().find(launch)->current;
        QVERIFY(!session->selectShortcuts(launch, {key("Ctrl+J"), key("Ctrl+J")}));
        QVERIFY(!session->selectShortcuts(launch, {key("Ctrl+J"), key("Ctrl+J, Ctrl+K")}));
        QVERIFY(!session->selectShortcuts(launch, {QKeySequence()}));
        QCOMPARE(session->staged().find(launch)->current, previous); QVERIFY(!session->isDirty());
    }
    void selectedAndAllDefaultsAreStagedAndCanBeReverted() {
        session->restoreDefaults(launch);
        QCOMPARE(session->staged().find(launch)->current, session->applied().find(launch)->defaults);
        QCOMPARE(session->staged().find(window)->current, session->applied().find(window)->current);
        session->restoreAllDefaults(); QVERIFY(session->staged().find(otherContext)->current.isEmpty());
        QCOMPARE(session->staged().find(window)->current, (QList<QKeySequence>{key("Meta+Q")}));
        QCOMPARE(backend->writes, 0); session->revert(); QVERIFY(!session->isDirty());
        QCOMPARE(session->staged().find(window)->current, (QList<QKeySequence>{key("Alt+F4"), key("Ctrl+Alt+Q")}));
    }
    void failedRevertKeepsDraftDirty() {
        QVERIFY(session->selectShortcuts(launch, {key("Meta+O")})); backend->readSuccess = false;
        session->revert(); QVERIFY(session->isDirty()); QVERIFY(session->hasConflict());
        QCOMPARE(session->staged().find(launch)->current, (QList<QKeySequence>{key("Meta+O")}));
        backend->readSuccess = true; session->revert(); QVERIFY(!session->isDirty()); QVERIFY(session->canApply());
    }
    void externalChangePreservesDraftAndBlocksApply() {
        QVERIFY(session->selectShortcuts(launch, {key("Meta+O")}));
        assign(backend->current, window, {key("Meta+W")}); backend->changed();
        QVERIFY(session->hasConflict()); QVERIFY(session->isDirty()); QVERIFY(!session->canApply());
        session->apply(); QCOMPARE(backend->writes, 0);
        QCOMPARE(session->staged().find(launch)->current, (QList<QKeySequence>{key("Meta+O")}));
        session->revert(); QVERIFY(session->canApply());
        QCOMPARE(session->staged().find(window)->current, (QList<QKeySequence>{key("Meta+W")}));
    }
    void falseSuccessFailsIndependentVerificationAndLocksOwner() {
        backend->autoWrite = false; QVERIFY(session->selectShortcuts(launch, {key("Meta+O")})); session->apply();
        QSignalSpy applied(session, &SettingsSession::applyFinished);
        backend->report(ShortcutsOutcome::Success, backend->current);
        QVERIFY(!session->isBusy()); QVERIFY(session->isDirty()); QVERIFY(session->hasConflict());
        QCOMPARE(applied.count(), 1); QVERIFY(!applied.takeFirst().at(0).toBool());
        session->revert(); QVERIFY(!session->canApply()); session->refresh(); QVERIFY(!session->canApply());
        backend->current.owner = QStringLiteral(":1.99"); backend->changed(); QVERIFY(session->canApply());
    }
    void successMustVerifyUntouchedActionsAndDaemonIdentity() {
        backend->autoWrite = false; QVERIFY(session->selectShortcuts(launch, {key("Meta+O")})); session->apply();
        auto report = backend->current; assign(report, launch, {key("Meta+O")}); assign(report, window, {});
        backend->report(ShortcutsOutcome::Success, report); QVERIFY(session->hasConflict()); QVERIFY(!session->resultText().contains("saved"));
    }
    void partialSaveKeepsRemainingDraftAndConflict() {
        backend->autoWrite = false; QVERIFY(session->selectShortcuts(launch, {key("Alt+F4")}, true)); session->apply();
        assign(backend->current, window, {key("Ctrl+Alt+Q")});
        backend->report(ShortcutsOutcome::Partial, backend->current, QStringLiteral("Second action failed"));
        QVERIFY(session->isDirty()); QVERIFY(session->hasConflict()); QCOMPARE(session->errorText(), "Second action failed");
        QCOMPARE(session->staged().find(launch)->current, (QList<QKeySequence>{key("Alt+F4")}));
        session->revert(); QVERIFY(session->canApply());
    }
    void uncertainOutcomeSurvivesRefreshAndRevertUntilOwnerChanges() {
        backend->autoWrite = false; QVERIFY(session->selectShortcuts(launch, {key("Meta+O")})); session->apply();
        backend->report(ShortcutsOutcome::Uncertain, backend->current, QStringLiteral("Timed out"));
        session->refresh(); QVERIFY(!session->canApply()); session->revert(); QVERIFY(!session->canApply());
        QVERIFY(!session->selectShortcuts(launch, {key("Meta+P")}));
        backend->current.owner = QStringLiteral(":1.80"); backend->changed(); QVERIFY(session->canApply());
    }
    void saveFailureIsVisibleAndDraftSurvives() {
        backend->autoWrite = false; QVERIFY(session->selectShortcuts(launch, {key("Meta+O")})); session->apply();
        backend->report(ShortcutsOutcome::Failed, backend->current, QStringLiteral("Preferences file could not be saved"));
        QVERIFY(session->isDirty()); QVERIFY(session->hasConflict()); QVERIFY(session->errorText().contains("could not be saved"));
        session->refresh(); QVERIFY(session->errorText().contains("could not be saved"));
    }
    void staleAndWrongPhaseSignalsCannotFinishAnOperation() {
        backend->autoRead = false; session->refresh(); const auto readToken = backend->readToken;
        emit backend->readFinished(readToken - 1, true, backend->current, {}); QVERIFY(session->isBusy());
        emit backend->writeFinished(readToken, ShortcutsOutcome::Success, backend->current, {}); QVERIFY(session->isBusy());
        emit backend->readFinished(readToken, true, backend->current, {}); QVERIFY(!session->isBusy());
        backend->autoWrite = false; QVERIFY(session->selectShortcuts(launch, {key("Meta+O")})); session->apply();
        const auto writeToken = backend->writeToken;
        emit backend->readFinished(writeToken, true, backend->current, {}); QVERIFY(session->isBusy());
        emit backend->writeFinished(writeToken - 1, ShortcutsOutcome::Failed, backend->current, {}); QVERIFY(session->isBusy());
        backend->finish(); QVERIFY(!session->isBusy()); QVERIFY(!session->isDirty());
        emit backend->writeFinished(writeToken, ShortcutsOutcome::Failed, backend->current, QStringLiteral("late"));
        QVERIFY(session->errorText().isEmpty());
    }
    void externalRefreshWhileWritingRunsAfterCompletion() {
        backend->autoWrite = false; QVERIFY(session->selectShortcuts(launch, {key("Meta+O")})); session->apply();
        backend->changed(); QCOMPARE(backend->reads, 1); backend->finish();
        QTRY_COMPARE(backend->reads, 2); QVERIFY(!session->isDirty()); QVERIFY(!session->hasConflict());
    }
    void unsupportedServiceCannotStageOrWrite() {
        backend->current.writable = false; backend->current.reason = QStringLiteral("Unsupported shortcut interface"); backend->changed();
        QVERIFY(!session->canApply()); QVERIFY(!session->selectShortcuts(launch, {key("Meta+O")}));
        session->restoreAllDefaults(); session->apply(); QCOMPARE(backend->writes, 0);
        QCOMPARE(session->errorText(), "Unsupported shortcut interface");
    }
    void syntheticLayoutRegistrationMustBeVerified_data() {
        QTest::addColumn<bool>("registered");
        QTest::addColumn<bool>("sameLayoutOwner");
        QTest::newRow("registered-and-stable") << true << true;
        QTest::newRow("missing-registration") << false << true;
        QTest::newRow("compositor-restarted") << true << false;
    }
    void syntheticLayoutRegistrationMustBeVerified() {
        QFETCH(bool, registered); QFETCH(bool, sameLayoutOwner);
        const ShortcutActionId layout{QStringLiteral("KDE Keyboard Layout Switcher"), QStringLiteral("default"), QStringLiteral("Switch keyboard layout to Deutsch")};
        ShortcutAction action{layout, QStringLiteral("Keyboard Layout Switcher"), QStringLiteral("Default"), QStringLiteral("Deutsch"), {}, {}, true};
        action.registered = false; action.layoutAction = true; backend->current.actions.append(action);
        backend->current.layouts.available = true;
        backend->current.layouts.owner = QStringLiteral(":1.7");
        backend->current.layouts.version = QStringLiteral("6.6.6");
        backend->current.layouts.actionNames = {layout.action}; backend->changed();
        QVERIFY(session->selectShortcuts(layout, {key("Meta+Alt+D")}));
        backend->autoWrite = false; session->apply();
        auto report = backend->current; assign(report, layout, {key("Meta+Alt+D")});
        report.actions.last().registered = registered;
        if (!sameLayoutOwner) report.layouts.owner = QStringLiteral(":1.8");
        report.revision = ShortcutsTypes::revision(report);
        QSignalSpy applied(session, &SettingsSession::applyFinished);
        backend->report(ShortcutsOutcome::Success, report);
        const bool expected = registered && sameLayoutOwner;
        QCOMPARE(applied.count(), 1); QCOMPARE(applied.takeFirst().at(0).toBool(), expected);
        QCOMPARE(session->hasConflict(), !expected); QCOMPARE(session->canApply(), expected);
    }
    void undiscoveredActionCannotBeRegisteredByDraft() {
        backend->current.actions.first().registered = false; backend->changed();
        QVERIFY(!session->selectShortcuts(launch, {key("Meta+O")}));
        QVERIFY(session->errorText().contains("cannot be registered")); QCOMPARE(backend->writes, 0);
    }
    void importedBindingsSwapAtomicallyWithoutStealingSelectedTargets() {
        QVERIFY(!session->selectShortcuts(launch, {key("Alt+F4")}));
        QVERIFY(session->selectChanges({{launch, {key("Alt+F4")}}, {window, {key("Meta+Space")}}}));
        QCOMPARE(session->staged().find(launch)->current, (QList<QKeySequence>{key("Alt+F4")}));
        QCOMPARE(session->staged().find(window)->current, (QList<QKeySequence>{key("Meta+Space")}));
        const auto draft = ShortcutsTypes::revision(session->staged());
        QVERIFY(!session->selectChanges({{launch, {key("Meta+P")}}, {window, {key("Meta+P")}}}, true));
        QCOMPARE(ShortcutsTypes::revision(session->staged()), draft); QCOMPARE(backend->writes, 0);
    }
    void twoScopeImportValidatesBothBeforeEitherDraftChanges() {
        auto *otherBackend = new FakeShortcutsBackend; ShortcutsSettings other(otherBackend); QTRY_VERIFY(other.isReady());
        QString error; const auto before = ShortcutsTypes::revision(session->staged());
        const auto otherBefore = ShortcutsTypes::revision(other.staged());
        QVERIFY(!ShortcutsSettings::stageTogether(session, {{launch, {key("Meta+O")}}}, &other,
            {{window, {key("Meta+P"), key("Meta+P")}}}, false, error));
        QVERIFY(!error.isEmpty()); QCOMPARE(ShortcutsTypes::revision(session->staged()), before);
        QCOMPARE(ShortcutsTypes::revision(other.staged()), otherBefore);
        bool observedComplete = false;
        connect(session, &SettingsSession::stateChanged, &other, [&] { observedComplete = session->isDirty() && other.isDirty(); });
        QVERIFY(ShortcutsSettings::stageTogether(session, {{launch, {key("Meta+O")}}}, &other,
            {{window, {key("Meta+P")}}}, false, error));
        QVERIFY(observedComplete); QCOMPARE(backend->writes, 0); QCOMPARE(otherBackend->writes, 0);
        session->revert(); other.revert(); QVERIFY(!session->isDirty()); QVERIFY(!other.isDirty());
    }
    void busyOtherScopePreventsImportWithoutClearingExistingDraft() {
        auto *otherBackend = new FakeShortcutsBackend; ShortcutsSettings other(otherBackend); QTRY_VERIFY(other.isReady());
        QVERIFY(session->selectShortcuts(launch, {key("Meta+O")}));
        const auto draft = ShortcutsTypes::revision(session->staged());
        otherBackend->autoRead = false; other.refresh(); QVERIFY(other.isBusy());
        QString error;
        QVERIFY(!ShortcutsSettings::stageTogether(session, {{launch, {key("Meta+P")}}}, &other,
            {{window, {key("Meta+Q")}}}, false, error));
        QCOMPARE(ShortcutsTypes::revision(session->staged()), draft); QVERIFY(session->isDirty());
        QCOMPARE(backend->writes, 0); QCOMPARE(otherBackend->writes, 0);
    }
    void completeImportMapMayPreserveUnchangedImmutableActions() {
        backend->current.actions[1].available = false; backend->changed();
        ShortcutChanges all;
        for (const auto &action : session->staged().actions) all.insert(action.id, action.current);
        all[launch] = {key("Meta+O")}; QVERIFY(session->selectChanges(all));
        QCOMPARE(session->staged().find(window)->current, backend->current.find(window)->current);
        QVERIFY(session->isDirty()); QCOMPARE(backend->writes, 0);
    }
private:
    FakeShortcutsBackend *backend = nullptr;
    ShortcutsSettings *session = nullptr;
};
QTEST_GUILESS_MAIN(ShortcutsSettingsTest)
#include "shortcuts_settings_test.moc"
