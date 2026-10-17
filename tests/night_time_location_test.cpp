#include <QtTest>
#include <QTimeZone>
#include "platform/location/NightTimeLocation.h"
#include <limits>
class FakeLocationSource final : public NightTimeLocationSource {
public:
    explicit FakeLocationSource(QObject *parent) : NightTimeLocationSource(parent) {}
    int requests = 0, stops = 0, timeout = 0;
    bool synchronous = false;
    void request(int value) override { ++requests; timeout = value; if (synchronous) emit position(fix()); }
    void stop() override { ++stops; }
    static NightTimeLocationFix fix() { return {12.5, -30.5, QDateTime::currentDateTimeUtc(), 25}; }
};
class NightTimeLocationTest : public QObject {
    Q_OBJECT
private slots:
    void passiveUntilExplicitRequestAndOneShotCleanup() {
        int creations = 0; QPointer<FakeLocationSource> source;
        NightTimeLocation location([&](QObject *parent) { ++creations; source = new FakeLocationSource(parent); return source.data(); });
        QSignalSpy done(&location, &NightTimeLocation::finished);
        QCOMPARE(creations, 0); QVERIFY(!location.isBusy());
        location.requestLocation(42, 4000); QCOMPARE(creations, 1); QVERIFY(location.isBusy());
        QCOMPARE(source->requests, 1); QCOMPARE(source->timeout, 4000);
        emit source->position(FakeLocationSource::fix());
        QVERIFY(!location.isBusy()); QCOMPARE(source->stops, 1); QCOMPARE(done.size(), 1);
        QCOMPARE(done[0][0].toULongLong(), quint64(42)); QVERIFY(done[0][1].toBool());
        const auto fix = qvariant_cast<NightTimeLocationFix>(done[0][2]); QCOMPARE(fix.latitude, 12.5);
        emit source->position(FakeLocationSource::fix()); QCOMPARE(done.size(), 1);
    }
    void replacementAndCancellationRejectLateResults() {
        QList<QPointer<FakeLocationSource>> sources;
        NightTimeLocation location([&](QObject *parent) { auto *s = new FakeLocationSource(parent); sources.append(s); return s; });
        QSignalSpy done(&location, &NightTimeLocation::finished);
        location.requestLocation(1); auto old = sources.last();
        // A queued old signal is already in flight when the replacement starts.
        QMetaObject::invokeMethod(old, [old] { if (old) emit old->position(FakeLocationSource::fix()); }, Qt::QueuedConnection);
        location.requestLocation(2); QCOMPARE(old->stops, 1);
        QCoreApplication::processEvents(); QCOMPARE(done.size(), 0); QVERIFY(location.isBusy());
        auto current = sources.last(); emit current->position(FakeLocationSource::fix());
        QCOMPARE(done.size(), 1); QCOMPARE(done[0][0].toULongLong(), quint64(2));
        location.requestLocation(3); current = sources.last(); location.cancel();
        QCOMPARE(current->stops, 1); QVERIFY(!location.isBusy());
        emit current->failed(QStringLiteral("late")); QCOMPARE(done.size(), 1);
    }
    void invalidFixFailureAndUnavailableFactory() {
        QPointer<FakeLocationSource> source;
        NightTimeLocation location([&](QObject *parent) { source = new FakeLocationSource(parent); return source.data(); });
        QSignalSpy done(&location, &NightTimeLocation::finished);
        location.requestLocation(1); auto invalid = FakeLocationSource::fix(); invalid.latitude = std::numeric_limits<double>::quiet_NaN();
        emit source->position(invalid); QCOMPARE(done.size(), 1); QVERIFY(!done[0][1].toBool()); QVERIFY(!location.isBusy());
        location.requestLocation(2); emit source->failed(QStringLiteral("permission denied"));
        QCOMPARE(done.size(), 2); QCOMPARE(done[1][3].toString(), QStringLiteral("permission denied"));
        NightTimeLocation missing(NightTimeLocation::Factory{}); QSignalSpy unavailable(&missing, &NightTimeLocation::finished);
        missing.requestLocation(3); QCOMPARE(unavailable.size(), 1); QVERIFY(!unavailable[0][1].toBool()); QVERIFY(!missing.isBusy());
    }
    void boundedTimeoutAndSynchronousSource() {
        int creations = 0; QPointer<FakeLocationSource> source;
        NightTimeLocation location([&](QObject *parent) { ++creations; source = new FakeLocationSource(parent); source->synchronous = true; return source.data(); });
        QSignalSpy done(&location, &NightTimeLocation::finished);
        location.requestLocation(1, 0); location.requestLocation(2, 120001); QCOMPARE(creations, 0); QCOMPARE(done.size(), 2);
        location.requestLocation(3, 1000); QCOMPARE(done.size(), 3); QVERIFY(done[2][1].toBool()); QVERIFY(!location.isBusy());
        QCOMPARE(source->stops, 1);
    }
    void staleCachedFixDoesNotCompleteExplicitRequest() {
        QPointer<FakeLocationSource> source;
        NightTimeLocation location([&](QObject *parent) { source = new FakeLocationSource(parent); return source.data(); });
        QSignalSpy done(&location, &NightTimeLocation::finished);
        location.requestLocation(4);
        auto cached = FakeLocationSource::fix(); cached.timestamp = cached.timestamp.addSecs(-60);
        emit source->position(cached); QCOMPARE(done.size(), 0); QVERIFY(location.isBusy()); QCOMPARE(source->stops, 0);
        emit source->position(FakeLocationSource::fix()); QCOMPARE(done.size(), 1); QVERIFY(done[0][1].toBool());
        QCOMPARE(source->stops, 1);
    }
    void deadlineStopsHeldFakeRequest() {
        QPointer<FakeLocationSource> source;
        NightTimeLocation location([&](QObject *parent) { source = new FakeLocationSource(parent); return source.data(); });
        QSignalSpy done(&location, &NightTimeLocation::finished);
        location.requestLocation(77, 1000);
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 2500);
        QCOMPARE(done[0][0].toULongLong(), quint64(77)); QVERIFY(!done[0][1].toBool());
        QVERIFY(done[0][3].toString().contains(QStringLiteral("timed out"))); QVERIFY(!location.isBusy());
    }
    void destructionStopsOwnedAcquisition() {
        QPointer<FakeLocationSource> source; int stopCount = 0;
        auto *location = new NightTimeLocation([&](QObject *parent) { source = new FakeLocationSource(parent); return source.data(); });
        location->requestLocation(1);
        // Stop can synchronously emit; disconnected callbacks must not escape destruction.
        connect(source, &QObject::destroyed, this, [&] { ++stopCount; });
        delete location; QVERIFY(source.isNull()); QCOMPARE(stopCount, 1);
    }
};
QTEST_GUILESS_MAIN(NightTimeLocationTest)
#include "night_time_location_test.moc"
