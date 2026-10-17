#include "operations/UpdateService.h"
#include "ui/pages/LinuxUpdatePage.h"
#include <QtTest>
#include <QFile>
#include <QDir>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QTemporaryDir>
#include <QTreeWidget>

class UpdateLifecycleTest final : public QObject {
    Q_OBJECT
private:
    QTemporaryDir m_dir;
    QByteArray m_oldPath, m_oldConfig;
    bool m_pathSet = false, m_configSet = false;
    bool writeScript(const QString &name, const QByteArray &body) {
        QFile file(m_dir.filePath(name));
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
        file.write("#!/bin/sh\n" + body);
        file.close();
        return file.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    }
    bool visibleLabel(QWidget *page, const QString &text) {
        for (auto *label : page->findChildren<QLabel *>())
            if (label->isVisible() && label->text().contains(text)) return true;
        return false;
    }
    LinuxUpdatePage *page() {
        auto *result = new LinuxUpdatePage(new QScrollArea);
        result->resize(900, 650);
        result->show();
        return result;
    }
private slots:
    void initTestCase() {
        QVERIFY(m_dir.isValid());
        m_pathSet = qEnvironmentVariableIsSet("PATH");
        m_configSet = qEnvironmentVariableIsSet("XDG_CONFIG_HOME");
        m_oldPath = qgetenv("PATH");
        m_oldConfig = qgetenv("XDG_CONFIG_HOME");
        qputenv("PATH", m_dir.path().toLocal8Bit());
        qputenv("XDG_CONFIG_HOME", m_dir.path().toLocal8Bit());
        QCoreApplication::setOrganizationName("linux-control-tests");
        QCoreApplication::setApplicationName("update-lifecycle-tests");
        QVERIFY(writeScript("pacman", "printf 'Name : sample\\nDownload Size : 1 MiB\\n'\n"));
        QVERIFY(writeScript("yay", "if [ \"$1\" = '-Si' ]; then\nprintf 'Name : sample\\nDownload Size : 1 MiB\\n'\nfi\nexit 0\n"));
    }
    void cleanupTestCase() {
        QTRY_VERIFY(!OperationManager::instance()->hasActiveJobs());
        if (m_pathSet) qputenv("PATH", m_oldPath); else qunsetenv("PATH");
        if (m_configSet) qputenv("XDG_CONFIG_HOME", m_oldConfig); else qunsetenv("XDG_CONFIG_HOME");
    }
    void checkSurvivesPageDeletionAndNewPageReceivesResult() {
        QVERIFY(writeScript("checkupdates", "/bin/sleep 0.15\nprintf 'sample 1.0 -> 1.1\\nsecond 2.0 -> 2.1\\n'\n"));
        auto *first = page();
        first->checkForUpdates();
        auto *service = UpdateService::instance();
        QCOMPARE(service->state().phase, UpdateService::Phase::Checking);
        const auto count = OperationManager::instance()->jobs().size();
        first->checkForUpdates();
        QCOMPARE(OperationManager::instance()->jobs().size(), count);
        delete first;
        auto *second = page();
        QVERIFY(visibleLabel(second, "Checking for updates"));
        QTRY_COMPARE(service->state().phase, UpdateService::Phase::Available);
        QTRY_VERIFY(visibleLabel(second, "Install updates for your computer"));
        QCOMPARE(service->state().packages.size(), 2);
        QTRY_VERIFY(!service->state().packageSizes.isEmpty());
        delete second;
    }
    void selectionAndDraftSurvivePageDeletionAndPartialUpgradeIsBlocked() {
        auto *service = UpdateService::instance();
        service->setSelection({"sample"});
        QVERIFY(service->installUpdates().contains("partial upgrades"));
        QVERIFY(!OperationManager::instance()->hasActiveMutatingJobs());
        auto *first = page();
        first->showSelectView();
        auto *tree = first->findChild<QTreeWidget *>();
        QVERIFY(tree);
        auto *group = tree->topLevelItem(0);
        QCOMPARE(group->childCount(), 2);
        for (int i = 0; i < group->childCount(); ++i)
            if (group->child(i)->data(0, Qt::UserRole).toString() == "second")
                group->child(i)->setCheckState(0, Qt::Checked);
        QVERIFY(service->state().checkedNames.contains("second"));
        delete first;
        auto *second = page();
        second->showSelectView();
        tree = second->findChild<QTreeWidget *>();
        group = tree->topLevelItem(0);
        QCOMPARE(group->checkState(0), Qt::Checked);
        delete second;
        service->setSelection({"sample", "second"});
    }
    void installSurvivesPageDeletionAndNewPageReceivesProgressAndCompletion() {
        QVERIFY(writeScript("pkexec", "printf ':: Retrieving packages...\\nsample-1.1.pkg.tar.zst\\n:: Processing package changes...\\nupgrading sample...\\n'\n/bin/sleep 0.25\nprintf 'upgrading second...\\n'\n"));
        auto *service = UpdateService::instance();
        auto *first = page();
        for (auto *button : first->findChildren<QPushButton *>())
            if (button->text() == "Install updates") button->click();
        QTRY_COMPARE(service->state().phase, UpdateService::Phase::Installing);
        QVERIFY(OperationManager::instance()->hasActiveMutatingJobs());
        QCOMPARE(service->state().installCount, 1);
        QVERIFY(service->installUpdates().contains("already running"));
        delete first;
        auto *second = page();
        QVERIFY(visibleLabel(second, "Installing updates"));
        QVERIFY(visibleLabel(second, "sample"));
        QTRY_COMPARE(service->state().phase, UpdateService::Phase::Done);
        QVERIFY(service->state().lastInstallOk);
        QVERIFY(service->state().output.contains("upgrading second"));
        QTRY_VERIFY(visibleLabel(second, "installed successfully"));
        QVERIFY(!OperationManager::instance()->hasActiveMutatingJobs());
        delete second;
        auto *third = page();
        QVERIFY(visibleLabel(third, "installed successfully"));
        delete third;
    }
    void failedInstallStartSurvivesNoViewAndReleasesTransaction() {
        QVERIFY(QFile::remove(m_dir.filePath("pkexec")));
        auto *service = UpdateService::instance();
        service->checkForUpdates();
        QTRY_COMPARE(service->state().phase, UpdateService::Phase::Available);
        QTRY_VERIFY(!OperationManager::instance()->hasActiveJobs());
        auto *first = page();
        QVERIFY(service->installUpdates().isEmpty());
        delete first;
        QTRY_COMPARE(service->state().phase, UpdateService::Phase::Done);
        QVERIFY(!service->state().lastInstallOk);
        QVERIFY(service->state().lastInstall.isValid());
        QVERIFY(!service->state().error.isEmpty());
        QVERIFY(!OperationManager::instance()->hasActiveMutatingJobs());
        auto *second = page();
        QVERIFY(visibleLabel(second, "failed to install"));
        delete second;
    }
};

QTEST_MAIN(UpdateLifecycleTest)
#include "update_lifecycle_test.moc"
