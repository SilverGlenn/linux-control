#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>
#include <QTimer>
#include <optional>
#include "platform/apps/DefaultAppsBackend.h"

// Read-only: parses installed desktop entries and saved association files. It
// never calls KApplicationTrader (which can auto-build KSycoca), KEMailSettings
// (whose constructor writes), a settings KCM, or an executable from a desktop entry.
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv); KdeDefaultAppsBackend backend;
    std::optional<DefaultAppsSnapshot> first;
    QObject::connect(&backend, &DefaultAppsBackend::readFinished, &app,
        [&app, &backend, &first](quint64, bool ok, const DefaultAppsSnapshot &state, const QString &error) {
        if (ok && !first) { first = state; backend.readCurrent(2); return; }
        const bool stable = ok && first && first->revision == state.revision && first->preferences == state.preferences;
        QJsonArray roles;
        for (const auto &r : DefaultAppsTypes::roles()) {
            int compatible = 0;
            for (const auto &candidate : state.applications) if (DefaultAppsTypes::eligible(candidate, r)) ++compatible;
            roles << QJsonObject{{"role", r.key}, {"savedDesktopId", DefaultAppsTypes::selected(state.preferences, r)},
                {"compatibleInstalledApplications", compatible}, {"unavailableReason", state.unavailableRoles.value(r.role)}};
        }
        const QJsonObject report{{"readOnly", true}, {"available", ok}, {"writable", state.writable},
            {"auditedPlasmaVersion", "6.6.6"}, {"auditedKServiceVersion", "6.24.0"},
            {"installedDesktopEntries", int(state.applications.size())}, {"mimeTypes", int(state.mimeTypes.size())},
            {"savedMimePreferences", int(state.preferences.mimeDefaults.size())}, {"roles", roles},
            {"repeatedReadStable", stable}, {"serviceCacheInspected", false}, {"adoptionVerified", false}, {"reason", state.reason}, {"error", error}};
        QTextStream(stdout) << QJsonDocument(report).toJson(QJsonDocument::Indented); app.exit(stable ? 0 : 1);
    });
    QTimer::singleShot(30000, &app, [&app] { QTextStream(stderr) << "Read-only default-programs probe timed out.\n"; app.exit(2); });
    backend.readCurrent(1); return app.exec();
}
