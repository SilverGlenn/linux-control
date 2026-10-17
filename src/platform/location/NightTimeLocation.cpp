#include "NightTimeLocation.h"
#include <QGeoPositionInfoSource>
#include <QGeoPositionInfo>
#include <QVariantMap>
#include <cmath>

namespace {
class QtLocationSource final : public NightTimeLocationSource {
public:
    explicit QtLocationSource(QObject *parent) : NightTimeLocationSource(parent) {
        m_source = QGeoPositionInfoSource::createDefaultSource(
            QVariantMap{{QStringLiteral("desktopId"), QStringLiteral("linux-controlpanel")}}, this);
        if (!m_source) return;
        connect(m_source, &QGeoPositionInfoSource::positionUpdated, this, [this](const QGeoPositionInfo &info) {
            const auto coordinate = info.coordinate();
            emit position({coordinate.latitude(), coordinate.longitude(), info.timestamp(),
                info.hasAttribute(QGeoPositionInfo::HorizontalAccuracy) ? info.attribute(QGeoPositionInfo::HorizontalAccuracy) : -1});
        });
        connect(m_source, &QGeoPositionInfoSource::errorOccurred, this, [this](QGeoPositionInfoSource::Error error) {
            if (error != QGeoPositionInfoSource::NoError)
                emit failed(error == QGeoPositionInfoSource::AccessError ? QStringLiteral("Location access was denied or unavailable.")
                    : error == QGeoPositionInfoSource::UpdateTimeoutError ? QStringLiteral("Location acquisition timed out.")
                    : QStringLiteral("The location provider failed."));
        });
    }
    void request(int timeout) override {
        Q_UNUSED(timeout);
        if (!m_source) { emit failed(QStringLiteral("No location provider is available.")); return; }
        // App-bounded one-shot: Qt GeoClue cannot cancel its requestUpdate timer.
        m_source->startUpdates();
    }
    void stop() override { if (m_source) m_source->stopUpdates(); }
private:
    QGeoPositionInfoSource *m_source = nullptr;
};
}
NightTimeLocation::NightTimeLocation(QObject *parent)
    : NightTimeLocation([](QObject *owner) { return new QtLocationSource(owner); }, parent) {}
NightTimeLocation::NightTimeLocation(Factory factory, QObject *parent) : QObject(parent), m_factory(std::move(factory))
{
    qRegisterMetaType<NightTimeLocationFix>();
    m_timeout.setSingleShot(true);
}
NightTimeLocation::~NightTimeLocation() { ++m_generation; dispose(); }
void NightTimeLocation::dispose()
{
    m_timeout.stop();
    disconnect(&m_timeout, nullptr, this, nullptr);
    auto *source = m_source.data(); m_source = nullptr;
    if (source) {
        disconnect(source, nullptr, this, nullptr);
        source->stop();
        source->deleteLater();
    }
}
void NightTimeLocation::cancel()
{
    ++m_generation;
    const bool wasBusy = m_busy; m_busy = false; dispose();
    if (wasBusy) emit busyChanged(false);
}
void NightTimeLocation::requestLocation(quint64 token, int timeout)
{
    const bool wasBusy = m_busy;
    ++m_generation; m_busy = false; dispose();
    const quint64 generation = ++m_generation;
    m_token = token;
    if (timeout < 1000 || timeout > 120000) {
        QPointer<NightTimeLocation> guard(this);
        if (wasBusy) emit busyChanged(false);
        if (guard) emit finished(token, false, {}, QStringLiteral("Location timeout must be between 1 and 120 seconds.")); return;
    }
    m_busy = true;
    m_requestedAfter = QDateTime::fromSecsSinceEpoch(QDateTime::currentSecsSinceEpoch());
    m_source = m_factory ? m_factory(this) : nullptr;
    if (!m_source) { complete(generation, false, {}, QStringLiteral("No location provider is available.")); return; }
    connect(m_source, &NightTimeLocationSource::position, this, [this, generation](const NightTimeLocationFix &fix) {
        const bool valid = std::isfinite(fix.latitude) && std::isfinite(fix.longitude)
            && fix.latitude >= -90 && fix.latitude <= 90 && fix.longitude >= -180 && fix.longitude <= 180
            && fix.timestamp.isValid() && std::isfinite(fix.horizontalAccuracy) && fix.horizontalAccuracy >= -1;
        if (valid && fix.timestamp < m_requestedAfter) return; // Cached pre-request fix: await a current sample.
        complete(generation, valid, fix, valid ? QString() : QStringLiteral("The location provider returned an invalid fix."));
    });
    connect(m_source, &NightTimeLocationSource::failed, this, [this, generation](const QString &error) { complete(generation, false, {}, error); });
    connect(&m_timeout, &QTimer::timeout, this, [this, generation] { complete(generation, false, {}, QStringLiteral("Location acquisition timed out.")); });
    m_timeout.start(timeout);
    QPointer<NightTimeLocation> guard(this);
    if (!wasBusy) emit busyChanged(true);
    if (guard && m_busy && generation == m_generation && m_source) m_source->request(timeout);
}
void NightTimeLocation::complete(quint64 generation, bool success, const NightTimeLocationFix &fix, const QString &error)
{
    if (generation != m_generation || !m_busy) return;
    const quint64 token = m_token;
    ++m_generation; m_busy = false; dispose();
    QPointer<NightTimeLocation> guard(this);
    emit busyChanged(false);
    if (guard) emit finished(token, success, fix, error);
}
