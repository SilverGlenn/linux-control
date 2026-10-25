#include <QtTest>
#include <QComboBox>
#include <QPushButton>
#include <QLabel>
#include "settings/FontSettings.h"
#include "ui/pages/FontSettingsPage.h"

class FakeFontBackend : public FontBackend {
public:
    int reads = 0, writes = 0;
    FontState baseline;
    FontPreferences requested;
    void readCurrent() override { ++reads; }
    void writePreferences(const FontState &state, const FontPreferences &p) override
    { ++writes; baseline = state; requested = p; }
    FontState state() const {
        FontState s; s.writable = true; s.reloadAvailable = true; s.revision = "original";
        for (auto &font : s.preferences.fonts) font = QFont(QStringLiteral("Noto Sans"), 10);
        return s;
    }
    void report(const FontState &s) { emit readFinished(true, s, {}); }
    void written(bool ok = true) { emit writeFinished(ok, ok ? QString() : QStringLiteral("Saved but refresh failed")); }
    void failedRead(const QString &error = QStringLiteral("Readback unavailable")) { emit readFinished(false, {}, error); }
    void changed() { emit externalChange(); }
    void pending(const QString &message) { emit operationPending(message); }
    void warning() { emit writeFinished(true, QStringLiteral("Saved, but refresh status is uncertain")); }
};
class FontSettingsTest : public QObject {
    Q_OBJECT
private slots:
    void init() {
        backend = new FakeFontBackend; session = new FontSettings(backend);
        QTRY_COMPARE(backend->reads, 1); initial = backend->state(); backend->report(initial);
    }
    void cleanup() { delete session; }
    void stageAndRecreatePage() {
        auto *page = new FontSettingsPage(nullptr, session, nullptr);
        auto *combo = page->findChild<QComboBox *>("fontAntialiasing");
        QVERIFY(combo); QVERIFY(!combo->accessibleName().isEmpty());
        combo->setCurrentIndex(combo->findData("false"));
        QVERIFY(session->isDirty()); QCOMPARE(backend->writes, 0);
        delete page;
        FontSettingsPage replacement(nullptr, session, nullptr);
        QCOMPARE(replacement.settingsSession(), static_cast<SettingsSession *>(session));
        QCOMPARE(replacement.findChild<QComboBox *>("fontAntialiasing")->currentData().toString(), "false");
        QVERIFY(replacement.findChild<QPushButton *>("applyFontSettings")->isEnabled());
    }
    void applyPreflightReloadAndReadback() {
        QSignalSpy finished(session, &SettingsSession::applyFinished);
        session->selectFont(5, QFont(QStringLiteral("Noto Serif"), 12));
        session->selectRendering("true", "hintslight", "rgb");
        session->apply(); QCOMPARE(backend->writes, 0); QVERIFY(session->isBusy());
        backend->report(initial); QCOMPARE(backend->writes, 1); QCOMPARE(backend->baseline.revision, initial.revision);
        backend->written(); QVERIFY(finished.isEmpty());
        auto saved = initial; saved.preferences = backend->requested; saved.revision = "saved"; backend->report(saved);
        QCOMPARE(finished.size(), 1); QVERIFY(finished[0][0].toBool()); QVERIFY(!session->isDirty());
        QVERIFY(session->resultText().contains("Restart applications"));
        QVERIFY(!session->resultText().contains("live"));
    }
    void revisionConflictProtectsEvenUnrelatedExternalEdits() {
        session->selectRendering("false", {}, {}); session->apply();
        auto external = initial; external.revision = "unrelated external edit"; backend->report(external);
        QCOMPARE(backend->writes, 0); QVERIFY(session->hasConflict());
        QCOMPARE(session->stagedPreferences().antialias, "false");
        session->revert(); backend->report(external); QVERIFY(!session->hasConflict()); QVERIFY(!session->isDirty());
    }
    void externalConflictPreservesFonts() {
        session->selectFont(0, QFont(QStringLiteral("Noto Serif"), 13));
        auto external = initial; external.preferences.fonts[0] = QFont(QStringLiteral("Hack"), 11);
        backend->changed(); backend->report(external);
        QVERIFY(session->hasConflict()); QCOMPARE(session->stagedPreferences().fonts[0].family(), "Noto Serif");
        session->apply(); QCOMPARE(backend->writes, 0);
    }
    void failedRefreshReadsSavedValuesWithoutSuccessClaim() {
        QSignalSpy finished(session, &SettingsSession::applyFinished);
        session->selectRendering("true", {}, {}); session->apply(); backend->report(initial); backend->written(false);
        auto saved = initial; saved.preferences = backend->requested; backend->report(saved);
        QVERIFY(!finished[0][0].toBool()); QVERIFY(session->hasConflict()); QVERIFY(session->resultText().isEmpty());
        QCOMPARE(session->appliedPreferences().antialias, "true"); QVERIFY(session->errorText().contains("refresh failed"));
        backend->changed(); backend->report(saved); QVERIFY(session->errorText().contains("refresh failed"));
    }
    void failedReadbackRequiresReconciliation() {
        session->selectRendering("false", {}, {}); session->apply(); backend->report(initial); backend->written(); backend->failedRead();
        QVERIFY(!session->isReady()); QVERIFY(session->hasConflict()); QCOMPARE(session->stagedPreferences().antialias, "false");
    }
    void missingCapabilityAndUnsupportedSelectionsNeverWrite() {
        session->selectRendering("unknown", {}, {}); QVERIFY(!session->isDirty());
        session->selectFont(0, QFont(QStringLiteral("Noto Sans"), 2)); QVERIFY(!session->isDirty());
        auto unavailable = initial; unavailable.reloadAvailable = false; unavailable.unavailableReason = "No KDE Wayland session";
        backend->changed(); backend->report(unavailable);
        FontSettingsPage page(nullptr, session, nullptr);
        QVERIFY(!page.findChild<QPushButton *>("generalFont")->isEnabled());
        QVERIFY(page.findChild<QLabel *>("fontSettingsMessage")->text().contains("No KDE Wayland"));
        session->apply(); QCOMPARE(backend->writes, 0);
    }
    void explicitWarningCannotBeClearedByMatchingReadback() {
        QSignalSpy finished(session, &SettingsSession::applyFinished);
        session->selectRendering("true", {}, {}); session->apply(); backend->report(initial); backend->warning();
        auto saved = initial; saved.preferences = backend->requested; backend->report(saved);
        QCOMPARE(finished.size(), 1); QVERIFY(!finished[0][0].toBool()); QVERIFY(session->hasConflict()); QVERIFY(session->isDirty());
        QVERIFY(session->errorText().contains("uncertain")); QVERIFY(session->resultText().isEmpty());
        backend->changed(); backend->report(saved); QVERIFY(session->errorText().contains("uncertain"));
    }
    void stagedFontUsesNativeStableAliasesWithoutDroppingNamedFaces() {
        auto alias = initial; alias.preferences.fonts[0].setStyleName(QStringLiteral("Regular"));
        backend->changed(); backend->report(alias); QVERIFY(session->appliedPreferences().fonts[0].styleName().isEmpty());
        auto selected = alias.preferences.fonts[0]; selected.setPointSize(13); session->selectFont(0, selected);
        QVERIFY(session->stagedPreferences().fonts[0].styleName().isEmpty());
        QCOMPARE(session->stagedPreferences().fonts[0].pointSize(), 13); QCOMPARE(session->stagedPreferences().fonts[0].family(), selected.family());
        session->apply(); backend->report(alias); QVERIFY(backend->requested.fonts[0].styleName().isEmpty());
        backend->written(); auto saved = alias; saved.preferences = backend->requested; backend->report(saved);
        QVERIFY(!session->hasConflict()); QVERIFY(!session->isDirty());
        selected.setStyleName(QStringLiteral("Decorative Display")); session->selectFont(0, selected);
        QCOMPARE(session->stagedPreferences().fonts[0].styleName(), QStringLiteral("Decorative Display"));
    }
    void failedRevertKeepsTheDraftAndPendingIsPhaseScoped() {
        session->selectRendering("false", {}, {});
        backend->pending("late idle status"); QVERIFY(session->pendingMessage().isEmpty());
        session->revert(); backend->pending("slow font read"); QCOMPARE(session->pendingMessage(), "slow font read");
        backend->failedRead(); QVERIFY(session->isDirty()); QCOMPARE(session->stagedPreferences().antialias, "false");
        QVERIFY(session->pendingMessage().isEmpty());
        session->revert(); backend->report(initial); QVERIFY(!session->isDirty());
        QCOMPARE(session->stagedPreferences().antialias, initial.preferences.antialias);
    }
    void failedRevertAfterUncertainApplyReportsCurrentReadFailure() {
        session->selectRendering("true", {}, {}); session->apply(); backend->report(initial); backend->written(false);
        auto saved = initial; saved.preferences = backend->requested; backend->report(saved);
        QVERIFY(session->errorText().contains("refresh failed")); QVERIFY(session->isDirty()); QVERIFY(session->hasConflict());
        session->revert(); backend->failedRead(QStringLiteral("Current reconciliation read denied"));
        QCOMPARE(session->errorText(), QStringLiteral("Current reconciliation read denied"));
        QVERIFY(session->isDirty()); QVERIFY(session->hasConflict()); QCOMPARE(session->stagedPreferences().antialias, QStringLiteral("true"));
    }
    void pendingWriteSurvivesPageReplacementAndClearAtReadback() {
        session->selectRendering("false", {}, {}); session->apply(); backend->report(initial);
        backend->pending("waiting for native font synchronization");
        auto *page = new FontSettingsPage(nullptr, session, nullptr);
        QVERIFY(page->findChild<QLabel *>("fontSettingsStatus")->text().contains("native"));
        QVERIFY(!page->findChild<QPushButton *>("applyFontSettings")->isEnabled()); delete page;
        FontSettingsPage replacement(nullptr, session, nullptr);
        QVERIFY(replacement.findChild<QLabel *>("fontSettingsStatus")->text().contains("native"));
        backend->written(); QVERIFY(session->pendingMessage().isEmpty());
        auto saved = initial; saved.preferences = backend->requested; backend->report(saved); QVERIFY(!session->isBusy());
    }
    void outwardSignalsCanDeleteTheSession() {
        session->selectRendering("false", {}, {});
        connect(session, &SettingsSession::stateChanged, this, [this] { delete session; session = nullptr; });
        session->apply(); QVERIFY(!session); backend = nullptr;
    }
    void terminalApplySignalCanDeleteTheSession() {
        session->selectRendering("false", {}, {}); session->apply(); backend->report(initial); backend->written();
        auto saved = initial; saved.preferences = backend->requested;
        connect(session, &SettingsSession::applyFinished, this, [this] { delete session; session = nullptr; });
        backend->report(saved); QVERIFY(!session); backend = nullptr;
    }
private:
    FakeFontBackend *backend = nullptr;
    FontSettings *session = nullptr;
    FontState initial;
};
QTEST_MAIN(FontSettingsTest)
#include "font_settings_test.moc"
