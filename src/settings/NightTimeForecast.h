#pragma once
#include <QDateTime>
#include <QList>
#include <QString>
#include <QTime>

namespace NightTimeForecast {
enum class Source { ManualLocation, CustomTimes, AutomaticLocation };
enum class CoordinateOrigin { None, SavedManual, DaemonCache, ExplicitAcquisition };
enum class Origin { CustomTimes, ManualSolar, CachedAutomaticSolar, AcquiredAutomaticSolar, AutomaticUnavailableCustomTimes, SolarUnavailableNativeDefaults };
enum class Fallback { None, AutomaticUnavailable, PolarDay, PolarNight, SolarUnavailable };
struct Coordinate {
    bool available = false;
    double latitude = 0;
    double longitude = 0;
    CoordinateOrigin origin = CoordinateOrigin::None;
    QDateTime timestamp;
};
struct Input {
    QDateTime reference;
    Source source = Source::CustomTimes;
    QTime sunrise = QTime(6, 0);
    QTime sunset = QTime(18, 0);
    int transitionSeconds = 1800;
    Coordinate coordinate;
};
struct Transition { bool morning = false; QDateTime start, end; };
struct Cycle { QDateTime noon; Transition morning, evening; };
struct Result {
    bool valid = false;
    Origin origin = Origin::CustomTimes;
    Fallback fallback = Fallback::None;
    QString explanation;
    QList<Cycle> cycles;
    Transition previous, next;
};
// Pure computation: never constructs a provider, subscribes, or changes preferences.
Result calculate(const Input &input);
bool validTimes(QTime sunrise, QTime sunset, int transitionSeconds);
QTime adjustedTime(QTime edited, QTime other, int transitionSeconds);
}
