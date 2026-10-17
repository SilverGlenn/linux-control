#include "platform/kde/NightLightBackend.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QTextStream>
#include <QTimer>

// Inert production discovery only. This executable has no path to writeChanges,
// preview, stopPreview, a reload, inhibition or a Nighttime provider subscription.
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    KdeNightLightBackend backend;
    QObject::connect(&backend, &NightLightBackend::readFinished, &app,
        [&](quint64 token, bool ok, const NightLightSnapshot &s, const QString &error) {
        if (token != 1) return;
        const auto &r = s.runtime;
        QJsonObject runtime{{"owner", r.owner}, {"epoch", qint64(r.epoch)}, {"version", r.version},
            {"serviceAvailable", r.serviceAvailable}, {"schemaSupported", r.schemaSupported}, {"known", r.known},
            {"available", r.available}, {"enabled", r.enabled}, {"running", r.running}, {"inhibited", r.inhibited},
            {"daylight", r.daylight}, {"mode", int(r.mode)}, {"currentTemperature", int(r.currentTemperature)},
            {"targetTemperature", int(r.targetTemperature)}, {"previewSupported", r.previewSupported}, {"reason", r.reason}};
        QJsonArray fields;
        for (auto i = s.fields.cbegin(); i != s.fields.cend(); ++i) {
            const auto &f = i.value();
            fields.append(QJsonObject{{"key", NightLightTypes::keyName(i.key())},
                {"value", QJsonValue::fromVariant(f.value)}, {"known", f.known},
                {"defaultValue", QJsonValue::fromVariant(f.defaultValue)}, {"defaultKnown", f.defaultKnown},
                {"defaultNoOp", f.defaultNoOp}, {"effectivePresent", f.effectivePresent}, {"localPresent", f.localPresent},
                {"inheritedPresent", f.inheritedPresent}, {"globalPresent", f.globalPresent},
                {"immutable", f.immutable}, {"writable", f.writable}, {"resettable", f.resettable},
                {"reason", f.reason}, {"resetReason", f.resetReason}});
        }
        QJsonObject result{{"success", ok}, {"error", error}, {"runtime", runtime}, {"fields", fields},
            {"knownRuntimeValuesMatch", NightLightTypes::runtimeMatches(s)}};
        QTextStream(stdout) << QJsonDocument(result).toJson(QJsonDocument::Indented);
        app.exit(ok ? 0 : 1);
    });
    QTimer::singleShot(0, &app, [&] { backend.readCurrent(1); });
    QTimer::singleShot(12000, &app, [&] { QTextStream(stderr) << "Read-only discovery timed out\n"; app.exit(2); });
    return app.exec();
}
