#include <QtTest>
#include "settings/DefaultAppsSettings.h"

class StagedAppsBackend final : public DefaultAppsBackend {
public:
    int reads = 0, writes = 0;
    quint64 readToken = 0, writeToken = 0;
    DefaultAppsSnapshot value;
    DefaultAppsPreferences requested;
    StagedAppsBackend() {
        value.writable = true; value.revision = "initial";
        value.mimeTypes = {"text/plain", "application/json", "x-scheme-handler/http", "x-scheme-handler/https"};
        DefaultApplication first; first.id = "old.desktop"; first.name = "Old browser"; first.exec = "never-run";
        first.mimeTypes = {"text/plain", "x-scheme-handler/http", "x-scheme-handler/https"}; first.identity = "old";
        DefaultApplication second = first; second.id = "new.desktop"; second.name = "New browser"; second.identity = "new";
        second.mimeTypes = {"x-scheme-handler/http"};
        DefaultApplication recommended = first; recommended.id = "org.kde.kate.desktop"; recommended.categories = {"TextEditor"};
        recommended.mimeTypes = {"text/plain", "application/json"}; recommended.identity = "kate";
        value.applications = {first, second, recommended};
        value.preferences.mimeDefaults = {{"text/plain", first.id}, {"x-scheme-handler/http", first.id}, {"x-scheme-handler/https", first.id}};
        value.preferences.terminalService = "org.kde.konsole.desktop";
    }
    void readCurrent(quint64 token) override { ++reads; readToken = token; }
    void writeChanges(quint64 token, const DefaultAppsSnapshot &, const DefaultAppsPreferences &p) override
    { ++writes; writeToken = token; requested = p; }
    void report() { emit readFinished(readToken, true, value, {}); }
    void complete(DefaultAppsOutcome outcome = DefaultAppsOutcome::Success) {
        value.preferences = requested; value.revision = "saved";
        emit writeFinished(writeToken, outcome, value, outcome == DefaultAppsOutcome::Success ? QString() : QStringLiteral("partial save"));
    }
};
class DefaultAppsSettingsTest final : public QObject {
    Q_OBJECT
private slots:
    void init() {
        backend = new StagedAppsBackend; session = new DefaultAppsSettings(backend);
        QTRY_COMPARE(backend->reads, 1); backend->report();
    }
    void cleanup() { delete session; }
    void stagesSupportedCommonTypesOnly() {
        session->selectRole(DefaultAppRole::Browser, "new.desktop"); QVERIFY(session->isDirty());
        QCOMPARE(session->staged().mimeDefaults.value("x-scheme-handler/http"), "new.desktop");
        QCOMPARE(session->staged().mimeDefaults.value("x-scheme-handler/https"), "old.desktop");
        QCOMPARE(session->staged().mimeDefaults.value("text/plain"), "old.desktop"); QCOMPARE(backend->writes, 0);
        session->revert(); backend->report(); QVERIFY(!session->isDirty());
    }
    void unknownAndUnsupportedRequestsNeverStage() {
        session->selectRole(static_cast<DefaultAppRole>(-1), "new.desktop");
        session->selectRole(DefaultAppRole::Terminal, "new.desktop");
        session->selectMime("text/plain", "new.desktop"); session->selectMime("invented/mime", "old.desktop");
        session->selectMime("text/plain", "../../danger.desktop"); QVERIFY(!session->isDirty()); QCOMPARE(backend->writes, 0);
    }
    void defaultsAreInstalledRecommendationsAndRemainStaged() {
        session->restoreDefaults();
        QCOMPARE(session->staged().mimeDefaults.value("text/plain"), "org.kde.kate.desktop");
        QCOMPARE(session->staged().mimeDefaults.value("application/json"), "org.kde.kate.desktop");
        QCOMPARE(session->staged().mimeDefaults.value("x-scheme-handler/http"), "old.desktop");
        QVERIFY(session->isDirty()); QCOMPARE(backend->writes, 0);
    }
    void applyWaitsForVerifiedCompletion() {
        session->selectMime("text/plain", "org.kde.kate.desktop"); QSignalSpy done(session, &SettingsSession::applyFinished);
        session->apply(); QVERIFY(session->isBusy()); QCOMPARE(backend->writes, 1); QVERIFY(done.isEmpty());
        session->selectRole(DefaultAppRole::Browser, "new.desktop");
        QCOMPARE(session->staged().mimeDefaults.value("x-scheme-handler/http"), "old.desktop");
        backend->complete(); QCOMPARE(done.size(), 1); QVERIFY(done[0][0].toBool()); QVERIFY(!session->isDirty());
        QVERIFY(session->resultText().contains("adoption has not been verified"));
    }
    void externalEditsPreserveStagingUntilRevert() {
        session->selectMime("text/plain", "org.kde.kate.desktop");
        backend->value.revision = "external"; session->refresh(); backend->report();
        QVERIFY(session->hasConflict()); QCOMPARE(session->staged().mimeDefaults.value("text/plain"), "org.kde.kate.desktop");
        session->apply(); QCOMPARE(backend->writes, 0);
        session->revert(); backend->report(); QVERIFY(!session->hasConflict()); QVERIFY(!session->isDirty());
    }
    void partialAndUncertainResultsRequireReconciliation() {
        session->selectRole(DefaultAppRole::Browser, "new.desktop"); session->apply(); backend->complete(DefaultAppsOutcome::Partial);
        QVERIFY(session->hasConflict()); QVERIFY(!session->canApply()); QVERIFY(session->resultText().isEmpty());
        QCOMPARE(session->errorText(), "partial save");
        backend->value.revision = "poll"; session->refresh(); backend->report(); QVERIFY(session->hasConflict());
        QCOMPARE(session->errorText(), "partial save"); session->revert(); backend->report(); QVERIFY(session->canApply());
    }
    void mismatchedSuccessIsNotReportedAsApplied() {
        session->selectRole(DefaultAppRole::Browser, "new.desktop"); session->apply();
        emit backend->writeFinished(backend->writeToken, DefaultAppsOutcome::Success, backend->value, {});
        QVERIFY(session->hasConflict()); QVERIFY(session->isDirty()); QVERIFY(session->resultText().isEmpty());
    }
    void staleCompletionAndBusyRefreshDoNotLoseSelections() {
        session->selectRole(DefaultAppRole::Browser, "new.desktop"); session->apply();
        emit backend->writeFinished(backend->writeToken - 1, DefaultAppsOutcome::Success, backend->value, {});
        QVERIFY(session->isBusy()); session->refresh(); backend->complete();
        QTRY_COMPARE(backend->reads, 2); backend->report(); QVERIFY(!session->isBusy()); QVERIFY(!session->isDirty());
    }
private:
    StagedAppsBackend *backend;
    DefaultAppsSettings *session;
};
QTEST_GUILESS_MAIN(DefaultAppsSettingsTest)
#include "default_apps_settings_test.moc"
