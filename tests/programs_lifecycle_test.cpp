#include "packages/ProgramsService.h"
#include "operations/OperationManager.h"
#include "ui/pages/ProgramsFeaturesPage.h"
#include <QtTest>
#include <QScrollArea>
#include <QTreeWidget>
#include <QLabel>
#include <QSemaphore>
#include <QTemporaryDir>
#include <QFile>
#include <QDir>
#include <atomic>

class ProgramsLifecycleTest final : public QObject {
    Q_OBJECT
private slots:
    void inventorySurvivesViewDestruction() {
        QSemaphore release;
        std::atomic<int> queries{0};
        ProgramsService service([&] {
            ++queries;
            release.acquire();
            ProgramsService::LoadResult result;
            ProgramsService::ProgramInfo info;
            info.name = QStringLiteral("test-program");
            info.display = QStringLiteral("Test Program");
            result.programs << info;
            return result;
        });
        auto *page = new ProgramsFeaturesPage(new QScrollArea, nullptr, &service);
        QTRY_COMPARE(queries.load(), 1);
        delete page;
        auto *replacement = new ProgramsFeaturesPage(new QScrollArea, nullptr, &service);
        release.release();
        QTRY_VERIFY(!service.isLoading());
        auto *tree = replacement->findChild<QTreeWidget *>();
        QVERIFY(tree);
        QCOMPARE(tree->topLevelItem(0)->text(0), QStringLiteral("Test Program"));
        QCOMPARE(queries.load(), 1);
        delete replacement;
    }
    void packageActionSurvivesViewDestruction() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        auto executable = [&](const QString &name, const QByteArray &code) {
            QFile file(dir.filePath(name));
            if (!file.open(QIODevice::WriteOnly)) return false;
            if (file.write(code) != code.size()) return false;
            file.close();
            return file.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        };
        QVERIFY(executable(QStringLiteral("pacman"), "#!/bin/sh\nexit 0\n"));
        QVERIFY(executable(QStringLiteral("pkexec"), "#!/bin/sh\nprintf 'transaction started\\n'\n/bin/sleep 0.15\nprintf 'transaction finished\\n'\n"));
        const QByteArray oldPath = qgetenv("PATH");
        qputenv("PATH", dir.path().toLocal8Bit());
        struct Restore { QByteArray path; ~Restore() { qputenv("PATH", path); } } restore{oldPath};
        ProgramsService service([] { return ProgramsService::LoadResult{}; });
        auto *page = new ProgramsFeaturesPage(new QScrollArea, nullptr, &service);
        service.startAction(QStringLiteral("test-program"), QStringLiteral("Test Program"), false);
        QVERIFY(service.isActing());
        QVERIFY(OperationManager::instance()->hasActiveMutatingJobs());
        delete page;
        auto *replacement = new ProgramsFeaturesPage(new QScrollArea, nullptr, &service);
        QTRY_VERIFY(!service.isActing());
        QVERIFY(!service.actionFailed());
        auto *status = replacement->findChild<QLabel *>(QStringLiteral("programsActionStatus"));
        QVERIFY(status);
        QVERIFY(status->text().contains(QStringLiteral("was uninstalled")));
        QVERIFY(!OperationManager::instance()->hasActiveMutatingJobs());
        delete replacement;
        QTRY_VERIFY(!service.isLoading());
    }
};
QTEST_MAIN(ProgramsLifecycleTest)
#include "programs_lifecycle_test.moc"
