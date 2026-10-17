#include <QtTest>
#include <AeroQt/stylesheet.h>
#include <QComboBox>
#include <QDir>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTreeWidget>
#include "ui/pages/DefaultAppsPage.h"

class AppsPageBackend final : public DefaultAppsBackend {
public:
    int writes = 0;
    bool delay = false;
    quint64 pendingToken = 0;
    DefaultAppsSnapshot state;
    DefaultAppsPreferences requested;
    AppsPageBackend() {
        state.writable = true; state.revision = "initial";
        state.mimeTypes = {"application/json", "text/plain", "x-scheme-handler/http"};
        DefaultApplication a; a.id = "old.desktop"; a.name = "Aero Browser"; a.exec = "never-run";
        a.mimeTypes = {"x-scheme-handler/http"}; a.identity = "old";
        DefaultApplication b = a; b.id = "new.desktop"; b.name = "Other Browser"; b.identity = "new";
        DefaultApplication c = a; c.id = "org.kde.kate.desktop"; c.name = "Aero Editor"; c.categories = {"TextEditor"};
        c.mimeTypes = {"text/plain", "application/json"}; c.identity = "editor";
        state.applications = {a, b, c}; state.preferences.mimeDefaults["x-scheme-handler/http"] = a.id;
    }
    void readCurrent(quint64 token) override { emit readFinished(token, true, state, {}); }
    void writeChanges(quint64 token, const DefaultAppsSnapshot &, const DefaultAppsPreferences &p) override
    { ++writes; pendingToken = token; requested = p; if (!delay) complete(); }
    void complete() { state.preferences = requested; state.revision = "saved"; emit writeFinished(pendingToken, DefaultAppsOutcome::Success, state, {}); }
};
class DefaultAppsPageTest final : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { if (qEnvironmentVariableIsSet("LINUX_CONTROL_UI_CAPTURE_DIR")) Aero::registerStylesheet(qApp); }
    void nativeRoleControlsStageAndSurvivePageRecreation() {
        auto *backend = new AppsPageBackend; DefaultAppsSettings session(backend); QTRY_VERIFY(session.isReady());
        auto *page = new DefaultAppsPage(nullptr, &session, nullptr);
        auto *browser = page->findChild<QComboBox *>("defaultRole_browser"); QVERIFY(browser); QVERIFY(!browser->accessibleName().isEmpty());
        QCOMPARE(page->findChildren<QComboBox *>().size(), 13);
        browser->setCurrentIndex(browser->findData("new.desktop")); emit browser->activated(browser->currentIndex());
        QVERIFY(session.isDirty()); QCOMPARE(backend->writes, 0); delete page;
        page = new DefaultAppsPage(nullptr, &session, nullptr);
        QCOMPARE(page->settingsSession(), static_cast<SettingsSession *>(&session));
        QCOMPARE(page->findChild<QComboBox *>("defaultRole_browser")->currentData().toString(), "new.desktop");
        const QString capture = qEnvironmentVariable("LINUX_CONTROL_UI_CAPTURE_DIR");
        if (!capture.isEmpty()) { QDir().mkpath(capture); page->resize(1100, 1050); page->show(); QApplication::processEvents(); QVERIFY(page->grab().save(capture + "/default-programs.png")); }
        page->findChild<QPushButton *>("applyDefaultApps")->click(); QCOMPARE(backend->writes, 1); QVERIFY(!session.isDirty()); delete page;
    }
    void fileTypeSearchAndCompatiblePickerStageOneAssociation() {
        auto *backend = new AppsPageBackend; DefaultAppsSettings session(backend); QTRY_VERIFY(session.isReady()); DefaultAppsPage page(nullptr, &session, nullptr);
        auto *search = page.findChild<QLineEdit *>("defaultAppsSearch"); search->setText(".txt");
        auto *tree = page.findChild<QTreeWidget *>("defaultAppsMimeTypes"); QVERIFY(tree->currentItem());
        QCOMPARE(tree->currentItem()->data(0, Qt::UserRole).toString(), "text/plain");
        auto *apps = page.findChild<QComboBox *>("defaultAppsMimeApplication"); QCOMPARE(apps->count(), 1);
        QCOMPARE(apps->currentData().toString(), "org.kde.kate.desktop");
        page.findChild<QPushButton *>("changeDefaultMimeProgram")->click();
        QCOMPARE(session.staged().mimeDefaults.value("text/plain"), "org.kde.kate.desktop");
        QCOMPARE(session.staged().mimeDefaults.value("x-scheme-handler/http"), "old.desktop"); QCOMPARE(backend->writes, 0);
        search->setText("no matching file type"); QVERIFY(!tree->currentItem()); QVERIFY(!page.findChild<QPushButton *>("changeDefaultMimeProgram")->isEnabled());
    }
    void busyApplySurvivesViewDestruction() {
        auto *backend = new AppsPageBackend; backend->delay = true; DefaultAppsSettings session(backend); QTRY_VERIFY(session.isReady());
        auto *page = new DefaultAppsPage(nullptr, &session, nullptr); session.selectRole(DefaultAppRole::Browser, "new.desktop");
        page->findChild<QPushButton *>("applyDefaultApps")->click(); QVERIFY(session.isBusy()); delete page;
        page = new DefaultAppsPage(nullptr, &session, nullptr); QVERIFY(!page->findChild<QComboBox *>("defaultRole_browser")->isEnabled());
        backend->complete(); QVERIFY(!session.isBusy()); QVERIFY(!session.isDirty()); QVERIFY(page->findChild<QLabel *>("defaultAppsMessage")->text().contains("adoption has not been verified")); delete page;
    }
    void missingRoleApplicationsAndProtectedTypesAreDisabled() {
        auto *backend = new AppsPageBackend; backend->state.unavailableMimeTypes["text/plain"] = "Protected association";
        DefaultAppsSettings session(backend); QTRY_VERIFY(session.isReady()); DefaultAppsPage page(nullptr, &session, nullptr);
        QVERIFY(!page.findChild<QComboBox *>("defaultRole_terminal")->isEnabled());
        page.findChild<QLineEdit *>("defaultAppsSearch")->setText("text/plain");
        QVERIFY(!page.findChild<QPushButton *>("changeDefaultMimeProgram")->isEnabled()); QCOMPARE(backend->writes, 0);
    }
};
QTEST_MAIN(DefaultAppsPageTest)
#include "default_apps_page_test.moc"
