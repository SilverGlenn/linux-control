#include "platform/input/InputBackend.h"
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>
#include <QTimer>
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    // Read-only by construction: never invoke writeChanges or a D-Bus setter.
    KWinInputBackend backend;
    InputSnapshot first; bool haveFirst = false;
    QObject::connect(&backend, &InputBackend::readFinished, &app,
        [&app, &backend, &first, &haveFirst](quint64, bool success, const InputSnapshot &snapshot, const QString &error) {
            if (success && !haveFirst) { first = snapshot; haveFirst = true; backend.readCurrent(2); return; }
            const bool stable = haveFirst && success && InputTypes::sameSnapshot(first, snapshot);
            QJsonArray devices;
            for (const auto &d : snapshot.devices) {
                QJsonArray controls;
                for (const auto &f : InputTypes::fields()) if (InputTypes::supported(d, f)) controls << QLatin1String(f.key);
                devices << QJsonObject{{"id", d.id}, {"touchpad", d.isTouchpad()},
                    {"supportedControls", controls}, {"values", QJsonObject::fromVariantMap(d.properties)}};
            }
            QJsonObject result{{"readOnly", true}, {"success", success}, {"repeatedReadStable", stable}, {"available", snapshot.available},
                {"writable", snapshot.writable}, {"version", snapshot.version}, {"reason", snapshot.reason},
                {"error", error}, {"devices", devices}};
            QTextStream(stdout) << QJsonDocument(result).toJson(QJsonDocument::Indented);
            app.exit(success && stable ? 0 : 1);
        });
    QTimer::singleShot(28000, &app, [&app] { QTextStream(stderr) << "Read-only input probe timed out.\n"; app.exit(2); });
    QTimer::singleShot(0, &backend, [&backend] { backend.readCurrent(1); });
    return app.exec();
}
