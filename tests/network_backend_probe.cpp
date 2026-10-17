#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>
#include <QTimer>
#include "platform/network/NetworkManagerBackend.h"

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    NetworkManagerBackend backend;
    QObject::connect(&backend, &NetworkBackend::readFinished, &app,
        [&app](bool ok, const NetworkSnapshot &snapshot, const QString &error) {
        QJsonObject report{{QStringLiteral("readOnly"), true}, {QStringLiteral("available"), ok},
            {QStringLiteral("version"), snapshot.version}, {QStringLiteral("checkpointsSupported"), snapshot.checkpointAvailable},
            {QStringLiteral("connectivity"), int(snapshot.connectivity)},
            {QStringLiteral("connectivityCheckEnabled"), snapshot.connectivityCheckEnabled},
            {QStringLiteral("connectivityCheckAvailable"), snapshot.connectivityCheckAvailable},
            {QStringLiteral("error"), error}};
        QJsonArray devices;
        for (const auto &device : snapshot.devices) devices << QJsonObject{
            {QStringLiteral("interface"), device.interfaceName}, {QStringLiteral("type"), int(device.type)},
            {QStringLiteral("state"), int(device.state)}, {QStringLiteral("managed"), device.managed},
            {QStringLiteral("ipv4Addresses"), QJsonArray::fromStringList(device.ipv4Addresses)},
            {QStringLiteral("ipv6Addresses"), QJsonArray::fromStringList(device.ipv6Addresses)}};
        report.insert(QStringLiteral("devices"), devices);
        report.insert(QStringLiteral("profiles"), int(snapshot.profiles.size()));
        report.insert(QStringLiteral("accessPoints"), int(snapshot.accessPoints.size()));
        QJsonObject permissions;
        for (auto i = snapshot.permissions.begin(); i != snapshot.permissions.end(); ++i) permissions.insert(i.key(), i.value());
        report.insert(QStringLiteral("permissions"), permissions);
        QTextStream(stdout) << QJsonDocument(report).toJson(QJsonDocument::Indented);
        app.exit(ok ? 0 : 1);
    });
    QTimer::singleShot(20000, &app, [&app] { QTextStream(stderr) << "Read-only NetworkManager probe timed out.\n"; app.exit(2); });
    backend.readCurrent();
    return app.exec();
}
