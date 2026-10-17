#include <QtTest>
#include <QTimeZone>
#include "settings/NightTimeForecast.h"
#include <limits>
using namespace NightTimeForecast;
class NightTimeForecastTest : public QObject {
    Q_OBJECT
    Input input() const {
        Input in; in.reference = QDateTime(QDate(2026, 3, 20), QTime(12, 0), QTimeZone::UTC); return in;
    }
private slots:
    void secondsAreNotMilliseconds() {
        auto in = input(); in.transitionSeconds = 1800;
        const auto result = calculate(in); QVERIFY(result.valid); QCOMPARE(result.origin, Origin::CustomTimes);
        QCOMPARE(result.cycles.size(), 8);
        for (const auto &cycle : result.cycles) {
            QCOMPARE(cycle.morning.start.secsTo(cycle.morning.end), qint64(1800));
            QCOMPARE(cycle.evening.start.secsTo(cycle.evening.end), qint64(1800));
        }
    }
    void automaticUnavailableUsesSavedTimes() {
        auto in = input(); in.source = Source::AutomaticLocation; in.sunrise = QTime(8, 0); in.sunset = QTime(20, 0); in.transitionSeconds = 3600;
        const auto r = calculate(in); QVERIFY(r.valid); QCOMPARE(r.origin, Origin::AutomaticUnavailableCustomTimes);
        QCOMPARE(r.fallback, Fallback::AutomaticUnavailable);
        QCOMPARE(r.cycles[1].morning.start.time(), QTime(8, 0));
        QCOMPARE(r.cycles[1].morning.start.secsTo(r.cycles[1].morning.end), qint64(3600));
    }
    void polarSolarUsesCompiledDefaults() {
        auto in = input(); in.reference = QDateTime(QDate(2026, 6, 21), QTime(12, 0), QTimeZone::UTC);
        in.source = Source::ManualLocation; in.coordinate = {true, 89, 0, CoordinateOrigin::SavedManual, {}};
        in.sunrise = QTime(9, 0); in.sunset = QTime(21, 0); in.transitionSeconds = 3600;
        const auto r = calculate(in); QVERIFY(r.valid); QCOMPARE(r.origin, Origin::SolarUnavailableNativeDefaults);
        QCOMPARE(r.fallback, Fallback::PolarDay); QCOMPARE(r.cycles[1].morning.start.time(), QTime(6, 0));
        QCOMPARE(r.cycles[1].morning.start.secsTo(r.cycles[1].morning.end), qint64(1800));
    }
    void solarOriginsAndInvalidCoordinates() {
        auto in = input(); in.source = Source::AutomaticLocation;
        in.coordinate = {true, 20, 30, CoordinateOrigin::DaemonCache, {}};
        auto r = calculate(in); QVERIFY(r.valid); QCOMPARE(r.origin, Origin::CachedAutomaticSolar);
        in.coordinate.origin = CoordinateOrigin::ExplicitAcquisition;
        r = calculate(in); QVERIFY(r.valid); QCOMPARE(r.origin, Origin::AcquiredAutomaticSolar);
        in.source = Source::ManualLocation; in.coordinate.latitude = std::numeric_limits<double>::quiet_NaN();
        QVERIFY(!calculate(in).valid);
        in.coordinate.latitude = 91; QVERIFY(!calculate(in).valid);
    }
    void fixedTimeAdjustmentAndDomains() {
        QVERIFY(validTimes(QTime(23, 0), QTime(11, 0), 36000));
        QVERIFY(!validTimes(QTime(6, 0), QTime(6, 0), 60));
        QVERIFY(validTimes(QTime(6, 0), QTime(18, 0), 59));
        QVERIFY(validTimes(QTime(6, 0), QTime(18, 0), 36001));
        QVERIFY(validTimes(QTime(6, 0), QTime(18, 0), 43200));
        QVERIFY(!validTimes(QTime(6, 0), QTime(18, 0), 43201));
        QVERIFY(validTimes(QTime(6, 0), QTime(6, 0), 0));
        QVERIFY(!validTimes(QTime(6, 0), QTime(18, 0), -1));
        QCOMPARE(adjustedTime(QTime(5, 59), QTime(6, 0), 1800), QTime(5, 30));
        QCOMPARE(adjustedTime(QTime(6, 0), QTime(6, 0), 1800), QTime(6, 30));
        QCOMPARE(adjustedTime(QTime(23, 59), QTime(0, 1), 1800), QTime(0, 31));
        auto in = input(); in.reference = {}; QVERIFY(!calculate(in).valid);
        in = input(); in.source = Source(99); QVERIFY(!calculate(in).valid);
    }
    void nativeDefaultsOutsideEditorDomainRemainComputable() {
        auto in = input(); in.transitionSeconds = 45;
        auto r = calculate(in); QVERIFY(r.valid);
        QCOMPARE(r.cycles[1].morning.start.secsTo(r.cycles[1].morning.end), qint64(45));
        in.transitionSeconds = 43200; r = calculate(in); QVERIFY(r.valid);
        QCOMPARE(r.cycles[1].morning.start.secsTo(r.cycles[1].morning.end), qint64(43200));
        in.transitionSeconds = 0; QVERIFY(calculate(in).valid);
        QCOMPARE(adjustedTime(QTime(6, 0), QTime(18, 0), 43200), QTime(6, 0));
    }
    void crossMidnightAndDstReference() {
        auto in = input(); in.sunrise = QTime(22, 0); in.sunset = QTime(10, 0);
        QVERIFY(calculate(in).valid);
        const QTimeZone zone("Europe/Berlin");
        if (!zone.isValid()) QSKIP("Time zone database is unavailable");
        in.reference = QDateTime(QDate(2026, 3, 29), QTime(12, 0), zone);
        const auto r = calculate(in); QVERIFY(r.valid);
        QVERIFY(r.previous.start.isValid()); QVERIFY(r.next.start.isValid());
        for (const auto &c : r.cycles) QCOMPARE(c.morning.start.secsTo(c.morning.end), qint64(1800));
    }
};
QTEST_GUILESS_MAIN(NightTimeForecastTest)
#include "night_time_forecast_test.moc"
