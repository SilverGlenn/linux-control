#include "operations/SharedLoad.h"

#include <QCoreApplication>
#include <QSemaphore>
#include <QtTest>

#include <atomic>

class SharedLoadTest : public QObject {
    Q_OBJECT
private slots:
    void deletedSubscriberIsIgnoredAndNextPageJoinsActiveLoad()
    {
        SharedLoad<int> load(QStringLiteral("Fake read"),
                             QCoreApplication::instance());
        QSemaphore started;
        QSemaphore release;
        std::atomic_int workerStarts{0};

        auto delayedRead = [&]() {
            return std::function<int()>([&]() {
                ++workerStarts;
                started.release();
                release.tryAcquire(1, 5000);
                return 42;
            });
        };

        int deletedPageCallbacks = 0;
        auto *oldPage = new QObject;
        load.subscribe(oldPage, delayedRead(),
            [&](const int &, bool) { ++deletedPageCallbacks; });
        QVERIFY(started.tryAcquire(1, 3000));
        QVERIFY(load.isRunning());
        QVERIFY(OperationManager::instance()->activityTitles().contains(
            QStringLiteral("Fake read")));
        delete oldPage;

        QObject currentPage;
        QList<QPair<int, bool>> currentPageEvents;
        load.subscribe(&currentPage, delayedRead(),
            [&](const int &value, bool fresh) {
                currentPageEvents.append(qMakePair(value, fresh));
            });
        QCOMPARE(workerStarts.load(), 1);

        release.release();
        QTRY_VERIFY(!load.isRunning());
        QCOMPARE(deletedPageCallbacks, 0);
        QCOMPARE(currentPageEvents.size(), 1);
        QCOMPARE(currentPageEvents.first().first, 42);
        QVERIFY(currentPageEvents.first().second);
        QVERIFY(load.hasResult());
        QCOMPARE(load.result(), 42);
        QVERIFY(!OperationManager::instance()->activityTitles().contains(
            QStringLiteral("Fake read")));

        // A later visit immediately gets the cached value, then subscribes to
        // one fresh pass so the visible page can replace its saved snapshot.
        QObject returningPage;
        QList<QPair<int, bool>> returningPageEvents;
        load.subscribe(&returningPage, delayedRead(),
            [&](const int &value, bool fresh) {
                returningPageEvents.append(qMakePair(value, fresh));
            });
        QCOMPARE(returningPageEvents.size(), 1);
        QCOMPARE(returningPageEvents.first().first, 42);
        QVERIFY(!returningPageEvents.first().second);
        QVERIFY(started.tryAcquire(1, 3000));
        QCOMPARE(workerStarts.load(), 2);

        release.release();
        QTRY_VERIFY(!load.isRunning());
        QCOMPARE(returningPageEvents.size(), 2);
        QVERIFY(returningPageEvents.last().second);
        QCOMPARE(returningPageEvents.last().first, 42);
    }
};

QTEST_GUILESS_MAIN(SharedLoadTest)
#include "shared_load_test.moc"
