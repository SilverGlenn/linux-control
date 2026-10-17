#include "NightTimeForecast.h"
#include <optional>
#include <chrono>
#include <KDarkLightSchedule>
#include <KHolidays/SunEvents>
#include <algorithm>
#include <cmath>

namespace NightTimeForecast {
bool validTimes(QTime sunrise, QTime sunset, int seconds)
{
    if (!sunrise.isValid() || !sunset.isValid() || seconds < 0) return false;
    const int distance = std::abs(sunrise.secsTo(sunset));
    return seconds <= std::min(distance, 86400 - distance);
}
QTime adjustedTime(QTime edited, QTime other, int seconds)
{
    if (!edited.isValid() || !other.isValid() || seconds < 0) return edited;
    const int distance = std::abs(edited.secsTo(other));
    if (seconds < std::min(distance, 86400 - distance)) return edited;
    return other.addSecs((edited < other ? -1 : 1) * seconds);
}
static Transition transition(const KDarkLightTransition &t)
{
    return {t.type() == KDarkLightTransition::Morning, t.startDateTime(), t.endDateTime()};
}
Result calculate(const Input &in)
{
    Result out;
    if (!in.reference.isValid()) { out.explanation = QStringLiteral("The forecast reference time is invalid."); return out; }
    if (in.source != Source::CustomTimes && in.source != Source::ManualLocation && in.source != Source::AutomaticLocation) {
        out.explanation = QStringLiteral("The schedule source is unsupported."); return out;
    }
    const auto &c = in.coordinate;
    const bool coordinateValid = c.available && std::isfinite(c.latitude) && std::isfinite(c.longitude)
        && c.latitude >= -90 && c.latitude <= 90 && c.longitude >= -180 && c.longitude <= 180;
    const bool automaticEvidence = c.origin == CoordinateOrigin::DaemonCache || c.origin == CoordinateOrigin::ExplicitAcquisition;
    const bool useSolar = in.source != Source::CustomTimes && coordinateValid
        && (in.source != Source::AutomaticLocation || automaticEvidence);
    if (in.source == Source::ManualLocation && !coordinateValid) {
        out.explanation = QStringLiteral("A valid manual coordinate is required."); return out;
    }
    // Automatic acquisition failure uses the saved custom-time tuple in the daemon.
    if (!useSolar && !validTimes(in.sunrise, in.sunset, in.transitionSeconds)) {
        out.explanation = QStringLiteral("Custom times must be valid and transitions must fit both day and night."); return out;
    }
    KDarkLightSchedule schedule;
    if (useSolar) {
        auto solar = KDarkLightSchedule::forecast(in.reference, c.latitude, c.longitude);
        if (solar) {
            schedule = *solar;
            out.origin = in.source == Source::ManualLocation ? Origin::ManualSolar
                : c.origin == CoordinateOrigin::ExplicitAcquisition ? Origin::AcquiredAutomaticSolar : Origin::CachedAutomaticSolar;
        } else {
            // KSolarDarkLightScheduler falls back to compiled defaults, not the saved Times group.
            schedule = KDarkLightSchedule::forecast(in.reference);
            out.origin = Origin::SolarUnavailableNativeDefaults;
            const KHolidays::SunEvents events(in.reference, c.latitude, c.longitude);
            out.fallback = events.isPolarDay() ? Fallback::PolarDay : events.isPolarNight() ? Fallback::PolarNight : Fallback::SolarUnavailable;
            out.explanation = QStringLiteral("Solar events are unavailable; the daemon uses 06:00/18:00 with 30-minute transitions.");
        }
    } else {
        schedule = KDarkLightSchedule::forecast(in.reference, in.sunrise, in.sunset, std::chrono::seconds(in.transitionSeconds));
        out.origin = in.source == Source::CustomTimes ? Origin::CustomTimes : Origin::AutomaticUnavailableCustomTimes;
        if (in.source == Source::AutomaticLocation) {
            out.fallback = Fallback::AutomaticUnavailable;
            out.explanation = QStringLiteral("No automatic coordinate is available; the daemon falls back to saved custom times.");
        }
    }
    for (const auto &cycle : schedule.cycles()) out.cycles.append({cycle.noonDateTime(), transition(cycle.morning()), transition(cycle.evening())});
    const auto previous = schedule.previousTransition(in.reference), next = schedule.nextTransition(in.reference);
    if (!previous || !next || out.cycles.isEmpty()) { out.explanation = QStringLiteral("No complete schedule could be computed."); return out; }
    out.previous = transition(*previous); out.next = transition(*next); out.valid = true;
    return out;
}
}
