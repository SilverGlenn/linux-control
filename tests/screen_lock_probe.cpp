#include "platform/kde/ScreenLockBackend.h"
#include "platform/kde/ScreenLockAppearance.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QTextStream>
#include <QTimer>

// Inert production discovery: no configure, Lock, inhibition, shortcut or QML APIs.
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv); KdeScreenLockBackend backend;
    QObject::connect(&backend, &ScreenLockBackend::readFinished, &app,
        [&](quint64 token, bool ok, const ScreenLockSnapshot &s, const QString &error) {
        if (token != 1) return;
        QJsonArray fields;
        for (auto key : ScreenLockTypes::keys()) {
            const auto f = s.fields.value(key);
            fields.append(QJsonObject{{"group", ScreenLockTypes::groupName(key)}, {"key", ScreenLockTypes::keyName(key)},
                {"value", QJsonValue::fromVariant(f.value)}, {"known", f.known}, {"defaultValue", QJsonValue::fromVariant(f.defaultValue)},
                {"defaultKnown", f.defaultKnown}, {"defaultNoOp", f.defaultNoOp}, {"localPresent", f.localPresent},
                {"effectivePresent", f.effectivePresent}, {"inheritedPresent", f.inheritedPresent}, {"globalPresent", f.globalPresent},
                {"immutable", f.immutable}, {"writable", f.writable}, {"resettable", f.resettable},
                {"reason", f.reason}, {"resetReason", f.resetReason}});
        }
        const auto r = s.runtime; const auto a = ScreenLockAppearance::readCurrent(); QString safety;
        const bool safe = ScreenLockTypes::activeBranchValid(s.fields, safety);
        QJsonObject result{{"success", ok}, {"error", error}, {"fields", fields}, {"safeReloadProjection", safe}, {"projectionReason", safety},
            {"runtime", QJsonObject{{"owner", r.owner}, {"epoch", qint64(r.epoch)}, {"version", r.version},
                {"installedVerified", r.installedVerified}, {"serviceAvailable", r.serviceAvailable}, {"providerVerified", r.providerVerified}, {"reason", r.reason}}},
            {"appearance", QJsonObject{{"approvedIdentity", a.approvedIdentity}, {"providerVerified", a.providerVerified},
                {"backgroundAvailable", a.backgroundAvailable}, {"editable", a.editable}, {"shellId", a.shellId}, {"loginTheme", a.loginTheme},
                {"backgroundPath", a.backgroundPath}, {"backgroundDigest", QString::fromLatin1(a.backgroundDigest)},
                {"description", a.description}, {"reason", a.reason}, {"issues", QJsonArray::fromStringList(a.issues)}}}};
        QTextStream(stdout) << QJsonDocument(result).toJson(QJsonDocument::Indented); app.exit(ok ? 0 : 1);
    });
    QTimer::singleShot(0, &app, [&] { backend.readCurrent(1); });
    QTimer::singleShot(25000, &app, [&] { QTextStream(stderr) << "Read-only discovery timed out\n"; app.exit(2); });
    return app.exec();
}
