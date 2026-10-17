#include "perf/WeiBenchmark.h"
#include "ui/pages/PerformancePage.h"

#include <QLabel>
#include <QFrame>
#include <QCoreApplication>
#include <QPointer>
#include <QProgressBar>
#include <QScrollArea>
#include <QtTest>

class BenchmarkLifecycleTest : public QObject {
    Q_OBJECT
private slots:
    void pageReplacementReattachesToRunningAndFinishedJob()
    {
        WeiBenchmark *engine = WeiBenchmark::instance();
        QVERIFY(engine);
        QVERIFY(engine->parent() == QCoreApplication::instance());
        QVERIFY(!engine->isRunning());

        engine->beginTestRun(45, QStringLiteral("Assessing memory (RAM)…"));
        QVERIFY(engine->isRunning());

        auto *firstPage = new PerformancePage(new QScrollArea);
        QPointer<PerformancePage> firstGuard(firstPage);
        auto *firstBar = firstPage->findChild<QProgressBar *>(
            QStringLiteral("weiProgressBar"));
        QVERIFY(firstBar);
        QCOMPARE(firstBar->value(), 45);
        auto *firstPanel = firstPage->findChild<QFrame *>(
            QStringLiteral("weiProgressPanel"));
        QVERIFY(firstPanel);
        QVERIFY(!firstPanel->isHidden());

        delete firstPage;
        QVERIFY(firstGuard.isNull());
        QVERIFY(engine->isRunning());

        PerformancePage secondPage(new QScrollArea);
        auto *secondBar = secondPage.findChild<QProgressBar *>(
            QStringLiteral("weiProgressBar"));
        auto *phase = secondPage.findChild<QLabel *>(
            QStringLiteral("weiProgressTitle"));
        QVERIFY(secondBar);
        QVERIFY(phase);
        QCOMPARE(secondBar->value(), 45);
        QCOMPARE(phase->text(), QStringLiteral("Assessing memory (RAM)…"));

        WeiResult result;
        result.valid = true;
        result.cpuScore = 8.5;
        result.baseScore = 8.5;
        engine->finishTestRun(result);

        QVERIFY(!engine->isRunning());
        QVERIFY(engine->hasResult());
        QCOMPARE(engine->result().baseScore, 8.5);
        auto *baseScore = secondPage.findChild<QLabel *>(
            QStringLiteral("weiBaseScore"));
        QVERIFY(baseScore);
        QCOMPARE(baseScore->text(), QStringLiteral("8.5"));
    }
};

QTEST_MAIN(BenchmarkLifecycleTest)
#include "benchmark_lifecycle_test.moc"
