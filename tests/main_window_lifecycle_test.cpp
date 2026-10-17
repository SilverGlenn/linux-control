#include "ui/MainWindow.h"
#include "ui/PageRegistry.h"
#include "ui/pages/PersonalizationPage.h"
#include "ui/SettingsNavigationGuard.h"
#include "aero_theme_fixture.h"
#include "operations/OperationManager.h"
#include "platform/kde/ScreenLockBackend.h"
#include <QtTest>
#include <QAbstractButton>
#include <QApplication>
#include <QDialog>
#include <QFile>
#include <QMessageBox>
#include <QScrollArea>
#include <QScopeGuard>
#include <QSemaphore>
#include <QTemporaryDir>
#include <QTimer>
#include <atomic>
#include <memory>

class LifecycleScreenLockService final : public ScreenLockService {
public:
    void query(quint64 token, const QString & = {}) override {
        ScreenLockRuntime runtime; runtime.installedVerified = true;
        emit queryFinished(token, true, runtime, {});
    }
    void configure(quint64 token, const QString &, quint64, Guard) override {
        QFAIL("An offline fixture must never request configure");
        emit configureFinished(token, false, QStringLiteral("Unexpected configure"));
    }
};

class MainWindowLifecycleTest final : public QObject {
    Q_OBJECT
    QTemporaryDir environment;
    QByteArray oldPath, oldConfig;
    static void answer(const QString &label) {
        QTimer::singleShot(0, qApp, [label]() {
            auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            if (!box) return;
            for (auto *button : box->buttons()) {
                QString text = button->text();
                text.remove('&');
                if (text == label) { button->click(); return; }
            }
        });
    }
    static void showInjectedPersonalization(MainWindow &window, AeroThemeSettings &session) {
        const auto path = PageRegistry::pathFor(PageId::Personalization);
        window.m_history = {QString(), path};
        window.m_historyIndex = 1;
        window.m_scroll->setWidget(new PersonalizationPage(new QScrollArea, &session, nullptr));
        window.setCrumbTrail(path.split('/'));
        window.updateNavButtons();
    }
private slots:
    void initTestCase() {
        QVERIFY(environment.isValid());
        oldPath = qgetenv("PATH");
        oldConfig = qgetenv("XDG_CONFIG_HOME");
        qputenv("XDG_CONFIG_HOME", environment.path().toLocal8Bit());
        qputenv("PATH", environment.path().toLocal8Bit());
        QFile tool(environment.filePath(QStringLiteral("kreadconfig6")));
        QVERIFY(tool.open(QIODevice::WriteOnly));
        tool.write("#!/bin/sh\nprintf 'test-a\\n'\n");
        tool.close();
        QVERIFY(tool.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        QCoreApplication::setOrganizationName(QStringLiteral("linux-control-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("window-lifecycle"));
    }
    void cleanupTestCase() {
        qputenv("PATH", oldPath);
        qputenv("XDG_CONFIG_HOME", oldConfig);
    }
    void cleanWindowCloses() {
        MainWindow window(nullptr, false);
        window.show();
        QVERIFY(!window.hasBackgroundWork());
        QVERIFY(window.close());
        QVERIFY(!window.isVisible());
    }
    void cancelPreservesHistoryAndDiscardMovesBack() {
        AeroThemeSettings session(new FixtureAeroThemeBackend);
        QTRY_VERIFY(!session.isBusy());
        MainWindow window(nullptr, false);
        window.show();
        showInjectedPersonalization(window, session);
        session.selectValue(QStringLiteral("aeroIntensity"), 60);
        answer(QStringLiteral("Cancel"));
        window.goBack();
        QCOMPARE(window.m_historyIndex, 1);
        QVERIFY(session.isDirty());
        answer(QStringLiteral("Discard"));
        window.goBack();
        QCOMPARE(window.m_historyIndex, 0);
        QVERIFY(!session.isDirty());
        QVERIFY(!window.m_navigationGuard->pending());
        QVERIFY(window.close());
    }
    void discardThenCloseRunsAfterTheOriginalCloseEvent() {
        AeroThemeSettings session(new FixtureAeroThemeBackend);
        QTRY_VERIFY(!session.isBusy());
        MainWindow window(nullptr, false);
        window.show();
        showInjectedPersonalization(window, session);
        session.selectValue(QStringLiteral("aeroIntensity"), 60);
        answer(QStringLiteral("Discard"));
        window.close();
        QTRY_VERIFY(!window.isVisible());
        QVERIFY(!session.isDirty());
    }
    void runningJobPreventsExitAndRemainsActive() {
        MainWindow window(nullptr, false);
        window.show();
        auto *job = OperationManager::instance()->startProcess(QStringLiteral("Fake transaction"),
            QStringLiteral("/bin/sleep"), {QStringLiteral("0.2")}, true);
        QVERIFY(job);
        answer(QStringLiteral("OK"));
        QVERIFY(!window.close());
        QVERIFY(window.isVisible());
        QVERIFY(job->active());
        QTRY_VERIFY(!job->active());
        QVERIFY(window.close());
    }
    void screenLockFileJobBlocksCloseAfterItsSessionIsDestroyed() {
        QTemporaryDir files; QVERIFY(files.isValid());
        const auto path = files.filePath(QStringLiteral("kscreenlockerrc"));
        QFile config(path); QVERIFY(config.open(QIODevice::WriteOnly));
        config.write("[Daemon]\nLockOnResume=true\n"); config.close();
        struct Gate { QSemaphore semaphore; std::atomic_bool entered{false}; };
        const auto gate = std::make_shared<Gate>();
        const auto release = qScopeGuard([gate] { gate->semaphore.release(); });
        KdeScreenLockBackend::ConfigContext context; context.fixture = true; context.softDeadlineMs = 20;
        context.beforeSync = [gate] { gate->entered = true; gate->semaphore.acquire(); };
        QPointer<LifecycleScreenLockService> service = new LifecycleScreenLockService;
        QPointer<ScreenLockSettings> session = new ScreenLockSettings(new KdeScreenLockBackend(path, service, context));
        QTRY_VERIFY(!session->isBusy()); QVERIFY(session->canEdit(ScreenLockKey::LockOnResume));
        MainWindow window(nullptr, false); window.show();
        session->selectValue(ScreenLockKey::LockOnResume, false); session->apply();
        QTRY_VERIFY(gate->entered.load()); QVERIFY(session->isBusy());
        QTRY_VERIFY(session->applySupportText().contains(QStringLiteral("continues"))
            || session->applySupportText().contains(QStringLiteral("longer")));
        delete session; QVERIFY(!session); QVERIFY(service);
        QVERIFY(OperationManager::instance()->activityTitles().contains(QStringLiteral("Saving Screen Locking")));
        QVERIFY(window.hasBackgroundWork()); answer(QStringLiteral("OK")); QVERIFY(!window.close());
        QVERIFY(window.isVisible()); QVERIFY(service);
        gate->semaphore.release();
        QTRY_VERIFY(!window.hasBackgroundWork()); QTRY_VERIFY(!service);
        QVERIFY(config.open(QIODevice::ReadOnly)); QVERIFY(config.readAll().contains("LockOnResume=false"));
        QVERIFY(window.close());
    }
    void mouseBackCannotDestroyAModalPage() {
        MainWindow window(nullptr, false);
        window.navigateTo(QStringLiteral("Programs"));
        window.show();
        const int before = window.m_historyIndex;
        QDialog modal(&window);
        modal.setModal(true);
        modal.show();
        QTRY_COMPARE(QApplication::activeModalWidget(), &modal);
        QTest::mousePress(&modal, Qt::XButton1);
        QCOMPARE(window.m_historyIndex, before);
        window.navigateHome(); // Covers queued navigation too.
        QCOMPARE(window.m_historyIndex, before);
        modal.close();
        QVERIFY(window.close());
    }
};
QTEST_MAIN(MainWindowLifecycleTest)
#include "main_window_lifecycle_test.moc"
