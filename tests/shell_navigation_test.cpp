#include "ui/MainWindow.h"
#include "ui/ActionCatalog.h"
#include "ui/CatalogView.h"
#include "ui/CategoryWidget.h"
#include "ui/LinkLabel.h"
#include "ui/PageRegistry.h"
#include "ui/SettingsNavigationGuard.h"
#include "ui/pages/PersonalizationPage.h"
#include "aero_theme_fixture.h"
#include <AeroQt/stylesheet.h>
#include <QtTest>
#include <QInputMethodEvent>
#include <QAccessible>
#include <QAbstractButton>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QScrollArea>
#include <QSettings>
#include <QPushButton>
#include <QTemporaryDir>
#include <QToolButton>
#include <QTimer>

class ShellNavigationTest final : public QObject {
    Q_OBJECT
    QTemporaryDir environment;
    QByteArray oldPath, oldConfig;
    static void cancelPrompt() {
        QTimer::singleShot(0, qApp, []() {
            if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
                box->button(QMessageBox::Cancel)->click();
        });
    }
    static QListWidget *catalogItems(MainWindow &window) {
        return window.m_scroll->widget()->findChild<QListWidget *>(QStringLiteral("catalogItems"));
    }
    static LinkLabel *actionLink(MainWindow &window, ActionId id) {
        for (auto *link : window.m_scroll->widget()->findChildren<LinkLabel *>())
            if (link->property("actionId").isValid() && link->property("actionId").toInt() == int(id))
                return link;
        return nullptr;
    }
private slots:
    void initTestCase() {
        QVERIFY(environment.isValid());
        oldPath = qgetenv("PATH"); oldConfig = qgetenv("XDG_CONFIG_HOME");
        qputenv("PATH", environment.path().toLocal8Bit());
        qputenv("XDG_CONFIG_HOME", environment.path().toLocal8Bit());
        QCoreApplication::setOrganizationName(QStringLiteral("linux-control-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("shell-navigation"));
        QFile tool(environment.filePath(QStringLiteral("kreadconfig6")));
        QVERIFY(tool.open(QIODevice::WriteOnly));
        tool.write("#!/bin/sh\nprintf 'test-a\\n'\n"); tool.close();
        QVERIFY(tool.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    }
    void init() { QSettings().clear(); }
    void cleanupTestCase() { qputenv("PATH", oldPath); qputenv("XDG_CONFIG_HOME", oldConfig); }

    void liveSearchAndHistoryUseOneStepPerQuery() {
        MainWindow window(nullptr, false);
        window.show();
        window.navigateTo(QStringLiteral("Hardware and Sound"));
        const int before = window.m_historyIndex;
        window.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&window));
        QTest::keyClick(&window, Qt::Key_F, Qt::ControlModifier);
        QTRY_VERIFY(window.m_searchBox->hasFocus());
        QVERIFY(window.m_searchBox->isEnabled());
        QCOMPARE(window.m_searchBox->accessibleName(), QStringLiteral("Search Control Panel"));
        QTest::keyClicks(window.m_searchBox, QStringLiteral("Windows Update"));
        QTRY_COMPARE(window.m_visibleSearch, QStringLiteral("Windows Update"));
        QCOMPARE(window.m_historyIndex, before + 1);
        QVERIFY(catalogItems(window));
        QTest::keyClicks(window.m_searchBox, QStringLiteral(" history"));
        QTRY_COMPARE(window.m_visibleSearch, QStringLiteral("Windows Update history"));
        QCOMPARE(window.m_historyIndex, before + 1);
        window.goBack();
        QCOMPARE(window.m_historyIndex, before);
        QCOMPARE(window.m_searchBox->text(), QString());
        window.goForward();
        QCOMPARE(window.m_searchBox->text(), QStringLiteral("Windows Update history"));
        QVERIFY(catalogItems(window));
        QTest::keyClick(&window, Qt::Key_Escape);
        QTRY_VERIFY(window.m_visibleSearch.isEmpty());
        QVERIFY(!catalogItems(window));
    }

    void accessibleEditingSearchesAndPreservesHistory() {
        MainWindow window(nullptr, false); window.show();
        window.navigateTo(QStringLiteral("Hardware and Sound"));
        const int before = window.m_historyIndex;
        auto *accessible = QAccessible::queryAccessibleInterface(window.m_searchBox);
        QVERIFY(accessible);
        auto *editable = accessible->editableTextInterface();
        QVERIFY(editable);
        QSignalSpy changed(window.m_searchBox, &QLineEdit::textChanged);
        QSignalSpy edited(window.m_searchBox, &QLineEdit::textEdited);
        // This is Qt's editable accessibility interface used by assistive
        // clients, which can update text without a keyboard textEdited signal.
        editable->replaceText(0, 0, QStringLiteral("Soun"));
        editable->insertText(4, QStringLiteral("d"));
        QCOMPARE(window.m_searchBox->text(), QStringLiteral("Sound"));
        QVERIFY(!changed.isEmpty());
        QCOMPARE(edited.size(), 0);
        QTRY_COMPARE(window.m_visibleSearch, QStringLiteral("Sound"));
        QCOMPARE(window.m_historyIndex, before + 1);
        auto *list = catalogItems(window);
        QVERIFY(list);
        bool foundSound = false;
        for (int i = 0; i < list->count(); ++i)
            foundSound |= list->item(i)->data(Qt::UserRole).toInt() == int(ActionId::Sound);
        QVERIFY(foundSound);
        editable->deleteText(0, 5);
        editable->insertText(0, QStringLiteral("Night Light"));
        QTRY_COMPARE(window.m_visibleSearch, QStringLiteral("Night Light"));
        QCOMPARE(window.m_historyIndex, before + 1);
        window.goBack();
        QCOMPARE(window.m_historyIndex, before);
        QCOMPARE(window.m_searchBox->text(), QString());
        QTest::qWait(window.m_searchTimer->interval() + 50);
        QCOMPARE(window.m_historyIndex, before);
        QVERIFY(window.m_visibleSearch.isEmpty());
        QVERIFY(!catalogItems(window));
        window.goForward();
        QCOMPARE(window.m_searchBox->text(), QStringLiteral("Night Light"));
        QTest::qWait(window.m_searchTimer->interval() + 50);
        QCOMPARE(window.m_historyIndex, before + 1);
        QCOMPARE(window.m_visibleSearch, QStringLiteral("Night Light"));
        // An accessible edit on a historical query branches in the same step.
        window.activateAction(ActionId::LinuxUpdate);
        window.goBack();
        editable->replaceText(0, window.m_searchBox->text().size(), QStringLiteral("Sound"));
        QTRY_COMPARE(window.m_visibleSearch, QStringLiteral("Sound"));
        QCOMPARE(window.m_historyIndex, before + 1);
        QCOMPARE(window.m_history.size(), before + 2);
        QVERIFY(!window.m_forwardBtn->isEnabled());
    }

    void readOnlySearchTextDoesNotQueueNavigation() {
        MainWindow window(nullptr, false); window.show();
        const auto history = window.m_history;
        const int before = window.m_historyIndex;
        window.m_searchBox->setReadOnly(true);
        // Even programmatic changes emit textChanged on a read-only line edit.
        // They must not leave a query queued behind a navigation decision.
        window.m_searchBox->setText(QStringLiteral("Sound"));
        QVERIFY(!window.m_searchTimer->isActive());
        window.m_searchBox->setReadOnly(false);
        QTest::qWait(window.m_searchTimer->interval() + 50);
        QCOMPARE(window.m_history, history);
        QCOMPARE(window.m_historyIndex, before);
        QVERIFY(window.m_visibleSearch.isEmpty());
        QVERIFY(!catalogItems(window));
    }

    void accessibleSearchCancellationRetainsDraftPageAndHistory() {
        auto *backend = new FixtureAeroThemeBackend;
        backend->refuseWrite = true;
        AeroThemeSettings session(backend);
        QTRY_VERIFY(!session.isBusy());
        MainWindow window(nullptr, false); window.show();
        const auto path = PageRegistry::pathFor(PageId::Personalization);
        window.m_history = {QString(), path}; window.m_historyIndex = 1;
        auto *page = new PersonalizationPage(new QScrollArea, &session, nullptr);
        window.m_scroll->setWidget(page);
        session.selectValue(QStringLiteral("aeroIntensity"), 60);
        QVERIFY(session.isDirty());
        auto *accessible = QAccessible::queryAccessibleInterface(window.m_searchBox);
        QVERIFY(accessible);
        auto *editable = accessible->editableTextInterface();
        QVERIFY(editable);
        int decisions = 0;
        connect(window.m_navigationGuard, &SettingsNavigationGuard::pendingChanged, &window, [&]() {
            if (window.m_navigationGuard->pending()) {
                ++decisions;
                QVERIFY(window.m_searchBox->isReadOnly());
                cancelPrompt();
            }
        });
        editable->replaceText(0, 0, QStringLiteral("Sound"));
        QTRY_COMPARE(decisions, 1);
        QTRY_VERIFY(!window.m_navigationGuard->pending());
        QTRY_COMPARE(window.m_searchBox->text(), QString());
        QCOMPARE(window.m_scroll->widget(), page);
        QCOMPARE(window.m_history, QStringList({QString(), path}));
        QCOMPARE(window.m_historyIndex, 1);
        QVERIFY(window.m_visibleSearch.isEmpty());
        QVERIFY(session.isDirty());
        QTest::qWait(window.m_searchTimer->interval() + 50);
        QCOMPARE(decisions, 1);
        QCOMPARE(window.m_scroll->widget(), page);
        QVERIFY(session.isDirty());
    }

    void deferredNavigationRefreshPreservesNewSearchText() {
        MainWindow window(nullptr, false); window.show();
        QTRY_VERIFY(!window.hasBackgroundWork());
        window.m_searchBox->setText(QStringLiteral("Windows Update"));
        window.navigateSearch(window.m_searchBox->text());
        QCOMPARE(window.m_visibleSearch, QStringLiteral("Windows Update"));
        // A committed input event arrives before the navigation guard's queued
        // refresh. It must survive that callback and reach the debounce timer.
        QInputMethodEvent input; input.setCommitString(QStringLiteral(" history"));
        QCoreApplication::sendEvent(window.m_searchBox, &input);
        QCOMPARE(window.m_searchBox->text(), QStringLiteral("Windows Update history"));
        QTRY_COMPARE(window.m_visibleSearch, QStringLiteral("Windows Update history"));
    }

    void searchActivationAndCrumbKeyboardNavigation() {
        MainWindow window(nullptr, false);
        window.show();
        window.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&window));
        window.navigateSearch(QStringLiteral("Windows Update"));
        auto *list = catalogItems(window);
        QVERIFY(list);
        int row = -1;
        for (int i = 0; i < list->count(); ++i)
            if (list->item(i)->data(Qt::UserRole).toInt() == int(ActionId::LinuxUpdate)) row = i;
        QVERIFY(row >= 0);
        list->setCurrentRow(row); list->setFocus();
        QTRY_VERIFY(list->hasFocus());
        QTest::keyClick(list, Qt::Key_Return);
        QTRY_COMPARE(window.m_history.last(), PageRegistry::pathFor(PageId::LinuxUpdate));
        QTRY_VERIFY(QApplication::focusWidget()
            && window.m_scroll->widget()->isAncestorOf(QApplication::focusWidget()));
        // The typed CheckUpdates callback has its provider explanation here.
        auto *check = actionLink(window, ActionId::CheckForUpdates);
        QVERIFY(check); QVERIFY(!check->isEnabled()); QVERIFY(!check->toolTip().isEmpty());
        window.goBack();
        QCOMPARE(window.m_searchBox->text(), QStringLiteral("Windows Update"));
        window.m_homeCrumb->setFocus();
        QTest::keyClick(window.m_homeCrumb, Qt::Key_Space);
        QTRY_VERIFY(window.m_history.last().isEmpty());
    }

    void editingPreviousSearchBranchesHistory() {
        MainWindow window(nullptr, false); window.show();
        window.navigateSearch(QStringLiteral("fonts"));
        window.activateAction(ActionId::LinuxUpdate);
        window.goBack();
        const int before = window.m_historyIndex;
        window.navigateSearch(QStringLiteral("sound"));
        QCOMPARE(window.m_historyIndex, before);
        QCOMPARE(window.m_history.size(), before + 1);
        QVERIFY(!window.m_forwardBtn->isEnabled());
        window.goForward();
        QCOMPARE(window.m_visibleSearch, QStringLiteral("sound"));
    }

    void allViewMenuActionsWorkAndSelectionPersists() {
        MainWindow window(nullptr, false); window.show();
        auto *viewBy = window.m_scroll->widget()->findChild<QToolButton *>(QStringLiteral("viewBy"));
        QVERIFY(viewBy); QVERIFY(viewBy->menu());
        QCOMPARE(viewBy->menu()->actions().size(), 3);
        for (auto *action : viewBy->menu()->actions()) QVERIFY(action->isEnabled());
        viewBy->menu()->actions().at(1)->trigger();
        QTRY_VERIFY(catalogItems(window));
        QCOMPARE(catalogItems(window)->iconSize(), QSize(32, 32));
        const int count = catalogItems(window)->count();
        window.setViewMode(MainWindow::ViewMode::SmallIcons);
        QCOMPARE(catalogItems(window)->iconSize(), QSize(16, 16));
        QCOMPARE(catalogItems(window)->count(), count);
        window.navigateTo(QStringLiteral("Programs")); window.navigateHome();
        QCOMPARE(catalogItems(window)->iconSize(), QSize(16, 16));
        MainWindow another(nullptr, false);
        QCOMPARE(catalogItems(another)->iconSize(), QSize(16, 16));
        window.setViewMode(MainWindow::ViewMode::Category);
        QVERIFY(!catalogItems(window));
        QCOMPARE(window.m_scroll->widget()->findChildren<CategoryWidget *>().size(), 8);
    }

    void categoryUsesCatalogTargetsAndAvailability() {
        MainWindow window(nullptr, false); window.show();
        window.navigateTo(QStringLiteral("System and Security"));
        auto *update = actionLink(window, ActionId::LinuxUpdate);
        auto *backup = actionLink(window, ActionId::BackUpYourComputer);
        QVERIFY(update); QVERIFY(backup);
        QVERIFY(update->isEnabled()); QVERIFY(!backup->isEnabled());
        QCOMPARE(backup->focusPolicy(), Qt::NoFocus);
        QVERIFY(!backup->accessibleDescription().isEmpty());
        update->setFocus(); QTest::keyClick(update, Qt::Key_Space);
        QTRY_COMPARE(window.m_history.last(), PageRegistry::pathFor(PageId::LinuxUpdate));
    }

    void cancelSearchRetainsStagedPageAndHistory() {
        auto *backend = new FixtureAeroThemeBackend;
        backend->refuseWrite = true;
        AeroThemeSettings session(backend);
        QTRY_VERIFY(!session.isBusy());
        MainWindow window(nullptr, false); window.show();
        const auto path = PageRegistry::pathFor(PageId::Personalization);
        window.m_history = {QString(), path}; window.m_historyIndex = 1;
        window.m_scroll->setWidget(new PersonalizationPage(new QScrollArea, &session, nullptr));
        session.selectValue(QStringLiteral("aeroIntensity"), 60);
        QVERIFY(session.isDirty());
        cancelPrompt();
        window.navigateSearch(QStringLiteral("fonts"));
        QCoreApplication::processEvents();
        QCOMPARE(window.m_history.last(), path);
        QCOMPARE(window.m_historyIndex, 1);
        QVERIFY(session.isDirty());
        QCOMPARE(window.m_searchBox->text(), QString());
        QVERIFY(!window.m_navigationGuard->pending());
    }

    void captureWindowsStyleIfRequested() {
        const QString output = qEnvironmentVariable("LINUX_CONTROL_UI_CAPTURE_DIR");
        if (output.isEmpty()) QSKIP("Optional visual captures");
        QVERIFY(QDir().mkpath(output));
        Aero::registerStylesheet(qApp);
        MainWindow window(nullptr, false); window.show();
        auto capture = [&](const QString &name) {
            QTest::qWait(300);
            return window.grab().save(output + QLatin1Char('/') + name + QStringLiteral(".png"));
        };
        QVERIFY(capture(QStringLiteral("category")));
        window.setViewMode(MainWindow::ViewMode::LargeIcons);
        QVERIFY(capture(QStringLiteral("large-icons")));
        window.setViewMode(MainWindow::ViewMode::SmallIcons);
        QVERIFY(capture(QStringLiteral("small-icons")));
        window.navigateSearch(QStringLiteral("Windows Firewall"));
        auto *list = catalogItems(window); list->setCurrentRow(0); list->setFocus();
        QVERIFY(capture(QStringLiteral("search")));
        window.navigateSearch(QStringLiteral("Back up"));
        QVERIFY(capture(QStringLiteral("unavailable-search")));
        window.navigateSearch(QStringLiteral("zzzznoresults"));
        QVERIFY(capture(QStringLiteral("no-results")));
    }
};
QTEST_MAIN(ShellNavigationTest)
#include "shell_navigation_test.moc"
