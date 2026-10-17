#include "platform/kde/NightTimeBackend.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QTextStream>
#include <QTimer>

// Production read-only discovery. No setters, subscriptions or location factory.
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv); KdeNightTimeBackend backend;
    QObject::connect(&backend, &NightTimeBackend::readFinished, &app,
        [&](quint64 token, bool ok, const NightTimeSnapshot &s, const QString &error) {
        if (token != 1) return;
        auto fields = [](const QMap<NightTimeKey, NightTimeField> &values) {
            QJsonArray array;
            for (auto key : NightTimeTypes::keys()) {
                const auto f = values.value(key);
                const bool coordinate = key == NightTimeKey::Latitude || key == NightTimeKey::Longitude;
                const auto value = [&](const QVariant &v) -> QJsonValue {
                    if (coordinate) return QStringLiteral("redacted");
                    if (v.metaType().id() == QMetaType::QTime) return v.toTime().toString(Qt::ISODateWithMs);
                    return QJsonValue::fromVariant(v);
                };
                array.append(QJsonObject{{"group", NightTimeTypes::groupName(key)}, {"key", NightTimeTypes::keyName(key)},
                    {"value", value(f.value)}, {"known", f.known}, {"defaultValue", value(f.defaultValue)},
                    {"defaultKnown", f.defaultKnown}, {"defaultNoOp", f.defaultNoOp}, {"localPresent", f.localPresent},
                    {"effectivePresent", f.effectivePresent}, {"inheritedPresent", f.inheritedPresent}, {"globalPresent", f.globalPresent},
                    {"immutable", f.immutable}, {"writable", f.writable}, {"resettable", f.resettable},
                    {"reason", f.reason}, {"resetReason", f.resetReason}});
            } return array;
        };
        const auto r = s.runtime;
        QJsonObject result{{"success", ok}, {"error", error}, {"editorFields", fields(s.fields)}, {"daemonFields", fields(s.daemonFields)},
            {"cachedCoordinateAvailable", s.cachedLocation.available}, {"cachedCoordinateAge", "unknown"},
            {"runtime", QJsonObject{{"owner", r.owner}, {"epoch", qint64(r.epoch)}, {"version", r.version}, {"apiVersion", int(r.apiVersion)},
                 {"installedVerified", r.installedVerified}, {"serviceAvailable", r.serviceAvailable}, {"providerVerified", r.providerVerified}, {"reason", r.reason}}}};
        QTextStream(stdout) << QJsonDocument(result).toJson(QJsonDocument::Indented); app.exit(ok ? 0 : 1);
    });
    QTimer::singleShot(0, &app, [&] { backend.readCurrent(1); });
    QTimer::singleShot(25000, &app, [&] { QTextStream(stderr) << "Read-only discovery timed out\n"; app.exit(2); });
    return app.exec();
}
